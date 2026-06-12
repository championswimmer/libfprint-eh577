# 2026-06-03 — repeated early-state sampling

To characterize the earliest varying status bytes, the first two EH575 post-init packets were sampled repeatedly in two modes:

1. 10 runs with a fresh usbfs reset before each run
2. 6 runs without resets between runs

Artifacts created:

- `logs/2026-06-03-eh577-first2-state-samples.txt`
- `logs/2026-06-03-eh577-first2-noreset-samples.txt`
- `logs/2026-06-03-eh577-first2-state-analysis.txt`

### Observed replies in the repeated sampling series

Across both repeated series, the replies were fully stable:

- `60 00 fc` -> `53 49 47 45 00 ab 01`
- `60 01 fc` -> `53 49 47 45 01 00 01`

This is different from earlier successful captures, which had shown:

- `60 00 fc` -> `... aa 01`
- `60 01 fc` -> `... 05 01`
- and one early one-off `60 01 fc` -> `... 01 01`

### Interpretation

EH577 clearly has multiple internal reply states for the earliest status-like commands.

At this point at least three packet-1 variants have been observed across the session history:

- `01 00 01`
- `01 05 01`
- `01 01 01`

And packet-0 has at least two observed variants:

- `00 aa 01`
- `00 ab 01`

This means the driver should not assume a single fixed early-state reply. Instead it should:

- treat those bytes as stateful
- branch conservatively
- and only rely on stronger invariants such as transport framing, packet lengths, and larger behavior patterns

### Early-state variation table

| Command | Observed replies so far | Notes |
|---|---|---|
| `60 00 fc` | `... 00 aa 01`, `... 00 ab 01` | varies across session history |
| `60 01 fc` | `... 01 00 01`, `... 01 05 01`, `... 01 01 01` | most important current branch/state indicator |
| `60 00 66` | `... 00 aa 01`, `... 00 ab 01` | also stateful |
| `60 01 66` | `... 01 05 01`, `... 01 00 01` | seen in post-init vs pre-init captures |
| `60 2d 02` | `... 2d 47 01`, `... 2d c7 01` | later status variation |
| `62 67 03` | `... 67 03 01 00 00 00`, `... 67 03 01 00 ff 00` | later status variation |
