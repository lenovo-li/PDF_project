#!/usr/bin/env python
"""Regenerate a .vcxproj.filters from its .vcxproj so Solution Explorer
mirrors the folder layout on disk.

    python scripts/gen_filters.py src/PDFCore/PDFCore.vcxproj

Each item lands in a filter named after its directory relative to the
project ("convert\\docx" for convert/docx/Foo.cpp); files at the project root
get no filter. Filter GUIDs are derived from the name so regeneration is
stable and diffs stay small. Run after adding or moving files.
"""
import os
import sys
import uuid
import xml.etree.ElementTree as ET

NS = "http://schemas.microsoft.com/developer/msbuild/2003"
KINDS = ("ClCompile", "ClInclude", "ResourceCompile", "Manifest", "None", "Image")


def main(path):
    tree = ET.parse(path)
    entries = []
    for kind in KINDS:
        for node in tree.getroot().iter(f"{{{NS}}}{kind}"):
            inc = node.get("Include")
            if inc:
                entries.append((kind, inc))
    folders = set()
    for _, inc in entries:
        d = os.path.dirname(inc.replace("/", "\\"))
        while d and not d.startswith(".."):
            folders.add(d)
            d = os.path.dirname(d)

    out = ['<?xml version="1.0" encoding="utf-8"?>',
           f'<Project ToolsVersion="4.0" xmlns="{NS}">', "  <ItemGroup>"]
    for f in sorted(folders):
        guid = uuid.uuid5(uuid.NAMESPACE_URL, "pdftool-filter:" + f.lower())
        out += [f'    <Filter Include="{f}">',
                f"      <UniqueIdentifier>{{{guid}}}</UniqueIdentifier>",
                "    </Filter>"]
    out.append("  </ItemGroup>")
    out.append("  <ItemGroup>")
    for kind, inc in entries:
        d = os.path.dirname(inc.replace("/", "\\"))
        if d and not d.startswith(".."):
            out += [f'    <{kind} Include="{inc}">',
                    f"      <Filter>{d}</Filter>", f"    </{kind}>"]
        else:
            out.append(f'    <{kind} Include="{inc}" />')
    out += ["  </ItemGroup>", "</Project>", ""]
    with open(path + ".filters", "w", encoding="utf-8", newline="\r\n") as fh:
        fh.write("\n".join(out))
    print(f"wrote {path}.filters ({len(entries)} items, {len(folders)} filters)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
