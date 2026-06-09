#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL="$REPO/refs/libfprint/build/examples/enroll"
STORAGE="$REPO/test-storage.variant"
LOG="$REPO/logs/enroll-$(date +%Y%m%d-%H%M%S).txt"

echo "=== EH577 Enrollment Script ==="
echo "Repo:    $REPO"
echo "Storage: $STORAGE"
echo "Log:     $LOG"
echo ""

# Pre-flight: device present
if ! lsusb | grep -q '1c7a:0577'; then
  echo "ERROR: EH577 not found. Check USB connection."
  exit 1
fi

# Pre-flight: stop fprintd if running
if systemctl is-active --quiet fprintd; then
  echo "Stopping fprintd..."
  sudo systemctl stop fprintd
fi

# Pre-flight: wake device from autosuspend
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

mkdir -p "$REPO/logs"

echo "Choose the finger to enroll:"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " FINGER

if ! [[ "$FINGER" =~ ^[0-9]$ ]]; then
  echo "ERROR: invalid selection"
  exit 1
fi

echo ""
echo "Starting enrollment for finger $FINGER..."
echo "When prompted, place your finger on the sensor and hold for ~2s."
echo "Output also logged to: $LOG"
echo ""

# cd into REPO inside sudo so test-storage.variant and enrolled.pgm land there
echo "$FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577 \
  '$ENROLL'
" 2>&1 | tee "$LOG"

echo ""
# Fix ownership of any files written by root
sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true

if [ -f "$STORAGE" ]; then
  echo "SUCCESS: print data saved to $STORAGE"
else
  echo "WARNING: test-storage.variant not found — enrollment may have failed. Check $LOG"
fi
