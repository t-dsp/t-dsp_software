// tsf_drum_probe.cpp — offline render check for a drum SoundFont, using the SAME
// tsf.h the firmware ships (lib/TDspTsf). Proves a built /sf2/drumkits.sf2 actually
// SOUNDS on the real engine before it ever touches the SD card — it is the offline
// half of the "send 'T' over serial" on-device sweep.
//
// It selects bank 128 / <program> on channel 10 (exactly like SynthBackendSF2Tsf /
// DrumTsf), fires each requested GM note, renders it, and reports the peak level so a
// silent (missing/mis-mapped) hit is caught here rather than on the card. Optionally
// writes every hit to a 48 kHz/16-bit stereo WAV so you can also verify BY EAR.
//
// Build (driven by tools/fetch_drumkits.py --validate, which locates MSVC via vswhere):
//   cl /nologo /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS /I<repo>/lib/TDspTsf/src \
//      /Fe:tsf_drum_probe.exe tsf_drum_probe.cpp
//
// Run:
//   tsf_drum_probe <font.sf2> <program> <note,note,...> [out.wav]
// Exit code 0 = every requested note produced audio; 1 = one or more were SILENT.

#define TSF_IMPLEMENTATION
#include "tsf.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>

static void write_wav_s16_stereo(const char* path, const std::vector<short>& interleaved, int rate)
{
    FILE* fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "  (could not open %s for writing)\n", path); return; }
    uint32_t dataBytes = (uint32_t)(interleaved.size() * sizeof(short));
    uint32_t byteRate = (uint32_t)rate * 2 /*ch*/ * 2 /*bytes*/;
    uint32_t riff = 36 + dataBytes;
    fwrite("RIFF", 1, 4, fp); fwrite(&riff, 4, 1, fp); fwrite("WAVE", 1, 4, fp);
    fwrite("fmt ", 1, 4, fp);
    uint32_t fmtLen = 16; uint16_t pcm = 1, ch = 2, bits = 16, blockAlign = 4;
    uint32_t sr = (uint32_t)rate;
    fwrite(&fmtLen, 4, 1, fp); fwrite(&pcm, 2, 1, fp); fwrite(&ch, 2, 1, fp);
    fwrite(&sr, 4, 1, fp); fwrite(&byteRate, 4, 1, fp);
    fwrite(&blockAlign, 2, 1, fp); fwrite(&bits, 2, 1, fp);
    fwrite("data", 1, 4, fp); fwrite(&dataBytes, 4, 1, fp);
    fwrite(interleaved.data(), sizeof(short), interleaved.size(), fp);
    fclose(fp);
}

int main(int argc, char** argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <font.sf2> <program> <note,note,...> [out.wav]\n", argv[0]);
        return 2;
    }
    const char* sf2 = argv[1];
    int program = atoi(argv[2]);
    const char* outwav = (argc > 4) ? argv[4] : nullptr;

    // parse comma-separated GM note list
    std::vector<int> notes;
    for (char* tok = strtok(argv[3], ","); tok; tok = strtok(nullptr, ","))
        notes.push_back(atoi(tok));

    tsf* f = tsf_load_filename(sf2);
    if (!f) { fprintf(stderr, "LOAD FAILED: %s\n", sf2); return 1; }

    const int SR = 48000;
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, SR, 0.0f);
    // ch 10 (index 9), bank 128, chosen program — mirrors the firmware's drum routing.
    if (!tsf_channel_set_bank_preset(f, 9, 128, program)) {
        fprintf(stderr, "no bank-128 preset at program %d in %s\n", program, sf2);
        tsf_close(f);
        return 1;
    }

    const int onN  = SR * 45 / 100;   // 0.45 s note
    const int offN = SR * 20 / 100;   // 0.20 s tail
    std::vector<short> out;
    std::vector<short> on(onN * 2), off(offN * 2);
    int silent = 0;

    for (size_t i = 0; i < notes.size(); ++i) {
        int key = notes[i];
        long peak = 0;
        tsf_channel_note_on(f, 9, key, 1.0f);
        tsf_render_short(f, on.data(), onN, 0);
        for (short s : on) { long a = s < 0 ? -(long)s : s; if (a > peak) peak = a; }
        out.insert(out.end(), on.begin(), on.end());
        tsf_channel_note_off(f, 9, key);
        tsf_render_short(f, off.data(), offN, 0);
        for (short s : off) { long a = s < 0 ? -(long)s : s; if (a > peak) peak = a; }
        out.insert(out.end(), off.begin(), off.end());

        bool quiet = peak < 64;              // ~ -54 dBFS: real hits are far louder
        if (quiet) ++silent;
        printf("  note %3d  peak %5ld  %s\n", key, peak, quiet ? "SILENT" : "ok");
    }

    if (outwav) { write_wav_s16_stereo(outwav, out, SR); printf("  wrote %s\n", outwav); }
    printf("RESULT program %d: %d/%d audible, %d silent\n",
           program, (int)notes.size() - silent, (int)notes.size(), silent);
    tsf_close(f);
    return silent ? 1 : 0;
}
