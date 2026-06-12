#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Starting Touch and Temperature Guard Validation Run..."
echo "This run will use the guided img-capture tool to collect a full cycle of idle, touch, and hold events."
echo "Please follow the prompts on screen (TOUCH, HOLD, REMOVE)."

# Ensure the library is built
echo "Rebuilding libfprint just in case..."
meson compile -C "$ROOT_DIR/refs/libfprint/build"

# Run the guided capture
"$ROOT_DIR/tools/eh577_guided_img_capture.sh" \
  --label "EH577 Guard Validation" \
  --log "$ROOT_DIR/logs/$(date +%F-%H%M%S)-guard-validation.txt" \
  --delay-before-start 3 \
  --delay-before-touch 3 \
  --hold 5 \
  --delay-after-remove 3 \
  --timeout 25

echo "Done. You can find the log path in the output above."
