import re, pathlib

base = pathlib.Path("lib/TDspYmfm/patches/pss140")
patches = [l for l in (base / "pss140_patches.txt").read_text().splitlines() if l.strip()]
names   = [l.rstrip() for l in (base / "pss140_names.txt").read_text().splitlines() if l.strip()]
assert len(patches) == len(names) == 100, (len(patches), len(names))

def cstr(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

rows = []
for i, (p, n) in enumerate(zip(patches, names)):
    bs = re.findall(r"\$?([0-9A-Fa-f]{2})", p)
    assert len(bs) == 8, (i, p, bs)
    hexs = ",".join("0x" + b.upper() for b in bs)
    rows.append("    {{ {} }}, // {:3d} {}".format(hexs, i, n))

namerows = "\n".join('    "{}",'.format(cstr(n)) for n in names)

hdr = """// Pss140Patches.h - GENERATED, do not edit by hand.
// Source: lib/TDspYmfm/patches/pss140/{{pss140_patches,pss140_names}}.txt (see that dir's NOTICE.md).
// 100 Yamaha PSS-140 OPLL (YM2413) user-voice patches = registers $00..$07, one row each.
// STUDY-ONLY data baked in for a personal dev build - do not ship/redistribute publicly.
#pragma once
#include <stdint.h>

static constexpr int kPss140Count = 100;

// Row i = OPLL user-voice registers $00,$01,...,$07 for PSS-140 patch i.
static const uint8_t kPss140Patches[kPss140Count][8] PROGMEM = {{
{rows}
}};

static const char* const kPss140Names[kPss140Count] = {{
{names}
}};
""".format(rows="\n".join(rows), names=namerows)

out = pathlib.Path("projects/spike_esp32_bt_spdif_mix_kit_f32/src/Pss140Patches.h")
out.write_text(hdr)
print("wrote", out, "-", len(hdr), "bytes,", len(patches), "patches")
print(rows[0])
print(rows[-1])
