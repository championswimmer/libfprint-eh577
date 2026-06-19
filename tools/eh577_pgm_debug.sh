#!/usr/bin/env bash
# Interactive EH577 PGM-debug session.
#
# Concise terminal output only; full libfprint/driver logs go to full.log.
# Keys while running:
#   f  start saving enhanced PGM frames + metrics
#   s  stop saving
#   x  exit
#
# Output:
#   artifacts/pgm-debug/<timestamp>/pgm/frame-*.pgm
#   artifacts/pgm-debug/<timestamp>/metrics.csv
#   artifacts/pgm-debug/<timestamp>/raw/*.bin
#   artifacts/pgm-debug/<timestamp>/full.log

set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_EXAMPLES="$REPO/refs/libfprint/build/examples"
LIBFP="$REPO/refs/libfprint/build/libfprint"
DEBUG_BIN="$BUILD_EXAMPLES/eh577-pgm-debug"
INTERVAL_MS="${1:-100}"

SESSION="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO/artifacts/pgm-debug/$SESSION"
PGM_DIR="$OUT/pgm"
RAW_DIR="$OUT/raw"
LOG="$OUT/full.log"
METRICS="$OUT/metrics.csv"
CONTROL="$OUT/control.state"

mkdir -p "$PGM_DIR" "$RAW_DIR"
echo '0' >"$CONTROL"
: >"$LOG"

cleanup() {
  echo '0' >"$CONTROL" 2>/dev/null || true
  if [[ -n "${SUDO_UID:-}" ]]; then
    chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$OUT" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' TERM

free_device() {
  systemctl stop fprintd fprintd.socket 2>/dev/null || true
  pkill -f fprintd 2>/dev/null || true
  pkill -f "eh577-pgm-debug" 2>/dev/null || true

  local devnode
  devnode=$(lsusb -d 1c7a:0577 2>/dev/null \
    | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
  [[ -z "$devnode" || ! -e "$devnode" ]] && return

  for i in 1 2 3 4 5; do
    fuser "$devnode" &>/dev/null || break
    echo "Waiting for USB device to be released (${i}s)..."
    sleep 1
  done
}

if [[ ! -x "$DEBUG_BIN" ]]; then
  echo "ERROR: $DEBUG_BIN not found. Build with: cd refs/libfprint/build && ninja" >&2
  exit 1
fi

if ! lsusb | grep -q '1c7a:0577'; then
  echo "ERROR: EH577 (1c7a:0577) not found via lsusb" >&2
  exit 1
fi

free_device

cat <<EOF
=== EH577 PGM Debug [$SESSION] ===
Session : $OUT
PGMs    : $PGM_DIR
Metrics : $METRICS
Raw     : $RAW_DIR
Full log: $LOG
Interval: ${INTERVAL_MS} ms

Keys: f=start captures, s=stop captures, x=exit
EOF

# fd 3 stays connected to the terminal for concise status messages while
# stdout/stderr go to the verbose log.  Keep the debug binary in the foreground:
# it reads raw keys from stdin, and a background process is not allowed to read
# from the controlling terminal reliably (the original version missed f/s/x).
exec 3>/dev/tty
set +e
(
  cd "$REPO"
  LD_LIBRARY_PATH="$LIBFP" \
  G_MESSAGES_DEBUG=all \
  EGIS0577_FRAME_DUMP_DIR="$RAW_DIR" \
  EGIS0577_PGM_DEBUG_DIR="$PGM_DIR" \
  EGIS0577_PGM_DEBUG_LOG="$METRICS" \
  EGIS0577_PGM_DEBUG_CONTROL="$CONTROL" \
  EGIS0577_PGM_DEBUG_INTERVAL_MS="$INTERVAL_MS" \
    "$DEBUG_BIN"
) >>"$LOG" 2>&1
status=$?
set -e
exec 3>&-

pgm_count=$(find "$PGM_DIR" -maxdepth 1 -type f -name 'frame-*.pgm' 2>/dev/null | wc -l)
metric_rows=0
if [[ -f "$METRICS" ]]; then
  metric_rows=$(( $(wc -l <"$METRICS") > 0 ? $(wc -l <"$METRICS") - 1 : 0 ))
fi

echo
echo "Session complete (status=$status)"
echo "PGMs captured: $pgm_count"
echo "Metric rows : $metric_rows"
echo "Open: $OUT"
exit "$status"
