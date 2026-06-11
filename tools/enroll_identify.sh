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
IDENTIFY_BIN="$REPO/refs/libfprint/build/examples/identify"
CLEAR_BIN="$REPO/refs/libfprint/build/examples/clear-storage"
SESSION="$(date +%Y%m%d-%H%M%S)"
LOG="$REPO/logs/identify-session-$SESSION.txt"

mkdir -p "$REPO/logs"

log() { echo "$@" >> "$LOG"; }          # file only
say() { echo "$@"; }                    # terminal only
tee_log() { tee -a "$LOG" > /dev/null; } # pipe: file only, swallow terminal

FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

# ── pre-flight ────────────────────────────────────────────────────────────────
log "=== EH577 Enroll + Identify [$SESSION] ==="
log "Log: $LOG"

if ! lsusb | grep -q '1c7a:0577'; then
  say "ERROR: EH577 not found. Check USB connection."; exit 1
fi
# Stop fprintd and any socket-activated restarts; wait until USB device is free
sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo pkill -f fprintd 2>/dev/null || true
USBDEV=$(ls /dev/bus/usb/003/00[0-9] 2>/dev/null | while read -r d; do lsusb -D "$d" 2>/dev/null | grep -q '1c7a:0577' && echo "$d"; done | head -1)
if [[ -n "$USBDEV" ]]; then
  for i in 1 2 3 4 5; do
    sudo fuser "$USBDEV" &>/dev/null || break
    sleep 1
  done
fi
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

# ── optional storage clear ────────────────────────────────────────────────────
say ""
printf "Clear all previously enrolled prints before starting? [y/N] "
read -r CLEAR_CHOICE
if [[ "$CLEAR_CHOICE" =~ ^[yY]$ ]]; then
  say "Clearing storage..."
  printf "Y\n" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=all '$CLEAR_BIN'
  " 2>&1 | tee_log || true
  sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" 2>/dev/null || true
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

  # Placement hints indexed by stage (0-based).  The 180°-rotated stage is
  # included because Bozorth3's ±11° beta tolerance cannot cover a full flip;
  # enrolling one stage upside-down covers that orientation directly.
  # Extra entries cover drivers with more than 5 stages; the fallback
  # "vary angle" is used for any stage beyond the last hint defined here.
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

  while IFS= read -r line; do
    log "$line"

    # Stage count from the binary's opening message
    if [[ -z "$NR_STAGES" && "$line" == *"times to complete the process"* ]]; then
      NR_STAGES=$(echo "$line" | grep -oP '\d+(?= times to complete)')
      STAGE_STATE="ready"
      say "  ▶  Touch 1/$NR_STAGES — ${STAGE_HINTS[0]:-normal, vary angle}"
    fi

    # Driver: finger just touched the sensor (first transition absent→present)
    if [[ "$STAGE_STATE" == "ready" && "$line" == *"Reporting finger present"* ]]; then
      STAGE_STATE="finger_down"
      say "  ⬤  Finger detected — hold still..."
    fi

    # Binary: stage officially passed
    if [[ "$line" == *"passed. Yay!"* ]]; then
      [[ -z "$NR_STAGES" ]] && NR_STAGES=$(echo "$line" | grep -oP 'of \K\d+(?= passed)')
      COMPLETED=$((COMPLETED + 1))
      STAGE_STATE="captured"
      say "  ✓  Touch $COMPLETED/${NR_STAGES:-?} captured"
    fi

    # Driver: finger absent after capture — inter-stage reset starting
    if [[ "$STAGE_STATE" == "captured" && "$line" == *"Reporting finger absent"* && "$line" == *"image submitted"* ]]; then
      say "  ↑  Lift your finger — sensor resetting"
      if [[ -n "$NR_STAGES" && $COMPLETED -lt $NR_STAGES ]]; then
        STAGE_STATE="ready"
        HINT="${STAGE_HINTS[$COMPLETED]:-normal, vary angle}"
        say "  ▶  Touch $((COMPLETED+1))/$NR_STAGES — $HINT"
      fi
    fi

    # Driver: finger lifted before image was captured — prompt retry
    if [[ "$STAGE_STATE" == "finger_down" && "$line" == *"Reporting finger absent"* ]]; then
      STAGE_STATE="ready"
      say "  ✗  Finger lifted too soon — hold until captured"
      say "  ▶  Touch $((COMPLETED+1))/${NR_STAGES:-?} — ${STAGE_HINTS[$COMPLETED]:-normal, vary angle} (retry)"
    fi

  sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo pkill -f fprintd 2>/dev/null || true
  done < <(printf "%d\nN\n" "$CHOICE" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint-egis0577 '$ENROLL_BIN'
  " 2>&1)

  sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true

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

while true; do
  say ""
  printf "Press Enter to scan (or 'done' to exit): "
  read -r CHOICE

  if [[ "$CHOICE" =~ ^[dD](one)?$ ]]; then
    break
  fi

  say "  ▶  Place your finger on the sensor"

  IDTMP=$(mktemp)
  printf "n\n" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=all '$IDENTIFY_BIN'
  " 2>&1 | tee -a "$LOG" > "$IDTMP"

  sudo chown "$(id -u):$(id -g)" "$REPO/identify.pgm" 2>/dev/null || true

  BZ3=$(grep -oP "score \K[0-9]+(?=/)" "$IDTMP" | sort -n | tail -1)
  MATCHED=$(grep -oP "matched finger \K.+?(?= successfully)" "$IDTMP" | tail -1)
  RESULT=$(grep -E "^(NOT )?IDENTIFIED!$" "$IDTMP" | tail -1)
  rm -f "$IDTMP"

  say "  ▶  Lift your finger"
  say ""
  if [[ "$RESULT" == "IDENTIFIED!" ]]; then
    [[ -z "$MATCHED" ]] && MATCHED="(see log)"
    say "  ✓  MATCHED: $MATCHED"
    [[ -n "$BZ3" ]] && say "     BZ3 score: $BZ3"
  elif [[ "$RESULT" == "NOT IDENTIFIED!" ]]; then
    say "  ✗  NO MATCH"
    [[ -n "$BZ3" ]] && say "     Best BZ3: $BZ3"
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
say "Log: $LOG"
