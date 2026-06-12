# Retry wedge after stricter minutiae gate: helper must ask for lift before re-capture

Date: 2026-06-12

## Context

A capture run got stuck at `touch 4/12` after the Stage-2 minutiae floor was raised to
`>=4` (later relaxed to `>=3` after this incident):

```text
EH577_CAPTURE reject minutiae=3 < 4
EH577_CAPTURE retry message=Please try again.
EH577_CAPTURE touch 4/12 — press and hold
EH577_CAPTURE reject stretch_p5=104 < 160; grain=8.990% >= 4.000%
EH577_CAPTURE retry message=Please try again.
EH577_CAPTURE touch 4/12 — press and hold
```

Run directory:

- [artifacts/capture12/20260612-223730](../../../artifacts/capture12/20260612-223730)

## What the log showed

At the end of `debug.log`, the driver was not blocked on USB. It was actively polling.
The repeating state was:

```text
finger_detected: coverage=26% intensity=163
Finger heuristic for current frame: present
Ignoring finger-like startup/transient frame ... until a clean no-finger baseline is observed
Finger remains absent (ignoring startup transient before capture is armed)
```

So the wedge was a UX/state-machine interaction, not a hard transport deadlock:

1. Stage-2 reject completed the public capture action as a retry.
2. The helper immediately started the next `fp_device_capture()`.
3. The user's finger was still physically on the sensor after the rejected touch.
4. The driver requires a clean no-finger baseline before arming a new capture.
5. Because the next capture started while the finger was still down, the driver kept
   seeing finger-like frames but intentionally ignored them as startup transients.

From the user's perspective this looked stuck at `touch 4/12`, but internally it was
looping while waiting for a clean no-finger frame that never arrived.

## Fix made

Updated [eh577-capture-helper.c](../../../refs/libfprint/examples/eh577-capture-helper.c):

- on retry, print `EH577_CAPTURE lift your finger before retry`
- wait `1500 ms`
- then start the same capture number again

This gives the driver time to observe a true lift / clean baseline before the helper
asks for another press.

## Immediate manual unwedge procedure

If the helper appears stuck in this state:

```bash
sudo pkill -f eh577-capture-helper
```

Then physically lift/remove the finger and rerun:

```bash
./tools/capture12.sh
```

If the USB node remains busy, also run:

```bash
sudo systemctl stop fprintd fprintd.socket
sudo pkill -f fprintd
sudo pkill -f eh577-capture-helper
```

Unplug/replug should only be needed if the kernel/libusb state itself is wedged.

## Current conclusion

Raising the minutiae gate made retries common enough that the helper must explicitly
enforce a lift gap between retries. The gate was later relaxed to `>=3`, but the helper
fix remains necessary because any Stage-2 retry can otherwise restart while the finger
is still down. The driver still needs a clean no-finger baseline before the next touch;
the helper now cooperates with that policy instead of immediately asking for another
press.
