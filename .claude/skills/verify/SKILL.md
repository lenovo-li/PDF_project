---
name: verify
description: Full PDFTool verification in an isolated builder agent — check_project.py, MSBuild Debug + Release, PDFCoreTests.exe — returning only failures. Use before reporting any code change complete.
argument-hint: "[test-filter]"
context: fork
agent: pdf-builder
---

Run the full verification for the PDFTool working tree, in this order, and
stop at the first failing stage:

1. `python scripts/check_project.py`
2. MSBuild Debug (`PDFTool.slnx`, x64)
3. MSBuild Release (`PDFTool.slnx`, x64)
4. `./x64/Debug/PDFCoreTests.exe $ARGUMENTS` (no argument runs everything)

Report in your standard format: one line per stage, deduplicated
warnings/errors as `file(line): code: message`, the runner's summary line,
and each failing test with its message. Separate environment failures
(Device Guard block) from real regressions.
