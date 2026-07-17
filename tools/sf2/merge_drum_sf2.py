#!/usr/bin/env python3
"""
merge_drum_sf2.py — splice a drum-only SoundFont into a base GM font for the TSF engine.

Why
---
The TSF engine (lib/TDspTsf) plays General MIDI with the whole font resident in PSRAM.
The stock GM percussion (TimGM6mb's bank-128 kit, and even GeneralUser's) is the weak
link when you're jamming a live melody over the looping ch10 grooves (project drum_grooves).
This tool takes the percussion from a dedicated *drum-only* SF2 (e.g. a bank-128 kit off
musical-artifacts) and grafts it onto a base GM font, replacing the base's bank-128
preset(s). Everything melodic (banks 0..) stays exactly as the base font had it.

TSF loads one font, has no per-channel font routing and no bank-merge (schellingb/
TinySoundFont issue #79), so the merge has to happen here, offline, producing a single
`.sf2` you drop on the SD card. No firmware change, no reflash — the font path is unchanged.

How (GC + merge, not in-place edit)
-----------------------------------
Rather than surgically deleting records (fragile: dangling phdr/inst/shdr indices), we
REBUILD the font from the final preset set and let a mark-sweep prune everything unreached:

  final presets = (all BASE presets whose bank != 128)  +  (DRUM font's bank-128 preset[s])

From those presets we walk the reference graph
  phdr -> pbag -> pgen(instrument=41) -> inst -> ibag -> igen(sampleID=53) -> shdr -> smpl
marking every instrument and sample actually reached (stereo link partners included), then
emit ONLY those, densely re-indexed. Side effect: the base font's now-unused GM drum
samples are garbage-collected, so the merged font is usually *smaller* despite better drums.

Correctness notes
  * Preset/instrument/sample cross-references (gen opers 41 and 53, shdr.wSampleLink) are all
    remapped through per-source dense tables (base and drum live in one shared new index space).
  * Per-zone modulators (pmod/imod) are copied verbatim with their zones — they carry no
    sample/instrument indices to remap.
  * Sample PCM is repacked with the SF2-spec 46-frame inter-sample guard; shdr start/end/
    loop are rewritten to the new offsets. Sample CONTENT is untouched (no resample), so the
    per-zone fine/coarse sample-address-offset generators stay valid (they are relative).
  * Terminal sentinel records (EOP/EOI/EOS + terminal bag/gen/mod entries) are regenerated.
  * 16-bit `smpl` only. If the source carries a 24-bit `sm24` chunk it is dropped (TSF is
    int16 anyway) with a warning.

Usage
-----
  # merge, replacing the base's bank 128 with the drum font's bank-128 kit:
  python merge_drum_sf2.py <base_gm.sf2> <drum.sf2> <out.sf2>

  # keep the base drums too, add the drum kit at a different bank/preset:
  python merge_drum_sf2.py base.sf2 drum.sf2 out.sf2 --drum-bank 128 --dest-bank 128 --keep-base-drums

  # inspect what a font contains (presets + bank map) without merging:
  python merge_drum_sf2.py --list <font.sf2>

  # prove the record-splice logic is correct on synthetic fonts (no external files):
  python merge_drum_sf2.py --selftest

Order-of-operations with build_gu_fonts.py
  Merge FIRST (full-rate drums), then optionally run build_gu_fonts.py on the result to fit a
  PSRAM budget — the downsampler treats the grafted drum samples like any other. Or merge into
  an already-downsampled base to keep the drums at native rate for extra punch. See DRUM_FONTS.md.
"""
import sys, os, struct

# ---- SF2 record sizes (bytes) ----
PHDR_SZ, PBAG_SZ, PMOD_SZ, PGEN_SZ = 38, 4, 10, 4
INST_SZ, IBAG_SZ, IMOD_SZ, IGEN_SZ = 22, 4, 10, 4
SHDR_SZ = 46
GEN_INSTRUMENT = 41   # preset-zone generator: amount = instrument index
GEN_SAMPLEID   = 53   # instrument-zone generator: amount = sample header index
GUARD = 46            # zero frames between samples (SF2 spec minimum)
DRUM_BANK = 128       # GM percussion bank


# ------------------------------------------------------------------ parsing
def _read_chunks(buf, off, end):
    while off + 8 <= end:
        cid = buf[off:off + 4]
        sz = struct.unpack_from("<I", buf, off + 4)[0]
        yield cid, off + 8, sz
        off += 8 + sz + (sz & 1)


def parse(path_or_bytes):
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else open(path_or_bytes, "rb").read()
    data = bytes(data)
    assert data[:4] == b"RIFF" and data[8:12] == b"sfbk", "not a SoundFont (RIFF/sfbk)"
    riffsz = struct.unpack_from("<I", data, 4)[0]
    sub = {}
    info_body = None
    for cid, doff, sz in _read_chunks(data, 12, 8 + riffsz):
        if cid == b"LIST":
            list_type = data[doff:doff + 4]
            if list_type == b"INFO":
                info_body = data[doff + 4:doff + sz]
            for scid, sdoff, ssz in _read_chunks(data, doff + 4, doff + sz):
                sub[scid.decode("latin1")] = (sdoff, ssz)
    f = {"raw": data, "info": info_body}

    def recs(name, sz):
        if name not in sub:
            return b""
        off, total = sub[name]
        return data[off:off + total]

    # sample PCM (int16)
    smpl_off, smpl_sz = sub["smpl"]
    f["pcm"] = memoryview(data)[smpl_off:smpl_off + smpl_sz]
    if "sm24" in sub:
        f["has_sm24"] = True

    # phdr
    f["phdr"] = []
    raw = recs("phdr", PHDR_SZ)
    for i in range(len(raw) // PHDR_SZ):
        o = i * PHDR_SZ
        name = raw[o:o + 20]
        preset, bank, pbag = struct.unpack_from("<HHH", raw, o + 20)
        lib, genre, morph = struct.unpack_from("<III", raw, o + 26)
        f["phdr"].append(dict(name=name, preset=preset, bank=bank, pbag=pbag,
                              lib=lib, genre=genre, morph=morph))
    # bags (gen_ndx, mod_ndx)
    def bags(name, sz):
        raw = recs(name, sz)
        return [struct.unpack_from("<HH", raw, i * sz) for i in range(len(raw) // sz)]
    f["pbag"] = [dict(gen=g, mod=m) for g, m in bags("pbag", PBAG_SZ)]
    f["ibag"] = [dict(gen=g, mod=m) for g, m in bags("ibag", IBAG_SZ)]
    # gens (oper, amount) — keep amount as raw 2 bytes; interpret per oper
    def gens(name):
        raw = recs(name, PGEN_SZ)
        out = []
        for i in range(len(raw) // PGEN_SZ):
            oper, = struct.unpack_from("<H", raw, i * PGEN_SZ)
            amt = raw[i * PGEN_SZ + 2:i * PGEN_SZ + 4]
            out.append([oper, amt])
        return out
    f["pgen"] = gens("pgen")
    f["igen"] = gens("igen")
    # mods — raw 10-byte records
    def mods(name, sz):
        raw = recs(name, sz)
        return [bytes(raw[i * sz:(i + 1) * sz]) for i in range(len(raw) // sz)]
    f["pmod"] = mods("pmod", PMOD_SZ)
    f["imod"] = mods("imod", IMOD_SZ)
    # inst
    f["inst"] = []
    raw = recs("inst", INST_SZ)
    for i in range(len(raw) // INST_SZ):
        o = i * INST_SZ
        name = raw[o:o + 20]
        ibag, = struct.unpack_from("<H", raw, o + 20)
        f["inst"].append(dict(name=name, ibag=ibag))
    # shdr
    f["shdr"] = []
    raw = recs("shdr", SHDR_SZ)
    for i in range(len(raw) // SHDR_SZ):
        o = i * SHDR_SZ
        name = raw[o:o + 20]
        start, end, sloop, eloop, rate = struct.unpack_from("<IIIII", raw, o + 20)
        opitch, pcorr, link, stype = struct.unpack_from("<BbHH", raw, o + 40)
        f["shdr"].append(dict(name=name, start=start, end=end, sloop=sloop, eloop=eloop,
                              rate=rate, opitch=opitch, pcorr=pcorr, link=link, stype=stype))
    return f


def _u16(amt_bytes):
    return struct.unpack("<H", amt_bytes)[0]


# ------------------------------------------------------------------ helpers
def preset_list(f):
    """Real presets (drop the terminal EOP record). Returns list of (phdr_index, rec)."""
    return [(i, p) for i, p in enumerate(f["phdr"][:-1])]


def zone_gens(f, level, gen_ndx0, gen_ndx1):
    genlist = f["pgen"] if level == "p" else f["igen"]
    return genlist[gen_ndx0:gen_ndx1]


def preset_zone_bounds(f, pi):
    return f["phdr"][pi]["pbag"], f["phdr"][pi + 1]["pbag"]


def inst_zone_bounds(f, ii):
    return f["inst"][ii]["ibag"], f["inst"][ii + 1]["ibag"]


# ------------------------------------------------------------------ the merge
def _mark_from_presets(f, preset_indices, need_inst, need_shdr):
    """Walk phdr->pgen(inst)->igen(sampleID)->shdr, filling need_inst/need_shdr sets."""
    for pi in preset_indices:
        b0, b1 = preset_zone_bounds(f, pi)
        for z in range(b0, b1):
            g0, g1 = f["pbag"][z]["gen"], f["pbag"][z + 1]["gen"]
            for oper, amt in zone_gens(f, "p", g0, g1):
                if oper == GEN_INSTRUMENT:
                    need_inst.add(_u16(amt))
    # close over instruments -> samples
    frontier = list(need_inst)
    for ii in frontier:
        ib0, ib1 = inst_zone_bounds(f, ii)
        for z in range(ib0, ib1):
            g0, g1 = f["ibag"][z]["gen"], f["ibag"][z + 1]["gen"]
            for oper, amt in zone_gens(f, "i", g0, g1):
                if oper == GEN_SAMPLEID:
                    sid = _u16(amt)
                    need_shdr.add(sid)
                    link = f["shdr"][sid]["link"]
                    # pull stereo link partner so wSampleLink stays valid
                    if f["shdr"][sid]["stype"] in (2, 4) and link < len(f["shdr"]):
                        need_shdr.add(link)


def merge(base, drum, *, drum_bank=DRUM_BANK, dest_bank=DRUM_BANK,
          keep_base_drums=False):
    """Return assembled SF2 bytes: base melodic presets + drum font's <drum_bank> kit."""
    # 1) pick final preset set --------------------------------------------------
    base_keep = []
    for pi, p in preset_list(base):
        if p["bank"] == DRUM_BANK and not keep_base_drums:
            continue
        base_keep.append(pi)
    drum_keep = [pi for pi, p in preset_list(drum) if p["bank"] == drum_bank]
    if not drum_keep:
        raise SystemExit("drum font has no preset in bank %d — run --list on it to see its banks"
                         % drum_bank)

    # 2) mark reachable inst/shdr in each source -------------------------------
    need = {"base": (set(), set()), "drum": (set(), set())}
    _mark_from_presets(base, base_keep, *need["base"])
    _mark_from_presets(drum, drum_keep, *need["drum"])

    # 3) dense remap tables (shared new index space: base first, then drum) ----
    base_inst_ids = sorted(need["base"][0]); drum_inst_ids = sorted(need["drum"][0])
    base_shdr_ids = sorted(need["base"][1]); drum_shdr_ids = sorted(need["drum"][1])
    inst_map = {("base", o): n for n, o in enumerate(base_inst_ids)}
    inst_map.update({("drum", o): n + len(base_inst_ids) for n, o in enumerate(drum_inst_ids)})
    shdr_map = {("base", o): n for n, o in enumerate(base_shdr_ids)}
    shdr_map.update({("drum", o): n + len(base_shdr_ids) for n, o in enumerate(drum_shdr_ids)})

    # 4) rebuild smpl + shdr ----------------------------------------------------
    new_shdr = []          # emitted shdr dicts (new order)
    smpl_parts = []
    cursor = 0             # frames
    ordered_shdr = [("base", o) for o in base_shdr_ids] + [("drum", o) for o in drum_shdr_ids]
    for src, o in ordered_shdr:
        f = base if src == "base" else drum
        s = dict(f["shdr"][o])
        pcm16 = f["pcm"]
        seg = bytes(pcm16[s["start"] * 2:s["end"] * 2])
        nframes = len(seg) // 2
        ns = cursor
        ne = cursor + nframes
        sl = ns + (s["sloop"] - s["start"])
        el = ns + (s["eloop"] - s["start"])
        # remap stereo link to the partner's NEW index (partner guaranteed present)
        if s["stype"] in (2, 4) and ("%s" % src, s["link"]) and (src, s["link"]) in shdr_map:
            s["link"] = shdr_map[(src, s["link"])]
        else:
            s["link"] = 0
            if s["stype"] in (2, 4):
                s["stype"] = 1  # partner missing -> demote to mono
        s.update(start=ns, end=ne, sloop=sl, eloop=el)
        new_shdr.append(s)
        smpl_parts.append(seg)
        smpl_parts.append(b"\x00\x00" * GUARD)
        cursor = ne + GUARD
    # terminal EOS record
    new_shdr.append(dict(name=b"EOS", start=0, end=0, sloop=0, eloop=0, rate=0,
                         opitch=0, pcorr=0, link=0, stype=0))
    new_smpl = b"".join(smpl_parts)
    if len(new_smpl) & 1:
        new_smpl += b"\x00"

    # 5) rebuild inst / ibag / igen / imod -------------------------------------
    new_inst, new_ibag, new_igen, new_imod = [], [], [], []
    ordered_inst = [("base", o) for o in base_inst_ids] + [("drum", o) for o in drum_inst_ids]
    for src, o in ordered_inst:
        f = base if src == "base" else drum
        new_inst.append(dict(name=f["inst"][o]["name"], ibag=len(new_ibag)))
        ib0, ib1 = inst_zone_bounds(f, o)
        for z in range(ib0, ib1):
            g0, g1 = f["ibag"][z]["gen"], f["ibag"][z + 1]["gen"]
            m0, m1 = f["ibag"][z]["mod"], f["ibag"][z + 1]["mod"]
            new_ibag.append(dict(gen=len(new_igen), mod=len(new_imod)))
            for oper, amt in f["igen"][g0:g1]:
                if oper == GEN_SAMPLEID:
                    amt = struct.pack("<H", shdr_map[(src, _u16(amt))])
                new_igen.append([oper, amt])
            for md in f["imod"][m0:m1]:
                new_imod.append(md)
    new_inst.append(dict(name=b"EOI", ibag=len(new_ibag)))     # terminal
    new_ibag.append(dict(gen=len(new_igen), mod=len(new_imod)))
    new_igen.append([0, b"\x00\x00"])
    new_imod.append(b"\x00" * IMOD_SZ)

    # 6) rebuild phdr / pbag / pgen / pmod -------------------------------------
    new_phdr, new_pbag, new_pgen, new_pmod = [], [], [], []
    ordered_pre = [("base", pi) for pi in base_keep] + [("drum", pi) for pi in drum_keep]
    for src, pi in ordered_pre:
        f = base if src == "base" else drum
        p = f["phdr"][pi]
        bank = dest_bank if (src == "drum") else p["bank"]
        new_phdr.append(dict(name=p["name"], preset=p["preset"], bank=bank, pbag=len(new_pbag),
                             lib=p["lib"], genre=p["genre"], morph=p["morph"]))
        b0, b1 = preset_zone_bounds(f, pi)
        for z in range(b0, b1):
            g0, g1 = f["pbag"][z]["gen"], f["pbag"][z + 1]["gen"]
            m0, m1 = f["pbag"][z]["mod"], f["pbag"][z + 1]["mod"]
            new_pbag.append(dict(gen=len(new_pgen), mod=len(new_pmod)))
            for oper, amt in f["pgen"][g0:g1]:
                if oper == GEN_INSTRUMENT:
                    amt = struct.pack("<H", inst_map[(src, _u16(amt))])
                new_pgen.append([oper, amt])
            for md in f["pmod"][m0:m1]:
                new_pmod.append(md)
    new_phdr.append(dict(name=b"EOP", preset=0, bank=0, pbag=len(new_pbag),
                         lib=0, genre=0, morph=0))            # terminal
    new_pbag.append(dict(gen=len(new_pgen), mod=len(new_pmod)))
    new_pgen.append([0, b"\x00\x00"])
    new_pmod.append(b"\x00" * PMOD_SZ)

    return _assemble(base.get("info"), new_smpl, new_phdr, new_pbag, new_pmod, new_pgen,
                     new_inst, new_ibag, new_imod, new_igen, new_shdr)


# ------------------------------------------------------------------ writing
def _assemble(info_body, smpl, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr):
    def chunk(cid, body):
        out = cid + struct.pack("<I", len(body)) + body
        if len(body) & 1:
            out += b"\x00"
        return out

    def pack_phdr(p):
        return (p["name"][:20].ljust(20, b"\x00")
                + struct.pack("<HHH", p["preset"], p["bank"], p["pbag"])
                + struct.pack("<III", p["lib"], p["genre"], p["morph"]))

    def pack_inst(i):
        return i["name"][:20].ljust(20, b"\x00") + struct.pack("<H", i["ibag"])

    def pack_shdr(s):
        return (s["name"][:20].ljust(20, b"\x00")
                + struct.pack("<IIIII", s["start"], s["end"], s["sloop"], s["eloop"], s["rate"])
                + struct.pack("<BbHH", s["opitch"], s["pcorr"], s["link"], s["stype"]))

    phdr_b = b"".join(pack_phdr(p) for p in phdr)
    pbag_b = b"".join(struct.pack("<HH", b["gen"], b["mod"]) for b in pbag)
    pmod_b = b"".join(pmod)
    pgen_b = b"".join(struct.pack("<H", o) + a for o, a in pgen)
    inst_b = b"".join(pack_inst(i) for i in inst)
    ibag_b = b"".join(struct.pack("<HH", b["gen"], b["mod"]) for b in ibag)
    imod_b = b"".join(imod)
    igen_b = b"".join(struct.pack("<H", o) + a for o, a in igen)
    shdr_b = b"".join(pack_shdr(s) for s in shdr)

    if info_body is None:
        info_body = (chunk(b"ifil", struct.pack("<HH", 2, 1))
                     + chunk(b"isng", b"EMU8000\x00")
                     + chunk(b"INAM", b"T-DSP drum merge\x00"))
    info = b"INFO" + info_body
    sdta = b"sdta" + chunk(b"smpl", smpl)
    pdta = (b"pdta"
            + chunk(b"phdr", phdr_b) + chunk(b"pbag", pbag_b) + chunk(b"pmod", pmod_b)
            + chunk(b"pgen", pgen_b) + chunk(b"inst", inst_b) + chunk(b"ibag", ibag_b)
            + chunk(b"imod", imod_b) + chunk(b"igen", igen_b) + chunk(b"shdr", shdr_b))
    body = chunk(b"LIST", info) + chunk(b"LIST", sdta) + chunk(b"LIST", pdta)
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"sfbk" + body


# ------------------------------------------------------------------ inspection
def list_font(path):
    f = parse(path)
    print("%s" % path)
    print("  samples: %d   instruments: %d   presets: %d   smpl: %.2f MB"
          % (len(f["shdr"]) - 1, len(f["inst"]) - 1, len(f["phdr"]) - 1,
             len(f["pcm"]) / 1048576))
    banks = {}
    for _, p in preset_list(f):
        banks.setdefault(p["bank"], []).append((p["preset"], p["name"].split(b"\x00")[0].decode("latin1", "replace")))
    for bank in sorted(banks):
        tag = " (PERCUSSION)" if bank == DRUM_BANK else ""
        names = ", ".join("%d:%s" % (pr, nm) for pr, nm in sorted(banks[bank])[:8])
        more = "" if len(banks[bank]) <= 8 else "  +%d more" % (len(banks[bank]) - 8)
        print("  bank %3d%s: %s%s" % (bank, tag, names, more))


# ------------------------------------------------------------------ self-test
def _synth_font(name, presets, samples):
    """Build a minimal-but-valid SF2 in memory for testing.
    presets: list of (bank, preset, inst_index)
    samples: list of (name, [int16 frames])   -> one inst per sample, mono, one zone.
    Each preset points at one instrument; each instrument at one sample of same index
    (we keep it 1:1 for a predictable graph). Returns parse()d dict via bytes round-trip.
    """
    import numpy as np
    # samples -> shdr + smpl
    shdr = []; pcm = []; cur = 0
    for nm, frames in samples:
        arr = np.asarray(frames, dtype="<i2")
        st = cur; en = cur + arr.size
        shdr.append(dict(name=nm.encode(), start=st, end=en, sloop=st, eloop=en,
                         rate=44100, opitch=60, pcorr=0, link=0, stype=1))
        pcm.append(arr.tobytes()); pcm.append(b"\x00\x00" * GUARD); cur = en + GUARD
    shdr.append(dict(name=b"EOS", start=0, end=0, sloop=0, eloop=0, rate=0,
                     opitch=0, pcorr=0, link=0, stype=0))
    smpl = b"".join(pcm)
    # one instrument per sample: single zone with sampleID gen
    inst = []; ibag = []; igen = []; imod = [b"\x00" * IMOD_SZ]
    for si in range(len(samples)):
        inst.append(dict(name=("i%d" % si).encode(), ibag=len(ibag)))
        ibag.append(dict(gen=len(igen), mod=0))
        igen.append([GEN_SAMPLEID, struct.pack("<H", si)])
    inst.append(dict(name=b"EOI", ibag=len(ibag)))
    ibag.append(dict(gen=len(igen), mod=0)); igen.append([0, b"\x00\x00"])
    # presets: single zone with instrument gen
    phdr = []; pbag = []; pgen = []; pmod = [b"\x00" * PMOD_SZ]
    for bank, preset, ii in presets:
        phdr.append(dict(name=("%s_b%dp%d" % (name, bank, preset)).encode(),
                         preset=preset, bank=bank, pbag=len(pbag), lib=0, genre=0, morph=0))
        pbag.append(dict(gen=len(pgen), mod=0))
        pgen.append([GEN_INSTRUMENT, struct.pack("<H", ii)])
    phdr.append(dict(name=b"EOP", preset=0, bank=0, pbag=len(pbag), lib=0, genre=0, morph=0))
    pbag.append(dict(gen=len(pgen), mod=0)); pgen.append([0, b"\x00\x00"])
    raw = _assemble(None, smpl, phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr)
    return parse(raw)


def selftest():
    import numpy as np
    # BASE: two melodic presets (piano bank0/p0 -> sampleA, strings bank0/p48 -> sampleB)
    #       and a WEAK drum kit (bank128/p0 -> sampleC). sampleC must be pruned after merge.
    base = _synth_font("base",
        presets=[(0, 0, 0), (0, 48, 1), (128, 0, 2)],
        samples=[("pianoA", list(range(1, 101))),
                 ("strngB", list(range(200, 260))),
                 ("wkdrumC", [7] * 40)])
    # DRUM: a good bank-128 kit (bank128/p0 -> sampleD) plus junk melodic preset that must NOT
    #       come across (bank0/p0 -> sampleE, unreached -> pruned).
    drum = _synth_font("drum",
        presets=[(128, 0, 0), (0, 0, 1)],
        samples=[("goodkitD", list(range(1000, 1120))),
                 ("junkE", [3] * 10)])

    out = merge(base, drum)
    m = parse(out)

    # -- assertions ----------------------------------------------------------
    banks = {(p["bank"], p["preset"]): p for _, p in preset_list(m)}
    assert (0, 0) in banks and (0, 48) in banks, "base melodic presets lost"
    assert (128, 0) in banks, "merged font has no bank-128 drum preset"
    # the bank-128 preset must resolve to the DRUM font's good kit sample, not the base's weak one
    names = {s["name"].split(b"\x00")[0] for s in m["shdr"][:-1]}
    assert b"goodkitD" in names, "drum kit sample missing from merge"
    assert b"wkdrumC" not in names, "base weak-drum sample was NOT pruned (GC failed)"
    assert b"junkE" not in names, "unreached drum-font sample leaked in (GC failed)"
    assert b"pianoA" in names and b"strngB" in names, "base melodic samples lost"
    # follow the (128,0) preset graph end-to-end and confirm PCM integrity
    def sample_of_preset(f, bank, preset):
        pi = next(i for i, p in preset_list(f) if p["bank"] == bank and p["preset"] == preset)
        b0, b1 = preset_zone_bounds(f, pi)
        ii = None
        for z in range(b0, b1):
            for oper, amt in f["pgen"][f["pbag"][z]["gen"]:f["pbag"][z + 1]["gen"]]:
                if oper == GEN_INSTRUMENT: ii = _u16(amt)
        ib0, ib1 = inst_zone_bounds(f, ii)
        for z in range(ib0, ib1):
            for oper, amt in f["igen"][f["ibag"][z]["gen"]:f["ibag"][z + 1]["gen"]]:
                if oper == GEN_SAMPLEID:
                    return f["shdr"][_u16(amt)]
        raise AssertionError("no sample reached")
    s = sample_of_preset(m, 128, 0)
    pcm = np.frombuffer(bytes(m["pcm"][s["start"] * 2:s["end"] * 2]), dtype="<i2")
    assert list(pcm) == list(range(1000, 1120)), "drum PCM corrupted through merge"
    # index sanity: every sampleID/instrument ref in range; no dangling
    assert len(m["shdr"]) - 1 == 3, "expected exactly 3 samples after GC (A,B,D), got %d" % (len(m["shdr"]) - 1)
    assert len(m["inst"]) - 1 == 3, "expected 3 instruments after GC"
    for oper, amt in m["igen"][:-1]:
        if oper == GEN_SAMPLEID:
            assert _u16(amt) < len(m["shdr"]) - 1, "dangling sampleID"
    for oper, amt in m["pgen"][:-1]:
        if oper == GEN_INSTRUMENT:
            assert _u16(amt) < len(m["inst"]) - 1, "dangling instrument ref"
    # round-trips through the parser cleanly
    parse(out)
    print("selftest OK — merge pruned base drums, grafted drum kit, remapped all indices, "
          "PCM intact, %d samples / %d instruments / %d presets in output."
          % (len(m["shdr"]) - 1, len(m["inst"]) - 1, len(m["phdr"]) - 1))


# ------------------------------------------------------------------ cli
def main(argv):
    if "--selftest" in argv:
        selftest(); return
    if "--list" in argv:
        list_font(argv[argv.index("--list") + 1]); return
    pos = [a for a in argv if not a.startswith("--")]
    opts = argv
    if len(pos) < 3:
        print(__doc__); sys.exit(1)
    base_p, drum_p, out_p = pos[0], pos[1], pos[2]

    def opt(name, default):
        return int(opts[opts.index(name) + 1]) if name in opts else default
    drum_bank = opt("--drum-bank", DRUM_BANK)
    dest_bank = opt("--dest-bank", DRUM_BANK)
    keep = "--keep-base-drums" in opts

    print("base : ", end=""); list_font(base_p)
    print("drum : ", end=""); list_font(drum_p)
    out = merge(parse(base_p), parse(drum_p),
                drum_bank=drum_bank, dest_bank=dest_bank, keep_base_drums=keep)
    open(out_p, "wb").write(out)
    m = parse(out)
    print("\nwrote %s  (%.2f MB smpl, %d samples, %d instruments, %d presets)"
          % (out_p, len(m["pcm"]) / 1048576, len(m["shdr"]) - 1,
             len(m["inst"]) - 1, len(m["phdr"]) - 1))
    print("  -> copy to the SD card at the path your build env expects (see DRUM_FONTS.md /")
    print("     tools/sf2/FONTS.md), e.g. /sf2/gm_tsf.sf2, then send 'T' over serial to sweep-test.")


if __name__ == "__main__":
    main(sys.argv[1:])
