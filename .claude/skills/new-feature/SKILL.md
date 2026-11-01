---
name: new-feature
description: End-to-end workflow for adding a PDFTool feature (e.g. merge, split, encrypt, annotate, convert to Word) — research, plan, implement, verify, review, document — delegating reading, building and review to project subagents.
argument-hint: "<feature> <what it should do>"
disable-model-invocation: true
---

Task: $ARGUMENTS

The main conversation plans, writes code and decides. Subagents read, build
and review so their output stays out of this context.

1. **Checkpoint.** Follow the `task-state` skill: create or update
   `.claude/TASK_STATE.md` with objective, acceptance criteria and this plan.
2. **Research, in parallel (both read-only):**
   - `pdf-explorer`: existing code in this repo for the feature; the PDFium /
     qpdf API that implements it; for UI, the FluentUI controls and the
     closest demo page in `../Fluent/FluentUIDemo/pages/`.
   - `pdf-spec-expert`: only if the feature touches format details (fonts,
     forms, encryption, DOCX output, tagged PDF).
3. **Plan.** Decide what goes in `PDFCore` (engine, headless, testable) versus
   `PDFTool` (UI). Write the engine API first. If the feature can touch
   unbounded data (pages, thumbnails, search hits), design the bounded /
   virtualized path first. If anything is ambiguous or consequential (new
   dependency, file format choice, changing `../Fluent`), ask the user.
4. **Implement** in this conversation. Register new files in `.vcxproj` +
   `.filters`; the PostToolUse hook runs `check_project.py`. Add tests in
   `tests/` that use the sample PDFs in `tests/data/` (add small ones as
   needed; never add copyrighted files).
5. **Verify** with `/verify`. Fix and repeat until Debug + Release are clean
   and tests are green. Update the checkpoint.
6. **Mutation check.** Break the core of the feature deliberately, have
   `pdf-builder` rebuild and run the relevant filter, confirm a test fails,
   restore.
7. **Review.** Delegate to `pdf-reviewer` with the scope (files), not your
   conclusions. Fix CONFIRMED findings; check PLAUSIBLE ones yourself.
8. **Document.** Update `docs/features.md` (what the feature does, its limits,
   which engine call it uses). Delete any `docs/design/` note for it after
   moving what is still true into a permanent doc.
9. **Final `/verify`** if anything changed after step 5, then report: what
   changed, which verification actually ran, and what the user must check on
   real hardware and exactly where. Do not commit unless asked.
