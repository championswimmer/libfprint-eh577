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
if systemctl is-active --quiet fprintd; then
  sudo systemctl stop fprintd 2>&1 | tee_log
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
  NR_STAGES=5   # matches IMG_ENROLL_STAGES in libfprint
  log "--- Enrolling: $FNAME (index $CHOICE) ---"

  # Angle hints for each stage — vary placement so the template covers
  # multiple orientations and matches from different approach angles.
  STAGE_HINTS=(
    "centre — flat and straight"
    "tilt slightly left"
    "tilt slightly right"
    "press from top (tip-heavy)"
    "press from bottom (base-heavy)"
  )

  say ""
  say "  Enrolling: $FNAME ($NR_STAGES touches — vary angle each time)"
  say "  Tip: different placements make matching more robust."
  say ""
  say "  ▶  Touch 1/$NR_STAGES — ${STAGE_HINTS[0]}"

  COMPLETED=0
  while IFS= read -r line; do
    log "$line"   # all driver output goes silently to log
    if [[ "$line" == *"passed. Yay!"* ]]; then
      COMPLETED=$((COMPLETED + 1))
      say "  ✓  Touch $COMPLETED/$NR_STAGES captured — lift your finger"
      if [[ $COMPLETED -lt $NR_STAGES ]]; then
        sleep 1   # visual beat while driver runs its 1.5 s inter-stage reset
        HINT="${STAGE_HINTS[$COMPLETED]}"
        say "  ▶  Touch $((COMPLETED+1))/$NR_STAGES — $HINT"
      fi
    fi
  done < <(printf "%d\nN\n" "$CHOICE" | sudo sh -c "
    cd '$REPO'
    exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint-egis0577 '$ENROLL_BIN'
  " 2>&1)

  sudo chown "$(id -u):$(id -g)" "$REPO/test-storage.variant" "$REPO/enrolled.pgm" 2>/dev/null || true

  say ""
  if [[ $COMPLETED -eq $NR_STAGES ]]; then
    say "  ✓✓  $FNAME enrolled ($NR_STAGES/$NR_STAGES)"
    ENROLLED_NAMES+=("$FNAME")
    ENROLLED_INDICES+=("$CHOICE")
    log "Enrolled: $FNAME"
  else
    say "  ✗   Enrollment incomplete ($COMPLETED/$NR_STAGES) — try again."
    log "Enrollment incomplete: $FNAME ($COMPLETED/$NR_STAGES)"
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
