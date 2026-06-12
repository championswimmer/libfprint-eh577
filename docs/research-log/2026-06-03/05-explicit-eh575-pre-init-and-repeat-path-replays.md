# 2026-06-03 — explicit EH575 pre-init and repeat path replays on EH577

After the successful post-init replay, the standalone probe was used to run the other EH575 bulk paths explicitly:

- `eh575-preinit`
- `eh575-repeat`

Artifacts created:

- `logs/2026-06-03-eh577-preinit-run.txt`
- `logs/2026-06-03-eh577-repeat-run.txt`
- `logs/2026-06-03-eh577-sequence-comparison.txt`
- `dumps/2026-06-03-preinit/`
- `dumps/2026-06-03-repeat/`

### Repeat path result

The full EH575 repeat path succeeded on EH577.

Important points:

- all 9 packets completed successfully
- `64 14 ec` again returned `5356` bytes
- the payload was again all zeros in the idle capture
- the smaller replies matched the same general EH575-compatible shape already seen in post-init

Notable replies:

- `61 2d 20` -> `53 49 47 45 2d 20 01`
- `60 00 20` -> `53 49 47 45 00 aa 01`
- `60 01 20` -> `53 49 47 45 01 05 01`
- `60 2d 02` -> `53 49 47 45 2d c7 01`
- `62 67 03` -> `53 49 47 45 67 03 01 00 ff 00`
- `64 14 ec` -> `5356` bytes, all zero in this run

### Pre-init path result

The full EH575 pre-init path also succeeded on EH577.

Important points:

- all 29 packets completed successfully
- EH577 accepted pre-init-specific commands without transport/protocol failure
- the pre-init path's `73 14 ec` command **did not** return a large payload
- instead, `73 14 ec` returned exactly 7 zero bytes

Notable replies:

- `60 00 00` -> `53 49 47 45 00 aa 01`
- `60 01 00` -> `53 49 47 45 01 05 01`
- `60 80 00` -> `53 49 47 45 80 02 01`
- `73 14 ec` -> `00 00 00 00 00 00 00`
- `60 40 ec` -> `53 49 47 45 40 00 01`
- `60 00 66` -> `53 49 47 45 00 ab 01`
- `60 01 66` -> `53 49 47 45 01 00 01`
- `60 40 66` -> `53 49 47 45 40 00 01`

### Interpretation of the new path coverage

This is an important strengthening of the EH575-family model.

EH577 now appears to accept not only the EH575 post-init path, but also:

- the EH575 pre-init path
- the EH575 repeat path

That means the main reverse-engineering question is no longer whether EH577 belongs to the EH575 protocol family; it almost certainly does.

The more precise open questions are now:

1. which path is the canonical capture path for EH577,
2. when the large payload becomes meaningful instead of zero-filled,
3. and how the variable state bytes should influence driver branching.

### Tooling note

A direct `sudo -n env EH577_DUMP_DIR=...` invocation behaved inconsistently once and triggered an unrelated-looking warning before sudo rejected the command.

Using the more explicit form below worked reliably for the pre-init replay:

```bash
sudo -n bash -lc 'export EH577_DUMP_DIR="$PWD/dumps/2026-06-03-preinit"; \
  ./build/eh577_usbfs_probe /dev/bus/usb/003/004 eh575-preinit'
```

So future scripted runs that need environment variables should prefer `sudo -n bash -lc '...'`.
