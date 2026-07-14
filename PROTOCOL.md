# info-watchface ⇄ companion app protocol

Shared contract between the **info-watchface** Pebble app (built on the `pebble-dev` VM,
`~/info-watchface/`) and its native Android **companion app** (built on this PC). Both sides
are co-developed against this document — if you change the message shape, update this file
in the same change.

- **Watchface UUID:** `8047c3ec-ee69-418d-b0f1-3da7371aee63`

## Architecture (v3 — multiple info panels)

```
companion app (Android)         PebbleKit JS (inside Pebble/Core app)      watchface (C)
┌─────────────────────┐         ┌──────────────────────────────────┐      ┌──────────────┐
│ CalendarSyncService  │  HTTP   │ src/pkjs/index.js                │ App  │ AppMessage   │
│  - reads Calendar    │◄────────│  - XHR GET /items                │Msg   │ inbox        │
│    + Weather sources │  GET    │  - Pebble.sendAppMessage(...)    │─────►│ handler      │
│  - serves panels     │ /items  │    (PanelCount, then per panel:  │      │ (per-panel   │
│    JSON on           │         │     ItemCount/PanelTitle, then   │      │  item store) │
│    127.0.0.1:47225   │         │     N items)                     │      │              │
└─────────────────────┘         └──────────────────────────────────┘      └──────────────┘
```

**Why this indirection, instead of the companion app sending `AppMessage` directly:** the
external `io.rebble.pebblegit2` (`PebbleKitAndroid2`) IPC path — `DefaultPebbleSender
.sendDataToPebble()` — consistently fails with `TransmissionResult.FailedDifferentAppOpen`
against this watchface specifically, even when independently confirmed (via the Core app's own
`content://coredevices.coreapp.pebblekit/activeApp/<watchId>` provider, and visually) to be the
active/displayed app. Confirmed via a controlled A/B test: a plain watchapp (explicitly
launched by the user) receives `sendDataToPebble()` sends from the same companion app
correctly — only the watchface is affected. Root cause traced into PebbleOS firmware:
`app_run_state_send_update()` (`src/fw/process_management/app_run_state.c`) silently no-ops
when `comm_session_get_system_session()` returns `NULL` at the moment a watchface becomes
active, so the phone's foreground-app tracking used for this specific permission check goes
stale — a firmware/Core-app bug, not something fixable from the companion app's side. Verified
this bug does **not** affect PebbleKit JS's `Pebble.sendAppMessage()`, since PKJS runs inside
the Pebble/Core app itself (a different code path than the external PebbleKitAndroid2 bound
service). So: the companion app no longer touches PebbleKit2 or the watch at all — it just
serves calendar data locally, and PKJS (which already reliably knows it's running, by
construction) does the actual `AppMessage` delivery.

Also considered and rejected: **Pebble timeline pins** (`PebbleSender.insertTimelinePin()`) —
not gated on foreground-app state either, but there is no public C API for a watchface to read
pin data back into its own custom UI (`pebble.h` has zero timeline declarations; pins are only
viewable via the system's own Timeline UI). Would have required dropping the custom info-feed
UI entirely. The PKJS bridge preserves it with zero C-code changes.

## Local HTTP API (companion app → PebbleKit JS)

The URL PKJS fetches is user-configurable (see `ServerUrl` under "Watch settings" below) and
not required to point at the companion app at all — anything serving the JSON shape below
works, e.g. a self-hosted web service returning something other than calendar items. The
companion app is just the **default** source:

- **Server:** `CalendarSyncService`, a foreground service in the companion app. Binds to
  `127.0.0.1:47225` only (not reachable off-device). Started automatically on app launch
  (after the `READ_CALENDAR` permission is granted); stoppable via the app's UI.
- **Endpoint:** `GET /items` → `200 OK`, `Content-Type: application/json`:
  ```json
  {"panels": [
    {"title": "Events", "items": [
      {"type": 1, "prefix": "Fri 09:00", "text": "Standup"},
      {"type": 1, "prefix": "Fri 12:30", "text": "Lunch w/ Sam"},
      {"type": 0, "prefix": "", "text": ""},
      {"type": 1, "prefix": "Sat 10:00", "text": "Market"}
    ]},
    {"title": "Weather", "items": [
      {"type": 2, "prefix": "22°C", "text": "Sunny"}
    ]}
  ]}
  ```
  Any other path → `404 Not Found`, `{}`.
- The response is a list of **panels** (up to `MAX_PANELS = 4`, see "Info panels" below), each with an
  optional `title` (≤15 chars, shown on the watch as the panel's header/divider label — see "Info
  panels") and its own `items` list, independently capped at `MAX_INFO_ITEMS` (8) the same way a
  single item list was capped in v2.
- **Back-compat:** a server may still respond with the flat v2 shape, `{"items": [...]}` — PKJS treats
  that as a single untitled panel. The companion app itself always emits the `panels` shape; the
  fallback exists for any other server implementing this endpoint (see the note about the URL being
  user-configurable, above).
- Each item carries a `type` (`0`=divider, `1`=event, `2`=weather, `255`=other; see `ItemType` above).
  Divider items have empty `prefix`/`text` and render as a horizontal rule on the watch. `type` is
  optional in the JSON for back-compat — PKJS and the watch both default a missing `type` to event (`1`).
  See "Field semantics" below for what `prefix`/`text` mean per type.
- Within the Events panel, items are **sorted by start time ascending**, grouped by calendar day with
  a divider inserted between day groups (never a leading divider), capped at 8 items total (dividers
  included), with `prefix`/`text` already truncated to the byte limits below (source of truth:
  `CalendarReader.kt`, reused from the pre-PKJS design). PKJS does not re-truncate.
- No auth — loopback-only is the security boundary (same phone, same user).

## PebbleKit JS (watchface → watch)

`src/pkjs/index.js`: on the `ready` event, and then every 15 minutes
(`REFRESH_INTERVAL_MS`), fetches the configured URL (`getServerUrl()`, see below) via
`XMLHttpRequest` and relays via `Pebble.sendAppMessage()` using the **same message-key
names** as the C side (PKJS resolves names to the build's numeric IDs automatically — no
manual mapping needed, unlike the old Kotlin-side integration). On any XHR failure
(companion app not running, service stopped, wrong URL, etc.), the fetch is skipped and the
watch keeps showing whatever it last had — no explicit "clear" on failure. Also re-fetches
immediately on `webviewclosed` (i.e. right after the Settings screen is saved) so a changed
`ServerUrl` takes effect without waiting for the next 15-minute cycle. Since v3, the fetched
JSON is a list of panels rather than a flat item list — PKJS walks `data.panels` (or wraps a
legacy `data.items` as one untitled panel) and sends one `PanelCount` reset followed by each
panel's `ItemCount`/`PanelTitle` and items in turn; see "Message flow" below.

### Message keys

Keys are declared by name in the watchface's `package.json` → `pebble.messageKeys`, and the
Pebble build tool (`waf`) assigns each one a `uint32` at build time, in declaration order,
starting at 10000.

Current build (`build/js/message_keys.json` on the VM, 2026-07-14):

| Key name        | Numeric ID | Pebble type | Constraint                          |
|-----------------|-----------:|-------------|--------------------------------------|
| `ItemCount`     | `10000`    | UInt8       | 0–8 (see `MAX_INFO_ITEMS`)           |
| `ItemPrefix`    | `10001`    | cstring     | ≤ 11 chars + NUL (`char prefix[12]`) |
| `ItemText`      | `10002`    | cstring     | ≤ 39 chars + NUL (`char text[40]`)   |
| `ItemIndex`     | `10003`    | UInt8       | 0-based, `< ItemCount`               |
| `ShowBattery`   | `10004`    | UInt8       | `0` or `1` (see below)               |
| `ShowQuietTime` | `10005`    | UInt8       | `0` or `1` (see below)               |
| `ShowBluetooth` | `10006`    | UInt8       | `0` or `1` (see below)               |
| `ServerUrl`     | `10007`    | cstring     | PKJS-only, see below — C ignores it  |
| `ItemType`      | `10008`    | UInt8       | `0`=divider, `1`=event, `2`=weather, `255`=other |
| `TapAxisX`      | `10009`    | UInt8       | `0` or `1` (see "Page-turn tap gesture" below) |
| `TapAxisY`      | `10010`    | UInt8       | `0` or `1`                           |
| `TapAxisZ`      | `10011`    | UInt8       | `0` or `1`                           |
| `TapThresholdMg`| `10012`    | **Int32**   | milli-Gs, Clay `slider` component    |
| `TapRingdownMs` | `10013`    | **Int32**   | milliseconds, Clay `slider` component |
| `TapMultiTapWindowMs` | `10014` | **Int32** | milliseconds, Clay `slider` component |
| `EnablePagination` | `10015` | UInt8 | `0` or `1`, default `0` (see "Page-turn tap gesture" below) |
| `PanelCount`    | `10016`    | UInt8       | 0–4 (see `MAX_PANELS`), master reset for the whole panel set |
| `PanelIndex`    | `10017`    | UInt8       | 0-based, `< PanelCount`; absent ⇒ `0` (back-compat with a v2-only sender) |
| `PanelTitle`    | `10018`    | cstring     | ≤ 15 chars + NUL (`char title[16]`); optional, empty ⇒ no header label |

`MAX_INFO_ITEMS = 8` (per-panel buffer cap) and `MAX_PANELS = 4` (watchface-side, both in
`src/c/info-watchface.c`).

**Note on key IDs:** these are assigned by declaration order in `package.json`'s
`pebble.messageKeys`, starting at 10000 — `PanelCount`/`PanelIndex`/`PanelTitle` were **appended**
to the end of that list rather than inserted alongside the related `ItemCount`/`ItemIndex`/etc., so
every existing ID stays stable. Don't reorder this list without re-checking every ID above.

## Watch settings (Clay)

The watch's Settings screen (accessed from the phone's Pebble/Core app, per-watchapp
"gear" icon) is a `src/pkjs/config.js` schema rendered by
[`@rebble/clay`](https://github.com/pebble-dev/clay) — **not** the stale official
`pebble-clay` npm package (frozen at 1.0.4, no `flint`/`gabbro` support). This is a
deliberate exception to generally preferring repebble over rebble sources: the
community-maintained fork is the only one that builds for two of our three target
platforms. Clay auto-registers the `showConfiguration`/`webviewclosed` PKJS
event handlers itself (`src/pkjs/index.js` just constructs `new Clay(clayConfig)`) and
sends the settings dict over the **same** `AppMessage` inbox as calendar sync — Clay's
`prepareForAppMessage()` converts JS booleans to `0`/`1` before sending.

- **`ShowBattery`** (`0`/`1`, default `1`): shows/hides the battery indicator in the
  top-right corner. Watchface persists it via `persist_write_bool()`
  (`PERSIST_KEY_SHOW_BATTERY = 1`) and re-reads it on every cold start, so it survives
  app relaunch without waiting for the phone to resend it.
- **`ShowQuietTime`** (`0`/`1`, default `1`): shows/hides the quiet-time (crescent moon)
  indicator in the top-left notification area, same persist/re-read pattern as `ShowBattery`
  (`PERSIST_KEY_SHOW_QUIET_TIME = 2`). Unlike the battery icon (pushed via
  `battery_state_service_subscribe()`), there's no subscribe/event API for quiet time —
  only the peek-style `quiet_time_is_active()` — so the icon's update proc is just
  re-triggered on every minute tick alongside the clock; a quiet-time toggle can take up to
  a minute to appear/disappear.
- **`ShowBluetooth`** (`0`/`1`, default `1`): enables the Bluetooth-disconnect alert — a
  Bluetooth-rune icon in the top-left notification area plus an obtrusive custom vibe
  (four long pulses, `vibes_enqueue_custom_pattern()`) on the transition to disconnected.
  Same persist/re-read pattern (`PERSIST_KEY_SHOW_BLUETOOTH = 3`). Connection state DOES have
  a subscribe API (`connection_service_subscribe()`), so the icon updates immediately on
  connect/disconnect. The vibe fires **only on a genuine connected→disconnected transition**:
  a `s_bt_connected` tracker is seeded from `connection_service_peek_pebble_app_connection()`
  at init (without calling the handler, so launching the watchface while disconnected never
  buzzes), and the handler buzzes only when it sees `s_bt_connected && !connected`. The icon
  itself always renders from a live peek, so it's correct from the first draw regardless. The
  setting gates both the icon and the vibration.
- **`ServerUrl`** (default `http://127.0.0.1:47225/items`, shared between `config.js` and
  `index.js` via `src/pkjs/config-defaults.js` so the two can't drift): the URL PKJS fetches
  items from — see "Local HTTP API" above, not restricted to the companion app. This one is
  declared as a message key purely so Clay's `prepareForAppMessage()` has a valid numeric ID
  to serialize it under (an undeclared key produces a `NaN` AppMessage key and silently
  breaks the whole save, including `ShowBattery` in the same message); the watch's C code
  never reads it. PKJS itself reads the live value straight out of Clay's
  `localStorage['clay-settings']` (`getServerUrl()`), not from AppMessage — see
  "PebbleKit JS" above.

### Page-turn tap gesture

Pagination itself is off by default — see **`EnablePagination`** below — and everything in this
section only matters while it's on.

The watchface has no touch or button input (touch is reserved for watchapps; the system shell
owns all buttons on a watchface — see `src/c/info-watchface.c`'s comment above
`prv_accel_data_handler`), so the info feed's page turns are driven by a hand-rolled
accelerometer jolt detector (`accel_data_service_subscribe()`, **not**
`accel_tap_service_subscribe()` — the latter is fed by the system's shared "Motion Sensitivity"
setting, not independently tunable from app code). Only a **triple** tap turns the page — a
single tap or a double tap is deliberately ignored. A tap sequence starts on the first jolt and
stays open, extending its wait window on every further jolt, until the window elapses with no
new jolt; the sequence's final tap count then decides whether to act (exactly 3) or discard
(anything else, including 1, 2, or 4+). All of the detector's parameters are exposed as Clay
settings so they can be tuned from the phone without recompiling:

- **`EnablePagination`** (`0`/`1`, default `0`): master switch for the whole feature. **Off**
  (the default): the info feed shows only as many events as fit on one screen — same
  hard-truncation behavior as before pagination existed — with no page indicator, and the
  accelerometer is not even subscribed to (`prv_unsubscribe_accel()`/never subscribed in
  `prv_init()`), so there's no extra battery draw for a gesture that couldn't do anything
  anyway. **On:** extra events spill onto additional pages, turned by the triple-tap gesture
  below; the watchface subscribes to `accel_data_service` live on the transition (no reinstall
  needed) via `prv_subscribe_accel()`, which also resets any leftover tap-detection state so a
  stale in-progress sequence from before a gap in subscription can't bleed through. Toggling
  **off** immediately resets `s_page` to `0` and unsubscribes. Persisted under
  `PERSIST_KEY_ENABLE_PAGINATION` (`26`).
- **`TapAxisX`/`TapAxisY`/`TapAxisZ`** (`0`/`1`, default `1` — all three enabled): which
  accelerometer axes contribute to the jolt magnitude. A disabled axis's sample-to-sample delta
  is treated as `0`, so motion on that axis alone can never register a tap. Persisted under
  `PERSIST_KEY_TAP_AXIS_X/Y/Z` (`20`/`21`/`22`).
- **`TapThresholdMg`** (milli-Gs, default `300`): the sample-to-sample jolt magnitude that
  counts as a tap. Lower = more sensitive. The watch squares it once (`s_tap_threshold_sq`) and
  compares against the squared per-axis delta sum every sample, avoiding a `sqrt()` per sample.
  Persisted under `PERSIST_KEY_TAP_THRESHOLD_MG` (`23`).
- **`TapRingdownMs`** (milliseconds, default `160`): minimum gap between two raw jolts for them
  to count as separate taps, so one physical knock's mechanical ringing isn't itself counted as
  a second tap. Converted to a sample count (`s_tap_ringdown_samples`) via the fixed 25Hz
  accelerometer sampling rate. Persisted under `PERSIST_KEY_TAP_RINGDOWN_MS` (`24`).
- **`TapMultiTapWindowMs`** (milliseconds, default `400`): how long after each jolt to wait for
  the next one before finishing the sequence. Reset on every jolt, not just the first, so each
  tap in a triple gets its own full window to be followed by the next. Must stay comfortably
  above `TapRingdownMs` or a deliberate next tap could be swallowed by the ringdown debounce
  instead of being recognized. Persisted under `PERSIST_KEY_TAP_MULTI_TAP_WINDOW_MS` (`25`).

All seven are read/written in `prv_inbox_received_handler()`/`prv_init()` alongside the other Clay
settings (same "any subset, independent `if` per key" pattern — see "Inbox handler contract"
below); the three numeric fields are recomputed into their derived sample-count form by
`prv_recompute_tap_params()` on load and on every settings save that touches one of them.

**Clay component gotcha (the reason for the Int32 column above):** the numeric fields use Clay's
`"slider"` component, not `"input"`. Clay's `"input"` manipulator (`val`) returns the raw HTML
value as a **string**, which fails `prepareForAppMessage()`'s `typeof value === 'number'` check
and gets sent as a `cstring` tuple instead of a number — silently wrong for a value meant to be
read as an int. `"slider"`'s manipulator does `parseFloat()`, producing a real JS number that
Clay serializes as a signed 32-bit int. The three toggles (`TapAxisX/Y/Z`) don't have this
problem — Clay's `"toggle"` sends a boolean, which also becomes a 0/1 **Int32** on the wire, but
since the value is always `0` or `1` it's safe to read via `->value->uint8` (matching
`ShowBattery` etc. above) — the low byte of a small little-endian Int32 is the correct value
regardless of the tuple's real width. **The `TapThresholdMg`/`TapRingdownMs`/
`TapMultiTapWindowMs` fields do not have that luxury** (values run well past 255) and must be
read via `->value->int32`, not `->value->uint8` — reading the latter would silently truncate to
the low byte.

**Inbox handler contract:** every save sends **all** changed Clay fields in a single
`AppMessage` dict (Clay's `getSettings()` re-serializes the whole form, not just the diff).
`prv_inbox_received_handler()` in `src/c/info-watchface.c` must therefore check every
settings key with an independent `if (dict_find(...))` block — not an early `return` after
the first match — before falling through to the calendar-sync (`ItemCount`/`ItemIndex`)
branches below. (This was a real bug during development: `ShowQuietTime` silently never
applied because the `ShowBattery` branch returned first.)

**Target platforms:** `emery` (Pebble Time 2), `flint` (Pebble 2 Duo), `gabbro` (Pebble
Round 2) only — `aplite`/`basalt`/`chalk`/`diorite` were dropped from
`package.json`'s `targetPlatforms` since those are the three watches actually in use.

### Info panels

The bottom half of the watchface can hold up to **`MAX_PANELS = 4`** independent info panels — each
one a self-contained item list from its own source (calendar events, weather, ...), configured in
the companion app's new "Panels" fieldset (see "Companion app" below). Only one panel is visible at
a time; a **quadruple wrist tap** rotates to the next one. This reuses the exact same accelerometer
jolt detector as the page-turn gesture below (same axis/threshold/ringdown/window settings, same tap
sequence state machine) — the only difference is the sequence's final tap count: **3** turns the
page within the current panel, **4** rotates to the next panel. Any other count (1, 2, 5+) is
discarded, same as before.

Panel rotation does **not** depend on `EnablePagination` — it works whether pagination is on or off,
since it's a different axis (which source you're looking at) than pagination (which page of that
source you're on). Consequently the accelerometer is now subscribed whenever **either**
`EnablePagination` is on **or** more than one panel is active (`prv_update_accel_subscription()`,
called from `prv_init()` and from both the `EnablePagination` and `PanelCount` inbox branches) —
previously it was gated on `EnablePagination` alone.

**Panel header / indicator:** rather than adding a separate row of dots for "which panel," the
existing plain divider line at the top of the info area doubles as the indicator. If the current
panel has a non-empty `PanelTitle`, it's drawn left-aligned at the start of that line and the rule
fills the rest of the width, e.g. `Events————————————`; switching panels (quadruple tap) changes the
label immediately. A panel with an empty title (or when only one panel is configured) falls back to
the plain full-width rule from v1/v2 — unchanged behavior for the common single-panel case.

Rotating panels resets the page within the new panel to `0` (`s_page = 0`) — a page position from
one panel has no meaning in another.

### Message flow

PKJS sends **one `PanelCount` message, then, for each panel, one `ItemCount`/`PanelTitle` message
followed by one message per item** (not one giant message) — Pebble's outbox is single-buffered, so
each `sendAppMessage()` call awaits its success/failure callback before sending the next (see the
flat panel/item step queue in `src/pkjs/index.js`):

1. **Reset:** `{PanelCount: P}` where `0 <= P <= 4`. Watch clears all panels, resets the current
   panel and page to `0`, and updates the accelerometer subscription (see "Info panels" above) —
   even if `P == 0` (no panels configured is valid, renders "Nothing to see").
2. **Per panel**, for `p` in `0 until P`:
   a. `{PanelIndex: p, ItemCount: N, PanelTitle: "<=15 chars>"}` where `0 <= N <= 8`. Watch clears
      panel `p`'s item list and records its title (even if `N == 0`).
   b. **Items ×N:** for `i` in `0 until N`, one message: `{PanelIndex: p, ItemIndex: i, ItemType:
      0|1|2|255, ItemPrefix: "<=11 chars>", ItemText: "<=39 chars>"}`. Watch writes into
      `s_panels[p].items[i]`, and marks the info layer dirty as each item lands (progressive
      rendering, only visibly so for the currently-displayed panel) rather than waiting for all N.
      `ItemType` is optional (defaults to event); divider items send empty (or omit)
      `ItemPrefix`/`ItemText`.

`PanelIndex` absent on an item/count message ⇒ panel `0`, so a legacy v2 sender that only ever sent
bare `ItemCount`/`ItemIndex` messages (no `PanelCount` at all) still lands its items in panel 0 and
renders exactly as before — full backward compatibility with a single-panel-only counterpart.

No acknowledgement message flows watch→phone in this version — phone-to-watch only.

## Field semantics

- `ItemType` = `1` (event) for calendar events, `0` (divider) for the day-separator rows the
  companion app inserts between calendar days, `2` (weather) for the weather panel's rows (see
  below). `255` (other) is reserved for future item kinds.
- `ItemPrefix` (events) = weekday + start time, `EEE HH:mm` (24h), e.g. `"Fri 09:00"`, or
  `"EEE •"` (e.g. `"Fri •"`) for all-day events. The weekday is included so items from different
  days are distinguishable at a glance, in addition to the divider between day groups. Empty for
  dividers. A **multi-day event is expanded into one item per calendar day it covers** (within the
  window), each grouped under its own day; the prefix carries a span marker instead of a time:
  `"EEE |->"` on the event's first day, `"EEE <->"` while it is ongoing, and `"EEE <-|"` on its
  last day (e.g. `"Fri |->"`, `"Sat <->"`, `"Sun <-|"`).
- `ItemText` (events) = event title, truncated to 39 chars by the **companion app** (`CalendarReader
  .kt`) before it's ever served over HTTP. Empty for dividers.
- The companion app reads events from the start of today through the next **7 days**, sorts them
  **by start time ascending**, inserts a divider before the first event of each new calendar day
  (never a leading divider), and caps the combined list (events + dividers) at the first 8 items.

### Weather (`ItemType = 2`)

A single item type covers every weather row — there is deliberately **no per-condition type code**
(no "rain type", "snow type", etc.). Instead:

- `ItemPrefix` = the temperature, formatted per the companion app's °C/°F setting (default °C), e.g.
  `"22°C"` or `"72°F"`.
- `ItemText` = a short human-readable condition, optionally with a percentage, e.g. `"Sunny"`,
  `"Cloudy"`, `"Rain 60%"`, `"Snow"`.
- The watch (`prv_weather_icon_for()` in `src/c/info-watchface.c`) picks which icon to draw beside
  the prefix by doing a **case-insensitive substring match on `ItemText`** for a handful of condition
  keywords (`"rain"`, `"cloud"`, `"snow"`, `"sun"`/`"clear"`, ...), falling back to a generic/default
  icon for anything it doesn't recognize. This is a deliberate design choice over a dedicated type
  code per condition: adding a new weather condition later (e.g. "Fog", "Windy") needs **no protocol
  change at all** — the temp + text still render correctly even if the specific icon isn't
  recognized yet, it just falls back to the default.
- The companion app's weather source (`WeatherReader.kt`) delegates to a swappable `WeatherProvider`
  (`WeatherProvider.kt`), chosen via radio buttons in the app's Weather fieldset:
  - **Open-Meteo** (`OpenMeteoProvider.kt`) — no API key or signup, ~10,000 free calls/day.
  - **MET Norway** (`MetNorwayProvider.kt`) — no API key, but their fair-use policy asks for a
    contact email in the request's User-Agent; the app has a settings field for it, and the
    provider is skipped (empty panel) until one is entered.
  - **Test data** — the original fixed fake forecast, needing neither a location fix nor network
    access; useful for testing indoors/offline.
  Both real providers need a device location fix (`DeviceLocation.kt`, coarse accuracy, no Play
  Services dependency) and render an empty panel without one. Every provider funnels through the
  same formatting into the wire shape described above, so **the protocol itself never changed** —
  swapping/adding a weather provider is entirely internal to the companion app.

## Implementation notes

- **Watchface C (`src/c/info-watchface.c`):** the PKJS bridge itself only changed *who* sends
  the `AppMessage`, not its shape — `app_message_register_inbox_received()` handler,
  `prv_load_dummy_items()` cold-start fallback, bounds-checking, `strncpy` truncation are all
  as in v1. The calendar-sync item path gained an `ItemType` field (divider vs. event; dividers
  render as a horizontal rule in `prv_info_update_proc`, events as before) and a wider
  `prefix[12]` buffer for the `EEE HH:mm` weekday prefix; corner-icon settings (`ShowBattery`,
  `ShowQuietTime`, see "Watch settings" above) were added later and share the same inbox
  handler. v3 replaced the single flat `s_items[MAX_INFO_ITEMS]` array with
  `s_panels[MAX_PANELS]`, each holding its own item array/count/title, and reworked the persist
  key layout to make room (see "Info panels" above); the pagination helpers
  (`prv_page_end`/`prv_num_pages`/`prv_page_start`/`prv_layout_num_pages`) now take an explicit
  `(items, count)` pair instead of reading the old globals, so they work against whichever panel
  is current.
- **`package.json`:** `pebble.companionApp.android.apps[0].package` =
  `family.dieflomis.infocompanion` is kept even though the companion app no longer uses
  PebbleKit2 — it's still useful for Core app onboarding UX (suggesting the companion app to
  install). Harmless if unused.
- **Companion app (Kotlin):** `CalendarSyncService` must actually be running (permission
  granted, service started) for PKJS's fetch to succeed — there's no retry/backoff beyond
  PKJS's own 15-minute interval, and no user-visible "sync failed" surfacing on the watch side
  currently (future enhancement, not in scope). Since v3, the service's JSON provider assembles
  a `List<Panel>` from up to 4 configured sources (`CalendarPrefs.panelSources`, set via the
  "Panels" fieldset in `MainActivity`) — currently `CALENDAR` (`readUpcomingEvents`, unchanged)
  and `WEATHER` (`WeatherReader.readWeather`, delegating to a selectable `WeatherProvider` — see
  "Weather" above) — and serializes them with `panelsToJson()` instead of the old flat
  `itemsToJson()`.
