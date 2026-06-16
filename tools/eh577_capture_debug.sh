#!/usr/bin/env bash
# Diagnostic capture run: launches the EH577 capture helper DIRECTLY (no output
# classifier) with full libfprint debug, sending BOTH stdout and stderr to one
# log so we can see exactly where it stops — device open, first pre-init, frame
# reads, etc. Use this when capture12.sh "does nothing" (no touch prompt / no
# frames), which usually means open never completed.
#
# Run as:
#   ./tools/eh577_capture_debug.sh [count]      (default 2)
#
# It prints the log path at the end — share that file.

set -uo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

# Resolve repo paths relative to this script (works regardless of CWD).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
HELPER="$REPO/refs/libfprint/build/examples/eh577-capture-helper"
COUNT="${1:-2}"
SESSION="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO/artifacts/capture-runs/$SESSION"
LOG="$OUT/full.log"
mkdir -p "$OUT/raw"

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
  echo "EH577 (1c7a:0577) not on USB — physically replug it."; exit 1
fi

# Free the device and wait until the USB interface is no longer held.
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

echo "Which libfprint-2.so the helper will load:"
LD_LIBRARY_PATH="$LIBFP" ldd "$HELPER" | grep -i fprint | sed 's/^/  /'
echo "Log: $LOG"
echo "Running helper directly (full debug). Press the finger when prompted to 'touch N'."
echo "----------------------------------------------------------------------"

# Full driver debug, no classifier.  stdout/stderr go to the log file only;
# fd 3 is wired to the terminal so the helper's prompts still reach the user.
exec 3>/dev/tty
LD_LIBRARY_PATH="$LIBFP" \
G_MESSAGES_DEBUG=all \
EGIS0577_FRAME_DUMP_DIR="$OUT/raw" \
  stdbuf -oL -eL "$HELPER" "$OUT" "$COUNT" >"$LOG" 2>&1 &
HELPER_PID=$!
wait "$HELPER_PID"
status=$?
exec 3>&-

echo "----------------------------------------------------------------------"
echo "helper exit status: $status"

# Hand artifacts back to the invoking (sudo) user.
if [[ -n "${SUDO_UID:-}" ]]; then
  chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT" 2>/dev/null || true
fi

echo "Raw frames dumped: $(ls "$OUT/raw" 2>/dev/null | wc -l)"
echo "PGMs accepted:    $(ls "$OUT"/*.pgm 2>/dev/null | wc -l) / $COUNT"
echo "Share this log:  $LOG"
