#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
VERIFY="$REPO/refs/libfprint/build/examples/verify"
STORAGE="$REPO/test-storage.variant"
LOG="$REPO/logs/verify-$(date +%Y%m%d-%H%M%S).txt"

echo "=== EH577 Verify Script ==="
echo "Repo:    $REPO"
echo "Storage: $STORAGE"
echo "Log:     $LOG"
echo ""

# Pre-flight: storage file exists
if [ ! -f "$STORAGE" ]; then
  echo "ERROR: no enrolled print found at $STORAGE — run enroll.sh first."
  exit 1
fi

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

echo "Choose the finger to verify (must match the enrolled finger):"
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
echo "Starting verification for finger $FINGER..."
echo "When prompted, place your finger on the sensor and hold for ~2s."
echo "Output also logged to: $LOG"
echo ""

echo "$FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577 \
  '$VERIFY'
" 2>&1 | tee "$LOG"

echo ""
sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" 2>/dev/null || true

# Print the verdict clearly
if grep -q "MATCH$\|: MATCH" "$LOG" 2>/dev/null; then
  echo "RESULT: MATCH"
elif grep -q "NO MATCH\|NO_MATCH" "$LOG" 2>/dev/null; then
  echo "RESULT: NO MATCH"
else
  echo "RESULT: unknown — check $LOG"
fi
