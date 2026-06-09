#!/usr/bin/env bash
# Loop the post-init sequence N times, with a guided TOUCH cue each cycle.
# Designed to reproduce the June 3 successful captures:
#   - finger arrives DURING the scan sequence (not held statically before)
#   - try swipe motion, not just a press
# Run from the repo root.
set -euo pipefail

REPO="$( cd "$(dirname "$0")/.." && pwd )"
PROBE="$REPO/build/eh577_usbfs_probe"
DEVICE="/dev/bus/usb/003/004"
LOOPS="${1:-15}"
DUMP_BASE="$REPO/dumps/loop-capture-$(date +%Y%m%d-%H%M%S)"
LOG="$REPO/logs/loop-capture-$(date +%Y%m%d-%H%M%S).txt"

echo ""
echo "=== EH577 loop capture experiment ==="
echo "Runs: $LOOPS   Log: $LOG"
echo ""
echo "INSTRUCTIONS:"
echo "  Each cycle: lift finger OFF sensor, then PLACE/SWIPE it on as the"
echo "  countdown hits 1 (so the finger arrives DURING the scan sequence)."
echo "  Try gentle pressure and vary position slightly each cycle."
echo ""
echo "Caching sudo credentials..."
sudo -v
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

mkdir -p "$DUMP_BASE"
echo "Dump dir: $DUMP_BASE"
echo "" | tee "$LOG"

NONZERO_COUNT=0

for i in $(seq 1 "$LOOPS"); do
  DUMP_DIR="$DUMP_BASE/run-$(printf '%02d' $i)"
  mkdir -p "$DUMP_DIR"

  echo "--- Cycle $i/$LOOPS ---"
  echo "Lift finger OFF sensor."
  sleep 1
  echo -n "PLACE/SWIPE finger in: 3... "
  sleep 1
  echo -n "2... "
  sleep 1
  echo "1 — GO"

  sudo env EH577_DUMP_DIR="$DUMP_DIR" \
    "$PROBE" "$DEVICE" eh575-postinit 18 \
    >> "$LOG" 2>&1

  FRAME="$DUMP_DIR/eh575-postinit-17-64_14_ec.bin"
  if [ -f "$FRAME" ]; then
    NZ=$(python3 -c "data=open('$FRAME','rb').read(); print(sum(1 for b in data if b!=0))" 2>/dev/null || echo "?")
    echo "  → nonzero bytes in frame: $NZ"
    if [ "$NZ" != "0" ] && [ "$NZ" != "?" ]; then
      NONZERO_COUNT=$((NONZERO_COUNT + 1))
      echo "  *** NON-ZERO FRAME CAPTURED! ***"
    fi
  else
    echo "  → no frame file found"
  fi
  echo ""
done

echo "=== DONE: $NONZERO_COUNT/$LOOPS cycles returned a non-zero frame ==="
echo "Log: $LOG"
echo "Dumps: $DUMP_BASE"
