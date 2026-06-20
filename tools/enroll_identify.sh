#!/usr/bin/env bash
# Multi-finger enrollment + identify session for EH577.
#
# Improvements over prior version:
#   1. Sudo resolved upfront — one password prompt, then non-interactive throughout.
#   2. All driver/debug output goes to the log file only; terminal stays clean.
#   3. Terminal shows step-by-step prompts for every critical manual step.
#   4. fprint-level device reset (open+close) at start for a clean slate.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
RESET_BIN="$REPO/refs/libfprint/build/examples/eh577-reset-helper"
ENROLL_HELPER_BIN="$REPO/refs/libfprint/build/examples/eh577-enroll-helper"
IDENTIFY_HELPER_BIN="$REPO/refs/libfprint/build/examples/eh577-identify-helper"

SESSION="$(date +%Y%m%d-%H%M%S)"
EVIDENCE_DIR="$REPO/artifacts/pgm-debug/$SESSION"
LOG="$EVIDENCE_DIR/session.log"

# say() is defined early so preflight and sudo prompts can use it.
# log() opens fd 3, which is set up after sudo acquires credentials and creates
# the evidence dir (the parent may be root-owned from a prior session).
say() { printf '%s\n' "$@"; }

FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

STAGE_HINTS=(
  "normal — fingertip toward you, flat"
  "tilt slightly left (~20°)"
  "tilt slightly right (~20°)"
  "ROTATED 180° — fingertip pointing away"
  "normal again — flat"
  "tilt slightly left again"
  "tilt slightly right again"
  "roll onto the upper pad of the finger"
  "roll onto the lower pad of the finger"
  "shift slightly left of centre"
  "shift slightly right of centre"
  "normal — firm, centred press"
)

# ── pre-flight ─────────────────────────────────────────────────────────────────
say ""
if ! lsusb | grep -q '1c7a:0577'; then
  say "ERROR: EH577 not found. Check USB connection."; exit 1
fi
for BIN in "$RESET_BIN" "$ENROLL_HELPER_BIN" "$IDENTIFY_HELPER_BIN"; do
  [[ -x "$BIN" ]] || {
    say "ERROR: binary not built: $BIN"
    say "Run: ninja -C '$REPO/refs/libfprint/build'"
    exit 1
  }
done

# ── 1. Sudo upfront ────────────────────────────────────────────────────────────
# Cache credentials once; all further helper invocations use sudo -n (no prompt).
if ! sudo -n true 2>/dev/null; then
  say "This script needs root to open the fingerprint sensor."
  say "Enter your password once — all remaining steps run without prompting."
  sudo -v || { say "ERROR: sudo authentication failed."; exit 1; }
fi

# Keep the credential cache alive (default sudo TTL is 5–15 min; refresh every 55s).
( while true; do sudo -n -v 2>/dev/null; sleep 55; done ) &
KEEPALIVE_PID=$!

# ── Evidence dir & log ─────────────────────────────────────────────────────────
# Use sudo to create in case parent dirs are root-owned from a prior session,
# then chown back so the user-level exec 3>>"$LOG" can open the file.
sudo -n mkdir -p "$EVIDENCE_DIR"
sudo -n chown -R "$(id -u):$(id -g)" "$REPO/artifacts/pgm-debug"
exec 3>>"$LOG" || exec 3>/dev/null   # fallback: fd 3 → /dev/null if still fails
log() { printf '%s\n' "$@" >&3; }    # log file only (fd 3 is now open)

say "Evidence directory: $EVIDENCE_DIR"
log "=== EH577 Enroll + Identify [$SESSION] ==="

# ── cleanup ────────────────────────────────────────────────────────────────────
CLEANED=""
_cleanup() {
  [[ -n "$CLEANED" ]] && return; CLEANED=1
  kill "$KEEPALIVE_PID" 2>/dev/null || true
  for BIN in "$ENROLL_HELPER_BIN" "$IDENTIFY_HELPER_BIN"; do
    sudo -n pkill -INT -f "$BIN" 2>/dev/null || true
  done
  local i; for i in $(seq 1 20); do
    sudo -n pgrep -f "$ENROLL_HELPER_BIN"   >/dev/null 2>&1 ||
    sudo -n pgrep -f "$IDENTIFY_HELPER_BIN" >/dev/null 2>&1 || break
    sleep 0.25
  done
  for BIN in "$ENROLL_HELPER_BIN" "$IDENTIFY_HELPER_BIN"; do
    sudo -n pkill -KILL -f "$BIN" 2>/dev/null || true
  done
  sudo -n systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo -n pkill -f fprintd 2>/dev/null || true
  sudo -n chown -R "$(id -u):$(id -g)" \
    "$EVIDENCE_DIR" \
    "$REPO/test-storage.variant" \
    "$REPO/enrolled.pgm" \
    "$REPO/identify.pgm" 2>/dev/null || true
}
trap '{ say ""; say "Interrupted — closing device cleanly..."; _cleanup; exit 130; }' INT TERM
trap '_cleanup' EXIT

# ── Stop fprintd; wait for USB device to be free ──────────────────────────────
sudo -n systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo -n pkill -f fprintd 2>/dev/null || true
sudo -n pkill -f "eh577-enroll-helper"   2>/dev/null || true
sudo -n pkill -f "eh577-identify-helper" 2>/dev/null || true

USBDEV=$(lsusb -d 1c7a:0577 2>/dev/null \
  | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
if [[ -n "$USBDEV" && -e "$USBDEV" ]]; then
  for i in 1 2 3 4 5; do
    sudo -n fuser "$USBDEV" &>/dev/null || break
    say "Waiting for USB device to be released (${i}/5)..."
    sleep 1
  done
fi

# ── 4. fprint-level device reset (open + close) ───────────────────────────────
say "Resetting device (fprint open+close)..."
RESET_OK=0
while IFS= read -r _rline; do
  log "reset: $_rline"
  [[ "$_rline" == "EH577_RESET device-closed" ]] && RESET_OK=1
done < <(sudo -n sh -c "
  exec env LD_LIBRARY_PATH='$LIBFP' \
       G_MESSAGES_DEBUG=libfprint,libfprint-egis0577 \
       '$RESET_BIN' 2>>'$LOG'
")
if [[ $RESET_OK -eq 1 ]]; then
  say "Device ready."
  log "Device reset OK."
else
  say "WARNING: reset did not complete cleanly — check log: $LOG"
  log "WARNING: reset incomplete."
fi
say ""

# ── optional storage clear ─────────────────────────────────────────────────────
printf "Clear all previously enrolled prints before starting? [y/N] "
read -r CLEAR_CHOICE
if [[ "$CLEAR_CHOICE" =~ ^[yY]$ ]]; then
  rm -f "$REPO/test-storage.variant" "$REPO/enrolled.pgm" "$REPO/identify.pgm" 2>/dev/null || true
  say "Prints cleared."
fi

# ── PHASE 1: Enrollment ────────────────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  ENROLL FINGERS"
say "══════════════════════════"
say "(debug log → $LOG)"

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
    [[ ${#ENROLLED_NAMES[@]} -eq 0 ]] && { say "Enroll at least one finger first."; continue; }
    break
  fi

  [[ ! "$CHOICE" =~ ^[0-9]$ ]] && { say "Invalid — enter 0–9 or 'd'."; continue; }

  FNAME="${FINGER_NAMES[$CHOICE]}"
  log "--- Enrolling: $FNAME (index $CHOICE) ---"

  say ""
  say "  Enrolling: $FNAME"
  say "  Tip: vary the angle slightly on each touch for better matching."
  say ""
  say "  Waiting for sensor to initialise..."

  NR_STAGES=""
  COMPLETED=0
  STAGE_STATE="init"   # init | ready | finger_down | captured | need_lift

  LAST_PROGRESS=$SECONDS
  while true; do
    IFS= read -r -t 2 line; rc=$?

    if (( rc > 128 )); then
      # read timeout: watchdog
      if [[ "$STAGE_STATE" == "finger_down" ]] && (( SECONDS - LAST_PROGRESS >= 12 )); then
        say "  ⏳  Still capturing — if the sensor seems stuck, lift and press again."
        LAST_PROGRESS=$SECONDS
      fi
      continue
    fi
    (( rc != 0 )) && break   # EOF: helper exited
    LAST_PROGRESS=$SECONDS
    log "$line"

    if [[ -z "$NR_STAGES" && "$line" =~ ^EH577_HELPER\ device-opened.*total=([0-9]+) ]]; then
      NR_STAGES="${BASH_REMATCH[1]}"
      STAGE_STATE="ready"
      say "  ▶  Touch 1/$NR_STAGES — ${STAGE_HINTS[0]}"
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=present" && "$STAGE_STATE" == "ready" ]]; then
      STAGE_STATE="finger_down"
      say "  ⬤  Finger detected — hold still..."
    fi

    if [[ "$line" =~ ^EH577_HELPER\ enroll-stage\ completed=([0-9]+)\ total=([0-9]+)$ ]]; then
      COMPLETED="${BASH_REMATCH[1]}"
      NR_STAGES="${BASH_REMATCH[2]}"
      STAGE_STATE="captured"
      say "  ✓  Touch $COMPLETED/$NR_STAGES captured"
      if (( COMPLETED < NR_STAGES )); then
        say "  ↑  Lift your finger — sensor resetting"
      fi
    fi

    if [[ "$line" =~ ^EH577_HELPER\ enroll-retry\ completed=([0-9]+)\ total=([0-9]+)\ code=([^[:space:]]+) ]]; then
      COMPLETED="${BASH_REMATCH[1]}"
      NR_STAGES="${BASH_REMATCH[2]}"
      case "${BASH_REMATCH[3]}" in
        remove-finger) say "  ✗  Remove finger and try again" ;;
        center-finger) say "  ✗  Center finger and try again" ;;
        too-short)     say "  ✗  Capture too short — hold longer" ;;
        too-fast)      say "  ✗  Capture too fast — press more steadily" ;;
        *)             say "  ✗  Not captured — lift finger and try again" ;;
      esac
      STAGE_STATE="need_lift"
    fi

    if [[ "$line" == "EH577_HELPER finger-status status=none" ]]; then
      case "$STAGE_STATE" in
        captured)
          if (( COMPLETED < NR_STAGES )); then
            STAGE_STATE="ready"
            HINT="${STAGE_HINTS[$COMPLETED]:-normal, vary angle}"
            say "  ▶  Touch $((COMPLETED+1))/$NR_STAGES — $HINT"
          fi
          ;;
        need_lift|finger_down)
          STAGE_STATE="ready"
          say "  ▶  Touch $((COMPLETED+1))/${NR_STAGES:-?} — ${STAGE_HINTS[$COMPLETED]:-normal, vary angle} (retry)"
          ;;
      esac
    fi
  done < <(sudo -n sh -c "
    cd '$REPO' && exec env \
      LD_LIBRARY_PATH='$LIBFP' \
      G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print \
      EGIS0577_FRAME_DUMP_DIR='$EVIDENCE_DIR/raw-enroll' \
      '$ENROLL_HELPER_BIN' --finger-index '$CHOICE' --save-image '$REPO/enrolled.pgm' \
      2>>'$LOG'
  ")

  sudo -n chown -R "$(id -u):$(id -g)" \
    "$REPO/test-storage.variant" "$REPO/enrolled.pgm" \
    "$EVIDENCE_DIR/raw-enroll" 2>/dev/null || true

  if [[ -f "$REPO/enrolled.pgm" ]]; then
    FNAME_SLUG="${FNAME// /-}"
    cp "$REPO/enrolled.pgm" "$EVIDENCE_DIR/enroll-${FNAME_SLUG}.pgm"
    say "  Saved enrolled PGM → $EVIDENCE_DIR/enroll-${FNAME_SLUG}.pgm"
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

# ── PHASE 2: Identify ──────────────────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  IDENTIFY FINGERS"
say "══════════════════════════"
say "Enrolled: ${ENROLLED_NAMES[*]}"
say "Touch any enrolled finger to identify.  Type 'done' to exit."

IDENT_ATTEMPT=0

while true; do
  say ""
  printf "Press Enter to scan (or 'done' to exit): "
  read -r CHOICE

  [[ "$CHOICE" =~ ^[dD](one)?$ ]] && break

  say "  ▶  Place your finger on the sensor"

  IDENT_STATE="ready"
  IDENT_RESULT=""
  IDENT_MATCHED=""
  RAW_ID_DIR="$EVIDENCE_DIR/raw-identify-$(printf "%02d" $((IDENT_ATTEMPT+1)))"

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
    (( rc != 0 )) && break   # EOF: helper exited
    LAST_PROGRESS=$SECONDS
    log "$line"

    if [[ "$line" == "EH577_IDENTIFY finger-status status=present" && "$IDENT_STATE" == "ready" ]]; then
      IDENT_STATE="finger_down"
      say "  ⬤  Finger detected — hold still..."
    fi

    if [[ "$line" == "EH577_IDENTIFY finger-status status=none" && "$IDENT_STATE" == "finger_down" ]]; then
      say "  ↑  Lift detected — processing..."
      IDENT_STATE="done"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-retry ]]; then
      say "  ✗  Not captured — lift finger and try again"
      IDENT_STATE="ready"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ image-saved ]]; then
      say "  ⬤  Image captured"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-result\ result=match\ finger=(.*)$ ]]; then
      IDENT_RESULT="match"
      IDENT_MATCHED="${BASH_REMATCH[1]}"
    fi

    if [[ "$line" == "EH577_IDENTIFY identify-result result=no-match" ]]; then
      IDENT_RESULT="no-match"
    fi

    if [[ "$line" =~ ^EH577_IDENTIFY\ identify-complete\ result=(retry|failure|no-gallery|unknown)(.*)$ ]]; then
      IDENT_RESULT="${BASH_REMATCH[1]}"
    fi
  done < <(sudo -n sh -c "
    cd '$REPO' && rm -f '$REPO/identify.pgm' && exec env \
      LD_LIBRARY_PATH='$LIBFP' \
      G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print \
      EGIS0577_FRAME_DUMP_DIR='$RAW_ID_DIR' \
      '$IDENTIFY_HELPER_BIN' --save-image '$REPO/identify.pgm' \
      2>>'$LOG'
  ")

  sudo -n chown -R "$(id -u):$(id -g)" "$REPO/identify.pgm" "$RAW_ID_DIR" 2>/dev/null || true
  ((IDENT_ATTEMPT++))

  if [[ -f "$REPO/identify.pgm" ]]; then
    PADDED=$(printf "%02d" $IDENT_ATTEMPT)
    cp "$REPO/identify.pgm" "$EVIDENCE_DIR/identify-${PADDED}-${IDENT_RESULT:-none}.pgm"
    say "  Saved identify PGM → $EVIDENCE_DIR/identify-${PADDED}-${IDENT_RESULT:-none}.pgm"
  fi

  say ""
  case "$IDENT_RESULT" in
    match)     say "  ✓  MATCHED: ${IDENT_MATCHED:-unknown}" ;;
    no-match)  say "  ✗  NO MATCH" ;;
    no-gallery) say "  ✗  No enrolled prints found — run enroll first" ;;
    retry|failure) say "  ✗  Capture failed — try again (details in log)" ;;
    *)         say "  ?  No result — check log: $LOG" ;;
  esac
done

# ── summary ────────────────────────────────────────────────────────────────────
say ""
say "══════════════════════════"
say "  SESSION COMPLETE"
say "══════════════════════════"
say "Enrolled: ${ENROLLED_NAMES[*]:-none}"
say "Evidence: $EVIDENCE_DIR"
say "Log:      $LOG"
