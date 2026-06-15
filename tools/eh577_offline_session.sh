#!/usr/bin/env bash
# Single-session offline enroll + identify loop.
#
# Runs entirely against our own built driver — no system fprintd.
# One finger enrolled, then repeated identify until user types 'q'.
#
# Usage: sudo ./tools/eh577_offline_session.sh
#
# Session artifacts land in artifacts/sessions/SESS/ and driver debug
# goes to artifacts/sessions/SESS/session.log via G_MESSAGES_DEBUG.

set -uo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Must run as root:  sudo $0"
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
ENROLL_BIN="$REPO/refs/libfprint/build/examples/eh577-enroll-helper"
IDENTIFY_BIN="$REPO/refs/libfprint/build/examples/eh577-identify-helper"
SESSION="$(date +%Y%m%d-%H%M%S)"
SESS_DIR="$REPO/artifacts/sessions/$SESSION"
LOG="$SESS_DIR/session.log"

for bin in "$ENROLL_BIN" "$IDENTIFY_BIN"; do
  if [[ ! -x "$bin" ]]; then
    echo "Not built: $bin"; exit 1
  fi
done

if ! lsusb | grep -q '1c7a:0577'; then
  echo "EH577 (1c7a:0577) not on USB — replug it."; exit 1
fi

mkdir -p "$SESS_DIR"

systemctl stop fprintd fprintd.socket 2>/dev/null || true
pkill -f fprintd 2>/dev/null || true

echo "Session: $SESSION"
echo "Log:     $LOG"
echo ""

FINGER_NAMES=(
  "left-thumb"  "left-index"  "left-middle"  "left-ring"  "left-little"
  "right-thumb" "right-index" "right-middle" "right-ring" "right-little"
)

# --- enroll phase ---

echo "Pick a finger to enroll:"
echo "  [0] left thumb    [5] right thumb"
echo "  [1] left index    [6] right index"
echo "  [2] left middle   [7] right middle"
echo "  [3] left ring     [8] right ring"
echo "  [4] left little   [9] right little"
read -rp "> " finger_idx

if [[ ! "$finger_idx" =~ ^[0-9]$ ]]; then
  echo "Invalid finger index — must be 0-9"; exit 1
fi

finger_name="${FINGER_NAMES[$finger_idx]}"
echo ""
echo "Enrolling: $finger_name"
echo "Touch the sensor when prompted. Lift and re-touch for each stage."
echo ""

# Run enroll helper in SESS_DIR so test-storage.variant lands there.
# Use process substitution to keep the main shell's stdin for later reads.
while IFS= read -r line; do
  case "$line" in
    EH577_HELPER\ device-opened*)
      echo "  Device opened, starting enroll..."
      ;;
    EH577_HELPER\ enroll-stage\ completed=*\ total=*)
      completed=$(echo "$line" | grep -oP 'completed=\K[0-9]+')
      total=$(echo "$line" | grep -oP 'total=\K[0-9]+')
      echo "  Stage $completed/$total captured"
      ;;
    EH577_HELPER\ enroll-retry*)
      code=$(echo "$line" | grep -oP 'code=\K\S+')
      echo "  Retry ($code) — lift and try again"
      ;;
    EH577_HELPER\ enroll-complete*)
      echo "  $line" | sed 's/EH577_HELPER enroll-complete/Enroll result:/'
      ;;
    EH577_HELPER\ image-saved*)
      path=$(echo "$line" | grep -oP 'path=\K\S+')
      echo "  Stage image: $path"
      ;;
  esac
  echo "[enroll] $line" >> "$LOG"
done < <(
  cd "$SESS_DIR"
  LD_LIBRARY_PATH="$LIBFP" \
  G_MESSAGES_DEBUG=libfprint-egis0577 \
  EGIS0577_FRAME_DUMP_DIR="$SESS_DIR/raw-enroll" \
    "$ENROLL_BIN" --finger-index "$finger_idx" \
                  --save-image "$SESS_DIR/enroll-${finger_name}.pgm" \
    2>>"$LOG"
)

# test-storage.variant is created by the helper in its CWD = $SESS_DIR
if [[ ! -f "$SESS_DIR/test-storage.variant" ]]; then
  echo ""
  echo "Enroll failed (no storage variant created) — see log: $LOG"
  exit 1
fi
echo ""
echo "Enrolled: $finger_name"

# --- identify loop ---

matches=0
attempts=0

echo ""
echo "--- Identify phase ---"
echo "Touch the sensor when prompted. Type 'q' to quit."
echo ""

while true; do
  read -rp "Touch finger (or 'q' to quit): " input
  [[ "$input" =~ ^[qQ]$ ]] && break

  ((attempts++))
  attempt_pad=$(printf "%03d" $attempts)
  raw_dir="$SESS_DIR/raw-identify-${attempt_pad}"
  identify_pgm="$SESS_DIR/identify-${attempt_pad}.pgm"

  # Run identify helper; capture structured output lines to a temp file.
  ident_out=$(mktemp)
  (
    cd "$SESS_DIR"
    LD_LIBRARY_PATH="$LIBFP" \
    G_MESSAGES_DEBUG=libfprint-egis0577 \
    EGIS0577_FRAME_DUMP_DIR="$raw_dir" \
      "$IDENTIFY_BIN" --save-image "$identify_pgm" \
      2>>"$LOG"
  ) | tee -a "$LOG" | grep "^EH577_IDENTIFY" > "$ident_out" || true

  result_line=$(grep "identify-complete" "$ident_out" | tail -1 || true)
  rm -f "$ident_out"

  if echo "$result_line" | grep -q "result=match"; then
    matched_finger=$(echo "$result_line" | grep -oP 'finger=\K\S+' || true)
    echo "  MATCH (${matched_finger:-unknown})"
    ((matches++))
  elif echo "$result_line" | grep -q "result=no-match"; then
    echo "  NO MATCH"
  else
    echo "  No result — check log: $LOG"
  fi
done

# --- summary ---

echo ""
echo "--- Session summary ---"
echo "Finger enrolled: $finger_name"
echo "Identify attempts: $attempts"
echo "Matches:           $matches"
echo "No-matches:        $((attempts - matches))"
echo "Session log:       $LOG"
echo "Session dir:       $SESS_DIR"

if [[ -n "${SUDO_UID:-}" ]]; then
  chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$SESS_DIR" 2>/dev/null || true
fi
