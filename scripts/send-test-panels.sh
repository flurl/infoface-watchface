#!/bin/bash
# Sends a fixed set of test panels/items straight to a running Pebble emulator or
# real watch via AppMessage, bypassing PKJS and the companion app entirely --
# useful for checking rendering (panels, pagination, dividers, feed items) without
# a phone. Message key IDs below match build/js/message_keys.json for this
# project's current package.json; re-check that file if package.json's
# pebble.messageKeys list is ever reordered (see PROTOCOL.md).
#
# Usage: ./send-test-panels.sh [pebble-tool target flags, default: --emulator gabbro]
#   ./send-test-panels.sh                       # gabbro emulator (default)
#   ./send-test-panels.sh --emulator emery
#   ./send-test-panels.sh --cloudpebble          # real hardware over CloudPebble
set -e
PEBBLE=~/.local/bin/pebble
TARGET="${*:---emulator gabbro}"
APP_UUID="8047c3ec-ee69-418d-b0f1-3da7371aee63"

# IMPORTANT: pebble-tool's --uint/--string/--int flags do NOT accumulate across
# repeated uses on one command line -- each repeated flag *replaces* the previous
# one's values, so e.g. `--uint 10017=0 --uint 10003=1` silently sends ONLY
# 10003=1 (10017 is dropped, no error/warning). Put every key=value pair destined
# for the same AppMessage dict as space-separated args after a SINGLE --uint (and
# a single --string), never as multiple flag occurrences. Confirmed via
# `pebble send-app-message -vvv` showing the actual dict sent.
send() {
  $PEBBLE send-app-message $TARGET --app-uuid "$APP_UUID" "$@"
}

# Turn on pagination so a panel with more items than fit on one screen pages.
send --uint 10015=1

# Reset: 3 panels.
send --uint 10016=3

# Panel 0: "Events" - 6 items incl. a divider, enough to force a second page.
send --uint 10017=0 10000=6 --string 10018=Events
send --uint 10017=0 10003=0 10008=1 --string 10001="Fri 09:00" 10002="Standup"
send --uint 10017=0 10003=1 10008=1 --string 10001="Fri 12:30" 10002="Lunch w/ Sam"
send --uint 10017=0 10003=2 10008=0 --string 10001="" 10002=""
send --uint 10017=0 10003=3 10008=1 --string 10001="Sat 10:00" 10002="Market"
send --uint 10017=0 10003=4 10008=1 --string 10001="Sat 14:00" 10002="Bike ride downtown"
send --uint 10017=0 10003=5 10008=1 --string 10001="Sun |->" 10002="Conference"

# Panel 1: "Weather" - 1 item.
send --uint 10017=1 10000=1 --string 10018=Weather
send --uint 10017=1 10003=0 10008=2 --string 10001="22°C" 10002="Sunny"

# Panel 2: "Feeds" - 2 two-line feed items.
send --uint 10017=2 10000=2 --string 10018=Feeds
send --uint 10017=2 10003=0 10008=3 --string 10001="ORF" 10002="Breaking: sample headline for the feed test item"
send --uint 10017=2 10003=1 10008=3 --string 10001="Standard" 10002="Another sample headline to test two-line rendering"

echo "All test panels sent."
