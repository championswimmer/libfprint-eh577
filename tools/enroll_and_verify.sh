#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL="$REPO/refs/libfprint/build/examples/enroll"
VERIFY="$REPO/refs/libfprint/build/examples/verify"
SESSION="$(date +%Y%m%d-%H%M%S)"
LOG="$REPO/logs/session-$SESSION.txt"

mkdir -p "$REPO/logs"

# ── helpers ───────────────────────────────────────────────────────────────────
log()    { echo "$@" | tee -a "$LOG"; }
banner() { log ""; log "════════════════════════════════════════"; log "  $*"; log "════════════════════════════════════════"; log ""; }

cue() {
  local msg="$1" delay="${2:-3}"
  log "$msg"
  for i in $(seq "$delay" -1 1); do printf "  %d... " "$i"; sleep 1; done
  echo ""
  log ""
}

# ── pre-flight ────────────────────────────────────────────────────────────────
banner "EH577 Enroll + Verify  [$SESSION]"
log "Repo:  $REPO"
log "Log:   $LOG"
log ""

if ! lsusb | grep -q '1c7a:0577'; then
  log "ERROR: EH577 not found. Check USB connection."; exit 1
fi
sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo pkill -f fprintd 2>/dev/null || true
USBDEV=$(lsusb -d 1c7a:0577 2>/dev/null \
  | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
if [[ -n "$USBDEV" && -e "$USBDEV" ]]; then
  for i in 1 2 3 4 5; do
    sudo fuser "$USBDEV" &>/dev/null || break
    echo "Waiting for USB device to be released (${i}s)..."
    sleep 1
  done
fi

# ── finger selection ──────────────────────────────────────────────────────────
echo "Choose the finger to enroll and verify:"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " FINGER
if ! [[ "$FINGER" =~ ^[0-9]$ ]]; then log "ERROR: invalid selection"; exit 1; fi
log "Finger: $FINGER"

# ── enroll ────────────────────────────────────────────────────────────────────
banner "PHASE 1: ENROLL"
log "The sensor captures one frame per stage. Keep your finger flat and still."
log "You will be told exactly when to TOUCH and when to REMOVE."
log ""

cue "Get ready — TOUCH finger on sensor in:" 5
log ">>> TOUCH NOW <<<"
log ""

echo "$FINGER" | sudo sh -c "
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

# ── pause between enroll and verify ──────────────────────────────────────────
cue "Lift finger completely. Verify starts in:" 5

# ── verify ────────────────────────────────────────────────────────────────────
banner "PHASE 2: VERIFY"
log "Use the SAME finger in the SAME position."
log ""

cue "Get ready — TOUCH finger on sensor in:" 3
log ">>> TOUCH NOW <<<"
log ""

echo "$FINGER" | sudo sh -c "
  cd '$REPO' && \
  LD_LIBRARY_PATH='$LIBFP' \
  G_MESSAGES_DEBUG=libfprint-egis0577:libfprint-print \
  '$VERIFY'
" 2>&1 | tee -a "$LOG"

log ""
log ">>> REMOVE finger <<<"
log ""

sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" 2>/dev/null || true

# ── result ────────────────────────────────────────────────────────────────────
banner "RESULT"

BZ3_SCORE=$(grep -oP "score \K[0-9]+" "$LOG" | tail -1)
if [[ -n "$BZ3_SCORE" ]]; then
  log "Bozorth3 score: $BZ3_SCORE (threshold: 15)"
fi

RESULT_LINE=$(grep -E "^(NO )?MATCH!$" "$LOG" | tail -1)
if [[ "$RESULT_LINE" == "MATCH!" ]]; then
  log "✓  MATCH — verification succeeded!"
elif [[ "$RESULT_LINE" == "NO MATCH!" ]]; then
  log "✗  NO MATCH — finger not recognised."
  log ""
  log "Tips: lower EGIS0577_BZ3_THRESHOLD (currently 15, try 10),"
  log "      or increase EGIS0577_CONSECUTIVE_CAPTURES for more strips."
else
  log "?  Unknown result — check log for details."
fi

log ""
log "Session log : $LOG"
log "Enroll image: $REPO/enrolled.pgm"
log "Verify image: $REPO/verify.pgm"
