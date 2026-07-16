// Diagnostics.inc.h — developer BENCH INSTRUMENTS for the mix-kit firmware.
//
// The instrument self-test ('T'), pitch-bend/MPE/axis proofs, pizz clip + onset
// capture, analog-loopback capture, TDM slot scan, worst-jump dumps, and the
// backend-agnostic ReplayGain sweep ('N') are DEVELOPER tools, not product
// behaviour. They were split out of main.cpp so the product firmware reads as a
// wire-up, not a monolith.
//
// This file is #included into main.cpp (NOT a separate translation unit) at the
// point where every audio-graph object (dxpClip/adcProbe/tdmScan/g_outCap), the
// synth backend hooks (g_synthSink/synth*), and the players (g_player/…) are
// already declared — so it needs no shared-globals header.
//
// Gated by TDSP_DIAGNOSTICS (default 1 in main.cpp). A lean product build sets
// -D TDSP_DIAGNOSTICS=0 to compile all of this out and reclaim flash.
#pragma once
// Instrument self-test ('T'): step through all 128 GM programs + the drum kit, play
// test notes on each, and log the resulting output peak. The "prog N -> on" line is
// printed and flushed BEFORE rendering, so if the engine hangs or faults on a specific
// patch the LAST serial line names the culprit. Backend-agnostic (drives g_synthSink).
FLASHMEM static void runInstrumentSelfTest() {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    delay(50);
    Serial.printf("[selftest] === %s: 128 GM programs (ch1) + drums (ch10) ===\n", synthName());
    const int notes[3] = {48, 60, 72};
    int silent = 0;
    for (int prog = 0; prog < 128; prog++) {
        Serial.printf("[selftest] prog %3d -> on ", prog); Serial.flush();
        g_synthSink->onProgramChange(1, (uint8_t)prog);
        for (int i = 0; i < 3; i++) g_synthSink->onNoteOn(1, notes[i], 110);
        float pk = 0.0f; uint32_t t0 = millis();
        while (millis() - t0 < 220) { if (peakOut.available()) { float p = peakOut.read(); if (p > pk) pk = p; } delay(4); }
        for (int i = 0; i < 3; i++) g_synthSink->onNoteOff(1, notes[i], 0);
        Serial.printf("peak=%.3f %s\n", pk, pk < 0.008f ? "*** SILENT ***" : "ok");
        if (pk < 0.008f) silent++;
        delay(70);
    }
    Serial.println("[selftest] --- drums (ch10, notes 35..81) ---");
    float drumMax = 0.0f; int drumSilent = 0;
    for (int note = 35; note <= 81; note++) {
        Serial.printf("[selftest] drum %2d -> on ", note); Serial.flush();
        g_synthSink->onNoteOn(10, (uint8_t)note, 110);
        float pk = 0.0f; uint32_t t0 = millis();
        while (millis() - t0 < 150) { if (peakOut.available()) { float p = peakOut.read(); if (p > pk) pk = p; } delay(4); }
        g_synthSink->onNoteOff(10, (uint8_t)note, 0);
        Serial.printf("peak=%.3f %s\n", pk, pk < 0.008f ? "silent" : "ok");
        if (pk > drumMax) drumMax = pk;
        if (pk < 0.008f) drumSilent++;
        delay(40);
    }
    g_synthSink->onAllNotesOff(0);
    Serial.printf("[selftest] DONE: %d/128 melodic SILENT; drums peakMax=%.3f, %d/47 notes silent\n",
                  silent, drumMax, drumSilent);
}

// Pitch-bend audible test ('B'): hold a sustained strings note on ch1 and sweep the
// bend 0 -> +2 -> -2 -> 0 semitones over ~4 s. If bend works you hear the note glide.
FLASHMEM static void runPitchBendTest() {
    if (g_player.isPlaying()) songStop();
    Serial.println("[pbtest] ch1 strings, note 60 held; bend sweep 0->+2->-2->0 semis (~4s)");
    g_synthSink->onProgramChange(1, 48);      // String Ensemble 1 (sustained -> bend clearly audible)
    g_synthSink->onPitchBend(1, 0.0f);
    g_synthSink->onNoteOn(1, 60, 110);
    for (int i = 0; i <= 80; i++) {
        float phase = i / 80.0f, semis;
        if      (phase < 0.25f) semis =  (phase / 0.25f) * 2.0f;                 // 0 -> +2
        else if (phase < 0.75f) semis =  2.0f - ((phase - 0.25f) / 0.5f) * 4.0f; // +2 -> -2
        else                    semis = -2.0f + ((phase - 0.75f) / 0.25f) * 2.0f;// -2 -> 0
        g_synthSink->onPitchBend(1, semis);
        delay(50);
    }
    g_synthSink->onPitchBend(1, 0.0f);
    g_synthSink->onNoteOff(1, 60, 0);
    Serial.println("[pbtest] done");
}

// MPE self-test ('A'): drive the router as if a LinnStrument sent one MPE note on a
// member channel — RPN bend range 48, a note, then a pressure swell + pitch-bend sweep.
// Verifies the router -> sink -> TSF expression path: outPeak should FOLLOW the pressure
// (swell up then down), proving per-note pressure->volume works, plus the bend glides.
FLASHMEM static void runMpeTest() {
    if (g_player.isPlaying()) songStop();
    applyMidiMode(true);                      // MPE mode (ch10 melodic, router bend 48)
    const uint8_t ch = 2;                     // an MPE member channel
    Serial.println("[mpetest] ch2 note 60: pressure swell + bend sweep (~5s). Watch outPeak follow pressure.");
    g_router.handleControlChange(ch, 101, 0); // RPN 0,0 = pitch-bend range...
    g_router.handleControlChange(ch, 100, 0);
    g_router.handleControlChange(ch, 6, 48);  // ...= 48 semitones
    g_router.handleChannelPressure(ch, 100);
    g_router.handleNoteOn(ch, 60, 100);
    for (int i = 0; i <= 50; i++) {
        float ph = i / 50.0f;
        uint8_t pr = (uint8_t)(127.0f * (0.5f - 0.5f * cosf(ph * 2.0f * PI)));   // 0 -> 127 -> 0
        int16_t bend = (int16_t)(8191.0f * sinf(ph * 2.0f * PI));               // 0 -> +bend -> -bend -> 0
        g_router.handleChannelPressure(ch, pr);
        g_router.handlePitchBend(ch, bend);
        float pk = 0; uint32_t t0 = millis();
        while (millis() - t0 < 90) { if (peakOut.available()) { float p = peakOut.read(); if (p > pk) pk = p; } delay(4); }
        if (i % 6 == 0) Serial.printf("[mpetest] pressure=%3u  outPeak=%.3f\n", pr, pk);
    }
    g_router.handleNoteOff(ch, 60, 0);
    applyMidiMode(false);
    Serial.println("[mpetest] done (back to normal MIDI)");
}

#ifdef TDSP_SYNTH_SF2_TSF
// --- MPE axis proof (@PROOF), TSF port --------------------------------------
// The Dexed-pool axis proof (below) depends on that backend's ClipProbe + routing
// masks, so here is the TSF equivalent. Hold ONE note on an MPE member channel with
// a single axis pushed to full and capture the synth sum, so the PC can measure that
// the axis modulates the waveform: pitch (bend, axis 2), spectral centroid
// (timbre -> lowpass cutoff, axis 1 — the CC#74 path this backend just gained),
// amplitude (pressure -> volume, axis 0), or a neutral reference (axis 3). TSF routes
// timbre/pressure natively (no routing masks), so this is just: MPE mode, note on,
// push the axis, capture. Compare @PROOF=1 (timbre pushed to fully-closed) against
// @PROOF=3 (neutral / patch-open): a LOWER spectral centroid proves CC#74 shut the filter.
static void dumpFloatsTagged(const char *tag, const float *c, int n) {
    Serial.printf("[lb] %s begin %d\n", tag, n);
    char lb[220];
    for (int i = 0; i < n; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < n; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
        Serial.println(lb);
    }
    Serial.printf("[lb] %s end\n", tag);
}

FLASHMEM static void runAxisProof(int axis) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    bool wasMpe = g_mpeMode;
    applyMidiMode(true);
    synthSetInstrument(48);                                // GM 48 = String Ensemble 1: sustained, filter-rich
    if (g_dvol < -30.0f) { g_dvol = -12.0f; if (g_codecOk) applyVol(); }
    delay(90);
    const char *nm = (axis == 0) ? "pressure" : (axis == 1) ? "timbre" : (axis == 2) ? "bend+7" : "neutral";
    Serial.printf("[proof] axis=%s note=60 ch2 N=%d\n", nm, ClipProbe_F32::kCapN);
    // Recenter ch2's expression before the note so each proof is INDEPENDENT of the
    // previous one's axis — else a prior bend/timbre latches on the channel and the next
    // capture starts pre-modulated. Neutral = bend 0, timbre open (1.0), full pressure.
    g_synthSink->onPitchBend(2, 0.0f);
    g_synthSink->onTimbre(2, 1.0f);
    g_synthSink->onPressure(2, 1.0f);
    g_synthSink->onNoteOn(2, 60, 110);
    if      (axis == 0) g_synthSink->onPressure(2, 1.0f);
    else if (axis == 1) g_synthSink->onTimbre(2, 0.0f);   // CC#74=0 -> filter fully closed (darkest vs neutral)
    else if (axis == 2) g_synthSink->onPitchBend(2, 7.0f);
    delay(150);
    dxpClip.armCapture();
    uint32_t t0 = millis();
    while (!dxpClip.captureDone() && millis() - t0 < 1000) delay(2);
    g_synthSink->onNoteOff(2, 60, 0);
    dumpFloatsTagged("PROOF", dxpClip.capture(), dxpClip.captureCount());
    Serial.println("[proof] done");
    g_synthSink->onAllNotesOff(0);
    applyMidiMode(wasMpe);
}
#endif

#ifdef TDSP_SYNTH_DEXED_POOL
// Pizz clip test ('K'): load a patch (default 273 = "PIZZ STGS"), fire a single note
// at rising velocities, and report the SYNTH-SUM peak + railed-sample count. The probe
// (dxpClip) sits BEFORE the 0.62 mix make-up, so per-engine int16 flat-topping shows up
// here even though the final-bus peak (outPeak) is scaled down and looks clean. This is
// the definitive answer to "is the snap at note-onset actually clipping?".
FLASHMEM static void runPizzClipTest(int inst) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    synthSetInstrument(inst);
    delay(60);
    Serial.printf("[cliptest] inst %d = %s ; rail=%.4f (synth-sum, pre-0.62-mix)\n",
                  inst, synthInstrumentName(inst), (double)ClipProbe_F32::kRail);
    const uint8_t vels[] = {40, 64, 80, 100, 110, 120, 127};
    const int note = 60;
    for (uint8_t v : vels) {
        g_synthSink->onAllNotesOff(0); delay(40);
        dxpClip.reset();
        float pkOut = 0.0f;
        g_synthSink->onNoteOn(1, note, v);
        uint32_t t0 = millis();
        while (millis() - t0 < 300) { if (peakOut.available()) { float p = peakOut.read(); if (p > pkOut) pkOut = p; } delay(2); }
        g_synthSink->onNoteOff(1, note, 0);
        uint32_t clipped = dxpClip.clipped(), total = dxpClip.total();
        float pkSum = dxpClip.peak();
        float pct = total ? (100.0f * (float)clipped / (float)total) : 0.0f;
        Serial.printf("[cliptest] vel %3u: sumPeak=%.4f  railed=%lu/%lu (%.2f%%)  outPeak=%.3f  %s\n",
                      v, (double)pkSum, (unsigned long)clipped, (unsigned long)total, (double)pct, (double)pkOut,
                      clipped > 8 ? "*** CLIPPING ***" : (pkSum >= ClipProbe_F32::kRail ? "(touches rail)" : "clean"));
        delay(120);
    }
    g_synthSink->onAllNotesOff(0);
    Serial.println("[cliptest] done");
}

// Onset capture ('J'): record the synth-sum waveform from a single note-on and dump it
// over serial as floats. The PC then FFTs it (aliasing = inharmonic fold-back partials)
// and inspects the first samples (zero-crossing / step discontinuity at note-onset).
static void captureOneNote(int note, int vel) {
    g_synthSink->onAllNotesOff(0); delay(60);
    dxpClip.armCapture();
    g_synthSink->onNoteOn(1, note, vel);
    uint32_t t0 = millis();
    while (!dxpClip.captureDone() && millis() - t0 < 1500) delay(2);
    g_synthSink->onNoteOff(1, note, 0);
    int n = dxpClip.captureCount();
    const float *c = dxpClip.capture();
    Serial.printf("[cap] note=%d vel=%d begin %d\n", note, vel, n);
    char lb[220];
    for (int i = 0; i < n; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < n; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
        Serial.println(lb);
    }
    Serial.println("[cap] end");
}

FLASHMEM static void runPizzCapture(int inst, int, int vel) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    synthSetInstrument(inst);
    delay(80);
    Serial.printf("[cap] inst %d = %s vel=%d rate=%.0f N=%d\n",
                  inst, synthInstrumentName(inst), vel,
                  (double)AUDIO_SAMPLE_RATE_EXACT, ClipProbe_F32::kCapN);
    // low -> high: FM aliasing (fold-back past Nyquist) worsens with fundamental pitch
    const int notes[] = {48, 60, 72, 84, 96};
    for (int nn : notes) captureOneNote(nn, vel);
    g_synthSink->onAllNotesOff(0);
    Serial.println("[cap] ALLDONE");
}

static void dumpFloatsTagged(const char *tag, const float *c, int n) {
    Serial.printf("[lb] %s begin %d\n", tag, n);
    char lb[220];
    for (int i = 0; i < n; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < n; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
        Serial.println(lb);
    }
    Serial.printf("[lb] %s end\n", tag);
}

// Loopback capture ('L'): fire one note and record BOTH the digital synth sum (dxpClip)
// and the re-digitized analog output (adcProbe, via the OUT->IN loopback) for the same
// event. Comparing them (after latency alignment) shows whether the codec/analog stage
// adds a per-note transient the clean digital signal doesn't have.
FLASHMEM static void runLoopbackCapture(int inst, int note, int vel) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    synthSetInstrument(inst);
    if (g_dvol < -30.0f) { g_dvol = -12.0f; if (g_codecOk) applyVol(); }   // ensure the DAC drives the loopback
    delay(80);
    Serial.printf("[lb] inst %d = %s note=%d vel=%d rate=%.0f N=%d dvol=%.0f\n",
                  inst, synthInstrumentName(inst), note, vel,
                  (double)AUDIO_SAMPLE_RATE_EXACT, ClipProbe_F32::kCapN, (double)g_dvol);
    dxpClip.armCapture();
    adcProbe.armCapture();
    g_synthSink->onNoteOn(1, note, vel);
    uint32_t t0 = millis();
    while ((!dxpClip.captureDone() || !adcProbe.captureDone()) && millis() - t0 < 2500) delay(2);
    g_synthSink->onNoteOff(1, note, 0);
    dumpFloatsTagged("DIG", dxpClip.capture(),  dxpClip.captureCount());
    dumpFloatsTagged("ADC", adcProbe.capture(), adcProbe.captureCount());
    Serial.println("[lb] ALLDONE");
    g_synthSink->onAllNotesOff(0);
}

// Axis proof ('Q' = pressure; @PROOF=<axis>): hold ONE note with one MPE axis pushed to
// full and capture the synth sum, so the PC can measure that the axis really modulates —
// pitch (bend, axis 2), spectral centroid (timbre->brightness, axis 1), amplitude
// (pressure->volume, axis 0), or a neutral reference (axis 3). Routings are forced to the
// obvious mapping for the measurement, then restored.
FLASHMEM static void runAxisProof(int axis) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    bool wasMpe = g_mpeMode;
    uint8_t sp = g_poolSink.pressureMask(), st = g_poolSink.timbreMask();
    applyMidiMode(true);
    synthSetInstrument(48);                                // a sustained voice
    if (g_dvol < -30.0f) { g_dvol = -12.0f; if (g_codecOk) applyVol(); }
    g_poolSink.setPressureMask(3);                         // pressure -> volume+brightness
    g_poolSink.setTimbreMask(2);                           // timbre   -> brightness
    delay(90);
    const char *nm = (axis == 0) ? "pressure" : (axis == 1) ? "timbre" : (axis == 2) ? "bend+7" : "neutral";
    Serial.printf("[proof] axis=%s note=60 ch2 N=%d\n", nm, ClipProbe_F32::kCapN);
    g_synthSink->onNoteOn(2, 60, 110);
    if      (axis == 0) g_synthSink->onPressure(2, 1.0f);
    else if (axis == 1) g_synthSink->onTimbre(2, 1.0f);
    else if (axis == 2) g_synthSink->onPitchBend(2, 7.0f);
    delay(150);
    dxpClip.armCapture();
    uint32_t t0 = millis();
    while (!dxpClip.captureDone() && millis() - t0 < 1000) delay(2);
    g_synthSink->onNoteOff(2, 60, 0);
    dumpFloatsTagged("PROOF", dxpClip.capture(), dxpClip.captureCount());
    Serial.println("[proof] done");
    g_synthSink->onAllNotesOff(0);
    g_poolSink.setPressureMask(sp); g_poolSink.setTimbreMask(st);
    applyMidiMode(wasMpe);
}
static void runPressureProof(void) { runAxisProof(0); }

// Compact per-note MPE gesture set: bend up/down, then timbre (CC74) sweep, then
// pressure swell — ~3 s. Uses whatever routing is currently set. Drives g_synthSink
// directly on member channel `ch`.
static void mpeGestures(uint8_t ch, uint8_t note) {
    g_synthSink->onNoteOn(ch, note, 100);                                    // bend (X)
    for (int i = 0; i <= 30; i++) { g_synthSink->onPitchBend(ch, 12.0f * sinf(i / 30.0f * 2 * PI)); delay(30); }
    g_synthSink->onPitchBend(ch, 0); g_synthSink->onNoteOff(ch, note, 0); delay(120);
    g_synthSink->onNoteOn(ch, note, 100);                                    // timbre (Y / CC74)
    for (int i = 0; i <= 30; i++) { g_synthSink->onTimbre(ch, 0.5f - 0.5f * cosf(i / 30.0f * 2 * PI)); delay(30); }
    g_synthSink->onTimbre(ch, 0.5f); g_synthSink->onNoteOff(ch, note, 0); delay(120);
    g_synthSink->onNoteOn(ch, note, 100);                                    // pressure (Z)
    for (int i = 0; i <= 30; i++) { g_synthSink->onPressure(ch, 0.5f - 0.5f * cosf(i / 30.0f * 2 * PI)); delay(30); }
    g_synthSink->onPressure(ch, 0); g_synthSink->onNoteOff(ch, note, 0); delay(120);
}

// MPE sweep ('Z' / @MPESWEEP=<start>): play the MPE gesture demo on EVERY instrument in
// turn so you can hear how each patch responds to bend/timbre/pressure. Resets to the
// default demo routing (pressure=vol+bright, timbre=bright, mod=vibrato), restores after.
// Abort by sending any byte. Resumable from an index via @MPESWEEP=<start>.
FLASHMEM static void runMpeSweep(int startIdx) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    bool wasMpe = g_mpeMode;
    uint8_t sp = g_poolSink.pressureMask(), sm = g_poolSink.modMask(), st = g_poolSink.timbreMask();
    applyMidiMode(true);
    g_poolSink.setPressureMask(3); g_poolSink.setTimbreMask(2); g_poolSink.setModMask(4);
    if (g_dvol < -20.0f) { g_dvol = -10.0f; if (g_codecOk) applyVol(); }
    if (startIdx < 0) startIdx = 0;
    Serial.printf("[mpesweep] MPE demo (bend/timbre/pressure) on each instrument from %d; send any key to stop\n", startIdx);
    for (int i = startIdx; i < synthNumInstruments(); i++) {
        if (Serial.available()) { Serial.read(); Serial.printf("[mpesweep] stopped at %d (resume: @MPESWEEP=%d)\n", i, i); break; }
        synthSetInstrument(i);
        Serial.printf("[mpesweep] %3d = %s\n", i, synthInstrumentName(i)); Serial.flush();
        mpeGestures(2, 60);
    }
    g_synthSink->onAllNotesOff(0);
    g_poolSink.setPressureMask(sp); g_poolSink.setModMask(sm); g_poolSink.setTimbreMask(st);
    applyMidiMode(wasMpe);
    Serial.println("[mpesweep] done");
}

// MPE check (@MPECHECK): fast automated QA over ALL instruments — play each with MPE
// expression (pressure+timbre+bend at once) and measure the synth-sum peak, flagging
// SILENT (broken/empty patch) or CLIP. The audible-listen equivalent, but measured.
FLASHMEM static void runMpeCheck(void) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    bool wasMpe = g_mpeMode;
    uint8_t sp = g_poolSink.pressureMask(), st = g_poolSink.timbreMask(), sm = g_poolSink.modMask();
    applyMidiMode(true);
    g_poolSink.setPressureMask(3); g_poolSink.setTimbreMask(3); g_poolSink.setModMask(4);
    if (g_dvol < -20.0f) { g_dvol = -12.0f; if (g_codecOk) applyVol(); }
    Serial.println("[mpecheck] every instrument w/ MPE expression; flags SILENT / CLIP. Any key stops.");
    int silent = 0, clip = 0;
    for (int i = 0; i < synthNumInstruments(); i++) {
        if (Serial.available()) { Serial.read(); Serial.printf("[mpecheck] stopped at %d\n", i); break; }
        synthSetInstrument(i);
        delay(40);                                        // let each engine's panic-release settle so
                                                          // fast-envelope patches' attack isn't swallowed
        dxpClip.reset();
        g_synthSink->onNoteOn(2, 60, 110);                // plain note = the true "does it sound?" test
        g_synthSink->onPitchBend(2, 2.0f);               // a small bend so MPE dispatch is exercised too
        uint32_t t0 = millis(); while (millis() - t0 < 300) delay(2);
        float pk = dxpClip.peak(); uint32_t railed = dxpClip.clipped();
        g_synthSink->onNoteOff(2, 60, 0);
        bool isSilent = pk < 0.01f, isClip = railed > 50;
        if (isSilent) silent++;
        if (isClip)   clip++;
        Serial.printf("[mpecheck] %3d peak=%.3f railed=%lu %-8s %s\n", i, (double)pk, (unsigned long)railed,
                      isSilent ? "SILENT" : isClip ? "CLIP" : "ok", synthInstrumentName(i));
        Serial.flush();
        delay(25);
    }
    g_synthSink->onAllNotesOff(0);
    g_poolSink.setPressureMask(sp); g_poolSink.setTimbreMask(st); g_poolSink.setModMask(sm);
    applyMidiMode(wasMpe);
    Serial.printf("[mpecheck] DONE: %d silent, %d clipping (of %d)\n", silent, clip, synthNumInstruments());
}

// Slot scan ('Y'): play a loud note and report the peak on EACH of the 8 TDM input
// slots, so we can see which slot (if any) carries the ADC loopback signal.
FLASHMEM static void runSlotScan(void) {
    if (g_player.isPlaying()) songStop();
    g_synthSink->onAllNotesOff(0);
    synthSetInstrument(13);
    if (g_dvol < -20.0f) { g_dvol = -6.0f; if (g_codecOk) applyVol(); }
    tdmScan.reset();
    Serial.printf("[scan] note 60 vel 120, dvol=%.0f, watching 8 TDM-in slots...\n", (double)g_dvol);
    g_synthSink->onNoteOn(1, 60, 120);
    uint32_t t0 = millis();
    while (millis() - t0 < 900) delay(5);
    g_synthSink->onNoteOff(1, 60, 0);
    for (int ch = 0; ch < 8; ch++)
        Serial.printf("[scan] slot %d peak = %.6f\n", ch, (double)tdmScan.pk[ch]);
    g_synthSink->onAllNotesOff(0);
    Serial.println("[scan] done");
}

// Dump the frozen window around the worst discontinuity seen since the last reset.
// A clean waveform gives a smooth window; a voice-steal click gives a visible step.
static void dumpWorstJump(void) {
    Serial.printf("[jump] worst=%.6f valid=%d (window %d samples, step at idx %d)\n",
                  (double)dxpClip.worstJump(), dxpClip.snapValid() ? 1 : 0,
                  ClipProbe_F32::kSnapN, ClipProbe_F32::kPre);
    if (!dxpClip.snapValid()) { Serial.println("[jump] (no discontinuity captured)"); return; }
    const float *c = dxpClip.snap();
    Serial.printf("[jump] begin %d\n", ClipProbe_F32::kSnapN);
    char lb[220];
    for (int i = 0; i < ClipProbe_F32::kSnapN; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < ClipProbe_F32::kSnapN; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
        Serial.println(lb);
    }
    Serial.println("[jump] end");
}

// Same as dumpWorstJump but for the ANALOG loopback (adcProbe). Tag [ajump] so the PC
// can split it. Compare its worst step against the digital [jump] at the same session:
// analog >> digital == a codec/DAC pop that isn't in the synthesis.
static void dumpAdcWorstJump(void) {
    Serial.printf("[ajump] worst=%.6f valid=%d (window %d samples, step at idx %d)\n",
                  (double)adcProbe.worstJump(), adcProbe.snapValid() ? 1 : 0,
                  AdcCaptureProbe_F32::kSnapN, AdcCaptureProbe_F32::kPre);
    if (!adcProbe.snapValid()) { Serial.println("[ajump] (no discontinuity captured)"); return; }
    const float *c = adcProbe.snap();
    Serial.printf("[ajump] begin %d\n", AdcCaptureProbe_F32::kSnapN);
    char lb[220];
    for (int i = 0; i < AdcCaptureProbe_F32::kSnapN; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < AdcCaptureProbe_F32::kSnapN; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.6g ", (double)c[i]);
        Serial.println(lb);
    }
    Serial.println("[ajump] end");
}
#endif  // TDSP_SYNTH_DEXED_POOL — end of Dexed-pool-only capture diagnostics

// --- ReplayGain sweep ('N') — BACKEND-AGNOSTIC (see REPLAYGAIN.md) ------------
// Measures the loudness of every one of the backend's synthNumInstruments() voices
// and prints a paste-ready trim table (labeled with synthTrimSymbol(), e.g.
// kDexedVoiceTrim[] / kOpllVoiceTrim[]) for that backend's table header. For each
// voice it plays a fixed reference note and records the MAX short-term K-weighted
// loudness (the loudest ~100 ms window) via the backend's ILoudnessMeter
// (synthLoudness()) — K-weighted per ITU-R BS.1770, matching PERCEIVED loudness, so
// bright and percussive patches no longer read quiet and then blast. Trims center on
// the median voice loudness; a loose peak cap keeps boosts sane and the downstream
// bus limiter/clamp catches whatever peaks through. Any backend that defines
// TDSP_HAS_REPLAYGAIN and provides the hooks gets this sweep.
#ifdef TDSP_HAS_REPLAYGAIN
static int cmpFloatAsc(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}
// Cooperative wait for the (otherwise blocking) sweep: keeps the USB stack serviced
// so Windows' MTP driver watchdog doesn't re-enumerate the device mid-sweep and drop
// the COM port (which is what killed the first capture attempts ~140s in). Audio runs
// from the ISR independently, so the note keeps sounding for the full duration.
static void sweepWait(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
#if TDSP_HAS_SDCARD
        MTP.loop();          // service the MTP endpoint so the host doesn't time it out
#endif
        g_usbHost.Task();
        yield();
    }
}
FLASHMEM static void runGainSweep(int startIdx) {
    static const int   kNote     = 60;     // C4 reference note
    static const int   kVel      = 100;
    static const int   kWinMs    = 100;    // short-term loudness window (~ear integration time)
    static const int   kWindows  = 14;     // 14 * 100ms = 1.4s hold: covers slow-attack pads
    static const int   kTailMs   = 350;    // let the note decay before the next voice
    static const float kPeakCeil = 1.40f;  // loose raw-peak cap; the bus limiter cleans the rest
    static const float kMinTrim  = 0.10f, kMaxTrim = 6.0f;
    const int N = synthNumInstruments();   // 320

    static DMAMEM float loud[320], peak[320];  // RAM2: keep 2.5 KB out of the tight RAM1/stack
    if (N > 320) { Serial.println("[gain] N>320, aborting"); return; }

    if (startIdx < 0) startIdx = 0;
    if (g_player.isPlaying()) songStop();
    const bool wasMpe   = g_mpeMode;
    const int  savedInst = synthInstrument();
    applyMidiMode(false);                  // deterministic: single note, normal alloc
    g_synthSink->onAllNotesOff(0);

    Serial.printf("[gain] sweep begin: voices %d..%d, note=%d vel=%d, K-weighted max-short-term (%dx%dms) (LOUD, ~%d min)\n",
                  startIdx, N - 1, kNote, kVel, kWindows, kWinMs,
                  ((N - startIdx) * (kWindows * kWinMs + kTailMs + 120)) / 60000 + 1);

    // Pass 1 — for each voice, max short-term K-weighted loudness + raw peak. EVERY voice is
    // printed (host stitches the per-voice "V=.. loud=.. peak=.." lines into the table), so a
    // freeze mid-sweep only loses the current voice — resume with "@GAIN=<next index>".
    tdsp::ILoudnessMeter *probe = synthLoudness();
    for (int i = startIdx; i < N; ++i) {
        g_synthSink->onAllNotesOff(0);
        synthSetInstrument(i);             // NOTE: this sets the audition trim to the baked value...
        synthAuditionTrim()->setGain(1.0f);// ...so force UNITY — we must measure RAW loudness.
        sweepWait(60);
        probe->reset();                    // clears RMS, peak, and K-weight filter state
        g_synthSink->onNoteOn(1, kNote, kVel);
        float maxST = 0.0f;
        for (int w = 0; w < kWindows; ++w) {
            probe->resetRms();             // new 100ms window; keep filter state (no restart) + peak
            sweepWait(kWinMs);
            float st = probe->rms();
            if (st > maxST) maxST = st;
        }
        g_synthSink->onNoteOff(1, kNote, 0);
        sweepWait(kTailMs);
        loud[i] = maxST;                   // perceptual loudness of this voice
        peak[i] = probe->peak();           // raw peak accumulated across the whole note
        Serial.printf("[gain] V=%d/%d loud=%.5f peak=%.5f  %s\n",
                      i, N, (double)loud[i], (double)peak[i], synthInstrumentName(i));
    }
    g_synthSink->onAllNotesOff(0);

    // Restore prior state now — the paste-block below is a convenience only when a FULL run
    // (startIdx==0) completes; otherwise the host computes the table from the V= lines.
    if (startIdx > 0) {
        synthSetInstrument(savedInst);
        applyMidiMode(wasMpe);
        Serial.printf("[gain] partial sweep done (%d..%d) — host stitches V= lines\n", startIdx, N - 1);
        return;
    }

    // Target = median perceptual loudness (over voices that actually sounded), so trims
    // center near 1.0 and roughly half the voices go up, half down.
    static DMAMEM float sorted[320]; int m = 0;
    for (int i = 0; i < N; ++i) if (loud[i] > 1e-5f) sorted[m++] = loud[i];
    qsort(sorted, m, sizeof(float), cmpFloatAsc);
    const float target = m ? sorted[m / 2] : 0.1f;
    Serial.printf("[gain] target(median) loud=%.4f over %d sounding voices; peakCeil=%.2f\n",
                  (double)target, m, (double)kPeakCeil);

    // Pass 2 — compute + print the paste-ready table (labeled per the active backend).
    Serial.printf("[gain] ---- paste the block below over %s[] in the backend's trim header ----\n",
                  synthTrimSymbol());
    Serial.printf("static const float %s[%d] = {\n", synthTrimSymbol(), N);
    char lb[200];
    for (int i = 0; i < N; ++i) {
        float byLoud = loud[i] > 1e-5f ? target    / loud[i] : 1.0f;
        float byPeak = peak[i] > 1e-5f ? kPeakCeil / peak[i] : kMaxTrim;
        float trim = byLoud < byPeak ? byLoud : byPeak;   // min: loudness-match but cap extreme boosts
        if (trim < kMinTrim) trim = kMinTrim;
        if (trim > kMaxTrim) trim = kMaxTrim;
        if (i % 32 == 0) Serial.printf("    // bank %d\n", i / 32);
        int col = i % 16;
        if (col == 0) { lb[0] = 0; strcat(lb, "    "); }
        char cell[16]; snprintf(cell, sizeof(cell), "%.3ff,", (double)trim);
        strcat(lb, cell);
        if (col == 15 || i == N - 1) Serial.println(lb);
    }
    Serial.println("};");
    Serial.println("[gain] ---- end paste block ----");

    // Restore prior state.
    synthSetInstrument(savedInst);         // reapplies the baked trim for this voice
    applyMidiMode(wasMpe);
    Serial.println("[gain] sweep done");
}
#endif
