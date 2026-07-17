# Infoface

A Pebble watchface that shows a rotating set of **info panels** — calendar events, weather,
RSS/Atom feeds, and generic JSON sources — fed live from its Android companion app,
[**Infoface Companion**](https://github.com/flurl/infoface-companion). The watchface has no
network access of its own; the companion app does all the fetching/parsing and hands the watch
a small, pre-formatted feed over Bluetooth every 15 minutes.

Battery, quiet-time, and Bluetooth-disconnect indicators; optional pagination and
quadruple/triple wrist-tap gestures to turn pages or rotate panels; all tunable from the watch's
Settings screen (Clay) on the phone.

**Requires the [Infoface Companion](https://github.com/flurl/infoface-companion) app** on your
phone — the watchface shows "Nothing to see" without it.

## Two builds

This repo produces two `.pbw` variants from the same source, both built for **emery** (Pebble
Time 2), **flint** (Pebble 2 Duo), and **gabbro** (Pebble Round 2):

| Variant | Firmware needed | What you lose without it |
|---|---|---|
| **`infoface-<version>-stock.pbw`** | Any stock PebbleOS | Nothing — this is the one on the [Pebble appstore](https://apps.repebble.com). |
| **`infoface-<version>-buttons.pbw`** | [flurl/PebbleOS](https://github.com/flurl/PebbleOS/tree/feature/watchface-button-notify) (the `feature/watchface-button-notify` fork) | Quick-launch buttons (Up/Down/Select/Back, tap or hold) can rotate panels and turn pages in addition to the wrist-tap gestures. **Will not launch on stock firmware** — it links a firmware service that only exists in the fork. |

Grab both from this repo's [Releases](../../releases) page. See
[flurl/PebbleOS](https://github.com/flurl/PebbleOS/tree/feature/watchface-button-notify) for what the fork changes and how to build
and sideload it onto real hardware.

## Building

Requires [pebble-tool](https://github.com/pebble-dev/pebble-tool) (the repebble fork; the
community-maintained one, not the frozen official `pebble-tool`).

**Stock (no buttons) — builds against any normal Pebble SDK:**

```sh
pebble sdk activate 4.17   # or whichever stock SDK you have installed
pebble build
```

**Button variant — builds against an SDK exported from the [flurl/PebbleOS](https://github.com/flurl/PebbleOS/tree/feature/watchface-button-notify) fork:**

The watchface's C code guards every button-related call with
`#ifdef PBL_CAPABILITY_QUICK_LAUNCH_BUTTON_SERVICE`, a macro that only a `pebble.h` generated
from that fork's build defines — so the same source compiles either way, with the feature
present or silently compiled out depending on which SDK is active.

```sh
# In a checkout of flurl/PebbleOS, on branch feature/watchface-button-notify:
./pbl configure --board=obelix@pvt   # or your target board
./waf build                          # also exports an app SDK to build/sdk/<platform>/

# Register that exported SDK with pebble-tool as a local SDK named "tintin"
# (re-run the install step after any firmware rebuild to refresh it):
pebble sdk uninstall tintin --keep-data   # if already installed
pebble sdk install --tintin /path/to/PebbleOS

# Then, back in this repo:
pebble sdk activate tintin
pebble build
```

Output lands in `build/<platform>/pebble-app.pbw` for each platform, bundled into a single
`build/infoface.pbw`.

## Installing

```sh
pebble install --phone <phone-ip>       # over the same Wi-Fi network
pebble install --cloudpebble            # via CloudPebble relay, no Wi-Fi needed
pebble install --emulator emery         # QEMU emulator, for development
```

## Testing without a phone

[`scripts/send-test-panels.sh`](scripts/send-test-panels.sh) sends a fixed set of test panels
(Events with several items + a divider, Weather, two-line Feed items) straight to a running
emulator or watch via `pebble send-app-message`, bypassing PKJS and the companion app entirely
— useful for checking rendering without a phone in the loop:

```sh
pebble install --emulator gabbro        # or emery / flint
./scripts/send-test-panels.sh           # defaults to --emulator gabbro
./scripts/send-test-panels.sh --emulator emery
./scripts/send-test-panels.sh --cloudpebble   # real hardware
```

**Gotcha:** `pebble send-app-message`'s `--uint`/`--string`/`--int` flags do **not** accumulate
across repeated uses on one command line — each repeated flag *replaces* the previous one's
values rather than adding to them, so e.g. `--uint 10017=0 --uint 10003=1` silently sends only
`10003=1` (`10017` is dropped, no error). Put every key=value pair for the same AppMessage in
one `--uint`/`--string` invocation as multiple space-separated args instead. Verify what's
actually being sent with `-vvv` if a message seems to have no effect.

## Project layout

```
src/c/info-watchface.c          C source — rendering, AppMessage inbox, button/tap gestures
src/pkjs/index.js               PebbleKit JS — polls the companion app, relays to the watch
src/pkjs/config.js              Settings screen (Clay)
scripts/send-test-panels.sh     Send test panel data directly, no phone needed (see above)
PROTOCOL.md                     Wire-format contract with the companion app
wscript                         waf build rules
```

## Protocol

See [`PROTOCOL.md`](PROTOCOL.md) for the full wire format between this watchface, its
PebbleKit JS bridge, and the companion app — message keys, panel/item shapes, and why the data
path goes through PKJS rather than the companion app talking to the watch directly.

## License

[GPL-3.0](LICENSE)
