// Fx.inc.h — hexefx reverb as a MASTER INSERT (Phase 2 of the FX plan).
//
// One reverb slot, chosen at build time: TDSP_FX_PLATE (stereo plate — rich, ~190 KB RAM2
// heap) or TDSP_FX_SPRING (spring — ~37 KB heap, fits no-PSRAM boards). Both are stereo
// 2-in/2-out and wired identically between the master sum and tdmOut (see main.cpp), so this
// file just maps the @FX.* command surface + the "fx" @STATE object onto whichever is built.
//
// Included into main.cpp's TU AFTER g_fxReverb is declared. INSERT topology: reverb.mix() is
// the global dry/wet; @FX.ON=0 -> bypass_set(true) in BYPASS_MODE_PASS = clean dry passthrough
// (so a FX build with the effect off is audibly identical to a non-FX build). The per-track
// SEND matrix + @TRK<i>.FXSEND is a later phase. See planning/plate-reverb-fx/DESIGN.md.
#pragma once

// Percent state (0..100) so the app's sliders map 1:1; pitch is signed semitones. A superset —
// each build uses only the fields its reverb supports (guarded below).
struct FxState {
    bool on;
    int  mix;                                   // both: dry/wet
    int  size, damp, lodamp, diff, lowpass, hipass, shimmer, pitch;  // plate
    bool freeze;                                // plate
    int  time, treble, bass;                    // spring
};
#if TDSP_FX_PLATE
static FxState g_fx = { false, 30, /*size*/70, /*damp*/40, /*lodamp*/10, /*diff*/65,
                        /*lowpass*/100, /*hipass*/0, /*shimmer*/0, /*pitch*/0, /*freeze*/false,
                        /*time*/0, /*treble*/0, /*bass*/0 };
#else  // TDSP_FX_SPRING
static FxState g_fx = { false, 35, 0,0,0,0,0,0,0,0, false, /*time*/55, /*treble*/40, /*bass*/30 };
#endif

static inline int fxClamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Anti-zipper glide for the wet/dry MIX — a crossfade gain, the zipper-prone FX param. The other
// reverb params are per-block applied by the effect and forgiving, so only mix is smoothed. The
// app streams throttled @FX.MIX during a slider drag (project_serial_bridge_throttle); those set
// the TARGET, and fxTick() (called from loop(), self-gated ~1 kHz) glides current->target so a
// fast sweep glides instead of stepping. This is what lets the wire stay slow AND the audio be smooth.
static float g_fxMixTarget = 0.0f, g_fxMixCur = 0.0f;
static void fxTick() {
    static elapsedMicros t;
    if (t < 1000) return;                                        // ~1 kHz glide update
    t = 0;
    if (fabsf(g_fxMixCur - g_fxMixTarget) > 0.0004f) {
        g_fxMixCur += (g_fxMixTarget - g_fxMixCur) * 0.15f;      // one-pole, ~30 ms settle
        g_fxReverb.mix(g_fxMixCur);
    }
}

FLASHMEM static void fxApplyAll() {
    g_fxReverb.bypass_setMode(BYPASS_MODE_PASS);   // "off" = clean dry, never muted
#if TDSP_FX_SEND
    g_fxMixTarget = g_fxMixCur = 1.0f;             // SEND bus: reverb is 100% wet; dry lives on the master
    g_fxReverb.mix(1.0f);
#else
    g_fxMixTarget = g_fxMixCur = g_fx.mix / 100.0f;   // INSERT: snap the wet/dry glide at init
    g_fxReverb.mix(g_fxMixCur);
#endif
#if TDSP_FX_PLATE
    g_fxReverb.size(g_fx.size / 100.0f);
    g_fxReverb.hidamp(g_fx.damp / 100.0f);
    g_fxReverb.lodamp(g_fx.lodamp / 100.0f);
    g_fxReverb.diffusion(g_fx.diff / 100.0f);
    g_fxReverb.lowpass(g_fx.lowpass / 100.0f);
    g_fxReverb.hipass(g_fx.hipass / 100.0f);
    g_fxReverb.shimmer(g_fx.shimmer / 100.0f);
    g_fxReverb.shimmerPitchSemitones((int8_t)g_fx.pitch);
    g_fxReverb.freeze(g_fx.freeze);
#else  // TDSP_FX_SPRING
    g_fxReverb.time(g_fx.time / 100.0f);
    g_fxReverb.treble_cut(g_fx.treble / 100.0f);
    g_fxReverb.bass_cut(g_fx.bass / 100.0f);
#endif
    g_fxReverb.bypass_set(!g_fx.on);
}
#if TDSP_FX_SEND
// ---- SEND matrix (per-voice aux sends into the reverb bus) ----------------------------------
// Each track keeps its dry path to the master; a per-track SEND gain feeds the reverb; the reverb's
// 100%-wet output returns to the master through post slot 3 at the RETURN level. All sends boot at 0
// (pure send: every track dry until you raise a send). See planning/plate-reverb-fx/SEND_MATRIX.md.
static uint8_t g_fxSend[8] = {0};   // per-track send level %, indexed by track (0..3 synths, kSynthVoices = drums)
static uint8_t g_fxReturn = 60;     // wet return level %

// Track index convention (matches @STATE tracks[] + the app's send rows):
//   0..kSynthVoices-1 = Synth A..D -> fxIn1 slots 0..3
//   kSynthVoices      = Drums      -> fxIn2 slot 1
//   kSynthVoices + 1  = Audio Loop -> fxIn2 slot 2 (tap present only when TDSP_AUDIOLOOP)
static const int kFxLoopTrk = kSynthVoices + 1;

// Map a track index -> its slot in the fxIn cascade and set the send gain. Mono-fed nodes drive L+R alike.
static void fxSendApply(int trk, int pct) {
    if (trk < 0 || trk >= (int)(sizeof(g_fxSend))) return;
    pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    g_fxSend[trk] = (uint8_t)pct;
    const float g = pct / 100.0f;
    if (trk >= 0 && trk < kSynthVoices && trk < 4) { fxIn1L.gain(trk, g); fxIn1R.gain(trk, g); }   // Synth A-D
    else if (trk == kSynthVoices)                  { fxIn2L.gain(1, g);   fxIn2R.gain(1, g); }      // Drums
    else if (trk == kFxLoopTrk)                    { fxIn2L.gain(2, g);   fxIn2R.gain(2, g); }      // Audio loop
}
static void fxReturnApply() {
    const float g = g_fxReturn / 100.0f;
    postL.gain(3, g); postR.gain(3, g);
}
FLASHMEM static void fxSendInit() {
    fxIn2L.gain(0, 1.0f); fxIn2R.gain(0, 1.0f);            // fxIn1 sub-sum passes through the cascade unattenuated
    for (int i = 0; i < (int)sizeof(g_fxSend); ++i) fxSendApply(i, g_fxSend[i]);   // boot sends (all 0 = dry)
    fxReturnApply();
}
#endif  // TDSP_FX_SEND

FLASHMEM static void fxInit() {
    fxApplyAll();
#if TDSP_FX_SEND
    fxSendInit();
#endif
}

// The "fx" object for @STATE (and the bare @FX query). "type" tells the app which reverb +
// which sliders to render.
static void fxEmitJson(Stream& reply) {
#if TDSP_FX_PLATE
    reply.printf("{\"type\":\"plate\",\"on\":%d,\"mix\":%d,\"size\":%d,\"damp\":%d,\"lodamp\":%d,"
                 "\"diff\":%d,\"lowpass\":%d,\"hipass\":%d,\"shimmer\":%d,\"pitch\":%d,\"freeze\":%d",
                 g_fx.on ? 1 : 0, g_fx.mix, g_fx.size, g_fx.damp, g_fx.lodamp, g_fx.diff,
                 g_fx.lowpass, g_fx.hipass, g_fx.shimmer, g_fx.pitch, g_fx.freeze ? 1 : 0);
#else  // TDSP_FX_SPRING
    reply.printf("{\"type\":\"spring\",\"on\":%d,\"mix\":%d,\"time\":%d,\"treble\":%d,\"bass\":%d",
                 g_fx.on ? 1 : 0, g_fx.mix, g_fx.time, g_fx.treble, g_fx.bass);
#endif
#if TDSP_FX_SEND
    // SEND-mode extras: routing mode + wet return + the audio-loop send (per-synth/drum sends ride
    // in @STATE tracks[].fxsend). Lets the app render the Return slider + loop send row.
    reply.printf(",\"route\":\"send\",\"return\":%d,\"loopsend\":%d", g_fxReturn, g_fxSend[kFxLoopTrk]);
#endif
    reply.print("}");
}

// @FX command surface. Common @FX.ON/@FX.MIX; the rest is reverb-specific. Returns true if the
// line was an @FX command. (Distinct from @FXUP / FlasherX.)
static bool handleFxCmd(const char* line, Stream& reply) {
    if (strcmp(line, "@FX") == 0) { reply.print("@FX="); fxEmitJson(reply); reply.print("\n"); return true; }
    if (strncmp(line, "@FX.", 4) != 0) return false;
    const char* eq = strchr(line, '=');
    const int v = eq ? atoi(eq + 1) : 0;
    if      (strncmp(line, "@FX.ON=", 7) == 0)  { g_fx.on = (v != 0); g_fxReverb.bypass_set(!g_fx.on); reply.printf("@FX.ON=%d\n", g_fx.on ? 1 : 0); }
#if TDSP_FX_SEND
    // SEND bus: the reverb is 100% wet, so "MIX" means the wet RETURN level. @FX.MIX is kept as an
    // alias so an insert-era client still works; the per-track sends arrive via @TRK<i>.FXSEND.
    else if (strncmp(line, "@FX.RETURN=", 11) == 0 ||
             strncmp(line, "@FX.MIX=", 8) == 0)     { g_fxReturn = fxClamp(v, 0, 100); fxReturnApply(); reply.printf("@FX.RETURN=%d\n", g_fxReturn); }
    else if (strncmp(line, "@FX.LOOPSEND=", 13) == 0){ fxSendApply(kFxLoopTrk, fxClamp(v, 0, 100));       reply.printf("@FX.LOOPSEND=%d\n", g_fxSend[kFxLoopTrk]); }
#else
    else if (strncmp(line, "@FX.MIX=", 8) == 0) { g_fx.mix = fxClamp(v, 0, 100); g_fxMixTarget = g_fx.mix / 100.0f; reply.printf("@FX.MIX=%d\n", g_fx.mix); }  // glide via fxTick()
#endif
#if TDSP_FX_PLATE
    else if (strncmp(line, "@FX.SIZE=", 9) == 0)     { g_fx.size = fxClamp(v, 0, 100);    g_fxReverb.size(g_fx.size / 100.0f);       reply.printf("@FX.SIZE=%d\n", g_fx.size); }
    else if (strncmp(line, "@FX.DAMP=", 9) == 0)     { g_fx.damp = fxClamp(v, 0, 100);    g_fxReverb.hidamp(g_fx.damp / 100.0f);     reply.printf("@FX.DAMP=%d\n", g_fx.damp); }
    else if (strncmp(line, "@FX.LODAMP=", 11) == 0)  { g_fx.lodamp = fxClamp(v, 0, 100);  g_fxReverb.lodamp(g_fx.lodamp / 100.0f);   reply.printf("@FX.LODAMP=%d\n", g_fx.lodamp); }
    else if (strncmp(line, "@FX.DIFF=", 9) == 0)     { g_fx.diff = fxClamp(v, 0, 100);    g_fxReverb.diffusion(g_fx.diff / 100.0f);  reply.printf("@FX.DIFF=%d\n", g_fx.diff); }
    else if (strncmp(line, "@FX.LOWPASS=", 12) == 0) { g_fx.lowpass = fxClamp(v, 0, 100); g_fxReverb.lowpass(g_fx.lowpass / 100.0f); reply.printf("@FX.LOWPASS=%d\n", g_fx.lowpass); }
    else if (strncmp(line, "@FX.HIPASS=", 11) == 0)  { g_fx.hipass = fxClamp(v, 0, 100);  g_fxReverb.hipass(g_fx.hipass / 100.0f);   reply.printf("@FX.HIPASS=%d\n", g_fx.hipass); }
    else if (strncmp(line, "@FX.SHIMMER=", 12) == 0) { g_fx.shimmer = fxClamp(v, 0, 100); g_fxReverb.shimmer(g_fx.shimmer / 100.0f); reply.printf("@FX.SHIMMER=%d\n", g_fx.shimmer); }
    else if (strncmp(line, "@FX.PITCH=", 10) == 0)   { g_fx.pitch = fxClamp(v, -12, 12);  g_fxReverb.shimmerPitchSemitones((int8_t)g_fx.pitch); reply.printf("@FX.PITCH=%d\n", g_fx.pitch); }
    else if (strncmp(line, "@FX.FREEZE=", 11) == 0)  { g_fx.freeze = (v != 0); g_fxReverb.freeze(g_fx.freeze); reply.printf("@FX.FREEZE=%d\n", g_fx.freeze ? 1 : 0); }
#else  // TDSP_FX_SPRING
    else if (strncmp(line, "@FX.TIME=", 9) == 0)     { g_fx.time = fxClamp(v, 0, 100);    g_fxReverb.time(g_fx.time / 100.0f);       reply.printf("@FX.TIME=%d\n", g_fx.time); }
    else if (strncmp(line, "@FX.TREBLE=", 11) == 0)  { g_fx.treble = fxClamp(v, 0, 100);  g_fxReverb.treble_cut(g_fx.treble / 100.0f); reply.printf("@FX.TREBLE=%d\n", g_fx.treble); }
    else if (strncmp(line, "@FX.BASS=", 9) == 0)     { g_fx.bass = fxClamp(v, 0, 100);    g_fxReverb.bass_cut(g_fx.bass / 100.0f);   reply.printf("@FX.BASS=%d\n", g_fx.bass); }
#endif
    else return false;
    return true;
}
