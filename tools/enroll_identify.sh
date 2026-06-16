#!/usr/bin/env bash
# Multi-finger enrollment + identification session.
#
# Phase 1: Enroll as many fingers as you like, one at a time.
#          Type 'd' when done enrolling.
#
# Phase 2: Touch any enrolled finger.  Script reports which finger
#          matched (or "NO MATCH" if none did).  Type 'done' to exit.
#
# Terminal output is limited to clean step-by-step prompts.
# All driver/debug output is written silently to $LOG.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL_BIN="$REPO/refs/libfprint/build/examples/enroll"
ENROLL_HELPER_BIN="$REPO/refs/libfprint/build/examples/eh577-enroll-helper"
IDENTIFY_HELPER_BIN="$REPO/refs/libfprint/build/examples/eh577-identify-helper"
IDENTIFY_BIN="$REPO/refs/libfprint/build/examples/identify"
CLEAR_BIN="$REPO/refs/libfprint/build/examples/clear-storage"
SESSION="$(date +%Y%m%d-%H%M%S)"
EVIDENCE_DIR="$REPO/artifacts/pgm-debug/$SESSION"
mkdir -p "$EVIDENCE_DIR"
LOG="$EVIDENCE_DIR/session.log"
exec 3>> "$LOG"
log() { echo -e "$@" >&3; }             # file only, fast append
say() { echo -e "$@"; }                 # terminal only
tee_log() { tee -a "$LOG" > /dev/null; } # pipe: file only, swallow terminal
FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

# ── pre-flight ────────────────────────────────────────────────────────────────
log "=== EH577 Enroll + Identify [$SESSION] ==="
log "Log: $LOG"
say "Evidence directory: $EVIDENCE_DIR"

cat <<EOF > "$EVIDENCE_DIR/metadata.json"
{
  "session": "$SESSION",
  "log": "$LOG"
}
EOF

if ! lsusb | grep -q '1c7a:0577'; then
  say "ERROR: EH577 not found. Check USB connection."; exit 1
fi
if [[ ! -x "$ENROLL_HELPER_BIN" ]]; then
  say "ERROR: $ENROLL_HELPER_BIN not built."; exit 1
fi
if [[ ! -x "$IDENTIFY_HELPER_BIN" ]]; then
  say "ERROR: $IDENTIFY_HELPER_BIN not built."; exit 1
fi
# Stop fprintd and any socket-activated restarts; wait until USB device is free
sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo pkill -f fprintd 2>/dev/null || true
sudo pkill -f "eh577-enroll-helper" 2>/dev/null || true
sudo pkill -f "eh577-identify-helper" 2>/dev/null || true
USBDEV=$(lsusb -d 1c7a:0577 2>/dev/null \
  | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
if [[ -n "$USBDEV" && -e "$USBDEV" ]]; then
  for i in 1 2 3 4 5; do
    sudo fuser "$USBDEV" &>/dev/null || break
    echo "Waiting for USB device to be released (${i}s)..."
    sleep 1
  done
fi

# Clean shutdown. On Ctrl-C (or any exit) ask whichever privileged helper is
# running to close the device first — both helpers trap SIGINT and run
# fp_device_close(), releasing the USB interface — then escalate to SIGKILL only
# if it ignores us. Afterwards keep fprintd (and its socket activation) off the
# just-freed device and hand session artifacts back to the invoking user.
# Idempotent so the INT and EXIT traps can both fire harmlessly.
CLEANED=""
cleanup() {
  [[ -n "$CLEANED" ]] && return
  CLEANED=1
  for BIN in "$ENROLL_HELPER_BIN" "$IDENTIFY_HELPER_BIN"; do
    sudo pkill -INT -f "$BIN" 2>/dev/null || true
  done
  for _ in $(seq 1 20); do
    sudo pgrep -f "$ENROLL_HELPER_BIN"   >/dev/null 2>&1 ||
    sudo pgrep -f "$IDENTIFY_HELPER_BIN" >/dev/null 2>&1 || break
    sleep 0.25
  done
  for BIN in "$ENROLL_HELPER_BIN" "$IDENTIFY_HELPER_BIN"; do
    sudo pkill -KILL -f "$BIN" 2>/dev/null || true
  done
  sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo pkill -f fprintd 2>/dev/null || true
  sudo chown -R "$(id -u):$(id -g)" "$EVIDENCE_DIR" \
       "$REPO/test-storage.variant" "$REPO/enrolled.pgm" "$REPO/identify.pgm" 2>/dev/null || true
}
on_interrupt() {
  say ""
  say "Interrupted — shutting down device cleanly..."
  cleanup
  exit 130
}
trap on_interrupt INT TERM
trap cleanup EXIT

# ── optional storage clear ────────────────────────────────────────────────────
say ""
printf "Clear all previously enrolled prints before starting? [y/N] "
read -r CLEAR_CHOICE
if [[ "$CLEAR_CHOICE" =~ ^[yY]$ ]]; then
  say "Clearing local example storage..."
  # EH577 currently uses host-side example storage (test-storage.variant), not
  # on-device storage. The generic clear-storage example talks to device
  # storage and can hang or fail on drivers that do not advertise STORAGE.
  rm -f "$REPO/test-storage.variant"
  rm -f "$REPO/enrolled.pgm" "$REPO/identify.pgm" 2>/dev/null || true
  say "Done."
fi

# ── PHASE 1: Enrollment loop ──────────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  ENROLL FINGERS"
say "══════════════════════════"
say "(log → $LOG)"
ENROLLED_NAMES=()
ENROLLED_INDICES=()

while true; do
  say ""
  say "Pick a finger to enroll, or 'd' to finish:"
  say "  [0] left thumb    [5] right thumb"
  say "  [1] left index    [6] right index"
  say "  [2] left middle   [7] right middle"
  say "  [3] left ring     [8] right ring"
  say "  [4] left little   [9] right little"
  say "  [d] done"
  read -rp "> " CHOICE

  if [[ "$CHOICE" =~ ^[dD]$ ]]; then
    if [[ ${#ENROLLED_NAMES[@]} -eq 0 ]]; then
      say "Enroll at least one finger first."; continue
    fi
    break
  fi

  if [[ ! "$CHOICE" =~ ^[0-9]$ ]]; then
    say "Invalid — enter 0–9 or 'd'."; continue
  fi

  FNAME="${FINGER_NAMES[$CHOICE]}"
  log "--- Enrolling: $FNAME (index $CHOICE) ---"

  STAGE_HINTS=(
    "normal — fingertip toward you, flat"
    "tilt slightly left (~20°)"
    "tilt slightly right (~20°)"
    "ROTATED 180° — fingertip pointing away from you"
    "normal again — flat"
    "tilt slightly left again"
    "tilt slightly right again"
    "normal — firm press"
  )

  say ""
  say "  Enrolling: $FNAME (number of touches set by driver)"
  say "  Tip: different placements make matching more robust."
  say ""
  say "  ▶  Waiting for sensor to initialize..."

  NR_STAGES=""
  COMPLETED=0
  STAGE_STATE="init"   # init | ready | finger_down | captured
  LAST_RETRY_MSG=""

  # Watchdog: read with a 2s timeout so a wedged helper never blocks the loop
  # forever. stderr/debug goes straight to $LOG, so this stream is only the clean
  # EH577_HELPER events — an idle gap here means the driver is actually stalled.
  LAST_PROGRESS=$SECONDS
  while true; do
    IFS= read -r -t 2 line; rc=$?
    if (( rc > 128 )); then
      if [[ "$STAGE_STATE" == "finger_down" ]] && (( SECONDS - LAST_PROGRESS >= 12 )); then
        say "  ⏳  Still capturing — if the sensor seems stuck, lift and press again."
        LAST_PROGRESS=$SECONDS
      fi
      continue
    fi
    (( rc != 0 )) && break    # EOF: helper exited
    LAST_PROGRESS=$SECONDS
    log "$line"

    if [[ -z "$NR_STAGES" && "$line" =~ ^EH577_HELPER\ device-opened\ .*total=([0-9]+) ]]; then
      NR_STAGES="${BASH_REMATCH[1]}"
      STAGE_STATE="ready"
      say "  ▶  Touch 1/$NR_STAGES — ${STAGE_HINTS[0]:-normal, vary angle}"
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=present" && "$STAGE_STATE" == "ready" ]]; then
      STAGE_STATE="finger_down"
      say "  ⬤  Finger detected — hold still..."
    fi

    if [[ "$line" =~ ^EH577_HELPER\ enroll-retry\ completed=([0-9]+)\ total=([0-9]+)\ code=([^[:space:]]+)\ message=(.*)$ ]]; then
      COMPLETED="${BASH_REMATCH[1]}"
      NR_STAGES="${BASH_REMATCH[2]}"
      case "${BASH_REMATCH[3]}" in
        remove-finger) LAST_RETRY_MSG="  ✗  Remove finger and try again" ;;
        center-finger) LAST_RETRY_MSG="  ✗  Center finger and try again" ;;
        too-short) LAST_RETRY_MSG="  ✗  Capture too short — hold longer" ;;
        too-fast) LAST_RETRY_MSG="  ✗  Capture too fast — press more steadily" ;;
        *) LAST_RETRY_MSG="  ✗  Capture not usable — lift and retry" ;;
      esac
      STAGE_STATE="retry_wait_lift"
      say "$LAST_RETRY_MSG"
    fi

    if [[ "$line" =~ ^EH577_HELPER\ enroll-stage\ completed=([0-9]+)\ total=([0-9]+)$ ]]; then
      COMPLETED="${BASH_REMATCH[1]}"
      NR_STAGES="${BASH_REMATCH[2]}"
      STAGE_STATE="captured"
      say "  ✓  Touch $COMPLETED/${NR_STAGES:-?} captured"
      if [[ -n "$NR_STAGES" && $COMPLETED -lt $NR_STAGES ]]; then
        say "  ↑  Lift your finger — sensor resetting"
      fi
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=none" && "$STAGE_STATE" == "captured" ]]; then
      if [[ -n "$NR_STAGES" && $COMPLETED -lt $NR_STAGES ]]; then
        STAGE_STATE="ready"
        HINT="${STAGE_HINTS[$COMPLETED]:-normal, vary angle}"
        say "  ▶  Touch $((COMPLETED+1))/$NR_STAGES — $HINT"
      fi
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=none" && "$STAGE_STATE" == "retry_wait_lift" ]]; then
      STAGE_STATE="ready"
      say "  ▶  Touch $((COMPLETED+1))/${NR_STAGES:-?} — ${STAGE_HINTS[$COMPLETED]:-normal, vary angle} (retry)"
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=none" && "$STAGE_STATE" == "finger_down" ]]; then
      STAGE_STATE="ready"
      say "  ✗  Finger lifted too soon — hold until captured"
      say "  ▶  Touch $((COMPLETED+1))/${NR_STAGES:-?} — ${STAGE_HINTS[$COMPLETED]:-normal, vary angle} (retry)"
    fi
  done < <(sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print EGIS0577_FRAME_DUMP_DIR='$EVIDENCE_DIR/raw-enroll' '$ENROLL_HELPER_BIN' --finger-index '$CHOICE' --save-image '$REPO/enrolled.pgm' 2>>'$LOG'
  ")

  sudo chown -R "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" "$EVIDENCE_DIR/raw-enroll" 2>/dev/null || true
  if [[ -f "$REPO/enrolled.pgm" ]]; then
    FNAME_SLUG="${FNAME// /-}"
    cp "$REPO/enrolled.pgm" "$EVIDENCE_DIR/enroll-${FNAME_SLUG}.pgm"
    say "  Saved enrolled PGM to $EVIDENCE_DIR/enroll-${FNAME_SLUG}.pgm"
  fi

  say ""
  if [[ -n "$NR_STAGES" && $COMPLETED -eq $NR_STAGES ]]; then
    say "  ✓✓  $FNAME enrolled ($COMPLETED/$NR_STAGES)"
    ENROLLED_NAMES+=("$FNAME")
    ENROLLED_INDICES+=("$CHOICE")
    log "Enrolled: $FNAME"
  else
    say "  ✗   Enrollment incomplete ($COMPLETED/${NR_STAGES:-?}) — try again."
    log "Enrollment incomplete: $FNAME ($COMPLETED/${NR_STAGES:-?})"
  fi
done

say ""
say "Enrolled:"
for F in "${ENROLLED_NAMES[@]}"; do say "  • $F"; done

# ── PHASE 2: Identification loop ──────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  IDENTIFY FINGERS"
say "══════════════════════════"
say "Enrolled: ${ENROLLED_NAMES[*]}"
say "Type 'done' to exit."

IDENT_ATTEMPT=0

while true; do
  say ""
  printf "Press Enter to scan (or 'done' to exit): "
  read -r CHOICE

  if [[ "$CHOICE" =~ ^[dD](one)?$ ]]; then
    break
  fi

  say "  ▶  Place your finger on the sensor"

  IDENT_STATE="ready"
  IDENT_RESULT=""
  IDENT_MATCHED=""
  IDENT_RETRY_MSG=""
  RAW_ID_DIR="$EVIDENCE_DIR/raw-identify-$(printf "%02d" $((IDENT_ATTEMPT+1)))"

  # Watchdog: 2s read timeout so a wedged helper never blocks forever.
  LAST_PROGRESS=$SECONDS
  while true; do
    IFS= read -r -t 2 line; rc=$?
    if (( rc > 128 )); then
      if [[ "$IDENT_STATE" == "finger_down" ]] && (( SECONDS - LAST_PROGRESS >= 12 )); then
        say "  ⏳  Still capturing — if the sensor seems stuck, lift and press again."
        LAST_PROGRESS=$SECONDS
      fi
      continue
    fi
    (( rc != 0 )) && break    # EOF: helper exited
    LAST_PROGRESS=$SECONDS
    log "$line"

    if [[ "$line" == "EH577_IDENTIFY finger-status status=present" && "$IDENT_STATE" == "ready" ]]; then
      IDENT_STATE="finger_down"
      say "  ⬤  Finger detected — hold still..."
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-retry\ code=([^[:space:]]+)\ message=(.*)$ ]]; then
      case "${BASH_REMATCH[1]}" in
        remove-finger) IDENT_RETRY_MSG="  ✗  Remove finger and try again" ;;
        center-finger) IDENT_RETRY_MSG="  ✗  Center finger and try again" ;;
        too-short) IDENT_RETRY_MSG="  ✗  Capture too short — hold longer" ;;
        too-fast) IDENT_RETRY_MSG="  ✗  Capture too fast — press more steadily" ;;
        *) IDENT_RETRY_MSG="  ✗  Capture not usable — try again" ;;
      esac
      say "$IDENT_RETRY_MSG"
      IDENT_STATE="ready"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ image-saved ]]; then
      say "  ↑  Image captured — lift your finger to see result"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-result\ result=match\ finger=(.*)$ ]]; then
      IDENT_RESULT="match"
      IDENT_MATCHED="${BASH_REMATCH[1]}"
    fi

    if [[ "$line" == "EH577_IDENTIFY identify-result result=no-match" ]]; then
      IDENT_RESULT="no-match"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-complete\ result=retry ]]; then
      IDENT_RESULT="retry"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-complete\ result=failure ]]; then
      IDENT_RESULT="failure"
    fi
  done < <(sudo sh -c "
    cd '$REPO'
    rm -f '$REPO/identify.pgm'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print EGIS0577_FRAME_DUMP_DIR='$RAW_ID_DIR' '$IDENTIFY_HELPER_BIN' --save-image '$REPO/identify.pgm' 2>>'$LOG'
  ")

  sudo chown -R "$(id -u):$(id -g)" "$REPO/identify.pgm" "$RAW_ID_DIR" 2>/dev/null || true

  ((IDENT_ATTEMPT++))
  IDENT_ATTEMPT_PAD=$(printf "%02d" $IDENT_ATTEMPT)

  if [[ -f "$REPO/identify.pgm" ]]; then
    cp "$REPO/identify.pgm" "$EVIDENCE_DIR/identify-attempt-${IDENT_ATTEMPT_PAD}-${IDENT_RESULT}.pgm"
    say "  Saved identify PGM to $EVIDENCE_DIR/identify-attempt-${IDENT_ATTEMPT_PAD}-${IDENT_RESULT}.pgm"
  fi

  say "  ▶  Lift your finger"
  say ""
  if [[ "$IDENT_RESULT" == "match" ]]; then
    [[ -z "$IDENT_MATCHED" ]] && IDENT_MATCHED="(unknown)"
    say "  ✓  MATCHED: $IDENT_MATCHED"
  elif [[ "$IDENT_RESULT" == "no-match" ]]; then
    say "  ✗  NO MATCH"
  elif [[ "$IDENT_RESULT" == "retry" ]]; then
    say "${IDENT_RETRY_MSG:-  ✗  Capture not usable — try again}"
  else
    say "  ?  No result — check log: $LOG"
  fi
done

# ── summary ───────────────────────────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  SESSION COMPLETE"
say "══════════════════════════"
say "Enrolled: ${ENROLLED_NAMES[*]}"
say "Evidence dir: $EVIDENCE_DIR"
say "Log: $LOG"
