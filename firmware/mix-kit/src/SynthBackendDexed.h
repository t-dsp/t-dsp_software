// SynthBackendDexed.h — Dexed (synth_dexed, 6-op DX7 FM) backend for the mix-kit.
//
// One of two interchangeable synth backends (see SynthBackendYmfm.h). main.cpp
// includes exactly one, chosen by the TDSP_SYNTH_* build flag, AFTER the mixers
// outL/outR are declared (this header wires the engine into mix slot 3).
//
// Both backends expose the same interface the mix-kit's player, SD catalog, and
// app control use, so nothing outside these headers is engine-specific:
//   tdsp::MidiSink *g_synthSink;   synthBegin(); synthSetInstrument(int);
//   synthNumInstruments(); synthInstrumentName(int); synthInstrument();
//   synthName(); synthDescription();
//
// Dexed is a MONO int16 engine: bridge to F32 and fan into both mix channels.
#pragma once
#include <synth_dexed.h>
#include "DexedSink.h"
#include "DexedVoiceBank.h"
#include "DexedSdBank.h"   // /dexed/*.syx carts off the SD card (thousands of DX7 voices)

AudioSynthDexed       g_dexed(16, AUDIO_SAMPLE_RATE_EXACT);   // 16-voice 6-op FM (int16 out)
AudioConvert_I16toF32 g_synthToF32;                           // int16 -> F32 bridge
AudioConnection       c_synthConv(g_dexed,      0, g_synthToF32, 0);
AudioConnection_F32   c_synthL   (g_synthToF32, 0, outL, 3);  // mono -> both channels
AudioConnection_F32   c_synthR   (g_synthToF32, 0, outR, 3);
DexedSink             g_dexedSink(&g_dexed);
tdsp::MidiSink       *g_synthSink = &g_dexedSink;

// Instrument index space is FLAT: [0 .. 320)   = the bundled PROGMEM voices
// (10 banks x 32, dexed_banks_data.h), then [320 .. 320 + 32*numSdBanks) = any
// /dexed/*.syx carts scanned off the SD card. The app sends a global index as
// @DXVOICE=<i>; names stream as "<bankName>: <voiceName>" so the control page
// regroups the flat list into per-bank sections by splitting on ": ".
static const int kNumBundled = tdsp::dexed::kNumBanks * tdsp::dexed::kVoicesPerBank;  // 320
static int g_synthInstrument = 0;
// Current /dexed cart voice (@DXPICK); empty rel = a bundled/flat voice is current. Read
// by main.cpp's @STATE so the app can restore which cart voice is loaded on reconnect.
static char g_curCartRel[160]  = {0};
static int  g_curCartVoice     = -1;
static char g_curCartName[tdsp::dexed::kVoiceNameBufBytes] = {0};

static const char *synthName()        { return "Dexed"; }
static const char *synthDescription() { return "6-op FM (DX7) synth. Bundled voices + any /dexed/*.syx carts on the SD card."; }
static bool        synthIsGM()         { return false; }  // full DX7 voice set -> names streamed to the app
static void        synthSetMpeMode(bool /*mpe*/) {}       // MPE not wired for this backend (router still bends)
static int         synthNumInstruments()        { return kNumBundled + tdsp::dexed::numSdVoices(); }
static int         synthInstrument()            { return g_synthInstrument; }

// "<bankName>: <voiceName>" for global index i. Returns a pointer to a shared
// static buffer — valid only until the next call, which is fine for the catalog
// stream (each name is printed before the next is fetched) and for logging.
static const char *synthInstrumentName(int i) {
    static char buf[40];
    char vname[tdsp::dexed::kVoiceNameBufBytes];
    if (i < kNumBundled) {
        int bank  = i / tdsp::dexed::kVoicesPerBank;
        int voice = i % tdsp::dexed::kVoicesPerBank;
        if (!tdsp::dexed::copyVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
        snprintf(buf, sizeof(buf), "%s: %s", tdsp::dexed::bankName(bank), vname);
    } else {
        int s     = i - kNumBundled;
        int bank  = s / tdsp::dexed::kVoicesPerBank;
        int voice = s % tdsp::dexed::kVoicesPerBank;
        if (!tdsp::dexed::copySdVoiceName(bank, voice, vname, sizeof(vname))) vname[0] = 0;
        snprintf(buf, sizeof(buf), "%s: %s", tdsp::dexed::sdBankName(bank), vname);
    }
    return buf;
}

// Load a voice by global index (runs from loop/handlers, never the audio ISR).
static void synthSetInstrument(int idx) {
    const int total = kNumBundled + tdsp::dexed::numSdVoices();
    if (idx < 0) idx = 0;
    if (idx >= total) idx = total - 1;
    g_dexed.panic();
    bool ok;
    if (idx < kNumBundled) {
        ok = tdsp::dexed::loadVoice(g_dexed, idx / tdsp::dexed::kVoicesPerBank,
                                             idx % tdsp::dexed::kVoicesPerBank);
    } else {
        int s = idx - kNumBundled;
        ok = tdsp::dexed::loadSdVoice(g_dexed, s / tdsp::dexed::kVoicesPerBank,
                                               s % tdsp::dexed::kVoicesPerBank);
    }
    if (ok) {
        g_synthInstrument = idx;
        g_curCartRel[0] = 0; g_curCartVoice = -1;   // a flat/bundled voice is now current
        Serial.printf("[synth] instrument %d = %s\n", idx, synthInstrumentName(idx));
    }
}

// Load an arbitrary /dexed subfolder cart voice directly (the paged-browser
// @DXPICK path). Returns the voice's display name (or nullptr on failure).
static const char *synthPickCartVoice(const char *relCart, int voice) {
    static char names[tdsp::dexed::kVoicesPerBank][tdsp::dexed::kVoiceNameBufBytes];
    g_dexed.panic();
    if (!tdsp::dexed::sdLoadCartVoice(g_dexed, relCart, voice)) return nullptr;
    int n = tdsp::dexed::sdCartVoiceNames(relCart, names);
    const char *nm = (n == tdsp::dexed::kVoicesPerBank && voice >= 0 &&
                      voice < tdsp::dexed::kVoicesPerBank) ? names[voice] : "";
    snprintf(g_curCartRel, sizeof(g_curCartRel), "%s", relCart);   // remember for @STATE
    g_curCartVoice = voice;
    snprintf(g_curCartName, sizeof(g_curCartName), "%s", nm);
    Serial.printf("[synth] pick %s v%d = %s\n", relCart, voice, nm);
    return nm;
}

static void synthBegin() {
    // Scan /dexed/*.syx off the SD card (bundled voices always present; SD adds
    // thousands more). g_sdReady is set by SD.begin() in setup(), before this.
    // Create the folder if missing so it appears on the USB (MTP) mount as a
    // drop target — same convention as /ymfm and /songs.
    int sdBanks = 0;
    if (g_sdReady) {
        if (!SD.exists("/dexed")) SD.mkdir("/dexed");
        sdBanks = tdsp::dexed::scanSdBanks();
    }
    Serial.printf("[synth] Dexed: %d bundled + %d SD voices (%d /dexed carts)\n",
                  kNumBundled, tdsp::dexed::numSdVoices(), sdBanks);

    // Dexed renders in float then saturates at the float->q15 rail; pull the
    // internal gain below unity so punchy notes don't flat-top (the 0.62 mixer
    // make-up in setup restores the level in the F32 domain).
    g_dexed.setGain(0.8f);
    g_dexedSink.setListenChannel(0);   // omni: one patch plays every channel
    g_dexed.setPitchbendRange(2);
    g_dexed.setPitchbend((int16_t)0);
    g_dexed.setModWheel(0);
    g_dexed.setSustain(false);
    synthSetInstrument(g_synthInstrument);
}
