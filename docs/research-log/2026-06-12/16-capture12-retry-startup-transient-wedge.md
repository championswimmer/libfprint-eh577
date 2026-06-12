# Capture12 retry startup-transient wedge: not log blocking, driver now force-arms persistent finger-like frames

Date: 2026-06-12

## Context

A 12-capture run got stuck at `touch 3/12` after two Stage-2 retries:

- [artifacts/capture12/20260612-225339](../../../artifacts/capture12/20260612-225339)

Terminal excerpt:

```text
EH577_CAPTURE touch 3/12 — press and hold
EH577_CAPTURE reject minutiae=2 < 3
EH577_CAPTURE retry message=Please try again.
EH577_CAPTURE lift your finger before retry
EH577_CAPTURE touch 3/12 — press and hold
EH577_CAPTURE reject stretch_p5=95 < 160; grain=8.718% >= 8.000%
EH577_CAPTURE retry message=Please try again.
EH577_CAPTURE lift your finger before retry
EH577_CAPTURE touch 3/12 — press and hold
```

Only captures `01` and `02` were saved.

## Was this caused by debug-log blocking?

Probably not as the primary cause.

The debug log was large:

- ~237k lines
- ~25.7 MB
- ~1505 raw dumps

but timestamps show the driver was still rapidly polling and writing frames up to the
interrupt. The log did not stop at a blocked write; the state machine kept cycling.

However, `G_MESSAGES_DEBUG=all` does create a huge amount of log output, so the capture
script now defaults to quiet logging unless `G_MESSAGES_DEBUG` is explicitly set.

## Actual driver state

The repeated state at the end was:

```text
finger_detected: coverage=29% intensity=166
Finger heuristic for current frame: present
Ignoring finger-like startup/transient frame ... until a clean no-finger baseline is observed
Finger remains absent (ignoring startup transient before capture is armed)
```

So the driver was not wedged at USB level. It was stuck in the **unarmed startup-transient
loop**:

1. Stage-2 rejected a touch.
2. The helper asked for lift and waited before retry.
3. On the next capture attempt, the driver still did not see a clean no-finger baseline.
4. It kept seeing finger-like frames while `capture_armed == FALSE`.
5. The guard kept treating those as startup transients forever.

This is a valid guard in principle, but it needs a cap.

## Fix made

The driver now tracks how long it has been seeing persistent finger-like frames while
unarmed. If this lasts more than `750ms`, it force-arms capture instead of ignoring
forever:

```text
Persistent finger-like startup frames for >750ms; arming capture to avoid retry wedge
```

This preserves the clean-baseline preference, but prevents the helper from getting
stuck at `touch N/12` when the user has already pressed and the sensor never returns to
a clean baseline.

## Script logging adjustment

[tools/capture12.sh](../../../tools/capture12.sh) now defaults to quiet logging:

- default: no `G_MESSAGES_DEBUG=all`
- warnings / reject reasons still surface
- use `G_MESSAGES_DEBUG=all ./tools/capture12.sh` only for deep debugging

This reduces log volume and makes the pipeline less likely to become a performance
factor during normal capture testing.

## Current conclusion

The immediate wedge cause was the driver's startup-transient guard lacking a timeout,
not the log file size. The fix is to force-arm after persistent unarmed finger-like
frames. The log volume was still excessive, so the script now avoids the debug firehose
by default.
