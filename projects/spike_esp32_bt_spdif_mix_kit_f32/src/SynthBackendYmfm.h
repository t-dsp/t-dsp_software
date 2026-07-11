// SynthBackendYmfm.h — ymfm OPM MULTITIMBRAL backend for the mix-kit.
//
// The BSD-3-Clause counterpart to SynthBackendDexed.h (same interface). Runs
// FOUR independent OPM banks routed by MIDI channel via YmfmOpmMulti, so the
// player's multi-channel songs play a DIFFERENT instrument per part. Program
// Change events in the song pick each channel's patch from the instrument
// catalog.
//
// Instrument catalog = a baked VOPM bank (8 patches) + every /ymfm/*.opm on the
// SD card (parsed at boot, no rebuild to add sounds) — parity with
// spike_midi_ymfm_opm, so this full BT + S/PDIF + app firmware is the single
// canonical build.
//
// Banks are int16 stereo -> int16 sub-mix -> bridged to F32 into mix slot 3
// (the mix-kit bus is F32). Included by main.cpp AFTER outL/outR + g_sdReady.
#pragma once
#include <AudioSynthYmfmOPM.h>
#include <YmfmOpmMulti.h>
#include <OpmBank.h>
#include "YmfmMultiSink.h"
#include "opm_bank_data.h"   // kBakedOpmBank — 8 baked VOPM instruments

constexpr int kBanks = 4;
AudioSynthYmfmOPM g_b0, g_b1, g_b2, g_b3;               // 4 OPM chips (each stereo int16)
AudioMixer4       g_synMixL, g_synMixR;                  // int16 sub-mix of the 4 banks
AudioConnection   sb0L(g_b0, 0, g_synMixL, 0), sb0R(g_b0, 1, g_synMixR, 0);
AudioConnection   sb1L(g_b1, 0, g_synMixL, 1), sb1R(g_b1, 1, g_synMixR, 1);
AudioConnection   sb2L(g_b2, 0, g_synMixL, 2), sb2R(g_b2, 1, g_synMixR, 2);
AudioConnection   sb3L(g_b3, 0, g_synMixL, 3), sb3R(g_b3, 1, g_synMixR, 3);
AudioConvert_I16toF32 g_synthToF32L, g_synthToF32R;      // int16 sub-mix -> F32
AudioConnection     c_synMixL(g_synMixL, 0, g_synthToF32L, 0);
AudioConnection     c_synMixR(g_synMixR, 0, g_synthToF32R, 0);
AudioConnection_F32 c_synthL (g_synthToF32L, 0, outL, 3);
AudioConnection_F32 c_synthR (g_synthToF32R, 0, outR, 3);

AudioSynthYmfmOPM *g_banks[kBanks] = { &g_b0, &g_b1, &g_b2, &g_b3 };
tdsp::ymfmopm::YmfmOpmMulti g_multi(g_banks, kBanks);
YmfmMultiSink   g_ymfmSink(&g_multi);
tdsp::MidiSink *g_synthSink = &g_ymfmSink;

// Instrument catalog: baked VOPM bank + SD /ymfm/*.opm, parsed once at boot.
static const int kMaxInstr = 48;
DMAMEM static tdsp::ymfmopm::OpmVoice        g_instr[kMaxInstr];      // OCRAM
static const tdsp::ymfmopm::OpmVoice        *g_instrPtrs[kMaxInstr];  // for setVoiceTable
static int  g_numInstr = 0;
static int  g_synthInstrument = 0;
DMAMEM static char g_opmFileBuf[24000];   // scratch to read one .opm file (OCRAM)

static const char *synthName()        { return "ymfm OPM x4"; }
static const char *synthDescription() { return "YM2151 (OPM) 4-op FM, 4-part multitimbral: songs play a patch per channel. Add banks in /ymfm/*.opm."; }
static int         synthNumInstruments()        { return g_numInstr; }
static const char *synthInstrumentName(int i)   { return (i >= 0 && i < g_numInstr) ? g_instr[i].name : ""; }
static int         synthInstrument()            { return g_synthInstrument; }

// The app's single picker forces ALL banks to one catalog patch (handy for live
// MIDI / auditioning); a song's Program Change events re-diversify per channel.
static void synthSetInstrument(int idx) {
    if (g_numInstr == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_numInstr) idx = g_numInstr - 1;
    for (int i = 0; i < kBanks; i++) g_multi.setBankVoice(i, g_instr[idx]);
    g_synthInstrument = idx;
    Serial.printf("[synth] all banks -> instrument %d = %s\n", idx, g_instr[idx].name);
}

#if TDSP_HAS_SDCARD
// Parse every /ymfm/*.opm on the card onto the end of the catalog.
static void synthScanOpmDir(const char *dir) {
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    for (File f = d.openNextFile(); f && g_numInstr < kMaxInstr; f = d.openNextFile()) {
        const char *nm = f.name();
        size_t L = nm ? strlen(nm) : 0;
        if (!f.isDirectory() && L > 4 && strcasecmp(nm + L - 4, ".opm") == 0) {
            size_t n = f.read(g_opmFileBuf, sizeof(g_opmFileBuf) - 1);
            g_opmFileBuf[n] = 0;
            int got = tdsp::ymfmopm::parseOpmBank(g_opmFileBuf, n,
                                                  g_instr + g_numInstr, kMaxInstr - g_numInstr);
            Serial.printf("[ymfm] %s: +%d instruments\n", nm, got);
            g_numInstr += got;
        }
        f.close();
    }
    d.close();
}
#endif

static void synthBegin() {
    g_multi.begin();
    // Build the catalog: baked bank first, then SD /ymfm/*.opm.
    g_numInstr = tdsp::ymfmopm::parseOpmBank(kBakedOpmBank, strlen(kBakedOpmBank), g_instr, kMaxInstr);
#if TDSP_HAS_SDCARD
    if (g_sdReady) {
        if (!SD.exists("/ymfm")) SD.mkdir("/ymfm");   // a home to drop .opm banks into
        synthScanOpmDir("/ymfm");
    }
#endif
    for (int i = 0; i < g_numInstr; i++) g_instrPtrs[i] = &g_instr[i];
    g_multi.setVoiceTable(g_instrPtrs, g_numInstr);   // song Program Change -> catalog voice
    // Seed each bank with a distinct patch so a multi-channel song is instantly
    // multi-instrument even before its first Program Change.
    if (g_numInstr > 0)
        for (int i = 0; i < kBanks; i++) g_multi.setBankVoice(i, g_instr[i % g_numInstr]);
    const float g = 0.7f;                              // per-bank sub-mix headroom
    g_synMixL.gain(0, g); g_synMixL.gain(1, g); g_synMixL.gain(2, g); g_synMixL.gain(3, g);
    g_synMixR.gain(0, g); g_synMixR.gain(1, g); g_synMixR.gain(2, g); g_synMixR.gain(3, g);
    Serial.printf("[synth] ymfm OPM x4 ready: %d instruments\n", g_numInstr);
}
