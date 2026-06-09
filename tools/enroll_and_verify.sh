#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL="$REPO/refs/libfprint/build/examples/enroll"
VERIFY="$REPO/refs/libfprint/build/examples/verify"
SESSION="$(date +%Y%m%d-%H%M%S)"
LOGDIR="$REPO/logs"
LOG="$LOGDIR/session-$SESSION.txt"

mkdir -p "$LOGDIR"

log() { echo "$@" | tee -a "$LOG"; }
banner() { log ""; log "========================================"; log "  $*"; log "========================================"; log ""; }

banner "EH577 Enroll + Verify  ($SESSION)"
log "Repo:    $REPO"
log "Log:     $LOG"

# Pre-flight
if ! lsusb | grep -q '1c7a:0577'; then
  log "ERROR: EH577 not found. Check USB connection."; exit 1
fi
if systemctl is-active --quiet fprintd; then
  log "Stopping fprintd..."; sudo systemctl stop fprintd
fi
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

# Finger selection
echo ""
echo "Choose the finger to enroll and verify:"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " FINGER
if ! [[ "$FINGER" =~ ^[0-9]$ ]]; then log "ERROR: invalid selection"; exit 1; fi
log "Selected finger: $FINGER"

# ── ENROLL ────────────────────────────────────────────────────────────────────
banner "PHASE 1: ENROLL"

log "The sensor will capture your finger once per stage."
log "Keep your finger FLAT and STILL on the sensor during each capture."
log ""

# Ask how many stages the device will require (driver sets it to 1 currently,
# but libfprint may request more). We warn the user up front.
log "Enroll will start in 5 seconds."
log "GET YOUR FINGER READY — place it on the sensor ON THE CUE below."
log ""
for i in 5 4 3 2 1; do
  echo -n "  $i... "; sleep 1
done
echo ""
log ""
log ">>> TOUCH NOW — place finger firmly on sensor <<<"
log ""

echo "$FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577 \
  '$ENROLL'
" 2>&1 | tee -a "$LOG"

# If multiple stages: the enroll binary prints "Enroll stage N of M passed."
# and "Scan your finger now." for each stage. We can't easily interleave cues
# with the running process, so tell the user to lift+replace between each
# "Scan your finger now." prompt they see on screen.
log ""
log ">>> REMOVE finger now <<<"
log ""

# Fix ownership
sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true

if [ ! -f "$REPO/test-storage.variant" ]; then
  log "ERROR: enrollment failed — test-storage.variant not found. Check log above."; exit 1
fi
log "Enrollment data saved to test-storage.variant"

# ── VERIFY ────────────────────────────────────────────────────────────────────
banner "PHASE 2: VERIFY"

log "Now we will verify the enrolled finger."
log "LIFT your finger completely off the sensor first."
log ""
echo -n "Waiting 3s for you to lift finger... "; sleep 3; echo ""
log ""
log "Verify will start in 3 seconds."
log "GET YOUR FINGER READY — same finger, same position."
for i in 3 2 1; do echo -n "  $i... "; sleep 1; done
echo ""
log ""
log ">>> TOUCH NOW — place finger firmly on sensor <<<"
log ""

echo "$FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577 \
  '$VERIFY'
" 2>&1 | tee -a "$LOG"

sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" 2>/dev/null || true

log ""
log ">>> REMOVE finger <<<"
log ""

# ── RESULT ────────────────────────────────────────────────────────────────────
banner "RESULT"

VERIFY_OUT=$(grep -E "MATCH|NO.MATCH|verify" "$LOG" | tail -5)
if echo "$VERIFY_OUT" | grep -qE "MATCH" && ! echo "$VERIFY_OUT" | grep -qiE "NO.MATCH|NO_MATCH"; then
  log "RESULT: *** MATCH *** — verification succeeded!"
elif echo "$VERIFY_OUT" | grep -qiE "NO.MATCH|NO_MATCH"; then
  log "RESULT: NO MATCH — finger not recognized."
  log ""
  log "Troubleshooting tips:"
  log "  - Try lowering BZ3_THRESHOLD in egis0577.h (currently ~15, try 10)"
  log "  - Try increasing EGIS0577_CONSECUTIVE_CAPTURES for a taller image"
  log "  - Build with pixman support to enable the resize step"
else
  log "RESULT: unknown — raw verify output:"
  echo "$VERIFY_OUT" | tee -a "$LOG"
fi

log ""
log "Full session log: $LOG"
log "Enrolled PGM images: $REPO/enrolled.pgm (last stage)"
