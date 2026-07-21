// BoardTest.inc.h — `@BOARDTEST`: a whole-board health + full-pitch-bend self-test.
//
// Answers "is THIS board OK?" for a heterogeneous build (e.g. 2 Dexed + 2 OPLL + TSF
// drums): every melodic track must make sound AND reproduce a full ±24-semitone (two-
// octave) pitch bend in each direction, and the drum voice must sound. It drives each
// track's sink DIRECTLY (bypassing routing/arp so the test is about the engine, not the
// wiring) and captures the output BUS (g_outCap taps outL, so it's build-agnostic — it
// sees whichever one track is sounding). The PC side (tools/board_bend_test.py) FFTs each
// capture.
//
// Why capture at bend 0 / +24 / −24 and check RATIOS: a pitch bend shifts EVERY partial by
// the same factor, so f(+24)/f(0) == f(0)/f(−24) == 4.0 (two octaves) no matter which
// partial the FFT locks onto. A clamped bend (the old ±48-router-vs-±24-Dexed-cap bug)
// would show a ratio < 4 — this is exactly the regression the ±24 range fix removed, so the
// test guards it. Absolute pitch is reported too but the ratio is the pass/fail.
//
// Self-contained + gated by TDSP_BOARDTEST (independent of TDSP_DIAGNOSTICS, which the
// dexed2_opll2 build turns off). Needs a real capture buffer, so the board-test env also
// sets TDSP_CAP_FULL (the lean build otherwise stubs g_capBuf to 64 samples). Blocking —
// like every other self-test here — but the audio ISR keeps rendering, so sound plays.
#pragma once
#if TDSP_BOARDTEST

// Arm the output-bus probe, wait for it to fill, and dump the first `n` samples in the same
// `[cap] begin N rate R` … `[cap] end` text format the @CAP parser already reads.
static void boardTestDumpCap(Stream &out, int n) {
    g_outCap.arm();
    uint32_t t0 = millis();
    while (!g_outCap.done() && millis() - t0 < 2000) delay(1);
    int got = g_outCap.count();
    if (got > n) got = n;
    out.printf("[cap] begin %d rate %d\n", got, (int)AUDIO_SAMPLE_RATE_EXACT);
    const float *c = g_outCap.data();
    char lb[220];
    for (int i = 0; i < got; ) {
        int p = 0;
        for (int k = 0; k < 16 && i < got; k++, i++)
            p += snprintf(lb + p, sizeof(lb) - p, "%.5g ", (double)c[i]);
        out.println(lb);
    }
    out.println("[cap] end");
}

// Strike one note on `sink` (channel `ch`) and glide the held note to `semis`, then capture.
// Re-struck per bend point so amplitude is fresh at each pitch (clean FFT at the −24 octave).
static void boardTestNoteCap(Stream &out, tdsp::MidiSink *sink, uint8_t ch, uint8_t note,
                             float semis, int capN) {
    sink->onAllNotesOff(0);
    delay(25);
    sink->onPitchBend(ch, 0.0f);           // neutral, then strike, then glide (the real MPE order)
    sink->onNoteOn(ch, note, 112);
    sink->onPitchBend(ch, semis);
    delay(90);                             // let the glide land + amplitude establish before capturing
    boardTestDumpCap(out, capN);
    sink->onNoteOff(ch, note, 0);
    sink->onAllNotesOff(0);
    delay(20);
}

FLASHMEM static void runBoardTest(Stream &out) {
    const int     capN    = 4096;          // ~85 ms @ 48 k: enough cycles for the host to pitch-track the −24 (≈131 Hz) note
    const uint8_t note     = 72;           // C5; ±24 semis → C7 (≈2093 Hz) / C3 (≈131 Hz), both well inside every engine's range
    const float   bends[3] = { 0.0f, 24.0f, -24.0f };

    // Silence transport + every voice so each capture is ONE track alone on the bus.
    for (int v = 0; v < kSynthVoices; ++v) {
        songStop(g_tracks[v]);
        if (g_tracks[v].sink) g_tracks[v].sink->onAllNotesOff(0);
        if (g_tracks[v].arp)  g_tracks[v].arp->panic();
    }
    if (g_drumTrack.sink) g_drumTrack.sink->onAllNotesOff(0);
    delay(80);

    out.printf("[boardtest] BEGIN dexed=%d opll=%d drums=1 note=%d capN=%d rate=%d\n",
               kDexedVoices, kSynthVoices - kDexedVoices, note, capN, (int)AUDIO_SAMPLE_RATE_EXACT);

    // Each melodic track: sound + full bend both directions.
    for (int v = 0; v < kSynthVoices; ++v) {
        tdsp::MidiSink *sink = g_tracks[v].sink;
        if (!sink) continue;
        for (int b = 0; b < 3; ++b) {
            out.printf("[boardtest] track=%d eng=%s note=%d bend=%.0f\n",
                       v, voiceEngineName(v), note, (double)bends[b]);
            boardTestNoteCap(out, sink, 1, note, bends[b], capN);
        }
    }

    // Drum voice: no bend — just prove the TSF kit's core pieces sound (kick / snare / closed hat).
    if (g_drumTrack.sink) {
        const uint8_t drums[3] = { 36, 38, 42 };
        for (int d = 0; d < 3; ++d) {
            out.printf("[boardtest] drum note=%d\n", drums[d]);
            g_drumTrack.sink->onAllNotesOff(0);
            delay(20);
            g_drumTrack.sink->onNoteOn(10, drums[d], 118);
            delay(45);                     // let the transient bloom into the capture window
            boardTestDumpCap(out, capN);
            g_drumTrack.sink->onNoteOff(10, drums[d], 0);
            delay(20);
        }
    }

    out.println("[boardtest] END");
}

// @MPETEST — the SAME full-bend check, but driven through the real MPE INPUT PATH instead of
// the sink directly: for each track we simulate a LinnStrument-in-MPE by driving that track's
// router (router -> arp[bypass] -> sink -> engine) with a per-member-channel RPN bend-range
// announce and RAW 14-bit pitch bend (0 / +8191 / -8192), exactly as USBHost_t36 delivers it.
// This proves the parse + scale + range + routing actually turn a full-surface drag into ±24 at
// the engine (whereas @BOARDTEST bypasses all of that). Emits the same [boardtest] markers so
// tools/board_bend_test.py analyzes it unchanged — a router that only reached ±12 would show
// up as +12 and FAIL. The `bend=` in the marker is the INTENDED semitones; the raw wheel value
// that produces it is 0 / +8191 / -8192, and we set the router's range to 24 via RPN.
FLASHMEM static void runMpeInputTest(Stream &out) {
    const int     capN     = 4096;
    const uint8_t note     = 72;
    const uint8_t ch       = 2;            // an MPE member channel (LinnStrument uses 2..16, master 1)
    const int16_t raws[3]  = { 0, 8191, -8192 };   // center, full up, full down — a full-surface drag
    const float   wants[3] = { 0.0f, 24.0f, -24.0f };

    const bool wasMpe = g_mpeMode;
    applyMidiMode(true);                    // MPE mode: router bend range 24 on every channel + backend MPE alloc

    for (int v = 0; v < kSynthVoices; ++v) {
        songStop(g_tracks[v]);
        if (g_tracks[v].sink) g_tracks[v].sink->onAllNotesOff(0);
        if (g_tracks[v].arp)  g_tracks[v].arp->panic();
    }
    if (g_drumTrack.sink) g_drumTrack.sink->onAllNotesOff(0);
    delay(80);

    out.printf("[boardtest] BEGIN mpe-input dexed=%d opll=%d note=%d capN=%d rate=%d ch=%d range=24\n",
               kDexedVoices, kSynthVoices - kDexedVoices, note, capN, (int)AUDIO_SAMPLE_RATE_EXACT, ch);

    for (int v = 0; v < kSynthVoices; ++v) {
        tdsp::MidiRouter *r = g_tracks[v].router;
        if (!r) continue;
        if (g_tracks[v].arp) g_tracks[v].arp->setEnabled(false);   // bypass -> pure passthrough (the real MPE case)
        // Simulate the LinnStrument announcing its per-note bend range (24 semis) via RPN 0,0.
        r->handleControlChange(ch, 101, 0);
        r->handleControlChange(ch, 100, 0);
        r->handleControlChange(ch, 6, 24);
        for (int b = 0; b < 3; ++b) {
            r->handleControlChange(ch, 123, 0);          // all notes off on the member channel
            delay(25);
            r->handlePitchBend(ch, 0);
            r->handleNoteOn(ch, note, 112);
            r->handlePitchBend(ch, raws[b]);             // RAW 14-bit, as the controller sends
            delay(90);
            out.printf("[boardtest] track=%d eng=%s note=%d bend=%.0f\n",
                       v, voiceEngineName(v), note, (double)wants[b]);
            boardTestDumpCap(out, capN);
            r->handleNoteOff(ch, note, 0);
            r->handleControlChange(ch, 123, 0);
            delay(20);
        }
    }

    out.println("[boardtest] END");
    applyMidiMode(wasMpe);                  // restore the prior mode
}
#endif  // TDSP_BOARDTEST
