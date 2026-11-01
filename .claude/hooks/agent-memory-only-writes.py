#!/usr/bin/env python
# PreToolUse hook for read-only subagents that have `memory: project`.
#
# Enabling `memory` makes Claude Code add Read/Write/Edit to the subagent so it can
# maintain .claude/agent-memory/<name>/. That would quietly turn a reviewer or
# explorer into an editor. This hook keeps the write tools usable for the memory
# directory only: any other target exits 2, which blocks the call and tells the
# agent why.
import json
import sys

try:
    payload = json.load(sys.stdin)
except (json.JSONDecodeError, ValueError):
    # A guard fails closed: if the target can't be read, don't allow the write.
    sys.stderr.write("Blocked: could not parse hook input; write refused.\n")
    sys.exit(2)

tool_input = payload.get("tool_input") or {}
path = (tool_input.get("file_path") or tool_input.get("notebook_path") or "")
normalized = path.replace("\\", "/")
if "/.claude/agent-memory/" in normalized or normalized.startswith(".claude/agent-memory/"):
    sys.exit(0)

sys.stderr.write(
    f"Blocked: this agent is read-only. It may write only under "
    f".claude/agent-memory/, not {path}. Report the change to the caller instead.\n")
sys.exit(2)
