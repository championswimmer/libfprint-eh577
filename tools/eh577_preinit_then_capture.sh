#!/usr/bin/env bash
# Run pre-init (29 packets, writes config registers) then post-init with finger cue.
# Tests whether pre-init restores scan capability after the stuck-state issue.
set -euo pipefail

REPO="$( cd "$(dirname "$0")/.." && pwd )"
PROBE="$REPO/build/eh577_usbfs_probe"
DEVICE="/dev/bus/usb/003/004"
LOOPS="${1:-8}"
DUMP_BASE="$REPO/dumps/preinit-capture-$(date +%Y%m%d-%H%M%S)"
LOG_BASE="$REPO/logs/preinit-capture-$(date +%Y%m%d-%H%M%S)"

echo ""
echo "=== EH577 preinit-then-capture experiment ==="
echo "Loops: $LOOPS"
echo ""
echo "Caching sudo credentials..."
sudo -v
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

mkdir -p "$DUMP_BASE"

NONZERO_COUNT=0

for i in $(seq 1 "$LOOPS"); do
  DUMP_DIR="$DUMP_BASE/run-$(printf '%02d' $i)"
  mkdir -p "$DUMP_DIR/preinit" "$DUMP_DIR/postinit"
  PREINIT_LOG="${LOG_BASE}-run$(printf '%02d' $i)-preinit.txt"
  POSTINIT_LOG="${LOG_BASE}-run$(printf '%02d' $i)-postinit.txt"

  echo ""
  echo "--- Cycle $i/$LOOPS ---"
  echo "Step 1: running pre-init (29 config-write packets)..."
  sudo env EH577_DUMP_DIR="$DUMP_DIR/preinit" \
    "$PROBE" "$DEVICE" eh575-preinit 29 \
    > "$PREINIT_LOG" 2>&1

  # Show key registers after preinit
  PREINIT_RX01=$(grep "RX\[01\]" "$PREINIT_LOG" | head -1 | grep -oP "[0-9a-f]{2}( [0-9a-f]{2})+" | head -1 || echo "?")
  echo "  preinit RX[01]: $PREINIT_RX01"

  echo "Step 2: PLACE/SWIPE finger NOW — starting post-init in:"
  echo -n "  "
  for c in 3 2 1; do
    echo -n "$c... "
    sleep 1
  done
  echo "GO"

  sudo env EH577_DUMP_DIR="$DUMP_DIR/postinit" \
    "$PROBE" "$DEVICE" eh575-postinit 18 \
    > "$POSTINIT_LOG" 2>&1

  FRAME=$(find "$DUMP_DIR/postinit" -name "*17*64_14_ec*" 2>/dev/null | head -1)
  if [ -f "$FRAME" ]; then
    NZ=$(python3 -c "data=open('$FRAME','rb').read(); print(sum(1 for b in data if b!=0))")
    STATUS=$(grep "RX\[13\]" "$POSTINIT_LOG" | head -1 | grep -oP "([0-9a-f]{2} ){9}[0-9a-f]{2}" | head -1 || echo "?")
    echo "  → frame nonzero: $NZ  |  62 67 03 status: $STATUS"
    if [ "$NZ" != "0" ] && [ "$NZ" != "?" ]; then
      NONZERO_COUNT=$((NONZERO_COUNT + 1))
      echo "  *** NON-ZERO FRAME! ***"
    fi
  else
    echo "  → no frame file"
  fi
done

echo ""
echo "=== DONE: $NONZERO_COUNT/$LOOPS cycles returned a non-zero frame ==="
echo "Dumps: $DUMP_BASE"
