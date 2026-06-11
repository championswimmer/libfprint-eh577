#!/usr/bin/env bash
# Multi-finger enrollment + identification session.
#
# Phase 1: Enroll as many fingers as you like, one at a time.
#          Type 'd' when done enrolling.
#
# Phase 2: Touch any enrolled finger.  Script reports which finger
#          matched (or "NO MATCH" if none did).  Type 'done' to exit.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL_BIN="$REPO/refs/libfprint/build/examples/enroll"
IDENTIFY_BIN="$REPO/refs/libfprint/build/examples/identify"
CLEAR_BIN="$REPO/refs/libfprint/build/examples/clear-storage"
SESSION="$(date +%Y%m%d-%H%M%S)"
LOG="$REPO/logs/identify-session-$SESSION.txt"

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

FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

# ── pre-flight ────────────────────────────────────────────────────────────────
banner "EH577 Enroll + Identify  [$SESSION]"
log "Log: $LOG"
log ""

if ! lsusb | grep -q '1c7a:0577'; then
  log "ERROR: EH577 not found. Check USB connection."; exit 1
fi
if systemctl is-active --quiet fprintd; then
  log "Stopping fprintd..."; sudo systemctl stop fprintd
fi
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

# ── optional storage clear ────────────────────────────────────────────────────
echo ""
printf "Clear all previously enrolled prints before starting? [y/N] "
read -r CLEAR_CHOICE
if [[ "$CLEAR_CHOICE" =~ ^[yY]$ ]]; then
  log "Clearing storage..."
  printf "Y\n" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=all '$CLEAR_BIN'
  " 2>&1 | tee -a "$LOG" || true
  sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" 2>/dev/null || true
  log "Storage cleared."
fi

# ── PHASE 1: Enrollment loop ──────────────────────────────────────────────────
banner "PHASE 1: ENROLL FINGERS"
log "Enroll as many fingers as you like.  Type 'd' when done."
ENROLLED_NAMES=()
ENROLLED_INDICES=()

while true; do
  echo ""
  echo "Pick a finger to enroll — or 'd' to finish:"
  echo "  [0] left thumb    [5] right thumb"
  echo "  [1] left index    [6] right index"
  echo "  [2] left middle   [7] right middle"
  echo "  [3] left ring     [8] right ring"
  echo "  [4] left little   [9] right little"
  echo "  [d] done enrolling"
  read -rp "> " CHOICE

  if [[ "$CHOICE" =~ ^[dD]$ ]]; then
    if [[ ${#ENROLLED_NAMES[@]} -eq 0 ]]; then
      echo "Enroll at least one finger first."; continue
    fi
    break
  fi

  if [[ ! "$CHOICE" =~ ^[0-9]$ ]]; then
    echo "Invalid — enter 0–9 or 'd'."; continue
  fi

  FNAME="${FINGER_NAMES[$CHOICE]}"
  NR_STAGES=5   # matches IMG_ENROLL_STAGES in libfprint
  log "Enrolling: $FNAME (index $CHOICE) — $NR_STAGES separate presses required"
  log ""

  # Run the enroll binary in the background so we can interleave stage prompts.
  # A temp FIFO feeds the finger-number answer + "N" for the update question.
  ENROLL_LOG=$(mktemp)
  ENROLL_FIFO=$(mktemp -u)
  mkfifo "$ENROLL_FIFO"

  # Keep the FIFO open for writing so the binary doesn't get EOF prematurely.
  exec 9>"$ENROLL_FIFO"
  printf "%d\nN\n" "$CHOICE" >&9

  sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint-egis0577 '$ENROLL_BIN'
  " <"$ENROLL_FIFO" 2>&1 | tee -a "$LOG" > "$ENROLL_LOG" &
  ENROLL_PID=$!

  COMPLETED=0
  while [[ $COMPLETED -lt $NR_STAGES ]]; do
    NEXT=$((COMPLETED + 1))
    cue ">>> Stage $NEXT/$NR_STAGES — TOUCH your $FNAME on the sensor in:" 3
    log ">>> TOUCH NOW ($FNAME — stage $NEXT/$NR_STAGES) <<<"

    # Wait for this stage's "passed. Yay!" line in the log.
    DEADLINE=$((SECONDS + 30))
    while [[ $SECONDS -lt $DEADLINE ]]; do
      DONE_COUNT=$(grep -c "passed\. Yay!" "$ENROLL_LOG" 2>/dev/null || true)
      if [[ "$DONE_COUNT" -gt "$COMPLETED" ]]; then
        COMPLETED=$DONE_COUNT
        break
      fi
      sleep 0.3
    done

    if [[ $COMPLETED -lt $NEXT ]]; then
      log "✗  Stage $NEXT timed out or failed — check output above."
      break
    fi

    log "✓  Stage $COMPLETED/$NR_STAGES passed."
    if [[ $COMPLETED -lt $NR_STAGES ]]; then
      log ">>> REMOVE finger now, then get ready for stage $((COMPLETED+1)) <<<"
      sleep 1
    fi
  done

  # Close the FIFO write end and wait for the binary to finish.
  exec 9>&-
  wait "$ENROLL_PID" 2>/dev/null || true
  rm -f "$ENROLL_FIFO" "$ENROLL_LOG"

  sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true
  log ""
  log ">>> REMOVE finger <<<"

  if [[ $COMPLETED -eq $NR_STAGES ]]; then
    log "✓  $FNAME enrolled (all $NR_STAGES stages complete)."
    ENROLLED_NAMES+=("$FNAME")
    ENROLLED_INDICES+=("$CHOICE")
  else
    log "✗  Enrollment incomplete ($COMPLETED/$NR_STAGES stages). Try again."
  fi
done

log ""
log "Enrolled fingers:"
for F in "${ENROLLED_NAMES[@]}"; do log "  • $F"; done

# ── PHASE 2: Identification loop ──────────────────────────────────────────────
banner "PHASE 2: IDENTIFY FINGERS"
log "Touch any enrolled finger to identify it."
log "Enrolled: ${ENROLLED_NAMES[*]}"
log "Type 'done' to exit."

while true; do
  echo ""
  printf "Press Enter to scan a finger (or type 'done' to exit): "
  read -r CHOICE

  if [[ "$CHOICE" =~ ^[dD](one)?$ ]]; then
    log "Session ended."; break
  fi

  cue "Get ready — TOUCH a finger on the sensor in:" 3
  log ">>> TOUCH NOW <<<"
  log ""

  # Use a temp file to capture the full debug output for parsing.
  # `exec env` sets G_MESSAGES_DEBUG=all explicitly so GLib shows:
  #   - NULL-domain debug (identify.c's "matched finger X successfully")
  #   - libfprint-print debug (bozorth3 "score X/threshold" lines)
  #   - libfprint-egis0577 debug (driver frames)
  # tee splits: LOG gets everything; terminal gets only non-DEBUG lines.
  IDTMP=$(mktemp)
  printf "n\n" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=all '$IDENTIFY_BIN'
  " 2>&1 | tee -a "$LOG" | tee "$IDTMP" | grep -vE "^\(process:"

  sudo chown "$(id -u):$(id -g)" "$REPO/identify.pgm" 2>/dev/null || true

  log ""
  log ">>> REMOVE finger <<<"

  # Parse results from the temp file that has the full debug stream.
  BZ3=$(grep -oP "score \K[0-9]+(?=/)" "$IDTMP" | sort -n | tail -1)
  MATCHED=$(grep -oP "matched finger \K.+?(?= successfully)" "$IDTMP" | tail -1)
  RESULT=$(grep -E "^(NOT )?IDENTIFIED!$" "$IDTMP" | tail -1)
  rm -f "$IDTMP"

  if [[ "$RESULT" == "IDENTIFIED!" ]]; then
    [[ -z "$MATCHED" ]] && MATCHED="(name unavailable — check log for 'matched finger')"
    log "✓  MATCHED: $MATCHED"
    [[ -n "$BZ3" ]] && log "   BZ3 score: $BZ3 (threshold: $(grep -oP 'EGIS0577_BZ3_THRESHOLD \K\d+' "$REPO/refs/libfprint/libfprint/drivers/egis0577.h"))"
  elif [[ "$RESULT" == "NOT IDENTIFIED!" ]]; then
    log "✗  NO MATCH — finger not in enrolled set."
    [[ -n "$BZ3" ]] && log "   Highest BZ3 score: $BZ3"
  else
    log "?  Unknown result — check log."
  fi
done

# ── summary ───────────────────────────────────────────────────────────────────
banner "SESSION COMPLETE"
log "Enrolled fingers: ${ENROLLED_NAMES[*]}"
log "Log: $LOG"
