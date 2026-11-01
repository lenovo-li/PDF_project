#!/usr/bin/env python
"""Restore every third-party dependency. Run once after cloning, and again
after editing vcpkg.json or bumping PDFIUM_TAG in fetch_pdfium.py.

    python scripts/restore_deps.py

1. PDFium prebuilt package -> third_party/pdfium/   (fetch_pdfium.py)
2. vcpkg manifest (vcpkg.json) -> vcpkg_installed/x64-windows-static/

vcpkg is the copy bundled with Visual Studio (located through vswhere), or
$VCPKG_ROOT if set. The triplet is x64-windows-static so qpdf, zlib and
libjpeg use the static CRT like FluentUI. Both outputs are gitignored.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRIPLET = "x64-windows-static"
VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"


def find_vcpkg():
    env = os.environ.get("VCPKG_ROOT")
    if env and os.path.isfile(os.path.join(env, "vcpkg.exe")):
        return os.path.join(env, "vcpkg.exe")
    if os.path.isfile(VSWHERE):
        vs = subprocess.run([VSWHERE, "-latest", "-property", "installationPath"],
                            capture_output=True, text=True).stdout.strip()
        exe = os.path.join(vs, "VC", "vcpkg", "vcpkg.exe")
        if os.path.isfile(exe):
            return exe
    return shutil.which("vcpkg")


def main():
    rc = subprocess.call([sys.executable, os.path.join(ROOT, "scripts", "fetch_pdfium.py")])
    if rc != 0:
        return rc
    vcpkg = find_vcpkg()
    if not vcpkg:
        print("error: vcpkg not found. Install the 'vcpkg package manager' component "
              "in Visual Studio Installer, or set VCPKG_ROOT.")
        return 1
    print(f"Using {vcpkg}")
    return subprocess.call(
        [vcpkg, "install", f"--triplet={TRIPLET}",
         f"--x-install-root={os.path.join(ROOT, 'vcpkg_installed')}"],
        cwd=ROOT)


if __name__ == "__main__":
    sys.exit(main())
