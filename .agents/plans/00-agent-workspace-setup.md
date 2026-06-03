# Agent Workspace Setup

## Goal

Create an in-repo agent workspace under `.agents/` with reusable EH577-focused skills and a numbered plan system that doubles as the project todo list.

## Context

- The repository already contains device notes in `AGENTS.md`.
- Shell writes into `.agents/` are blocked by the current environment, but `apply_patch` can populate it.
- The immediate need is reusable agent guidance for probing, libfprint porting, and planning.

## Steps

1. Create `.agents/skills/` with the EH577 probe/test skill.
2. Create `.agents/skills/` with the libfprint patch/build skill.
3. Create `.agents/skills/` with the planning skill and numbering rules.
4. Create `.agents/plans/` and add the first numbered plan.
5. Verify the resulting structure and contents.

## Validation

- Confirm the skill files exist under `.agents/skills/`.
- Confirm the first numbered plan exists under `.agents/plans/`.
- Confirm the planning skill defines numbered filenames and checkbox todos.

## Todo

- [x] Create EH577 probe/test skill
- [x] Create libfprint patch/build skill
- [x] Create planning skill
- [x] Create `.agents/plans/` with a numbered plan file
- [x] Verify final structure and content
