---
name: planning
description: Use when creating, updating, or executing work plans for this repository. Defines the numbered plan file format under .agents/plans, the internal checklist style, and how plans should be maintained as the project todo list.
---

# Planning

Plans for this repository live under `.agents/plans/`.

## Naming

Every plan file uses a two-digit numeric prefix followed by a short slug:

- `00-agent-workspace-setup.md`
- `01-finger-interaction-captures.md`
- `02-libfprint-driver-bringup.md`

Use zero-padded numbers and keep the filename stable once work has started. Create a new numbered plan instead of renaming an active one.

## Plan file shape

Every plan should contain these sections:

1. `# Title`
2. `## Goal`
3. `## Context`
4. `## Steps`
5. `## Validation`
6. `## Todo`

The `Todo` section is mandatory. It is the live execution checklist.

## Todo format

Use Markdown checkboxes:

- `[ ]` not started
- `[x]` done

Keep checklist items concrete and observable. Bad item: `work on driver`. Better item: `capture post-init dump while finger is held on sensor`.

## Execution rules

1. Read the relevant existing plan before starting work.
2. Update the checklist as work advances, not only at the end.
3. If scope changes, revise the plan in place and add new checklist items.
4. Record blockers explicitly instead of silently abandoning items.
5. Prefer one active plan per focused stream of work.

## Numbering rules

- Start at `00-...`
- Increment by one for new plans unless there is a deliberate reason to reserve a slot
- Do not reuse old numbers for unrelated work

## Minimal template

```md
# Short Plan Title

## Goal

One paragraph describing the concrete outcome.

## Context

- Relevant files
- Device or environment constraints
- Assumptions already validated

## Steps

1. First action
2. Second action
3. Third action

## Validation

- What command, build, test, or artifact proves the work is done

## Todo

- [ ] First concrete task
- [ ] Second concrete task
- [ ] Validation captured
```

## Repository-specific expectation

For EH577 work, plans should usually say which of these the work touches:

- live probe runs under `tools/` and `build/`
- research notes under `docs/` and `logs/`
- raw captures under `dumps/`
- driver work under `wip-libfprint/` and `refs/libfprint/`
