#!/usr/bin/env bash
# Interactive PGM capture loop — guides the user through capturing N images.
# Each accepted press is saved as a numbered PGM and counted.
#
# Usage: ./tools/eh577_pgm_loop.sh [N]    # default N=5
#
# Captures land in artifacts/pgm-runs/YYYYMMDD-HHMMSS/ (log, raw dumps,
# helper PGMs), and accepted PGMs are also copied to artifacts/pgm/.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
HELPER="$REPO/refs/libfprint/build/examples/eh577-capture-helper"

COUNT="${1:-5}"
SESSION="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$REPO/artifacts/pgm-runs/$SESSION"
PGM_DIR="$REPO/artifacts/pgm"
RAW_DIR="$OUT_DIR/raw"

# 1. Figure out sudo upfront
log() { echo -e "$@" >&3; }
say() { echo -e "$@"; }

say "Requesting sudo privileges for device access..."
sudo -v || { say "ERROR: sudo access required."; exit 1; }

sudo mkdir -p "$OUT_DIR" "$RAW_DIR" "$PGM_DIR"
sudo chown -R "$(id -u):$(id -g)" "$OUT_DIR" "$PGM_DIR" 2>/dev/null || true

LOG="$OUT_DIR/pgm-loop.log"
exec 3>> "$LOG"

say "=== EH577 PGM Capture Loop [$SESSION] ==="
say "Log: $LOG"
say "Evidence directory: $OUT_DIR"
log "=== EH577 PGM Capture Loop [$SESSION] ==="

if [[ ! -x "$HELPER" ]]; then
  say "ERROR: Helper not built: $HELPER"
  exit 1
fi
if ! lsusb | grep -q '1c7a:0577'; then
  say "ERROR: EH577 (1c7a:0577) not on USB — replug it."
  exit 1
fi

# 4. Fprint level reset / freeing device
say "▶ Resetting fprint device state (stopping fprintd)..."
sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo pkill -f fprintd 2>/dev/null || true
sudo pkill -f "eh577-capture-helper" 2>/dev/null || true

USBDEV=$(lsusb -d 1c7a:0577 2>/dev/null | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
if [[ -n "$USBDEV" && -e "$USBDEV" ]]; then
  for i in 1 2 3 4 5; do
    sudo fuser "$USBDEV" &>/dev/null || break
    say "Waiting for USB device to be released (${i}s)..."
    sleep 1
  done
fi

CLEANED=""
cleanup() {
  [[ -n "$CLEANED" ]] && return
  CLEANED=1
  sudo pkill -INT -f "$HELPER" 2>/dev/null || true
  for _ in $(seq 1 20); do
    sudo pgrep -f "$HELPER" >/dev/null 2>&1 || break
    sleep 0.25
  done
  sudo pkill -KILL -f "$HELPER" 2>/dev/null || true
  sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo chown -R "$(id -u):$(id -g)" "$OUT_DIR" "$PGM_DIR" 2>/dev/null || true
}
on_interrupt() {
  say ""
  say "Interrupted — shutting down device cleanly..."
  cleanup
  exit 130
}
trap on_interrupt INT TERM
trap cleanup EXIT

# 2. Full debug logs to file descriptor (already handled via exec 3>> and stderr redirect)
# 3. Terminal output guides the user
say ""
say "Capturing $COUNT frame(s). Follow the prompts below."
say "══════════════════════════════════════════════════"

CAPTURED=0
LAST_PROGRESS=$SECONDS
INIT_STATE="starting"
LAST_STATE=""

say "▶ Waiting for sensor to initialize..."

while true; do
  IFS= read -r -t 2 line; rc=$?
  if (( rc > 128 )); then
    # Timeout, just a heartbeat
    if [[ "$INIT_STATE" == "starting" ]] && (( SECONDS - LAST_PROGRESS >= 6 )); then
      say "  ⏳ Device initialization is taking unusually long..."
      say "     If it remains stuck, the USB interface is likely wedged."
      say "     (Fix: physically unplug and replug the sensor, or run usbfs reset, then try again)."
      LAST_PROGRESS=$SECONDS
    fi
    continue
  fi
  (( rc != 0 )) && break
  LAST_PROGRESS=$SECONDS
  log "$line"

  if [[ "$line" =~ ^EH577_CAPTURE\ device-opened ]]; then
    INIT_STATE="opened"
    say "▶ Device opened. Waiting for your finger..."
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ touch\ ([0-9]+)/([0-9]+) ]]; then
    say "▶ Touch ${BASH_REMATCH[1]}/${BASH_REMATCH[2]} — press and hold"
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ finger-status\ status=present ]]; then
    say "  ⬤ Finger present on sensor..."
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ state\ (.*) ]]; then
    STATE=${BASH_REMATCH[1]}
    if [[ "$STATE" != "$LAST_STATE" ]]; then
      case "$STATE" in
        arming)     say "  ⟳ Arming sensor..." ;;
        settling)   say "  ~ Waiting for 400ms settle window..." ;;
        evaluating) say "  ⚙ Starting frame evaluation..." ;;
        success)    say "  ★ Potentially successful frame captured!" ;;
        timeout)    say "  ⏱ Turn timed out (held too long)" ;;
        wait-lift)  say "  ↑ Waiting for finger to be lifted to re-arm..." ;;
      esac
      LAST_STATE="$STATE"
    fi
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ saved\ ([0-9]+)/([0-9]+)\ path=(.*) ]]; then
    CAPTURED=${BASH_REMATCH[1]}
    P=${BASH_REMATCH[3]}
    say "  ✓ Captured image saved"
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ retry\ message=(.*) ]]; then
    MSG=${BASH_REMATCH[1]}
    say "  ✗ Retry required: $MSG"
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ lift\ your\ finger ]]; then
    say "  ↑ Lift your finger for rearming..."
  fi

  if [[ "$line" =~ ^EH577_CAPTURE\ complete\ count=([0-9]+) ]]; then
    CAPTURED=${BASH_REMATCH[1]}
    say "✓ Done capturing $CAPTURED frames."
    break
  fi
done < <(sudo sh -c "
  cd '$REPO'
  exec env LD_LIBRARY_PATH='$LIBFP' \
    G_MESSAGES_DEBUG=all \
    EGIS0577_FRAME_DUMP_DIR='$RAW_DIR' \
    EH577_CAPTURE_VERBOSE_STATUS=1 \
    sh -c \"exec 3>&1; exec \\\"\\\$1\\\" \\\"\\\$2\\\" \\\"\\\$3\\\" 2>>\\\"\\\$4\\\"\" _ '$HELPER' '$OUT_DIR' '$COUNT' '$LOG'
")

sudo chown -R "$(id -u):$(id -g)" "$OUT_DIR" "$PGM_DIR" 2>/dev/null || true

# Rename and move accepted PGMs
accepted=0
for pgm in "$OUT_DIR"/capture-*.pgm; do
  [[ -e "$pgm" ]] || continue
  seq=$(printf "%03d" $((accepted + 1)))
  dest="$PGM_DIR/${SESSION}-${seq}.pgm"
  cp "$pgm" "$dest"
  accepted=$((accepted + 1))
  log "Copied to $dest"
done

say "══════════════════════════════════════════════════"
say "Session Complete"
say "Requested: $COUNT | Captured: $CAPTURED | Saved: $accepted"
say "Raw frames: $RAW_DIR"
say "PGM folder: $PGM_DIR"
