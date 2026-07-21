#!/usr/bin/env python3
"""fx_cost.py — measure the FLASH/RAM (and optionally CPU) cost of each hexefx effect.

Builds every `fx_*` env of projects/spike_fx_plate_reverb, parses the Teensy memory
report, and writes a per-effect cost table (deltas vs the `fx_none` baseline). This is
the data that decides how many FX slots each mix-kit env can afford — see
planning/plate-reverb-fx/DESIGN.md §6.5 ("whatever compiles is tested, so we don't max out").

Static cost (FLASH / RAM1 / RAM2) needs no hardware — just builds. CPU% needs a board:
pass --port COMx to also upload each env and capture its runtime `[FXCOST]` line.

Usage:
  python tools/fx_cost.py                       # build all envs, write COST.md
  python tools/fx_cost.py --envs fx_none fx_plate fx_delay
  python tools/fx_cost.py --port COM4           # also measure CPU on hardware
  python tools/fx_cost.py --pio /path/to/platformio.exe

The mix-kit's pio.exe (Windows) is typically at
  C:/Users/<you>/AppData/Roaming/Python/Python313/Scripts/platformio.exe
"""
import argparse, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SPIKE = REPO / "projects" / "spike_fx_plate_reverb"
OUT_MD = REPO / "planning" / "plate-reverb-fx" / "COST.md"
OUT_JSON = REPO / "planning" / "plate-reverb-fx" / "cost.json"
BASELINE = "fx_none"

# Teensy builder memory report (teensy_size), printed at the end of a successful build.
RE_FLASH = re.compile(r"FLASH:\s*code:(\d+),\s*data:(\d+),\s*headers:(\d+)")
RE_RAM1  = re.compile(r"RAM1:\s*variables:(\d+),\s*code:(\d+),\s*padding:(\d+)")
RE_RAM2  = re.compile(r"RAM2:\s*variables:(\d+)")
# Fallback: PlatformIO's generic checkprogsize line (aggregate, less precise).
RE_PIO_RAM   = re.compile(r"RAM:\s*\[[=\s]*\]\s*[\d.]+%\s*\(used (\d+) bytes")
RE_PIO_FLASH = re.compile(r"Flash:\s*\[[=\s]*\]\s*[\d.]+%\s*\(used (\d+) bytes")
RE_FXCOST = re.compile(r"\[FXCOST\]\s*effect=(\S+)\s*cpu=([\d.]+)\s*memI16=(\d+)"
                       r"\s*memF32=(\d+)\s*heapRAM2=(\d+)")


def find_pio(explicit):
    if explicit:
        return explicit
    cand = Path.home() / "AppData/Roaming/Python/Python313/Scripts/platformio.exe"
    if cand.exists():
        return str(cand)
    for name in ("platformio", "pio"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("platformio not found — pass --pio /path/to/platformio(.exe)")


def find_teensy_size():
    """teensy_size prints the Teensy region breakdown (FLASH / RAM1 / RAM2) — the
    numbers PlatformIO's stdout omits on this setup. Bundled with the tool-teensy pkg."""
    pkgs = Path.home() / ".platformio" / "packages"
    for name in ("teensy_size.exe", "teensy_size"):
        hits = list(pkgs.glob(f"**/{name}"))
        if hits:
            return str(hits[0])
    return None


def list_envs(pio):
    """All [env:fx_*] names from the spike's platformio.ini, baseline first."""
    envs = []
    for line in (SPIKE / "platformio.ini").read_text().splitlines():
        m = re.match(r"\[env:(fx_\w+)\]", line.strip())
        if m:
            envs.append(m.group(1))
    envs.sort(key=lambda e: (e != BASELINE, e))  # baseline first
    return envs


def parse_size(text):
    mf, m1, m2 = RE_FLASH.search(text), RE_RAM1.search(text), RE_RAM2.search(text)
    if mf and m1 and m2:
        return {"flash": sum(int(x) for x in mf.groups()),
                "ram1": sum(int(x) for x in m1.groups()),
                "ram2": int(m2.group(1))}
    return None


def build(pio, tsize, env):
    print(f"  building {env} ...", flush=True)
    r = subprocess.run([pio, "run", "-e", env], cwd=SPIKE,
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        # A hard RAM/FLASH overflow is exactly what the guardrail is meant to surface.
        tail = "\n".join(out.splitlines()[-12:])
        return {"env": env, "ok": False, "error": tail}
    d = {"env": env, "ok": True, "flash": None, "ram1": None, "ram2": None}
    # Preferred: run teensy_size on the freshly-built ELF (exact region breakdown).
    elf = SPIKE / ".pio" / "build" / env / "firmware.elf"
    sz = None
    if tsize and elf.exists():
        s = subprocess.run([tsize, str(elf)], capture_output=True, text=True)
        sz = parse_size(s.stdout + s.stderr)
    if sz is None:  # fallbacks: teensy_size block or aggregate line in pio stdout
        sz = parse_size(out)
    if sz:
        d.update(sz)
    else:
        pr, pf = RE_PIO_RAM.search(out), RE_PIO_FLASH.search(out)
        d["flash"] = int(pf.group(1)) if pf else None
        d["ram1"] = int(pr.group(1)) if pr else None
        d["note"] = "aggregate (teensy_size not found)"
    return d


def find_teensy_port():
    """The Teensy's COM number changes across a reflash (bootloader re-enumerates), so
    locate it by USB VID (PJRC = 0x16C0) rather than trusting a fixed port name."""
    try:
        import serial.tools.list_ports as lp
    except ImportError:
        return None
    for p in lp.comports():
        if p.vid == 0x16C0:
            return p.device
    return None


def measure_cpu(pio, env, seconds):
    try:
        import serial  # pyserial
    except ImportError:
        print("    (pyserial not installed — skipping CPU measure)")
        return None
    print(f"    uploading {env} for CPU/heap measure ...", flush=True)
    # Let the Teensy loader find the board itself (auto-reboot via the running sketch's
    # USB serial). If the port is held, this fails and the board needs the PROGRAM button.
    up = subprocess.run([pio, "run", "-e", env, "-t", "upload"],
                        cwd=SPIKE, capture_output=True, text=True)
    if up.returncode != 0:
        print("    upload failed (port busy? press PROGRAM?) — skipping CPU")
        return None
    # After a reflash the COM number can change — re-find it, retrying while it re-enumerates.
    port = None
    for _ in range(10):
        time.sleep(1)
        port = find_teensy_port()
        if port:
            break
    if not port:
        print("    Teensy serial port not found after upload — skipping CPU")
        return None
    time.sleep(2)  # warm-up (firmware skips its first 2 s of peaks)
    print(f"    reading {port} ...", flush=True)
    best = None
    try:
        with serial.Serial(port, 115200, timeout=1) as s:
            t_end = time.time() + seconds
            while time.time() < t_end:
                line = s.readline().decode("utf-8", "ignore")
                m = RE_FXCOST.search(line)
                if m:
                    cpu = float(m.group(2))
                    rec = {"cpu": cpu, "memI16": int(m.group(3)),
                           "memF32": int(m.group(4)), "heapRAM2": int(m.group(5))}
                    if best is None or cpu > best["cpu"]:
                        best = rec
    except Exception as e:
        print(f"    serial read failed: {e}")
    return best


def d(cur, base, key):
    if cur.get(key) is None or base.get(key) is None:
        return None
    return cur[key] - base[key]


def kb(n):
    return "—" if n is None else f"{n/1024:.1f}"


def write_md(rows, baseline):
    OUT_MD.parent.mkdir(parents=True, exist_ok=True)
    L = []
    L.append("# hexefx FX cost table\n")
    L.append("Auto-generated by `tools/fx_cost.py`. Deltas are vs the `fx_none` baseline")
    L.append("(tone → DAC, no effect). One effect instantiated per build; `--gc-sections`")
    L.append("strips the rest, so each row is that effect's real marginal cost.\n")
    if baseline.get("ok"):
        L.append(f"**Baseline `fx_none`:** FLASH {kb(baseline.get('flash'))} KB · "
                 f"RAM1 {kb(baseline.get('ram1'))} KB · static RAM2 {kb(baseline.get('ram2'))} KB\n")
    L.append("Columns: **ΔFLASH / ΔRAM1(static)** from `teensy_size` (build-only). "
             "**CPU% / heapRAM2(run)** are runtime, hardware-measured (need `--port`) — "
             "the effects `malloc()` their delay buffers from the RAM2 heap, which the static "
             "report can't see, so `heapRAM2` is the number that really decides RAM fit.\n")
    base_heap = baseline.get("cpu", {}).get("heapRAM2") if baseline.get("cpu") else None
    L.append("| effect | ΔFLASH KB | ΔRAM1 KB (static) | CPU % | heapRAM2 KB (run) | memF32 blk | build |")
    L.append("|---|--:|--:|--:|--:|--:|:--|")
    for r in rows:
        if r["env"] == BASELINE:
            continue
        if not r.get("ok"):
            L.append(f"| {r['env']} | — | — | — | — | — | ❌ FAIL |")
            continue
        cpu = mf = heap = "—"
        if r.get("cpu"):
            cpu = f"{r['cpu']['cpu']:.1f}"
            mf = str(r["cpu"]["memF32"])
            if base_heap is not None:
                heap = kb(r["cpu"]["heapRAM2"] - base_heap)
        L.append(f"| {r['env']} | {kb(d(r,baseline,'flash'))} | {kb(d(r,baseline,'ram1'))} "
                 f"| {cpu} | {heap} | {mf} | ✅ |")
    L.append("\n_CPU% and heapRAM2 are blank unless run with `--port COMx` (they need the board)._")
    L.append("_ReverbSC allocs its ~396 KB buffer from **PSRAM** (`use_psram=true`), so its heapRAM2_")
    L.append("_delta stays low but it needs a PSRAM board to run at all._\n")
    # Surface build failures prominently — that's the guardrail doing its job.
    fails = [r for r in rows if not r.get("ok")]
    if fails:
        L.append("## Build failures (over budget / won't fit)\n")
        for r in fails:
            L.append(f"### {r['env']}\n```\n{r.get('error','')}\n```\n")
    OUT_MD.write_text("\n".join(L), encoding="utf-8")
    print(f"\nWrote {OUT_MD}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pio", help="path to platformio(.exe)")
    ap.add_argument("--envs", nargs="*", help="subset of fx_* envs (default: all)")
    ap.add_argument("--measure", action="store_true",
                    help="also upload each env + measure CPU/heap on hardware (Teensy auto-found by USB VID)")
    ap.add_argument("--seconds", type=int, default=6, help="serial capture window per env")
    args = ap.parse_args()

    pio = find_pio(args.pio)
    tsize = find_teensy_size()
    envs = args.envs or list_envs(pio)
    if BASELINE not in envs:
        envs = [BASELINE] + envs
    print(f"pio: {pio}\nteensy_size: {tsize}\nenvs: {', '.join(envs)}")

    rows = []
    for env in envs:
        r = build(pio, tsize, env)
        if r.get("ok") and args.measure:
            r["cpu"] = measure_cpu(pio, env, args.seconds)
        rows.append(r)

    baseline = next((r for r in rows if r["env"] == BASELINE), {"ok": False})
    OUT_JSON.write_text(json.dumps(rows, indent=2), encoding="utf-8")
    write_md(rows, baseline)
    print(f"Wrote {OUT_JSON}")


if __name__ == "__main__":
    main()
