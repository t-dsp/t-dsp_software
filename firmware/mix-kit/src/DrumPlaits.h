// DrumPlaits.h — a synthesized drum kit from the AUTHENTIC Mutable Plaits drum
// models (engines 13 Bass Drum / 14 Snare / 15 Hi-Hat, Emilie Gillet, MIT). A
// no-PSRAM peer of the OPLL rhythm voice (DrumVoice.h) and the sampled TSF kit
// (DrumTsf.h): a small round-robin POOL of Plaits voices, each reconfigured live
// to the model/pitch/decay for the GM ch10 note it plays. Ported from the proven
// projects/spike_plaits_drums (the spike's crash-loop was 6 voices busting OCRAM;
// this uses TDSP_DRUM_PLAITS_VOICES = 3, well under the ~2-3 Plaits-voice OCRAM
// budget on the no-PSRAM board).
//
//   g_drumPlayer (ch10) -> g_drumPlaitsSink -> pool[rr] (int16) -> mixer -> F32 -> slot
//
// MUTUALLY EXCLUSIVE with TDSP_DRUM_VOICE / TDSP_DRUM_TSF (all own the drum slot);
// main.cpp #errors if more than one is defined. Included by main.cpp AFTER outL/outR.
#pragma once
#ifdef TDSP_DRUM_PLAITS
#include <AudioSynthPlaits.h>
#include <MidiSink.h>

#ifndef TDSP_DRUM_SLOT
#define TDSP_DRUM_SLOT 2
#endif
#ifndef TDSP_DRUM_PLAITS_VOICES
#define TDSP_DRUM_PLAITS_VOICES 3          // kick+snare+hat overlap; each voice ~16 KB shared_buffer_
#endif

// The Plaits voices are in DMAMEM (OCRAM) like HeteroPlaits' g_hpVoice — off the tight DTCM budget.
DMAMEM AudioSynthPlaits g_drumPlaitsV[TDSP_DRUM_PLAITS_VOICES];
AudioMixer4             g_drumPlaitsMix;   // sums the pool (<=4 voices)
AudioConvert_I16toF32   g_drumPlaitsToF32;
static AudioConnection c_dpm0(g_drumPlaitsV[0], 0, g_drumPlaitsMix, 0);
#if TDSP_DRUM_PLAITS_VOICES >= 2
static AudioConnection c_dpm1(g_drumPlaitsV[1], 0, g_drumPlaitsMix, 1);
#endif
#if TDSP_DRUM_PLAITS_VOICES >= 3
static AudioConnection c_dpm2(g_drumPlaitsV[2], 0, g_drumPlaitsMix, 2);
#endif
#if TDSP_DRUM_PLAITS_VOICES >= 4
static AudioConnection c_dpm3(g_drumPlaitsV[3], 0, g_drumPlaitsMix, 3);
#endif
static AudioConnection     c_dp_conv(g_drumPlaitsMix, 0, g_drumPlaitsToF32, 0);
static AudioConnection_F32 c_dp_outL(g_drumPlaitsToF32, 0, outL, TDSP_DRUM_SLOT);
static AudioConnection_F32 c_dp_outR(g_drumPlaitsToF32, 0, outR, TDSP_DRUM_SLOT);

// GM ch10 note -> a Plaits drum model + pitch/shape/LPG-decay (verbatim from spike_plaits_drums):
// kick/toms/low-perc -> Bass Drum (13, retuned), snare/clap/rim -> Snare (14), hats/cymbals -> Hi-Hat
// (15, short vs long decay). Falls back to a mid-tom so a dense groove never goes silent.
struct DrumPlaitsPatch { int8_t engine; float note, harm, timbre, morph, decay; };
static DrumPlaitsPatch drumPlaitsPatch(uint8_t n) {
    switch (n) {
        case 35: return { 13, 33.0f, 0.5f, 0.4f, 0.5f, 0.55f };   // Acoustic Bass Drum
        case 36: return { 13, 36.0f, 0.6f, 0.5f, 0.5f, 0.50f };   // Bass Drum 1
        case 37: return { 14, 64.0f, 0.4f, 0.7f, 0.3f, 0.18f };   // Side Stick
        case 38: return { 14, 57.0f, 0.5f, 0.5f, 0.5f, 0.40f };   // Acoustic Snare
        case 39: return { 14, 60.0f, 0.4f, 0.85f,0.6f, 0.35f };   // Hand Clap
        case 40: return { 14, 60.0f, 0.6f, 0.5f, 0.5f, 0.38f };   // Electric Snare
        case 41: return { 13, 43.0f, 0.4f, 0.4f, 0.4f, 0.60f };   // Low Floor Tom
        case 43: return { 13, 47.0f, 0.4f, 0.4f, 0.4f, 0.58f };   // High Floor Tom
        case 45: return { 13, 50.0f, 0.4f, 0.4f, 0.4f, 0.55f };   // Low Tom
        case 47: return { 13, 53.0f, 0.4f, 0.4f, 0.4f, 0.52f };   // Low-Mid Tom
        case 48: return { 13, 57.0f, 0.4f, 0.4f, 0.4f, 0.50f };   // Hi-Mid Tom
        case 50: return { 13, 60.0f, 0.4f, 0.4f, 0.4f, 0.48f };   // High Tom
        case 42: return { 15, 72.0f, 0.5f, 0.5f, 0.5f, 0.14f };   // Closed Hi-Hat
        case 44: return { 15, 72.0f, 0.5f, 0.5f, 0.5f, 0.12f };   // Pedal Hi-Hat
        case 46: return { 15, 72.0f, 0.5f, 0.6f, 0.5f, 0.55f };   // Open Hi-Hat
        case 49: return { 15, 76.0f, 0.6f, 0.85f,0.6f, 0.90f };   // Crash 1
        case 57: return { 15, 78.0f, 0.6f, 0.85f,0.6f, 0.88f };   // Crash 2
        case 51: return { 15, 80.0f, 0.6f, 0.80f,0.6f, 0.80f };   // Ride
        case 59: return { 15, 80.0f, 0.6f, 0.80f,0.6f, 0.78f };   // Ride 2
        case 53: return { 15, 84.0f, 0.6f, 0.75f,0.6f, 0.70f };   // Ride Bell
        case 52: return { 15, 74.0f, 0.6f, 0.90f,0.6f, 0.85f };   // China
        case 55: return { 15, 82.0f, 0.6f, 0.85f,0.6f, 0.60f };   // Splash
        case 54: return { 15, 88.0f, 0.5f, 0.6f, 0.5f, 0.20f };   // Tambourine
        case 56: return { 13, 72.0f, 0.7f, 0.3f, 0.3f, 0.18f };   // Cowbell
        case 60: return { 13, 62.0f, 0.4f, 0.4f, 0.4f, 0.30f };   // Hi Bongo
        case 61: return { 13, 58.0f, 0.4f, 0.4f, 0.4f, 0.32f };   // Low Bongo
        case 62: return { 13, 60.0f, 0.4f, 0.5f, 0.4f, 0.28f };   // Mute Hi Conga
        case 63: return { 13, 56.0f, 0.4f, 0.5f, 0.4f, 0.34f };   // Open Hi Conga
        case 75: return { 15, 90.0f, 0.6f, 0.4f, 0.5f, 0.12f };   // Claves
        case 76: return { 13, 74.0f, 0.7f, 0.3f, 0.3f, 0.14f };   // Hi Wood Block
        case 77: return { 13, 70.0f, 0.7f, 0.3f, 0.3f, 0.16f };   // Low Wood Block
        default: return { 13, 52.0f, 0.4f, 0.4f, 0.4f, 0.45f };   // fallback: mid tom
    }
}

// The ch10 groove/live-drum sink: each note-ON round-robins across the voice pool, reconfigures that
// voice to the note's drum patch and pulses its gate (a one-shot; service() drops the gate ~6 ms later
// so the LPG decay makes the tail). note-OFF is ignored — drums are one-shots.
class DrumPlaitsSink : public tdsp::MidiSink {
public:
    void onNoteOn(uint8_t /*ch*/, uint8_t note, uint8_t vel) override {
        if (vel == 0) return;
        const DrumPlaitsPatch p = drumPlaitsPatch(note);
        const int v = rr_; rr_ = (rr_ + 1) % TDSP_DRUM_PLAITS_VOICES;
        AudioSynthPlaits &vc = g_drumPlaitsV[v];
        vc.setEngineIndex(p.engine); vc.setNote(p.note); vc.setHarmonics(p.harm);
        vc.setTimbre(p.timbre); vc.setMorph(p.morph); vc.setDecay(p.decay);
        vc.setLpgColour(0.5f); vc.setLevel(vel / 127.0f); vc.gate(true);
        slot_[v].releaseAt = millis() + 6; slot_[v].active = true;
    }
    // Release one-shot gates whose hold has elapsed. Call once per main-loop pass.
    void service() {
        const uint32_t now = millis();
        for (int v = 0; v < TDSP_DRUM_PLAITS_VOICES; ++v)
            if (slot_[v].active && (int32_t)(now - slot_[v].releaseAt) >= 0) { g_drumPlaitsV[v].gate(false); slot_[v].active = false; }
    }
private:
    uint8_t rr_ = 0;
    struct { uint32_t releaseAt; bool active; } slot_[TDSP_DRUM_PLAITS_VOICES] = {};
};
DrumPlaitsSink g_drumPlaitsSink;

// Bring up the Plaits drum pool + open its mix slot. Returns true (Plaits needs no font) — matches
// drumVoiceBegin()/drumTsfBegin() so setup() wires them the same way.
static bool drumPlaitsBegin() {
    for (int v = 0; v < TDSP_DRUM_PLAITS_VOICES; ++v) { g_drumPlaitsV[v].setEngineIndex(13); g_drumPlaitsMix.gain(v, 0.7f); }
    outL.gain(TDSP_DRUM_SLOT, 0.62f); outR.gain(TDSP_DRUM_SLOT, 0.62f);   // drum bus make-up (mirrors the synth slot / OPLL drums)
    Serial.printf("[drumplaits] %d-voice authentic Plaits drum kit ready (ch10) -> mix slot %d\n",
                  TDSP_DRUM_PLAITS_VOICES, TDSP_DRUM_SLOT);
    return true;
}
#endif
