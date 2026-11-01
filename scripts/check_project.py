#!/usr/bin/env python
"""Check that every source file on disk is registered in its project.

For each .vcxproj under src/ and tests/:
  * every .cpp/.h/.rc under the project directory must appear in the .vcxproj
    (ClCompile / ClInclude / ResourceCompile) and in the .vcxproj.filters;
  * every file the .vcxproj names must exist on disk.

MSBuild has no globbing here, so a file that exists but isn't listed is
silently not compiled (or not shown in Solution Explorer). Read-only, fast.
Exit code 0 = clean, 1 = problems found.
"""
import os
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
KINDS = ("ClCompile", "ClInclude", "ResourceCompile")
SOURCE_EXT = (".cpp", ".h", ".hpp", ".rc")
SKIP_DIRS = {"x64", "obj", "bin", ".vs"}


def items(path):
    if not os.path.isfile(path):
        return None
    tree = ET.parse(path)
    found = set()
    for kind in KINDS:
        for node in tree.getroot().iterfind(f".//m:{kind}", NS):
            inc = node.get("Include")
            if inc:
                found.add(os.path.normcase(os.path.normpath(inc)))
    return found


def on_disk(project_dir):
    found = set()
    for dirpath, dirnames, filenames in os.walk(project_dir):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name.lower().endswith(SOURCE_EXT):
                rel = os.path.relpath(os.path.join(dirpath, name), project_dir)
                found.add(os.path.normcase(os.path.normpath(rel)))
    return found


def check(vcxproj):
    problems = []
    project_dir = os.path.dirname(vcxproj)
    listed = items(vcxproj)
    filters = items(vcxproj + ".filters")
    disk = on_disk(project_dir)
    rel_proj = os.path.relpath(vcxproj, ROOT)
    # Only files inside the project directory are checked against disk;
    # entries pointing elsewhere (..\) are checked for existence only.
    local = {p for p in listed if not p.startswith("..")}
    for p in sorted(disk - local):
        problems.append(f"{rel_proj}: not registered: {p}")
    for p in sorted(listed):
        if not os.path.exists(os.path.join(project_dir, p)):
            problems.append(f"{rel_proj}: listed but missing on disk: {p}")
    if filters is None:
        problems.append(f"{rel_proj}: .vcxproj.filters missing")
    else:
        for p in sorted(listed - filters):
            problems.append(f"{rel_proj}.filters: missing entry: {p}")
        for p in sorted(filters - listed):
            problems.append(f"{rel_proj}.filters: entry not in .vcxproj: {p}")
    return problems


def main():
    projects = []
    for top in ("src", "tests"):
        for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, top)):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            projects += [os.path.join(dirpath, f) for f in filenames
                         if f.endswith(".vcxproj")]
    problems = []
    for proj in sorted(projects):
        problems += check(proj)
    for line in problems:
        print(line)
    print(f"check_project: {len(projects)} projects, "
          f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
