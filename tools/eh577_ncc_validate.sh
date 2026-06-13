#!/usr/bin/env bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
BUILD_EXAMPLES="$REPO/refs/libfprint/build/examples"
ENROLL_BIN="$BUILD_EXAMPLES/eh577-enroll-helper"
VERIFY_BIN="$BUILD_EXAMPLES/eh577-verify-helper"
IDENTIFY_BIN="$BUILD_EXAMPLES/eh577-identify-helper"
CLEAR_BIN="$BUILD_EXAMPLES/clear-storage"
SESSION="$(date +%Y%m%d-%H%M%S)"
EVIDENCE_DIR="$REPO/artifacts/ncc-validation/$SESSION"
RAW_DIR="$EVIDENCE_DIR/raw"
LOG="$EVIDENCE_DIR/session.log"
mkdir -p "$RAW_DIR"
exec 3>> "$LOG"

log() { echo -e "$*" >&3; }
say() { echo -e "$*"; }

FINGER_NAMES=(
  "left thumb"   "left index"   "left middle"  "left ring"   "left little"
  "right thumb"  "right index"  "right middle" "right ring"  "right little"
)

need_bin() {
  [[ -x "$1" ]] || { echo "ERROR: missing binary $1"; exit 1; }
}

chown_artifacts() {
  sudo chown -R "$(id -u):$(id -g)" "$EVIDENCE_DIR" "$REPO/test-storage.variant" 2>/dev/null || true
}

stop_fprintd() {
  sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo pkill -f fprintd 2>/dev/null || true
}

run_clear_storage() {
  say "Clearing saved prints..."
  printf 'Y\n' | sudo sh -c "cd '$REPO' && exec env LD_LIBRARY_PATH='$LIBFP' '$CLEAR_BIN'" >>"$LOG" 2>&1 || true
  rm -f "$REPO/test-storage.variant"
}

run_enroll() {
  local finger_index="$1"
  local label="$2"
  local out_image="$EVIDENCE_DIR/enroll-${label}.pgm"
  local out_log="$EVIDENCE_DIR/enroll-${label}.out"
  local raw_sub="$RAW_DIR/enroll-${label}"
  mkdir -p "$raw_sub"

  say ""
  say "=== ENROLL: ${FINGER_NAMES[$finger_index]} (${label}) ==="
  say "You will need 7 successful press captures. Lift between presses."
  say "Press Enter when ready..."
  read -r _

  sudo sh -c "cd '$REPO' && rm -f '$out_image' && exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print EGIS0577_FRAME_DUMP_DIR='$raw_sub' '$ENROLL_BIN' --finger-index '$finger_index' --save-image '$out_image'" \
    >"$out_log" 2>>"$LOG"

  chown_artifacts
  say "Enroll helper output saved: $out_log"
  say "Enroll image saved:         $out_image"
  grep -E 'EH577_HELPER (enroll-stage|enroll-complete|enroll-retry|device-opened)' "$out_log" || true
}

run_verify() {
  local finger_index="$1"
  local scan_label="$2"
  local out_image="$EVIDENCE_DIR/verify-${scan_label}.pgm"
  local out_log="$EVIDENCE_DIR/verify-${scan_label}.out"
  local raw_sub="$RAW_DIR/verify-${scan_label}"
  mkdir -p "$raw_sub"

  say ""
  say "=== VERIFY against enrolled ${FINGER_NAMES[$finger_index]} ; now scan: ${scan_label} ==="
  say "Press the requested finger once and hold still until capture completes."
  say "Press Enter when ready..."
  read -r _

  sudo sh -c "cd '$REPO' && rm -f '$out_image' && exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print EGIS0577_FRAME_DUMP_DIR='$raw_sub' '$VERIFY_BIN' --finger-index '$finger_index' --save-image '$out_image'" \
    >"$out_log" 2>>"$LOG"

  chown_artifacts
  say "Verify helper output saved: $out_log"
  say "Verify image saved:         $out_image"
  grep -E 'EH577_VERIFY (verify-result|verify-complete|verify-retry|device-opened|verify-start)' "$out_log" || true
}

run_identify() {
  local scan_label="$1"
  local out_image="$EVIDENCE_DIR/identify-${scan_label}.pgm"
  local out_log="$EVIDENCE_DIR/identify-${scan_label}.out"
  local raw_sub="$RAW_DIR/identify-${scan_label}"
  mkdir -p "$raw_sub"

  say ""
  say "=== IDENTIFY scan: ${scan_label} ==="
  say "Press the requested finger once and hold still until capture completes."
  say "Press Enter when ready..."
  read -r _

  sudo sh -c "cd '$REPO' && rm -f '$out_image' && exec env LD_LIBRARY_PATH='$LIBFP' G_MESSAGES_DEBUG=libfprint,libfprint-egis0577,libfprint-print EGIS0577_FRAME_DUMP_DIR='$raw_sub' '$IDENTIFY_BIN' --save-image '$out_image'" \
    >"$out_log" 2>>"$LOG"

  chown_artifacts
  say "Identify helper output saved: $out_log"
  say "Identify image saved:         $out_image"
  grep -E 'EH577_IDENTIFY (identify-result|identify-complete|identify-retry|gallery-loaded|device-opened)' "$out_log" || true
}

main() {
  need_bin "$ENROLL_BIN"
  need_bin "$VERIFY_BIN"
  need_bin "$IDENTIFY_BIN"
  need_bin "$CLEAR_BIN"

  say "Evidence dir: $EVIDENCE_DIR"
  log "=== EH577 NCC validation session $SESSION ==="

  if ! lsusb | grep -q '1c7a:0577'; then
    say "ERROR: EH577 not found via lsusb"
    exit 1
  fi

  say "Choose enrolled finger A index (0-9):"
  for i in "${!FINGER_NAMES[@]}"; do echo "  [$i] ${FINGER_NAMES[$i]}"; done
  read -r A_INDEX
  [[ "$A_INDEX" =~ ^[0-9]$ ]] || { say "Bad A index"; exit 1; }

  say "Choose different finger B index (0-9):"
  for i in "${!FINGER_NAMES[@]}"; do echo "  [$i] ${FINGER_NAMES[$i]}"; done
  read -r B_INDEX
  [[ "$B_INDEX" =~ ^[0-9]$ ]] || { say "Bad B index"; exit 1; }
  [[ "$B_INDEX" != "$A_INDEX" ]] || { say "B must differ from A"; exit 1; }

  say "Choose optional non-enrolled finger C index (0-9), or blank to skip final no-match identify test:"
  for i in "${!FINGER_NAMES[@]}"; do echo "  [$i] ${FINGER_NAMES[$i]}"; done
  read -r C_INDEX || true
  if [[ -n "$C_INDEX" ]]; then
    [[ "$C_INDEX" =~ ^[0-9]$ ]] || { say "Bad C index"; exit 1; }
    [[ "$C_INDEX" != "$A_INDEX" && "$C_INDEX" != "$B_INDEX" ]] || { say "C must differ from A and B"; exit 1; }
  fi

  stop_fprintd
  run_clear_storage

  run_enroll "$A_INDEX" "A"
  cp -f "$REPO/test-storage.variant" "$EVIDENCE_DIR/storage-after-A.variant"

  say "\nSTEP: verify SAME finger as A. Touch ${FINGER_NAMES[$A_INDEX]} when prompted."
  run_verify "$A_INDEX" "same-A"

  say "\nSTEP: verify DIFFERENT finger against enrolled A. Touch ${FINGER_NAMES[$B_INDEX]} when prompted."
  run_verify "$A_INDEX" "different-B-vs-A"

  say "\nSTEP: enroll finger B so identify has a 2-print gallery. Touch ${FINGER_NAMES[$B_INDEX]} for enrollment."
  run_enroll "$B_INDEX" "B"
  cp -f "$REPO/test-storage.variant" "$EVIDENCE_DIR/storage-after-A-plus-B.variant"

  say "\nSTEP: identify finger A from [A,B] gallery. Touch ${FINGER_NAMES[$A_INDEX]}."
  run_identify "A"

  say "\nSTEP: identify finger B from [A,B] gallery. Touch ${FINGER_NAMES[$B_INDEX]}."
  run_identify "B"

  if [[ -n "$C_INDEX" ]]; then
    say "\nSTEP: identify non-enrolled finger C from [A,B] gallery. Touch ${FINGER_NAMES[$C_INDEX]}."
    run_identify "C-no-match"
  fi

  say ""
  say "=== SESSION COMPLETE ==="
  say "Evidence dir: $EVIDENCE_DIR"
  say "Main log:      $LOG"
  say "Artifacts:"
  find "$EVIDENCE_DIR" -maxdepth 2 -type f | sort
}

main "$@"
