#!/usr/bin/env bash
# Test 3: pre-init (29) THEN post-init (18) — mimics the driver's order, which the
# clean post-init-only probe runs skip. Checks whether running pre-init first is
# what pushes the sensor into the saturated state.
#
# Run, then press and HOLD your left thumb when prompted (the pre-init arming step
# runs first with no finger needed, then you'll be prompted for the capture).

source "$(dirname "$0")/_lib.sh"

DUMP_DIR="$REPO/dumps/$(date +%Y-%m-%d)-probe-preinit-then-postinit"

probe_prep
probe_preinit "$DUMP_DIR"
probe_guided_capture "$DUMP_DIR" "eh575-postinit"
probe_report "$DUMP_DIR"
