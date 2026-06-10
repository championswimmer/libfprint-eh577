#!/usr/bin/env bash
# Enroll one finger, then verify with a DIFFERENT finger.
# Expected result: NO MATCH. Reports a false-match bug if MATCH is returned.
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL="$REPO/refs/libfprint/build/examples/enroll"
VERIFY="$REPO/refs/libfprint/build/examples/verify"
SESSION="$(date +%Y%m%d-%H%M%S)"
LOG="$REPO/logs/mismatch-$SESSION.txt"

mkdir -p "$REPO/logs"

log()    { echo "$@" | tee -a "$LOG"; }
banner() { log ""; log "════════════════════════════════════════"; log "  $*"; log "════════════════════════════════════════"; log ""; }

cue() {
  local msg="$1" delay="${2:-3}"
  log "$msg"
  for i in $(seq "$delay" -1 1); do printf "  %d... " "$i"; sleep 1; done
  echo ""
  log ""
}

banner "EH577 Mismatch Test  [$SESSION]"
log "Purpose: enroll one finger, verify with a DIFFERENT finger."
log "Expected result: NO MATCH.  A MATCH here means a false-match bug."
log "Repo: $REPO"
log "Log:  $LOG"
log ""

if ! lsusb | grep -q '1c7a:0577'; then
  log "ERROR: EH577 not found. Check USB connection."; exit 1
fi
if systemctl is-active --quiet fprintd; then
  log "Stopping fprintd..."; sudo systemctl stop fprintd
fi
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

# ── finger selection ──────────────────────────────────────────────────────────
FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

echo "Choose the ENROLL finger:"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " ENROLL_FINGER
if ! [[ "$ENROLL_FINGER" =~ ^[0-9]$ ]]; then log "ERROR: invalid selection"; exit 1; fi

echo ""
echo "Choose a DIFFERENT finger for the verify step (should NOT match):"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " VERIFY_FINGER
if ! [[ "$VERIFY_FINGER" =~ ^[0-9]$ ]]; then log "ERROR: invalid selection"; exit 1; fi

if [[ "$ENROLL_FINGER" == "$VERIFY_FINGER" ]]; then
  log "WARNING: same finger selected for both steps — run enroll_and_verify.sh for that instead."
fi

log "Enroll finger: $ENROLL_FINGER (${FINGER_NAMES[$ENROLL_FINGER]})"
log "Verify finger: $VERIFY_FINGER (${FINGER_NAMES[$VERIFY_FINGER]})"

# ── enroll ────────────────────────────────────────────────────────────────────
banner "PHASE 1: ENROLL (finger $ENROLL_FINGER — ${FINGER_NAMES[$ENROLL_FINGER]})"
log "Keep your finger flat and still on the sensor."
log ""

cue "Get ready — TOUCH enroll finger on sensor in:" 5
log ">>> TOUCH NOW (${FINGER_NAMES[$ENROLL_FINGER]}) <<<"
log ""

echo "$ENROLL_FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577:libfprint-print \
  '$ENROLL'
" 2>&1 | tee -a "$LOG"

log ""
log ">>> REMOVE finger now <<<"
log ""

sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true

if [ ! -f "$REPO/test-storage.variant" ]; then
  log "ERROR: enrollment failed — test-storage.variant not found."; exit 1
fi
log "Enroll data saved."

# ── pause ─────────────────────────────────────────────────────────────────────
cue "Lift enroll finger. Prepare your DIFFERENT finger. Verify starts in:" 8

# ── verify with different finger ──────────────────────────────────────────────
banner "PHASE 2: VERIFY (finger $VERIFY_FINGER — ${FINGER_NAMES[$VERIFY_FINGER]})"
log "Place your DIFFERENT finger (${FINGER_NAMES[$VERIFY_FINGER]}) on the sensor."
log "Expected result: NO MATCH"
log ""

cue "Get ready — TOUCH DIFFERENT finger on sensor in:" 5
log ">>> TOUCH NOW (${FINGER_NAMES[$VERIFY_FINGER]} — DIFFERENT FINGER) <<<"
log ""

echo "$VERIFY_FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577:libfprint-print \
  '$VERIFY'
" 2>&1 | tee -a "$LOG"

log ""
log ">>> REMOVE finger <<<"
log ""

sudo chown "$(id -u):$(id -g)" "$REPO/verify.pgm" 2>/dev/null || true

# ── result ────────────────────────────────────────────────────────────────────
banner "RESULT"

BZ3_SCORE=$(grep -oP "score \K[0-9]+" "$LOG" | tail -1)
if [[ -n "$BZ3_SCORE" ]]; then
  log "Bozorth3 score: $BZ3_SCORE (threshold: 15)"
fi

RESULT_LINE=$(grep -E "^(NO )?MATCH!$" "$LOG" | tail -1)
if [[ "$RESULT_LINE" == "NO MATCH!" ]]; then
  log "CORRECT — different finger was rejected (NO MATCH)."
  if [[ -n "$BZ3_SCORE" ]]; then
    log "  BZ3 score $BZ3_SCORE < threshold 15 — rejection margin looks fine."
  fi
elif [[ "$RESULT_LINE" == "MATCH!" ]]; then
  log "FALSE MATCH — different finger was accepted!"
  log "  Enrolled:  finger $ENROLL_FINGER (${FINGER_NAMES[$ENROLL_FINGER]})"
  log "  Verified:  finger $VERIFY_FINGER (${FINGER_NAMES[$VERIFY_FINGER]})"
  if [[ -n "$BZ3_SCORE" ]]; then
    log "  BZ3 score: $BZ3_SCORE (threshold 15)"
    log "  Try raising EGIS0577_BZ3_THRESHOLD in egis0577.h, rebuild, re-test."
  fi
  log ""
  log "Compare enrolled.pgm vs verify.pgm for visual differences."
  log "If images look identical, the problem is in image assembly, not threshold."
else
  log "?  Unknown result — check log for details."
fi

log ""
log "Session log:    $LOG"
log "Enroll image:   $REPO/enrolled.pgm"
log "Verify image:   $REPO/verify.pgm"
