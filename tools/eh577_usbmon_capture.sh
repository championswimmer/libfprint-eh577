#!/usr/bin/env bash
set -euo pipefail

USBMON_IFACE="usbmon3"
PCAP_PATH=""
TCPDUMP_LOG_PATH=""
STARTUP_DELAY_MS=500
TCPDUMP_PID=""
CMD_RC=0

usage() {
  cat <<'EOF'
Usage:
  eh577_usbmon_capture.sh [options] -- <command...>

Wrap a capture command in a usbmon tcpdump session and stop tcpdump cleanly
when the command exits.

Options:
  --iface NAME          usbmon interface (default: usbmon3)
  --pcap PATH           output .pcap path (required)
  --tcpdump-log PATH    stderr log path for tcpdump (required)
  --startup-delay-ms N  wait after starting tcpdump before launching command
                        (default: 500)
  -h, --help            Show this help

Example:
  ./tools/eh577_usbmon_capture.sh \
    --iface usbmon3 \
    --pcap "$PWD/logs/runtime.pcap" \
    --tcpdump-log "$PWD/logs/runtime-tcpdump.txt" \
    -- ./tools/eh577_guided_img_capture.sh --log "$PWD/logs/runtime.txt"
EOF
}

is_nonneg_int() {
  [[ ${1:-} =~ ^[0-9]+$ ]]
}

cleanup() {
  if [[ -n "$TCPDUMP_PID" ]] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
    kill -INT "$TCPDUMP_PID" >/dev/null 2>&1 || true
    wait "$TCPDUMP_PID" 2>/dev/null || true
  fi
}

while (($#)); do
  case "$1" in
    --iface)
      USBMON_IFACE="$2"
      shift 2
      ;;
    --pcap)
      PCAP_PATH="$2"
      shift 2
      ;;
    --tcpdump-log)
      TCPDUMP_LOG_PATH="$2"
      shift 2
      ;;
    --startup-delay-ms)
      is_nonneg_int "${2:-}" || { echo "invalid value for --startup-delay-ms" >&2; exit 2; }
      STARTUP_DELAY_MS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$PCAP_PATH" || -z "$TCPDUMP_LOG_PATH" ]]; then
  echo "--pcap and --tcpdump-log are required" >&2
  exit 2
fi

if (($# == 0)); then
  echo "missing capture command after --" >&2
  exit 2
fi

if ! command -v tcpdump >/dev/null 2>&1; then
  echo "tcpdump is required" >&2
  exit 3
fi

mkdir -p "$(dirname "$PCAP_PATH")" "$(dirname "$TCPDUMP_LOG_PATH")"
rm -f "$PCAP_PATH" "$TCPDUMP_LOG_PATH"

trap cleanup EXIT

if [[ -t 0 && -t 1 ]]; then
  sudo tcpdump -i "$USBMON_IFACE" -s 0 -U -w "$PCAP_PATH" \
    2>"$TCPDUMP_LOG_PATH" &
else
  sudo -n tcpdump -i "$USBMON_IFACE" -s 0 -U -w "$PCAP_PATH" \
    2>"$TCPDUMP_LOG_PATH" &
fi
TCPDUMP_PID=$!

sleep "$(python3 - <<'PY' "$STARTUP_DELAY_MS"
import sys
print(int(sys.argv[1]) / 1000)
PY
)"

if ! kill -0 "$TCPDUMP_PID" 2>/dev/null; then
  echo "tcpdump failed to stay up; see $TCPDUMP_LOG_PATH" >&2
  wait "$TCPDUMP_PID" || true
  exit 5
fi

if "$@"; then
  CMD_RC=0
else
  CMD_RC=$?
fi

cleanup
TCPDUMP_PID=""

exit "$CMD_RC"
