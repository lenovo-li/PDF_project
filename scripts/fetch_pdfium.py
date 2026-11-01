#!/usr/bin/env python
"""Download the prebuilt PDFium package into third_party/pdfium/.

Source: https://github.com/bblanchon/pdfium-binaries (BSD-3 / Apache-2 for
PDFium itself; the build scripts are MIT). The release tag is pinned so every
machine builds against the same headers; bump PDFIUM_TAG deliberately.

The "pdfium-win-x64" package (no V8/XFA) ships pdfium.dll + pdfium.dll.lib.
The DLL is copied next to the executables by the PDFCore post-build step.
"""
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request

PDFIUM_TAG = "chromium/8125"
PACKAGE = "pdfium-win-x64.tgz"
URL = (f"https://github.com/bblanchon/pdfium-binaries/releases/download/"
       f"{PDFIUM_TAG}/{PACKAGE}")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "third_party", "pdfium")
STAMP = os.path.join(DEST, "TAG.txt")


def main():
    if os.path.isfile(STAMP):
        with open(STAMP, encoding="utf-8") as f:
            if f.read().strip() == PDFIUM_TAG:
                print(f"PDFium {PDFIUM_TAG} already present in {DEST}")
                return 0
    # Download and extract into a fresh temp directory, then move into place,
    # so an interrupted run never leaves a half-extracted third_party/pdfium.
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, PACKAGE)
        print(f"Downloading {URL}")
        urllib.request.urlretrieve(URL, archive)
        out = os.path.join(tmp, "pdfium")
        with tarfile.open(archive, "r:gz") as tar:
            tar.extractall(out, filter="data")  # rejects absolute/.. paths
        if not os.path.isfile(os.path.join(out, "include", "fpdfview.h")):
            print("error: archive layout unexpected (no include/fpdfview.h)")
            return 1
        with open(os.path.join(out, "TAG.txt"), "w", encoding="utf-8") as f:
            f.write(PDFIUM_TAG + "\n")
        if os.path.isdir(DEST):
            shutil.rmtree(DEST)
        os.makedirs(os.path.dirname(DEST), exist_ok=True)
        shutil.move(out, DEST)
    print(f"PDFium {PDFIUM_TAG} installed to {DEST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
