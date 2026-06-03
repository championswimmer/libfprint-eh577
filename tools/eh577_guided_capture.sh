#!/usr/bin/env bash
set -euo pipefail

DELAY_BEFORE_START=5
DELAY_BEFORE_TOUCH=2
HOLD_SECONDS=8
DELAY_AFTER_REMOVE=2
CYCLES=1
LABEL="EH577 guided capture"
SUDO_UPFRONT_MODE="auto"
SUDO_KEEPALIVE_PID=""

usage() {
  cat <<'EOF'
Usage:
  eh577_guided_capture.sh [options] -- <capture command...>
  eh577_guided_capture.sh [options]

This helper decouples finger-timing prompts from the agent loop.
It can optionally launch a capture command in the background, then prints
clear "touch / hold / remove" cues at scheduled times.

If the capture command starts with `sudo`, the script will by default request
sudo credentials *up front* before any delays begin, and keep them alive for
the duration of the run so the capture is not interrupted mid-sequence.

Options:
  --delay-before-start N   Seconds before starting the capture command (default: 5)
  --delay-before-touch N   Seconds to wait after command start before first touch (default: 2)
  --hold N                 Seconds to keep finger on sensor per cycle (default: 8)
  --delay-after-remove N   Seconds to wait after remove before next cycle/end (default: 2)
  --cycles N               Number of touch/hold/remove cycles (default: 1)
  --label TEXT             Label to print in the session banner
  --sudo-upfront           Force a sudo credential check before any delays/cues
  --no-sudo-upfront        Never do an upfront sudo credential check
  -h, --help               Show this help

Examples:
  # Interrupt polling with two touch/remove cycles
  ./tools/eh577_guided_capture.sh \
    --delay-before-start 3 \
    --delay-before-touch 2 \
    --hold 4 \
    --delay-after-remove 2 \
    --cycles 2 \
    -- sudo -n ./build/eh577_usbfs_probe /dev/bus/usb/003/004 poll-int 40

  # Repeat-path payload capture with immediate finger hold
  ./tools/eh577_guided_capture.sh \
    --delay-before-start 3 \
    --delay-before-touch 0 \
    --hold 10 \
    --cycles 1 \
    -- sudo -n bash -lc 'export EH577_DUMP_DIR="$PWD/dumps/run-name"; ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-repeat'
EOF
}

log() {
  printf '[%(%H:%M:%S)T] %s\n' -1 "$*"
}

countdown() {
  local seconds=$1
  local message=$2
  while (( seconds > 0 )); do
    log "$message in ${seconds}s"
    sleep 1
    ((seconds--))
  done
}

is_nonneg_int() {
  [[ ${1:-} =~ ^[0-9]+$ ]]
}

command_needs_sudo() {
  (( ${#COMMAND[@]} > 0 )) || return 1
  [[ ${COMMAND[0]} == sudo ]]
}

cleanup() {
  if [[ -n "$SUDO_KEEPALIVE_PID" ]]; then
    kill "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
    wait "$SUDO_KEEPALIVE_PID" 2>/dev/null || true
  fi
}

acquire_sudo_upfront() {
  log "requesting sudo credentials up front"
  sudo -v

  (
    while true; do
      sleep 15
      sudo -n -v >/dev/null 2>&1 || exit 0
    done
  ) &
  SUDO_KEEPALIVE_PID=$!
}

while (($#)); do
  case "$1" in
    --delay-before-start)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-before-start" >&2; exit 2; }
      DELAY_BEFORE_START=$2
      shift 2
      ;;
    --delay-before-touch)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-before-touch" >&2; exit 2; }
      DELAY_BEFORE_TOUCH=$2
      shift 2
      ;;
    --hold)
      is_nonneg_int "${2:-}" || { echo "invalid value for --hold" >&2; exit 2; }
      HOLD_SECONDS=$2
      shift 2
      ;;
    --delay-after-remove)
      is_nonneg_int "${2:-}" || { echo "invalid value for --delay-after-remove" >&2; exit 2; }
      DELAY_AFTER_REMOVE=$2
      shift 2
      ;;
    --cycles)
      is_nonneg_int "${2:-}" || { echo "invalid value for --cycles" >&2; exit 2; }
      CYCLES=$2
      shift 2
      ;;
    --label)
      LABEL=${2:-}
      shift 2
      ;;
    --sudo-upfront)
      SUDO_UPFRONT_MODE="always"
      shift
      ;;
    --no-sudo-upfront)
      SUDO_UPFRONT_MODE="never"
      shift
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

if (( CYCLES < 1 )); then
  echo "--cycles must be >= 1" >&2
  exit 2
fi

COMMAND=("$@")
CMD_PID=""
CMD_RC=0

trap cleanup EXIT

log "=== ${LABEL} ==="
log "Plan: start-delay=${DELAY_BEFORE_START}s touch-delay=${DELAY_BEFORE_TOUCH}s hold=${HOLD_SECONDS}s remove-gap=${DELAY_AFTER_REMOVE}s cycles=${CYCLES}"

if (( ${#COMMAND[@]} > 0 )); then
  log "Command: ${COMMAND[*]}"
else
  log "Command: (none; cue-only mode)"
fi

if [[ $SUDO_UPFRONT_MODE == always ]] || { [[ $SUDO_UPFRONT_MODE == auto ]] && command_needs_sudo; }; then
  acquire_sudo_upfront
fi

if (( DELAY_BEFORE_START > 0 )); then
  countdown "$DELAY_BEFORE_START" "capture starts"
fi

if (( ${#COMMAND[@]} > 0 )); then
  log "starting capture command"
  "${COMMAND[@]}" &
  CMD_PID=$!
fi

if (( DELAY_BEFORE_TOUCH > 0 )); then
  log "keep finger OFF sensor"
  sleep "$DELAY_BEFORE_TOUCH"
fi

for ((cycle=1; cycle<=CYCLES; cycle++)); do
  log "CYCLE ${cycle}/${CYCLES}: TOUCH sensor now"
  if (( HOLD_SECONDS > 0 )); then
    sleep "$HOLD_SECONDS"
  fi
  log "CYCLE ${cycle}/${CYCLES}: REMOVE finger now"

  if (( cycle < CYCLES )) && (( DELAY_AFTER_REMOVE > 0 )); then
    sleep "$DELAY_AFTER_REMOVE"
    log "prepare for next cycle"
  fi
done

if (( DELAY_AFTER_REMOVE > 0 )); then
  sleep "$DELAY_AFTER_REMOVE"
fi

log "cue sequence finished"

if [[ -n "$CMD_PID" ]]; then
  if kill -0 "$CMD_PID" 2>/dev/null; then
    log "waiting for capture command to finish"
  else
    log "capture command already exited before cues finished"
  fi

  if wait "$CMD_PID"; then
    CMD_RC=0
    log "capture command finished successfully"
  else
    CMD_RC=$?
    log "capture command exited with status ${CMD_RC}"
  fi
fi

exit "$CMD_RC"
