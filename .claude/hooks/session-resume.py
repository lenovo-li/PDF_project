#!/usr/bin/env python
# SessionStart hook (matcher: compact|resume). If a checkpoint exists, print the
# resume steps; SessionStart stdout is added to Claude's context. Prints nothing
# when there is no checkpoint, so a fresh clone (where .claude/TASK_STATE.md is
# gitignored and absent) gets no noise.
import os
import sys

root = os.environ.get("CLAUDE_PROJECT_DIR") or "."
state = os.path.join(root, ".claude", "TASK_STATE.md")
if os.path.isfile(state):
    first = ""
    with open(state, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.strip():
                first = line.strip()
                break
    print(
        "A task checkpoint exists at .claude/TASK_STATE.md "
        f"({first[:200]}). Before continuing: read it, run `git status --short`, "
        "inspect the relevant diff, correct anything stale in the checkpoint, then "
        "continue from the first item under 'Exact Next Actions' (see the "
        "task-state skill).")
sys.exit(0)
