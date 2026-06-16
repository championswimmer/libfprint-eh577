#!/usr/bin/env bash
# Lean PGM capture loop — no cue timers, no finger-selection UI.
# Each accepted press is saved as a numbered PGM and counted.
#
# Usage: ./tools/eh577_pgm_loop.sh [N]    # default N=5
#
# Captures land in artifacts/pgm-runs/YYYYMMDD-HHMMSS/ (log, raw dumps,
# helper PGMs), and accepted PGMs are also copied to artifacts/pgm/.
# Exit 0 if at least one frame was accepted; exit 1 if all were rejected.

set -uo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
HELPER="$REPO/refs/libfprint/build/examples/eh577-capture-helper"
COUNT="${1:-5}"
SESSION="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$REPO/artifacts/pgm-runs/$SESSION"
PGM_DIR="$REPO/artifacts/pgm"
RAW_DIR="$OUT_DIR/raw"
LOG="$OUT_DIR/pgm-loop.log"

cleanup() {
  if [[ -n "${HELPER_PID:-}" ]] && kill -0 "$HELPER_PID" 2>/dev/null; then
    echo "Interrupted; stopping capture helper (pid $HELPER_PID)..." >&2
    kill -TERM "$HELPER_PID" 2>/dev/null || true
    wait "$HELPER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT TERM

if [[ ! -x "$HELPER" ]]; then
  echo "Helper not built: $HELPER"; exit 1
fi
if ! lsusb | grep -q '1c7a:0577'; then
  echo "EH577 (1c7a:0577) not on USB — replug it."; exit 1
fi

free_device() {
  systemctl stop fprintd fprintd.socket 2>/dev/null || true
  pkill -f fprintd 2>/dev/null || true
  pkill -f "eh577-capture-helper" 2>/dev/null || true

  local devnode
  devnode=$(lsusb -d 1c7a:0577 2>/dev/null \
    | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
  [[ -z "$devnode" || ! -e "$devnode" ]] && return

  local waited=0
  while fuser "$devnode" >/dev/null 2>&1; do
    if (( waited >= 5 )); then
      echo "Warning: device still busy after 5 s — holder PIDs: $(fuser "$devnode" 2>/dev/null || true)"
      return
    fi
    echo "Waiting for USB device to be released (${waited}s)..."
    sleep 1
    (( waited++ )) || true
  done
}
free_device

mkdir -p "$OUT_DIR" "$RAW_DIR" "$PGM_DIR"

echo "Capturing $COUNT frame(s) — touch the sensor when prompted."
echo "Debug log: $LOG"
# Open fd 3 on the terminal so the helper can print prompts there while
# stdout/stderr (driver debug logs) go to the log file.
exec 3>/dev/tty
LD_LIBRARY_PATH="$LIBFP" \
G_MESSAGES_DEBUG=libfprint-egis0577 \
EGIS0577_FRAME_DUMP_DIR="$RAW_DIR" \
  stdbuf -oL -eL "$HELPER" "$OUT_DIR" "$COUNT" >"$LOG" 2>&1 &
HELPER_PID=$!
wait "$HELPER_PID"
exec 3>&-

# Rename and move accepted PGMs with session-scoped sequence numbers.
accepted=0
for pgm in "$OUT_DIR"/capture-*.pgm; do
  [[ -e "$pgm" ]] || continue
  seq=$(printf "%03d" $((accepted + 1)))
  dest="$PGM_DIR/${SESSION}-${seq}.pgm"
  cp "$pgm" "$dest"
  accepted=$((accepted + 1))
  echo "  saved: $dest"
done

rejected=$((COUNT - accepted))

# Print stage-2 accept/reject summary from driver log.
echo ""
echo "--- Stage-2 summary (from driver log) ---"
grep -E "Stage-2 (accept|reject)" "$LOG" | sed 's/^/  /' || true
echo ""
echo "Frames requested: $COUNT"
echo "Accepted:         $accepted"
echo "Rejected:         $rejected"

if [[ -n "${SUDO_UID:-}" ]]; then
  chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT_DIR" "$PGM_DIR" 2>/dev/null || true
fi

[[ $accepted -ge 1 ]]
