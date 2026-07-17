"""
load_env.py -- PlatformIO PRE-script: inject secrets from a gitignored .env as -D macros.

Keeps WiFi credentials (and anything else secret) OUT of the tracked platformio.ini.
Use it from an env with:

    extra_scripts = pre:../../tools/load_env.py

Every KEY=VALUE line in <project>/.env becomes -DKEY="VALUE".

Values are ALWAYS emitted as C *string* literals. That is deliberate: a purely-numeric
password (e.g. 12345678) must not silently become an integer macro. Numeric build
options (TDSP_WS_PORT, ...) belong in platformio.ini, where they already have #ifndef
defaults -- not here.

A missing or empty .env is NOT an error: the firmware's #ifndef fallbacks + #warning
already make an unconfigured build obvious (see src/main.cpp). This keeps a fresh clone
building without forcing everyone to create a .env first.

Only KEY NAMES are logged -- never the values, so secrets don't leak into build output
or CI logs.
"""

import os

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)

env_path = os.path.join(env.subst("$PROJECT_DIR"), ".env")  # noqa: F821

if not os.path.isfile(env_path):
    print("[load_env] no .env next to platformio.ini -- see .env.example "
          "(build proceeds; WiFi will not join a network)")
else:
    defines = []
    with open(env_path) as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, val = line.split("=", 1)
            key, val = key.strip(), val.strip()
            if not key:
                continue
            # Tolerate a value the user wrapped in quotes themselves.
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
                val = val[1:-1]
            # Escape so the result is a valid C string literal.
            val = val.replace("\\", "\\\\").replace('"', '\\"')
            defines.append((key, '\\"%s\\"' % val))
    if defines:
        env.Append(CPPDEFINES=defines)  # noqa: F821
        print("[load_env] .env -> " + ", ".join(k for k, _ in defines))
    else:
        print("[load_env] .env is empty -- nothing injected")
