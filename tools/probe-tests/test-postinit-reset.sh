#!/usr/bin/env bash
# Test 2: USB reset after claim, then post-init (EH575 Rust-prototype style). The
# driver never resets the device; this checks whether a reset clears the sensor's
# saturated hot-pixel state.
#
# Run, then press and HOLD your left thumb when prompted.

source "$(dirname "$0")/_lib.sh"

DUMP_DIR="$REPO/dumps/$(date +%Y-%m-%d)-probe-postinit-reset"

probe_prep
probe_guided_capture "$DUMP_DIR" "eh575-postinit-reset"
probe_report "$DUMP_DIR"
