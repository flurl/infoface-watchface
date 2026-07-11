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
(`REFRESH_INTERVAL_MS`), fetches `/items` via `XMLHttpRequest` and relays via
`Pebble.sendAppMessage()` using the **same message-key names** as the C side (PKJS resolves
names to the build's numeric IDs automatically — no manual mapping needed, unlike the old
Kotlin-side integration). On any XHR failure (companion app not running, service stopped,
etc.), the fetch is skipped and the watch keeps showing whatever it last had — no explicit
"clear" on failure.

### Message keys

Keys are declared by name in the watchface's `package.json` → `pebble.messageKeys`, and the
Pebble build tool (`waf`) assigns each one a `uint32` at build time, in declaration order,
starting at 10000.

Current build (`build/js/message_keys.json` on the VM, 2026-07-11):

| Key name     | Numeric ID | Pebble type | Constraint                          |
|--------------|-----------:|-------------|--------------------------------------|
| `ItemCount`  | `10000`    | UInt8       | 0–8 (see `MAX_INFO_ITEMS`)           |
| `ItemPrefix` | `10001`    | cstring     | ≤ 7 chars + NUL (`char prefix[8]`)   |
| `ItemText`   | `10002`    | cstring     | ≤ 39 chars + NUL (`char text[40]`)   |
| `ItemIndex`  | `10003`    | UInt8       | 0-based, `< ItemCount`               |

`MAX_INFO_ITEMS = 8` (watchface-side buffer cap, `src/c/info-watchface.c`).

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

- **Watchface C (`src/c/info-watchface.c`):** ✅ unchanged since v1 — this bridge only changes
  *who* sends the `AppMessage`, not its shape. `app_message_register_inbox_received()` handler,
  `prv_load_dummy_items()` cold-start fallback, bounds-checking, `strncpy` truncation — all as
  before.
- **`package.json`:** `pebble.companionApp.android.apps[0].package` =
  `family.dieflomis.infocompanion` is kept even though the companion app no longer uses
  PebbleKit2 — it's still useful for Core app onboarding UX (suggesting the companion app to
  install). Harmless if unused.
- **Companion app (Kotlin):** `CalendarSyncService` must actually be running (permission
  granted, service started) for PKJS's fetch to succeed — there's no retry/backoff beyond
  PKJS's own 15-minute interval, and no user-visible "sync failed" surfacing on the watch side
  currently (future enhancement, not in scope).
