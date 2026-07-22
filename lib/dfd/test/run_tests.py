#!/usr/bin/env python3
"""Compile + run the dfd desktop unit tests with MSVC.

Discovers the VS C++ toolchain via vswhere exactly like tools/fetch_drumkits.py's _find_msvc()
/ render_check(), then `cl /EHsc /std:c++17 /I..\\include test_region.cpp`, runs the exe, and
propagates its pass/fail exit code. No boost, no cmake. Windows-only (that is where this repo's
desktop checks run); on other platforms use any C++17 compiler by hand:
    g++ -std=c++17 -I../include test_region.cpp -o test_region && ./test_region
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INCLUDE = os.path.join(HERE, "..", "include")


def _find_msvc():
    """Locate a VS C++ toolchain via vswhere; return the vcvars64.bat path or None."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    if not os.path.exists(vswhere):
        return None
    try:
        out = subprocess.check_output(
            [vswhere, "-latest", "-products", "*", "-requires",
             "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
             "-property", "installationPath"], text=True).strip()
    except Exception:
        return None
    if not out:
        return None
    vcvars = os.path.join(out, "VC", "Auxiliary", "Build", "vcvars64.bat")
    return vcvars if os.path.exists(vcvars) else None


def main():
    src = os.path.join(HERE, "test_region.cpp")
    exe = os.path.join(HERE, "test_region.exe")

    if os.name != "nt":
        # Fall back to g++/clang++ on non-Windows.
        cxx = os.environ.get("CXX", "g++")
        r = subprocess.run([cxx, "-std=c++17", "-I", INCLUDE, src, "-o",
                            os.path.join(HERE, "test_region")], text=True)
        if r.returncode != 0:
            print("compile FAILED")
            return 2
        return subprocess.run([os.path.join(HERE, "test_region")]).returncode

    vcvars = _find_msvc()
    if not vcvars:
        print("no MSVC toolchain found (vswhere). Install VS Build Tools or compile by hand.")
        return 3

    bat = os.path.join(HERE, "_build_tests.bat")
    with open(bat, "w") as fh:
        fh.write('@echo off\r\ncall "%s" >nul 2>nul\r\n' % vcvars)
        fh.write('cd /d "%s"\r\n' % HERE)                       # .obj lands here; avoids /Fo quoting
        fh.write('cl /nologo /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I"%s" /Fe:"%s" "%s"\r\n'
                 % (INCLUDE, exe, src))
    r = subprocess.run(["cmd", "/c", bat], capture_output=True, text=True)
    if not os.path.exists(exe):
        print("compile FAILED:\n" + (r.stdout or "") + (r.stderr or ""))
        return 2
    print("compiled OK; running...\n")
    return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
