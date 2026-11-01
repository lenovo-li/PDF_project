#!/usr/bin/env python
# PostToolUse hook (Edit|Write): after an edit to a .vcxproj or .vcxproj.filters,
# or after creating/editing a source file under src/ or tests/, run
# scripts/check_project.py. On failure exit 2 so Claude Code shows the checker
# output to the model in the same turn instead of at the next build.
import json
import os
import subprocess
import sys

WATCHED_SUFFIXES = (".vcxproj", ".vcxproj.filters", ".cpp", ".h", ".rc")


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0
    path = (payload.get("tool_input") or {}).get("file_path") or ""
    lower = path.replace("\\", "/").lower()
    if not lower.endswith(WATCHED_SUFFIXES):
        return 0
    # A new source file is legitimately unregistered until the next edit adds
    # it to the project, so only project-file edits fail hard; source edits
    # just surface the warning.
    is_project_file = lower.endswith((".vcxproj", ".vcxproj.filters"))

    root = os.environ.get("CLAUDE_PROJECT_DIR") or payload.get("cwd") or "."
    result = subprocess.run(
        [sys.executable, os.path.join(root, "scripts", "check_project.py")],
        cwd=root, capture_output=True, text=True)
    if result.returncode == 0:
        return 0
    if not is_project_file:
        print(f"check_project.py: unregistered files after editing {path}; "
              "register them in the .vcxproj/.filters before building.\n"
              + result.stdout[-2000:])
        return 0
    sys.stderr.write(f"check_project.py failed after editing {path}:\n")
    sys.stderr.write(result.stdout[-4000:])
    sys.stderr.write(result.stderr[-1000:])
    return 2


if __name__ == "__main__":
    sys.exit(main())
