#!/usr/bin/env python3
"""fetch_handpan.py -- build a compact, PSRAM-sized HANDPAN SoundFont for the T-DSP "Synth F" track.

Source: the FreePats "Hang tuned in D minor" sound bank (CC0 1.0 public domain), recorded in 2017
at Medialab-Prado by Gonzalo and Roberto for the FreePats project, performer Mar.
    https://freepats.zenvoid.org/ChromaticPercussion/hang.html
Its SF2 release is 25 MB: 9 pitches (A3 D4 E4 F4 G4 A4 Bb4 C5 D5), 5-7 round-robin takes per
pitch, stereo 44.1 kHz, each take 1.7-4.2 s, with the takes shuffled across the 128 velocities
("randomized layers"). The whole thing can't live in the box's PSRAM next to the drum font, so
this tool rebuilds it compactly:

  * keep N takes per pitch (--variants, default 2) and spread them over velocity bands, so
    playing softer/harder still alternates takes (the SF2 way to get variation);
  * mono mixdown (L+R)/2, resampled to --rate (default 32000 Hz: a handpan's shimmer lives
    well below 12 kHz), trimmed to --seconds (default 2.5 s) with a short fade;
  * one preset "Hang D minor" that plays ONLY the real pitches (FreePats' advice: a shifted
    note drags its sympathetic chord to a random scale), plus "Hang chromatic" that fills the
    gaps from the nearest take for keyboard players who want every key to sound.

Output: tools/sf2/fonts/handpan.sf2 (that folder is git-ignored; nothing is committed).

USAGE
    pip install numpy py7zr            # once
    python tools/fetch_handpan.py                      # fetch + build (defaults ~3 MB)
    python tools/fetch_handpan.py --variants 3 --seconds 3 --rate 32000   # bigger/longer
    python tools/fetch_handpan.py --sf2 path/to/other.sf2 --label "My Handpan"   # any SF2 source

PUSH TO THE CARD (then reboot the Teensy; the font loads at boot)
    python tools/sync_assets.py --skip-manifest --soundfont \\
        --sf2-src tools/sf2/fonts/handpan.sf2 --sf2-dest /sf2/handpan.sf2 [--port COMx|/dev/ttyACM0]
  or over Wi-Fi through the ESP32 tunnel:
    python projects/t-dsp_esp32_bt_receiver/tools/esp32_ota_wifi.py 192.168.4.1 \\
        tools/sf2/fonts/handpan.sf2 --stage-only --path /sf2/handpan.sf2
"""
import argparse, io, os, struct, sys, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "sf2"))
import merge_drum_sf2 as M   # validated SF2 RIFF writer (_assemble) + parser (parse)

SRC_URL = "https://github.com/freepats/hang-D-minor/releases/download/2022-03-30/Hang-D-minor-SF2-20220330.7z"
OUT_DIR = os.path.join(REPO, "tools", "sf2", "fonts")
CACHE = os.path.join(OUT_DIR, "cache")

GEN_KEYRANGE, GEN_VELRANGE, GEN_SAMPLEMODES, GEN_OVERRIDINGROOTKEY, GEN_PAN = 43, 44, 54, 58, 17
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(n):
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def fetch_source():
    os.makedirs(CACHE, exist_ok=True)
    arc = os.path.join(CACHE, os.path.basename(SRC_URL))
    if not os.path.isfile(arc):
        print("downloading", SRC_URL)
        urllib.request.urlretrieve(SRC_URL, arc)
    try:
        import py7zr
    except ImportError:
        sys.exit("need: pip install py7zr   (the FreePats release is a .7z archive)")
    with py7zr.SevenZipFile(arc, "r") as z:
        names = [n for n in z.getnames() if n.lower().endswith(".sf2")]
        z.extract(path=CACHE, targets=[n for n in z.getnames() if n.lower().endswith((".sf2", ".txt"))])
    sf2 = os.path.join(CACHE, names[0])
    print("source SF2:", sf2, "%.1f MB" % (os.path.getsize(sf2) / 1e6))
    return sf2


def zones_of(f):
    """Instrument zones of the first instrument as dicts {gen op: raw bytes}."""
    ibag, igen = f["ibag"], f["igen"]
    out = []
    for zi in range(len(ibag) - 1):
        g0, g1 = ibag[zi]["gen"], ibag[zi + 1]["gen"]
        gens = {}
        for op, raw in igen[g0:g1]:
            gens[op] = raw
        out.append(gens)
    return out


def collect_takes(f):
    """-> {midi_note: [take_name, ...]} and {take_name: (L shdr, R shdr|None)}."""
    shdr = f["shdr"]
    notes, takes = {}, {}
    for z in zones_of(f):
        if M.GEN_SAMPLEID not in z or GEN_KEYRANGE not in z:
            continue
        sid = struct.unpack("<H", z[M.GEN_SAMPLEID])[0]
        lo, hi = z[GEN_KEYRANGE][0], z[GEN_KEYRANGE][1]
        if lo != hi:
            continue
        sh = shdr[sid]
        nm = sh["name"].split(b"\0")[0].decode("ascii", "replace")
        base = nm[:-2] if nm.endswith(("_L", "_R")) else nm
        side = "R" if nm.endswith("_R") else "L"
        notes.setdefault(lo, [])
        if base not in notes[lo]:
            notes[lo].append(base)
        pair = takes.setdefault(base, [None, None])
        pair[0 if side == "L" else 1] = sh
    return notes, takes


def render_take(pcm, pair, src_rate, out_rate, seconds):
    """Mono int16 at out_rate, trimmed + faded. pcm = the font's 16-bit sample block."""
    import numpy as np
    def words(sh):
        return np.frombuffer(pcm, dtype="<i2", count=sh["end"] - sh["start"], offset=sh["start"] * 2).astype(np.float32)
    L = words(pair[0])
    R = words(pair[1]) if pair[1] is not None else None
    x = (L + R[:len(L)]) * 0.5 if R is not None and len(R) >= len(L) else L
    # resample: windowed-sinc lowpass at ~0.45*out_rate, then linear interpolation
    if out_rate != src_rate:
        cutoff = 0.45 * out_rate / src_rate          # cycles/sample at the source rate
        taps = 63
        n = np.arange(taps) - (taps - 1) / 2
        h = 2 * cutoff * np.sinc(2 * cutoff * n) * np.hamming(taps)
        h /= h.sum()
        x = np.convolve(x, h, mode="same")
        t_out = np.arange(0, len(x) - 1, src_rate / out_rate)
        x = np.interp(t_out, np.arange(len(x)), x)
    n_keep = int(seconds * out_rate)
    if len(x) > n_keep:
        x = x[:n_keep]
        fade = int(0.25 * out_rate)
        x[-fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32)
    return x


def build(sf2_src, out_path, label, variants, seconds, rate):
    import numpy as np
    f = M.parse(sf2_src)
    notes, takes = collect_takes(f)
    src_rate = f["shdr"][0]["rate"]
    pcm = f["pcm"]
    print("pitches:", " ".join(note_name(n) for n in sorted(notes)), "| source rate", src_rate)

    # render chosen takes
    rendered = {}   # (note, k) -> float array
    for n in sorted(notes):
        names = notes[n][:variants]
        for k, nm in enumerate(names):
            rendered[(n, k)] = render_take(pcm, takes[nm], src_rate, rate, seconds)
            print("  %-4s take %d %-10s %.2fs" % (note_name(n), k + 1, nm, len(rendered[(n, k)]) / rate))
    peak = max(float(np.max(np.abs(a))) for a in rendered.values()) or 1.0
    gain = 32767.0 * 0.89 / peak    # -1 dBFS

    shdr, smpl_parts, cursor = [], [], 0
    sid_of = {}
    for (n, k), a in sorted(rendered.items()):
        q = np.clip(np.round(a * gain), -32768, 32767).astype("<i2")
        st, en = cursor, cursor + int(q.size)
        shdr.append(dict(name=("%s_%d" % (note_name(n), k + 1)).encode()[:20], start=st, end=en, sloop=st, eloop=en,
                         rate=rate, opitch=n, pcorr=0, link=0, stype=1))
        smpl_parts.append(q.tobytes()); smpl_parts.append(b"\x00\x00" * M.GUARD)
        cursor = en + M.GUARD
        sid_of[(n, k)] = len(shdr) - 1

    inst, ibag, igen = [], [], []
    imod = [b"\x00" * M.IMOD_SZ]
    def add_zone(lo, hi, vlo, vhi, root, sid):
        ibag.append(dict(gen=len(igen), mod=0))
        igen.append([GEN_KEYRANGE, struct.pack("<BB", lo, hi)])
        igen.append([GEN_VELRANGE, struct.pack("<BB", vlo, vhi)])
        igen.append([GEN_SAMPLEMODES, struct.pack("<H", 0)])
        igen.append([GEN_OVERRIDINGROOTKEY, struct.pack("<h", root)])
        igen.append([M.GEN_SAMPLEID, struct.pack("<H", sid)])
    pitches = sorted(notes)
    def vel_bands(count):
        edges = [round(i * 128 / count) for i in range(count + 1)]
        return [(edges[i], edges[i + 1] - 1) for i in range(count)]

    # instrument 0: real pitches only
    inst.append(dict(name=label.encode()[:20], ibag=len(ibag)))
    for n in pitches:
        ks = [k for (nn, k) in rendered if nn == n]
        for k, (vlo, vhi) in zip(sorted(ks), vel_bands(len(ks))):
            add_zone(n, n, vlo, vhi, n, sid_of[(n, k)])
    # instrument 1: chromatic fill from the nearest real pitch
    inst.append(dict(name=(label + " chromatic").encode()[:20], ibag=len(ibag)))
    lo_key, hi_key = pitches[0] - 7, pitches[-1] + 7
    for key in range(lo_key, hi_key + 1):
        n = min(pitches, key=lambda p: (abs(p - key), p))
        ks = [k for (nn, k) in rendered if nn == n]
        for k, (vlo, vhi) in zip(sorted(ks), vel_bands(len(ks))):
            add_zone(key, key, vlo, vhi, n, sid_of[(n, k)])
    inst.append(dict(name=b"EOI", ibag=len(ibag))); ibag.append(dict(gen=len(igen), mod=0)); igen.append([0, b"\x00\x00"])
    shdr.append(dict(name=b"EOS", start=0, end=0, sloop=0, eloop=0, rate=0, opitch=0, pcorr=0, link=0, stype=0))
    smpl = b"".join(smpl_parts)
    if len(smpl) & 1:
        smpl += b"\x00"

    phdr, pbag, pgen = [], [], []
    pmod = [b"\x00" * M.PMOD_SZ]
    for i, nm in enumerate((label, label + " chromatic")):
        phdr.append(dict(name=nm.encode()[:20], preset=i, bank=0, pbag=len(pbag), lib=0, genre=0, morph=0))
        pbag.append(dict(gen=len(pgen), mod=0))
        pgen.append([M.GEN_INSTRUMENT, struct.pack("<H", i)])
    phdr.append(dict(name=b"EOP", preset=0, bank=0, pbag=len(pbag), lib=0, genre=0, morph=0))
    pbag.append(dict(gen=len(pgen), mod=0)); pgen.append([0, b"\x00\x00"])

    data = M._assemble(None, smpl, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    open(out_path, "wb").write(data)
    print("wrote %s  %.2f MB  (%d samples, %d zones)" % (os.path.relpath(out_path, REPO), len(data) / 1e6, len(shdr) - 1, len(ibag) - 1))
    chk = M.parse(data)   # round-trip the writer's output through the parser
    print("re-parsed OK: %d presets, %d instruments" % (len(chk["phdr"]) - 1, len(chk["inst"]) - 1))
    with open(os.path.splitext(out_path)[0] + "_CREDITS.txt", "w", encoding="utf-8") as c:
        c.write("handpan.sf2 is built from the FreePats 'Hang tuned in D minor' sound bank (CC0 1.0 public domain).\n"
                "Recorded 2017 at Medialab-Prado (Madrid) by Gonzalo and Roberto for FreePats; Hang played by Mar.\n"
                "https://freepats.zenvoid.org/ChromaticPercussion/hang.html\n"
                "Rebuilt compactly by tools/fetch_handpan.py: %d take(s)/pitch, %.1f s, %d Hz mono.\n" % (variants, seconds, rate))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf2", help="use this source SF2 instead of downloading the FreePats Hang")
    ap.add_argument("--out", default=os.path.join(OUT_DIR, "handpan.sf2"))
    ap.add_argument("--label", default="Hang Dm", help="preset/instrument name; SF2 caps names at 20 chars incl. the ' chromatic' suffix")
    ap.add_argument("--variants", type=int, default=2, help="round-robin takes kept per pitch (spread over velocity)")
    ap.add_argument("--seconds", type=float, default=2.5, help="max take length (fade-out at the end)")
    ap.add_argument("--rate", type=int, default=32000, help="output sample rate")
    a = ap.parse_args()
    src = a.sf2 or fetch_source()
    build(src, a.out, a.label[:20], a.variants, a.seconds, a.rate)
    print("push: python tools/sync_assets.py --skip-manifest --soundfont --sf2-src %s --sf2-dest /sf2/handpan.sf2" % os.path.relpath(a.out, REPO))
    return 0


if __name__ == "__main__":
    sys.exit(main())
