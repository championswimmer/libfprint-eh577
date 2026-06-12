#!/usr/bin/env bash
# Test 1 (decisive): post-init only, no pre-init, no reset — exactly the probe mode
# that produced the clean 2026-06-03 frames. Does the probe still capture a clean
# (low-saturation) finger frame TODAY with the same finger?
#
# Run, then press and HOLD your left thumb when prompted.

source "$(dirname "$0")/_lib.sh"

DUMP_DIR="$REPO/dumps/$(date +%Y-%m-%d)-probe-postinit-only"

probe_prep
probe_guided_capture "$DUMP_DIR" "eh575-postinit"
probe_report "$DUMP_DIR"
