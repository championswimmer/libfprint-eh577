#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUIDED_HELPER="$ROOT_DIR/tools/eh577_guided_capture.sh"
IMG_CAPTURE="$ROOT_DIR/refs/libfprint/build/examples/img-capture"
LIBFP_DIR="$ROOT_DIR/refs/libfprint/build/libfprint"

DELAY_BEFORE_START=3
DELAY_BEFORE_TOUCH=0
HOLD_SECONDS=10
DELAY_AFTER_REMOVE=2
CYCLES=1
TIMEOUT_SECONDS=20
LABEL="EH577 img-capture guided touch"
LOG_PATH="$ROOT_DIR/logs/$(date +%F)-img-capture-guided-touch.txt"
FRAME_DUMP_DIR=""

usage() {
  cat <<EOF
Usage:
  $(basename "$0") [options]

Options:
  --log PATH               Log file path (default: $LOG_PATH)
  --delay-before-start N   Seconds before starting capture (default: $DELAY_BEFORE_START)
  --delay-before-touch N   Seconds after start before TOUCH cue (default: $DELAY_BEFORE_TOUCH)
  --hold N                 Seconds to keep finger on sensor (default: $HOLD_SECONDS)
  --delay-after-remove N   Seconds after REMOVE cue (default: $DELAY_AFTER_REMOVE)
  --cycles N               Number of touch/remove cycles (default: $CYCLES)
  --timeout N              img-capture timeout in seconds (default: $TIMEOUT_SECONDS)
  --label TEXT             Session label
  --frame-dump-dir PATH    Dump raw 5356-byte runtime frames to PATH
  -h, --help               Show this help
EOF
}

is_nonneg_int() {
  [[ ${1:-} =~ ^[0-9]+$ ]]
}

find_eh577_sysfs() {
  local d
  for d in /sys/bus/usb/devices/*; do
    [[ -f "$d/idVendor" && -f "$d/idProduct" ]] || continue
    if [[ "$(<"$d/idVendor")" == "1c7a" && "$(<"$d/idProduct")" == "0577" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
  done
  return 1
}

while (($#)); do
  case "$1" in
    --log)
      LOG_PATH="$2"
      shift 2
      ;;
    --delay-before-start)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-before-start" >&2; exit 2; }
      DELAY_BEFORE_START="$2"
      shift 2
      ;;
    --delay-before-touch)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-before-touch" >&2; exit 2; }
      DELAY_BEFORE_TOUCH="$2"
      shift 2
      ;;
    --hold)
      is_nonneg_int "${2:-}" || { echo "invalid value for --hold" >&2; exit 2; }
      HOLD_SECONDS="$2"
      shift 2
      ;;
    --delay-after-remove)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-after-remove" >&2; exit 2; }
      DELAY_AFTER_REMOVE="$2"
      shift 2
      ;;
    --cycles)
      is_nonneg_int "${2:-}" || { echo "invalid value for --cycles" >&2; exit 2; }
      CYCLES="$2"
      shift 2
      ;;
    --timeout)
      is_nonneg_int "${2:-}" || { echo "invalid value for --timeout" >&2; exit 2; }
      TIMEOUT_SECONDS="$2"
      shift 2
      ;;
    --label)
      LABEL="$2"
      shift 2
      ;;
    --frame-dump-dir)
      FRAME_DUMP_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

mkdir -p "$(dirname "$LOG_PATH")"

if [[ ! -x "$GUIDED_HELPER" ]]; then
  echo "missing guided helper: $GUIDED_HELPER" >&2
  exit 3
fi

if [[ ! -x "$IMG_CAPTURE" ]]; then
  echo "missing img-capture binary: $IMG_CAPTURE" >&2
  exit 3
fi

if ! command -v lsusb >/dev/null 2>&1; then
  echo "lsusb is required for EH577 preflight" >&2
  exit 3
fi

if ! command -v timeout >/dev/null 2>&1; then
  echo "timeout is required for guided img-capture runs" >&2
  exit 3
fi

if ! lsusb | grep -q '1c7a:0577'; then
  echo "EH577 not present on USB bus; aborting before guided run" >&2
  exit 4
fi

if ! EH577_SYSFS="$(find_eh577_sysfs)"; then
  echo "EH577 not found in sysfs; aborting before guided run" >&2
  exit 4
fi

echo "Preflight OK: EH577 present"
echo "  sysfs: $EH577_SYSFS"
for f in busnum devnum product speed devpath; do
  [[ -f "$EH577_SYSFS/$f" ]] && printf '  %s: %s\n' "$f" "$(<"$EH577_SYSFS/$f")"
done

if [[ -x "$ROOT_DIR/build/eh577_libusb_smoketest" ]]; then
  echo "Running live libusb preflight..."
  if ! sudo -n "$ROOT_DIR/build/eh577_libusb_smoketest" >/dev/null 2>&1; then
    echo "Live libusb preflight failed; aborting before guided run" >&2
    exit 5
  fi
  echo "Live libusb preflight OK"
fi

echo "Log: $LOG_PATH"

dump_env=""
if [[ -n "$FRAME_DUMP_DIR" ]]; then
  mkdir -p "$FRAME_DUMP_DIR"
  dump_env="EGIS0577_FRAME_DUMP_DIR=\"$FRAME_DUMP_DIR\" "
fi

action_cmd="cd \"$ROOT_DIR\" && sudo -n sh -c '${dump_env}LD_LIBRARY_PATH=\"$LIBFP_DIR\" G_MESSAGES_DEBUG=all timeout --foreground ${TIMEOUT_SECONDS}s \"$IMG_CAPTURE\"' > \"$LOG_PATH\" 2>&1"

set +e
"$GUIDED_HELPER" \
  --sudo-upfront \
  --delay-before-start "$DELAY_BEFORE_START" \
  --delay-before-touch "$DELAY_BEFORE_TOUCH" \
  --hold "$HOLD_SECONDS" \
  --delay-after-remove "$DELAY_AFTER_REMOVE" \
  --cycles "$CYCLES" \
  --label "$LABEL" \
  -- bash -lc "$action_cmd"
RC=$?
set -e

echo "Log saved to: $LOG_PATH"

if [[ -f "$LOG_PATH" ]]; then
  if (( RC != 0 )); then
    echo "--- last 80 log lines ---"
    tail -n 80 "$LOG_PATH" || true
  else
    echo "--- last 40 log lines ---"
    tail -n 40 "$LOG_PATH" || true
  fi
fi

exit "$RC"
