"""fx_ld_compat.py — PlatformIO post-script: drop `--no-warn-rwx-segments`.

The Teensy platform's arduino.py appends `-Wl,--gc-sections,--relax,--no-warn-rwx-segments`
to LINKFLAGS unconditionally. `--no-warn-rwx-segments` only *suppresses a benign linker
warning*, and it was added in GNU binutils 2.39 — so any box whose teensy `ld` is older
(e.g. jay-mint ships ld 2.38 with the gcc-11 toolchain) fails to link with
"unrecognized option '--no-warn-rwx-segments'". Stripping it is safe everywhere: on new ld
you just might see the RWX warning again; the binary is identical.

Registered as `post:` so it runs after arduino.py has populated LINKFLAGS.
"""
Import("env")   # noqa: F821  (injected by PlatformIO/SCons)

_BAD = "--no-warn-rwx-segments"


def _clean(flag):
    return flag.replace("," + _BAD, "").replace(_BAD, "")


env.Replace(LINKFLAGS=[c for c in (_clean(f) for f in env["LINKFLAGS"]) if c])
