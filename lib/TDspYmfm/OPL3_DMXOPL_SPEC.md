# OPL3 + DMXOPL engine — spec for `lib/TDspYmfm`

**Goal:** add a **General-MIDI OPL3 synth** to the ymfm library so the T-DSP can
play GM `.mid` files with the *right* instrument per part and real drums —
something the OPM (YM2151) backend can't do (arcade chip, 4 hand-mapped banks).

OPL3 (YMF262) is natively multitimbral (18× 2-op channels, or 6× 4-op + 6× 2-op
+ 5 percussion), and **DMXOPL** is a complete, expertly-voiced GM bank (all 128
melodic programs + a drum kit — the refined Doom/DMX OPL3 set). Together they are
the correct engine for a MIDI-file player.

This spec is the **API contract** the mix-kit's `SynthBackendOpl3.h` /
`Opl3Sink.h` are already written against (see
`projects/spike_esp32_bt_spdif_mix_kit_f32/src/`). Match these signatures and the
backend drops in with no changes — just flip `-D TDSP_SYNTH_OPL3`.

---

## 1. Vendor the OPL core

Add ymfm's OPL core next to the OPM core already vendored
(`src/ymfm/ymfm_opm.{h,cpp}`): `ymfm_opl.h` + `ymfm_opl.cpp` (they reuse the same
`ymfm.h` / `ymfm_fm.*` base already present). Use `ymfm::ymf262` (OPL3).

Keep it BSD-3-Clause clean — OPL core only, no ADPCM/PCM.

## 2. `AudioSynthYmfmOPL3` — the engine (`src/AudioSynthYmfmOPL3.{h,cpp}`)

A Teensy `AudioStream` source, **stereo int16** (output 0 = L, 1 = R), mirroring
`AudioSynthYmfmOPM`'s resample-to-AUDIO_SAMPLE_RATE approach. The chip runs at
its native rate (~14.32 MHz / 288 for OPL3); `update()` linear-resamples to the
graph rate, same as the OPM wrapper.

It is a **self-contained GM MIDI synth**: it owns the 18-channel voice allocator,
the per-MIDI-channel program state, the drum-channel (10) percussion mapping, and
the loaded DMXOPL bank. The caller only speaks GM MIDI.

```cpp
class AudioSynthYmfmOPL3 : public AudioStream {  // 2 outputs: 0=L, 1=R (int16)
public:
    AudioSynthYmfmOPL3();

    // Reset chip + resampler and load the BAKED default bank (DMXOPL). Call once
    // in setup() after AudioMemory. After this the engine is playable.
    void begin();

    // --- GM MIDI (call from loop / MIDI handlers, not the ISR) -----------------
    // channel is 1..16 (GM). Channel 10 is the drum channel: `note` selects the
    // percussion instrument from the bank; program on ch10 is ignored.
    void noteOn (uint8_t channel, uint8_t note, uint8_t velocity);
    void noteOff(uint8_t channel, uint8_t note);
    void programChange(uint8_t channel, uint8_t program);      // melodic patch (0..127)
    void pitchBend(uint8_t channel, float semitones);          // per-channel, already scaled
    void controlChange(uint8_t channel, uint8_t cc, uint8_t v); // 1=mod 7=vol 64=sustain 120/123=off
    void allNotesOff();

    void setGain(float g);        // output trim (match OPM's ~2.0 default feel)
    int  activeVoices() const;    // telemetry (OPL channels sounding)

    // --- bank -----------------------------------------------------------------
    // Override the baked bank at runtime. Accept DMXOPL in WOPL (preferred, has
    // OPL3 4-op) and/or OP2 (Doom GENMIDI, 175 instruments). Return #instruments
    // loaded, or -1 on parse error. No I/O here — caller hands a whole buffer.
    int loadBankWopl(const uint8_t *data, size_t len);
    int loadBankOp2 (const uint8_t *data, size_t len);

    // --- catalog (for the app instrument picker) ------------------------------
    int         numMelodic() const;              // typically 128 (GM)
    const char *melodicName(int gm) const;       // GM program name, "" if out of range

    virtual void update(void) override;
};
```

### Behavior notes the backend relies on
- **`begin()` must load a usable default bank** (bake DMXOPL as a `const` header,
  e.g. `dmxopl_bank_data.h`, parsed in `begin()`), so the engine is playable with
  zero SD card. The mix-kit optionally overrides it from `/opl/*.wopl` on SD.
- **Drums on channel 10** are the engine's job (GM). The mix-kit will run the
  player with drums UNMASKED for OPL3 and route ch10 straight in.
- **Voice allocation**: dynamic across the 18 (2-op) channels with oldest-note
  stealing, like the OPM allocator — but here the *instrument* is per MIDI
  channel (from its last `programChange`), not per OPL channel.
- **Velocity** → OPL total-level on the carrier(s), per the DMXOPL instrument.
- **Stereo**: use OPL3's L/R enable bits; simplest is both on (mono-ish) to start,
  per-channel panning later.

## 3. WOPL / OP2 parser (`src/OplBank.{h,cpp}`)

Mirror `OpmBank.h`'s shape (pure, no I/O):

```cpp
namespace tdsp { namespace ymfmopl {
struct OplVoice { char name[24]; /* 2 operators × params, feedback/algo, 4-op flag, … */ };
int parseWoplBank(const uint8_t *data, size_t len, OplVoice *melodic, int maxMel,
                  OplVoice *drums, int maxDrum);   // returns melodic count, -1 on error
int parseOp2Bank (const uint8_t *data, size_t len, OplVoice *melodic, int maxMel,
                  OplVoice *drums, int maxDrum);   // Doom GENMIDI: 175 instruments
}}
```

Format refs: **WOPL** = libADLMIDI "OPL3 Bank Editor" format (magic `"WOPL3-BANK\0"`,
versioned header, 2×(melodic+percussion) bank slots). **OP2** = Doom `GENMIDI`
lump (magic `"#OPL_II#"`, 175 fixed 36-byte records + name table). DMXOPL ships in
both; WOPL preserves its OPL3 4-op voices, OP2 is 2-op only.

## 4. Baked DMXOPL data (`src/dmxopl_bank_data.h`)

Bake the DMXOPL bank as a `const` byte array (or the pre-parsed `OplVoice[]`) so
`begin()` has a default with no SD. DMXOPL is CC-BY-SA — keep the license/attrib
header in the file.

## 5. Standalone bring-up (optional but recommended)

A `spike_midi_opl3` (like `spike_midi_ymfm_opm`) that plays the baked player songs
through OPL3 → TAC5212, with serial cmds to confirm: note, program change, drum
hit, `activeVoices` in the heartbeat, CPU. Verify a dense multi-channel song
(Daft Punk) sounds *right* and fits CPU on the Teensy 4.1 before we flip the
mix-kit env.

---

## What's already done on the mix-kit side (waiting on the above)
- `src/Opl3Sink.h` — `tdsp::MidiSink` → the GM methods above (channel-addressed,
  no filtering; the engine does GM routing incl. drums).
- `src/SynthBackendOpl3.h` — engine wired **stereo → int16→F32 bridge → mix slot
  3**, the `synth*` interface (name/description/catalog from `numMelodic`/
  `melodicName`), SD `/opl/*.wopl` override, and it sets the player's channel mask
  to **all channels** (drums on).
- `main.cpp` has the `#elif defined(TDSP_SYNTH_OPL3)` include branch.
- `platformio.ini` has a commented `[env:teensy41_opl3]` — uncomment + build once
  the engine + bank land.

So the only work is Sections 1–4 (engine + parser + bank). When
`AudioSynthYmfmOPL3` compiles, `pio run -e teensy41_opl3` builds the full
BT + S/PDIF + songs + app firmware on OPL3/DMXOPL with no mix-kit changes.
