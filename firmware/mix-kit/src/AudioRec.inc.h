// AudioRec.inc.h — the "Audio Recorder" card: record the MASTER OUTPUTS to /recordings/*.wav on
// the SD card and play recordings back. Opt-in: -D TDSP_AUDIOREC=1 (needs the post mixer).
//
// Signal path (see the post-mixer block in main.cpp, which includes this file's objects):
//   postL/postR --> g_arec (in 0/1: RECORDED + passed through) --> tdmOut (app master fader) --> DAC
//   g_arecPlay (SD WAV, int16) --> I16toF32 --> g_arec (in 2/3: SUMMED into the output, not recorded)
// So a take captures everything that reaches the outputs at FULL level (the header VOL fader is
// after the tap), and a take played back at the same fader position is exactly as loud in the
// phones as the live performance was. Playback can't re-record itself.
//
// Commands (@AREC.<cmd>): START | STOP | PLAY=<name|/path> | PAUSE (toggle) | STOPPLAY |
//   DEL=<name> | STATUS. Every command answers with the status line "@AREC=<json>" on BOTH lanes
//   (ctrl broadcast), and the same line is pushed once a second while recording or playing so the
//   app's timer/progress stay live. @STATE carries the same object as "arec" (+ caps.arec).
// The app lists takes with the generic @LS browser (/recordings, .wav) and steps prev/next itself.
//
// SD contention: the WAV player reads the card inside the audio update (like the SD drum sampler)
// while recording writes from loop(); the two never run together (START stops playback, PLAY is
// refused while a take is open). One file per take: rec_NNNN.wav, numbered from what's on the card.
#pragma once
#if TDSP_AUDIOREC

static const char* kArecDir = "/recordings";
static char     g_arecPlayFile[48] = "";   // basename of the take playing/paused ("" = stopped)
static uint32_t g_arecPlayStart = 0;
static uint32_t g_arecLastPush  = 0;
static bool     g_arecReady     = false;
static bool     g_arecPaused    = false;   // AudioPlaySdResmp has no pause: we freeze its rate (0) and mute its inputs

static const char* arecBase(const char* p) { const char* b = strrchr(p, '/'); return b ? b + 1 : p; }
static void arecPath(char* out, size_t cap, const char* name) {
    if (name[0] == '/') snprintf(out, cap, "%s", name); else snprintf(out, cap, "%s/%s", kArecDir, name);
}
static const char* arecPlayState() {
    if (!g_arecPlayFile[0]) return "stop";
    return g_arecPaused ? "pause" : "play";
}
static void arecSetPaused(bool p) {
    g_arecPaused = p;
    g_arec.setPlayerMute(p);
    g_arecPlay.setPlaybackRate(p ? 0.0f : 1.0f);   // 0 = the reader stops advancing (position holds)
}
FLASHMEM static void arecEmitJson(Print& p) {
    const bool playing = g_arecPlayFile[0] != 0;
    // AudioPlaySdResmp reports position/length in MONO-sample time (file bytes/2), i.e. 2x for our
    // stereo takes -> halve. ppk = player peak 0..1000 (proves playback reaches the outputs).
    p.printf("{\"ok\":%d,\"rec\":%d,\"file\":\"%s\",\"sec\":%lu,\"drop\":%lu,\"play\":{\"file\":\"%s\",\"st\":\"%s\",\"pos\":%lu,\"len\":%lu,\"ppk\":%d}}",
             g_arecReady ? 1 : 0, g_arec.recording() ? 1 : 0, g_arec.currentFile(),
             (unsigned long)g_arec.recordedSeconds(), (unsigned long)g_arec.dropped(),
             g_arecPlayFile, arecPlayState(),
             (unsigned long)(playing ? g_arecPlay.positionMillis() / 2000 : 0),
             (unsigned long)(playing ? g_arecPlay.lengthMillis() / 2000 : 0),
             (int)(g_arec.playerPeak() * 1000.0f));
}
FLASHMEM static void arecPush() { ctrl.print("@AREC="); arecEmitJson(ctrl); ctrl.print("\n"); }

// setup(), after the card is mounted.
FLASHMEM static void arecBegin() {
    g_arecReady = g_sdReady && g_arec.begin(SD, kArecDir, (uint32_t)(AUDIO_SAMPLE_RATE_EXACT + 0.5f));
    Serial.printf("[arec] %s (ring in %s)\n", g_arecReady ? "ready: /recordings" : "unavailable (no card or no RAM)",
                  g_arec.inPsram() ? "PSRAM" : "heap");
}
static void arecStopPlay() { g_arecPlay.stop(); g_arecPlayFile[0] = 0; arecSetPaused(false); }

// loop(): drain the recorder, notice the end of playback, push status once a second while active.
static void arecService() {
    g_arec.service();
    if (g_arecPlayFile[0] && !g_arecPlay.isPlaying() && !g_arecPaused && millis() - g_arecPlayStart > 500) {
        g_arecPlayFile[0] = 0;           // reached the end (play() takes a block or two to report isPlaying -> grace)
        arecPush();
    }
    const uint32_t now = millis();
    if ((g_arec.busy() || g_arecPlayFile[0]) && now - g_arecLastPush >= 1000) { g_arecLastPush = now; arecPush(); }
}

FLASHMEM static void arecCommand(const char* cmd, Stream& reply) {
    if (!g_arecReady) { reply.print("@AREC_ERR=no SD card\n"); return; }
    if (!strcmp(cmd, "START")) {
        arecStopPlay();                                           // one SD stream at a time
        if (!g_arec.startRecording()) { reply.print("@AREC_ERR=could not start (busy, or the card refused the file)\n"); return; }
        Serial.printf("[arec] recording %s/%s\n", kArecDir, g_arec.currentFile());
    } else if (!strcmp(cmd, "STOP")) {
        g_arec.stopRecording();
        g_arec.service();                                         // flush + close now so the take is listable at once
        Serial.printf("[arec] saved %s (%lu s, %lu dropped blocks)\n", g_arec.currentFile(),
                      (unsigned long)g_arec.recordedSeconds(), (unsigned long)g_arec.dropped());
    } else if (!strncmp(cmd, "PLAY=", 5)) {
        if (g_arec.busy()) { reply.print("@AREC_ERR=stop recording first\n"); return; }
        char path[80]; arecPath(path, sizeof(path), cmd + 5);
        arecStopPlay();
        if (!g_arecPlay.playWav(path)) { reply.printf("@AREC_ERR=cannot play %s\n", path); return; }
        strncpy(g_arecPlayFile, arecBase(path), sizeof(g_arecPlayFile) - 1); g_arecPlayFile[sizeof(g_arecPlayFile) - 1] = 0;
        g_arecPlayStart = millis();
    } else if (!strcmp(cmd, "PAUSE")) {
        if (g_arecPlayFile[0]) arecSetPaused(!g_arecPaused);
    } else if (!strcmp(cmd, "STOPPLAY")) {
        arecStopPlay();
    } else if (!strncmp(cmd, "DEL=", 4)) {
        char path[80]; arecPath(path, sizeof(path), cmd + 4);
        if (g_arec.busy() && !strcmp(arecBase(path), g_arec.currentFile())) { reply.print("@AREC_ERR=that take is still recording\n"); return; }
        if (!strcmp(arecBase(path), g_arecPlayFile)) arecStopPlay();
        if (!SD.remove(path)) { reply.printf("@AREC_ERR=cannot delete %s\n", path); return; }
    } else if (strcmp(cmd, "STATUS") != 0 && *cmd) {
        reply.printf("@AREC_ERR=unknown %s\n", cmd); return;
    }
    arecPush();
}

#endif // TDSP_AUDIOREC
