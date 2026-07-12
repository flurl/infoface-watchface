# info-watchface ⇄ companion app protocol

Shared contract between the **info-watchface** Pebble app (built on the `pebble-dev` VM,
`~/info-watchface/`) and its native Android **companion app** (built on this PC). Both sides
are co-developed against this document — if you change the message shape, update this file
in the same change.

- **Watchface UUID:** `8047c3ec-ee69-418d-b0f1-3da7371aee63`

## Architecture (v2 — PebbleKit JS bridge)

```
companion app (Android)         PebbleKit JS (inside Pebble/Core app)      watchface (C)
┌─────────────────────┐         ┌──────────────────────────────────┐      ┌──────────────┐
│ CalendarSyncService  │  HTTP   │ src/pkjs/index.js                │ App  │ AppMessage   │
│  - reads Calendar    │◄────────│  - XHR GET /items                │Msg   │ inbox        │
│    Provider          │  GET    │  - Pebble.sendAppMessage(...)    │─────►│ handler      │
│  - serves JSON on    │ /items  │    (ItemCount, then N items)     │      │ (unchanged)  │
│    127.0.0.1:47225   │         │                                  │      │              │
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
  {"items": [{"prefix": "09:00", "text": "Standup"}, {"prefix": "12:30", "text": "Lunch w/ Sam"}]}
  ```
  Any other path → `404 Not Found`, `{}`.
- Items are **sorted by start time ascending**, capped at 8, `prefix`/`text` already truncated
  to the byte limits below (source of truth: `CalendarReader.kt`, reused from the pre-PKJS
  design). PKJS does not re-truncate.
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
`ServerUrl` takes effect without waiting for the next 15-minute cycle.

### Message keys

Keys are declared by name in the watchface's `package.json` → `pebble.messageKeys`, and the
Pebble build tool (`waf`) assigns each one a `uint32` at build time, in declaration order,
starting at 10000.

Current build (`build/js/message_keys.json` on the VM, 2026-07-12):

| Key name        | Numeric ID | Pebble type | Constraint                          |
|-----------------|-----------:|-------------|--------------------------------------|
| `ItemCount`     | `10000`    | UInt8       | 0–8 (see `MAX_INFO_ITEMS`)           |
| `ItemPrefix`    | `10001`    | cstring     | ≤ 7 chars + NUL (`char prefix[8]`)   |
| `ItemText`      | `10002`    | cstring     | ≤ 39 chars + NUL (`char text[40]`)   |
| `ItemIndex`     | `10003`    | UInt8       | 0-based, `< ItemCount`               |
| `ShowBattery`   | `10004`    | UInt8       | `0` or `1` (see below)               |
| `ShowQuietTime` | `10005`    | UInt8       | `0` or `1` (see below)               |
| `ShowBluetooth` | `10006`    | UInt8       | `0` or `1` (see below)               |
| `ServerUrl`     | `10007`    | cstring     | PKJS-only, see below — C ignores it  |

`MAX_INFO_ITEMS = 8` (watchface-side buffer cap, `src/c/info-watchface.c`).

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

### Message flow

PKJS sends **one `ItemCount` message, then one message per item** (not one giant message) —
Pebble's outbox is single-buffered, so each `sendAppMessage()` call awaits its success/failure
callback before sending the next (see `sendItemAt()`'s recursive continuation in
`src/pkjs/index.js`):

1. **Reset:** `{ItemCount: N}` where `0 <= N <= 8`.
   Watch clears its current item list on receipt (even if `N == 0` — an empty list is valid,
   e.g. no events today).
2. **Items ×N:** for `i` in `0 until N`, one message:
   `{ItemIndex: i, ItemPrefix: "<=7 chars>", ItemText: "<=39 chars>"}`.
   Watch writes into `s_items[i]`, and marks the info layer dirty as each item lands
   (progressive rendering) rather than waiting for all N.

No acknowledgement message flows watch→phone in this version — v1 is phone-to-watch only.

## Field semantics (calendar use case)

- `ItemPrefix` = event start time, `HH:mm` (24h), e.g. `"09:00"`, or `"•"` for all-day events.
- `ItemText` = event title, truncated to 39 chars by the **companion app** (`CalendarReader
  .kt`) before it's ever served over HTTP.
- Items sent **sorted by start time ascending**, capped at the first 8 upcoming events for
  the current day.

## Implementation notes

- **Watchface C (`src/c/info-watchface.c`):** the PKJS bridge itself only changed *who* sends
  the `AppMessage`, not its shape — `app_message_register_inbox_received()` handler,
  `prv_load_dummy_items()` cold-start fallback, bounds-checking, `strncpy` truncation are all
  as in v1. The calendar-sync item path is unchanged; corner-icon settings (`ShowBattery`,
  `ShowQuietTime`, see "Watch settings" above) were added later and share the same inbox
  handler.
- **`package.json`:** `pebble.companionApp.android.apps[0].package` =
  `family.dieflomis.infocompanion` is kept even though the companion app no longer uses
  PebbleKit2 — it's still useful for Core app onboarding UX (suggesting the companion app to
  install). Harmless if unused.
- **Companion app (Kotlin):** `CalendarSyncService` must actually be running (permission
  granted, service started) for PKJS's fetch to succeed — there's no retry/backoff beyond
  PKJS's own 15-minute interval, and no user-visible "sync failed" surfacing on the watch side
  currently (future enhancement, not in scope).
