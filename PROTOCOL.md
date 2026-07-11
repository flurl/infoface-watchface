# info-watchface ⇄ companion app protocol

Shared contract between the **info-watchface** Pebble app (built on the `pebble-dev` VM,
`~/info-watchface/`) and its native Android **companion app** (built on this PC). Both sides
are co-developed against this document — if you change the message shape, update this file
in the same change.

- **Watchface UUID:** `8047c3ec-ee69-418d-b0f1-3da7371aee63`
- **Transport:** Pebble `AppMessage`, sent by the companion app via `io.rebble.pebblekit2`'s
  `DefaultPebbleSender.sendDataToPebble(APP_UUID, dict)`.

## Message keys

Keys are declared by name in the watchface's `package.json` → `pebble.messageKeys`, and the
Pebble build tool (`waf`) assigns each one a `uint32` at build time, in declaration order,
starting at 10000. **The companion app has no visibility into the watchface's build**, so
these numbers are recorded here and must be kept in sync by hand if `messageKeys` in
`package.json` is ever reordered or extended (rebuild the watchface, diff the new
`build/js/message_keys.json` against the table below).

Current build (`build/js/message_keys.json` on the VM, 2026-07-11):

| Key name     | Numeric ID | Pebble type | Constraint                          |
|--------------|-----------:|-------------|--------------------------------------|
| `ItemCount`  | `10000`    | UInt8       | 0–8 (see `MAX_INFO_ITEMS`)           |
| `ItemPrefix` | `10001`    | cstring     | ≤ 7 chars + NUL (`char prefix[8]`)   |
| `ItemText`   | `10002`    | cstring     | ≤ 39 chars + NUL (`char text[40]`)   |
| `ItemIndex`  | `10003`    | UInt8       | 0-based, `< ItemCount`               |

`MAX_INFO_ITEMS = 8` (watchface-side buffer cap, `src/c/info-watchface.c`).

## Message flow

The companion app sends **one `ItemCount` message, then one message per item** (not one giant
message) — Pebble's outbox is single-buffered, so each `sendDataToPebble()` call must be
awaited (it's a suspending call) before sending the next:

1. **Reset:** `{ItemCount: N}` where `0 <= N <= 8`.
   Watch clears its current item list on receipt (even if `N == 0` — an empty list is valid,
   e.g. no events today).
2. **Items ×N:** for `i` in `0 until N`, one message:
   `{ItemIndex: i, ItemPrefix: "<=7 chars>", ItemText: "<=39 chars>"}`.
   Watch writes into `s_items[i]`, and should mark the info layer dirty as each item lands
   (progressive rendering) rather than waiting for all N.

No acknowledgement message flows watch→phone in this version — v1 is phone-to-watch only.
(The library supports a `BasePebbleListenerService` for the reverse direction if we need it
later, e.g. for a "refresh now" request from the watch.)

## Field semantics (calendar use case)

- `ItemPrefix` = event start time, `HH:MM` (24h), e.g. `"09:00"`.
- `ItemText` = event title, truncated to 39 chars by the **companion app** before sending
  (don't rely on the watch to truncate gracefully — it will hard-cut via `strncpy`).
- Items sent **sorted by start time ascending**, capped at the first 8 upcoming events for
  the current day.

## Implementation notes (for whoever picks up each side)

- **Watchface (`src/c/info-watchface.c`):** ✅ done. `app_message_register_inbox_received()`
  handler added (kept `prv_load_dummy_items()` as the cold-start fallback, overwritten once
  real messages arrive). Bounds-checks `ItemIndex < MAX_INFO_ITEMS` before writing, uses
  `strncpy(dest, src, sizeof(dest) - 1); dest[sizeof(dest) - 1] = '\0';` for both string
  fields. Verified in the emulator via `pebble send-app-message` with the real numeric keys.
- **Companion app (Kotlin):** truncate `prefix`/`text` to the byte limits above *before*
  building the dictionary — Pebble cstring tuples are NUL-terminated on the wire, so
  oversized strings won't corrupt the message, but they will get hard-truncated without
  ellipsis if left to the watch.
- **`package.json`:** ✅ done. `pebble.companionApp.android.apps[0].package` =
  `family.dieflomis.infocompanion` (the companion app's `applicationId`). The `url` field
  (where a user without the app installed would be sent to get it) is currently an empty
  string — there's no public distribution yet, only local `adb install` of the debug build.
  Fill it in if this ever gets a GitHub release or Play Store listing.
