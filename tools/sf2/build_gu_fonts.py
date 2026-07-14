#!/usr/bin/env python3
"""
build_gu_fonts.py — make PSRAM-sized GeneralUser variants for the TSF engine.

TSF (lib/TDspTsf) loads a whole SoundFont's sample data resident in PSRAM (int16, the
T-DSP patch). GeneralUser's sample chunk is ~30.6 MB, which fits neither 8 MB nor 16 MB
Teensy PSRAM. This tool produces downsampled variants (per-sample anti-aliased resample to
a target rate ceiling) so the font fits a chosen PSRAM budget, letting us A/B fidelity vs.
size on device. TimGM6mb stays the small-tier baseline; these are the GeneralUser tier.

What it does, correctly:
  * Resamples only samples whose rate is ABOVE the target ceiling (others untouched).
  * scipy resample_poly = polyphase FIR with anti-aliasing (no cheap decimation aliasing).
  * Rewrites shdr start/end/startloop/endloop (absolute frame indices) + dwSampleRate.
  * Scales the per-zone FINE sample-address-offset generators (startAddrsOffset=0,
    endAddrsOffset=1, startloopAddrsOffset=2, endloopAddrsOffset=3) by each sample's
    resample ratio, IN PLACE (downsampling only shrinks them, so no int16 overflow and no
    gen-list restructuring). Samples referenced by a zone carrying a COARSE offset gen
    (4/12/45/50) are kept at native rate, so coarse offsets never need touching.
  * Keeps sample count/order (so wSampleLink + sampleID gens stay valid), preserves all
    other chunks (INFO/phdr/pbag/pmod/pgen/inst/ibag/imod), 46-frame inter-sample guard.

Usage:  python build_gu_fonts.py <GeneralUser.sf2> <out_dir> [rate1 rate2 ...]
Default rates: 22050 16000 12000 8000   (measure the emitted sizes and pick per budget).
"""
import sys, os, struct, math
import numpy as np
from scipy.signal import resample_poly

# --- SFGenerator opers we care about (see lib/TDspSF2/src/sf22aswt/sf22aswt_enums.h) ---
GEN_START_FINE, GEN_END_FINE, GEN_STARTLOOP_FINE, GEN_ENDLOOP_FINE = 0, 1, 2, 3
GEN_COARSE = {4, 12, 45, 50}           # start/end/startloop/endloop coarse offsets
FINE_OFFSET_GENS = {GEN_START_FINE, GEN_END_FINE, GEN_STARTLOOP_FINE, GEN_ENDLOOP_FINE}
GEN_SAMPLEID = 53
SHDR_SZ, INST_SZ, BAG_SZ, GEN_SZ = 46, 22, 4, 4
GUARD = 46                             # zero frames between samples (SF2 spec minimum)


def read_chunks(buf, off, end):
    while off + 8 <= end:
        cid = buf[off:off + 4]
        sz = struct.unpack_from("<I", buf, off + 4)[0]
        yield cid, off + 8, sz
        off += 8 + sz + (sz & 1)


def parse(path):
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"sfbk"
    riffsz = struct.unpack_from("<I", data, 4)[0]
    sub = {}
    for cid, doff, sz in read_chunks(data, 12, 8 + riffsz):
        if cid == b"LIST":
            for scid, sdoff, ssz in read_chunks(data, doff + 4, doff + sz):
                sub[scid.decode("latin1")] = (sdoff, ssz)
    return data, sub


def build(src, sub, target_rate, out_path):
    smpl_off, smpl_sz = sub["smpl"]
    pcm = np.frombuffer(src, dtype="<i2", count=smpl_sz // 2, offset=smpl_off)

    # shdr records
    shdr_off, shdr_sz = sub["shdr"]
    nsh = shdr_sz // SHDR_SZ
    shdr = []
    for i in range(nsh):
        o = shdr_off + i * SHDR_SZ
        name = src[o:o + 20]
        start, end, sloop, eloop, rate = struct.unpack_from("<IIIII", src, o + 20)
        opitch, pcorr, link, stype = struct.unpack_from("<BBHH", src, o + 40)
        shdr.append(dict(name=name, start=start, end=end, sloop=sloop, eloop=eloop,
                         rate=rate, opitch=opitch, pcorr=pcorr, link=link, stype=stype))

    # zones -> which sample each references + its offset gens (parse inst/ibag/igen)
    inst_off, inst_sz = sub["inst"]
    ibag_off, ibag_sz = sub["ibag"]
    igen_off, igen_sz = sub["igen"]
    n_inst, n_ibag, n_igen = inst_sz // INST_SZ, ibag_sz // BAG_SZ, igen_sz // GEN_SZ
    inst_bag = [struct.unpack_from("<H", src, inst_off + i * INST_SZ + 20)[0] for i in range(n_inst)]
    ibag_gen = [struct.unpack_from("<H", src, ibag_off + i * BAG_SZ)[0] for i in range(n_ibag)]
    igen = bytearray(src[igen_off:igen_off + igen_sz])   # mutable copy (we rescale fine offsets)

    def gen(k):
        return struct.unpack_from("<H", igen, k * GEN_SZ)[0], struct.unpack_from("<h", igen, k * GEN_SZ + 2)[0]

    # First pass: any sample touched by a zone with a COARSE offset gen stays native.
    pin_native = set()
    zone_ranges = []  # (sampleID, [fine-gen igen-indices]) for second pass
    for i in range(n_inst - 1):                       # last inst = terminal
        for z in range(inst_bag[i], inst_bag[i + 1]):
            g0, g1 = ibag_gen[z], ibag_gen[z + 1]
            sid = None; fine = []; has_coarse = False
            for k in range(g0, g1):
                oper, amt = gen(k)
                if oper == GEN_SAMPLEID: sid = amt & 0xFFFF
                elif oper in GEN_COARSE and amt != 0: has_coarse = True
                elif oper in FINE_OFFSET_GENS and amt != 0: fine.append(k)
            if sid is None: continue
            if has_coarse: pin_native.add(sid)
            zone_ranges.append((sid, fine))

    # Per-sample resample ratio.
    ratio = [1.0] * nsh
    for i in range(nsh - 1):                          # last shdr = terminal EOS
        r = shdr[i]["rate"]
        if r > target_rate and i not in pin_native and r > 0:
            ratio[i] = target_rate / r

    # Second pass: scale fine offset gens in place by their sample's ratio.
    for sid, fine in zone_ranges:
        if sid >= nsh or ratio[sid] == 1.0:
            continue
        for k in fine:
            oper, amt = gen(k)
            struct.pack_into("<h", igen, k * GEN_SZ + 2, int(round(amt * ratio[sid])))

    # Rebuild smpl + shdr.
    out_pcm = []
    cursor = 0
    for i in range(nsh):
        s = shdr[i]
        if i == nsh - 1:                              # terminal record: keep zeroed
            s["start"] = s["end"] = s["sloop"] = s["eloop"] = 0
            continue
        seg = pcm[s["start"]:s["end"]]
        r = ratio[i]
        if r != 1.0 and seg.size:
            up, down = target_rate, s["rate"]
            g = math.gcd(up, down); up //= g; down //= g
            f = seg.astype(np.float32)
            rs = resample_poly(f, up, down)
            rs = np.clip(np.round(rs), -32768, 32767).astype("<i2")
            newrate = target_rate
        else:
            rs = seg.astype("<i2")
            newrate = s["rate"]
        sl = s["sloop"] - s["start"]; el = s["eloop"] - s["start"]
        if r != 1.0:
            sl = int(round(sl * r)); el = int(round(el * r))
        ns = cursor
        ne = cursor + rs.size
        s["start"], s["end"] = ns, ne
        s["sloop"] = ns + max(0, min(sl, rs.size))
        s["eloop"] = ns + max(0, min(el, rs.size))
        s["rate"] = newrate
        out_pcm.append(rs)
        out_pcm.append(np.zeros(GUARD, dtype="<i2"))
        cursor = ne + GUARD

    new_smpl = np.concatenate(out_pcm).tobytes() if out_pcm else b""
    if len(new_smpl) & 1:
        new_smpl += b"\x00"

    new_shdr = bytearray()
    for s in shdr:
        new_shdr += s["name"][:20].ljust(20, b"\x00")
        new_shdr += struct.pack("<IIIII", s["start"], s["end"], s["sloop"], s["eloop"], s["rate"])
        new_shdr += struct.pack("<BBHH", s["opitch"], s["pcorr"], s["link"], s["stype"])

    # Reassemble RIFF: LIST INFO | LIST sdta(smpl) | LIST pdta(...)
    def chunk(cid, body):
        out = cid + struct.pack("<I", len(body)) + body
        if len(body) & 1: out += b"\x00"
        return out

    info_off, info_sz = sub["ifil"][0], None
    # INFO: copy the whole original INFO LIST body verbatim.
    def list_body(names, overrides):
        b = b""
        for nm in names:
            off, sz = sub[nm]
            body = overrides.get(nm, src[off:off + sz])
            b += chunk(nm.encode("latin1"), body)
        return b

    info_names = [n for n in ["ifil", "isng", "INAM", "IENG", "IPRD", "ICOP", "ICMT", "ISFT", "ICRD"] if n in sub]
    info = b"INFO" + list_body(info_names, {})
    sdta = b"sdta" + chunk(b"smpl", new_smpl)
    pdta_names = ["phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"]
    pdta = b"pdta" + list_body(pdta_names, {"igen": bytes(igen), "shdr": bytes(new_shdr)})

    body = chunk(b"LIST", info) + chunk(b"LIST", sdta) + chunk(b"LIST", pdta)
    riff = b"RIFF" + struct.pack("<I", 4 + len(body)) + b"sfbk" + body
    open(out_path, "wb").write(riff)
    return len(new_smpl)


def main():
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    srcpath, outdir = sys.argv[1], sys.argv[2]
    rates = [int(x) for x in sys.argv[3:]] or [22050, 16000, 12000, 8000]
    os.makedirs(outdir, exist_ok=True)
    src, sub = parse(srcpath)
    print("source smpl = %.2f MB" % (sub["smpl"][1] / 1048576))
    for rate in rates:
        out = os.path.join(outdir, "gm_gu%d.sf2" % (rate // 1000))
        smpl = build(src, sub, rate, out)
        print("  %-16s target %5d Hz  smpl %.2f MB  file %.2f MB"
              % (os.path.basename(out), rate, smpl / 1048576, os.path.getsize(out) / 1048576))


if __name__ == "__main__":
    main()
