#!/bin/bash
# Sends a simulated wrist-tap ("jolt") sequence to a running Pebble emulator by
# feeding raw accelerometer samples via `pebble emu-accel custom <file>`.
#
# IMPORTANT: `pebble emu-tap` does NOT work for this -- it fires the system's
# accel_tap_service event, but info-watchface deliberately does not subscribe
# to that service (see the block comment above prv_accel_data_handler() in
# src/c/info-watchface.c). It instead watches raw accel_data_service samples
# for its own jolt threshold, so exercising the gesture means synthesizing raw
# sample deltas large enough to cross that threshold, not firing the OS event.
#
# The watchface counts however many jolts land within its multi-tap window
# (400ms by default) as one gesture: exactly 3 turns the page (only if
# pagination is on), exactly 4 rotates to the next panel (only if more than
# one panel is configured -- see send-test-panels.sh), anything else (1, 2,
# 5+) is discarded. This script's count IS that gesture's tap count -- pass 3
# or 4 to see a visible effect; other counts are for exercising detection
# (threshold/ringdown/axis settings) with no expected visible result.
#
# Sample timing below (25Hz, 8 samples between jolts) assumes this
# watchface's stock/default tap-tuning settings (threshold 300mg, ringdown
# 160ms, window 400ms, all three axes enabled). If those were changed via
# the Clay config page, the spacing here may no longer land inside the real
# ringdown/window and taps may under- or over-count.
#
# Usage: ./send-test-taps.sh [count] [pebble-tool target flags, default: --emulator gabbro]
#   ./send-test-taps.sh                       # 1 jolt (no gesture effect), gabbro (default)
#   ./send-test-taps.sh 3                     # triple tap -> page turn, gabbro
#   ./send-test-taps.sh 4 --emulator flint     # quadruple tap -> panel rotate, flint
set -e
PEBBLE=~/.local/bin/pebble

COUNT=1
if [[ "$1" =~ ^[0-9]+$ ]]; then
  COUNT="$1"
  shift
fi
TARGET="${*:---emulator gabbro}"

# Samples between one jolt and the next: comfortably more than the 4-sample
# (160ms) ringdown so each one registers as its own jolt, and within the
# 10-sample (400ms) multi-tap window so they all combine into one sequence.
JOLT_SPACING=7
# Extra baseline samples appended after the last jolt so the sequence closes
# out (finalizes its tap count) instead of waiting for a jolt that never
# comes -- finalization needs 11+ jolt-free samples after the last jolt.
TRAILING=12
MAX_SAMPLES=255

TOTAL=$((2 + COUNT * (1 + JOLT_SPACING) + TRAILING))
if (( TOTAL > MAX_SAMPLES )); then
  echo "error: a $COUNT-tap gesture needs $TOTAL accel samples, but pebble-tool caps a single emu-accel send at $MAX_SAMPLES (max ~30 taps)." >&2
  exit 1
fi

ACCEL_FILE=$(mktemp)
trap 'rm -f "$ACCEL_FILE"' EXIT

# Absolute values don't matter to the watchface's detector -- only the
# sample-to-sample delta does -- so a resting baseline of all zeros is fine.
BASELINE="0, 0, -1000"
SPIKE="1000, 0, -1000"

{
  echo "$BASELINE"
  echo "$BASELINE"
  for ((i = 1; i <= COUNT; i++)); do
    echo "$SPIKE"
    for ((j = 0; j < JOLT_SPACING; j++)); do
      echo "$BASELINE"
    done
  done
  for ((i = 0; i < TRAILING; i++)); do
    echo "$BASELINE"
  done
} > "$ACCEL_FILE"

$PEBBLE emu-accel $TARGET custom "$ACCEL_FILE"
echo "Sent a $COUNT-tap gesture ($TOTAL accel samples)."
