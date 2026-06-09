#!/usr/bin/env bash
# Experiment: first post-init vs immediate repeat — do both return non-zero?
# Run this from the repo root.
set -euo pipefail

REPO="$( cd "$(dirname "$0")/.." && pwd )"
PROBE="$REPO/build/eh577_usbfs_probe"
DEVICE="/dev/bus/usb/003/004"
RUN1_DUMP="$REPO/dumps/exp-first-vs-repeat/run1"
RUN2_DUMP="$REPO/dumps/exp-first-vs-repeat/run2"
RUN1_LOG="$REPO/logs/exp-first-vs-repeat-run1.txt"
RUN2_LOG="$REPO/logs/exp-first-vs-repeat-run2.txt"

echo ""
echo "=== EH577 first-vs-repeat experiment ==="
echo ""
echo "Caching sudo credentials..."
sudo -v

# Clean previous run data
echo "Cleaning previous run data..."
sudo rm -rf "$RUN1_DUMP" "$RUN2_DUMP" "$RUN1_LOG" "$RUN2_LOG"
mkdir -p "$RUN1_DUMP" "$RUN2_DUMP"

# Wake device from autosuspend
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

echo ""
echo "=== RUN 1 ==="
echo "Place your finger on the sensor NOW."
echo -n "Starting in: "
for i in 3 2 1; do
  echo -n "$i... "
  sleep 1
done
echo "GO — HOLD YOUR FINGER"

sudo env EH577_DUMP_DIR="$RUN1_DUMP" \
  "$PROBE" "$DEVICE" eh575-postinit-reset 18 \
  > "$RUN1_LOG" 2>&1

RUN1_NONZERO=0
if [ -f "$RUN1_DUMP/packet_17.bin" ]; then
  RUN1_NONZERO=$(python3 -c "
data=open('$RUN1_DUMP/packet_17.bin','rb').read()
print(sum(1 for b in data if b != 0))
" 2>/dev/null || echo "?")
fi

echo "Run 1 done. packet_17.bin nonzero bytes: $RUN1_NONZERO"
echo ""
echo "=== RUN 2 — KEEP FINGER ON SENSOR ==="
echo -n "Starting immediately in: "
for i in 3 2 1; do
  echo -n "$i... "
  sleep 1
done
echo "GO — KEEP HOLDING"

sudo env EH577_DUMP_DIR="$RUN2_DUMP" \
  "$PROBE" "$DEVICE" eh575-postinit 18 \
  > "$RUN2_LOG" 2>&1

RUN2_NONZERO=0
if [ -f "$RUN2_DUMP/packet_17.bin" ]; then
  RUN2_NONZERO=$(python3 -c "
data=open('$RUN2_DUMP/packet_17.bin','rb').read()
print(sum(1 for b in data if b != 0))
" 2>/dev/null || echo "?")
fi

echo "Run 2 done. packet_17.bin nonzero bytes: $RUN2_NONZERO"
echo ""
echo "=== RESULT SUMMARY ==="
echo "Run 1 nonzero: $RUN1_NONZERO"
echo "Run 2 nonzero: $RUN2_NONZERO"
echo ""
echo "Log files:"
echo "  $RUN1_LOG"
echo "  $RUN2_LOG"
echo "Dump dirs:"
echo "  $RUN1_DUMP"
echo "  $RUN2_DUMP"
