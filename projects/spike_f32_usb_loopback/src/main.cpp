// Spike: F32 USB stereo loopback through unity mixer to TDM slots 0/1.
//
// M4c state: full F32 audio graph wired:
//
//   USB host  ---24-bit/48k--->  AudioInputUSB_F32  (Q31 ring -> F32)
//                                     |
//                                     +-> AudioMixer4_F32 (unity)  -> AudioOutputTDM_F32 slot 0/1
//                                     |
//                                     +-> AudioOutputUSB_F32  ---24-bit/48k---> USB host  (loopback)
//
// The mixer is a placeholder for future DSP -- DSP blocks drop into
// the slot between usbIn and tdmOut without touching any other wiring.
//
// Loopback to host means the user can confirm round-trip by playing a
// signal from a DAW into the device and recording it back from the
// device input track. Round-trip latency ~ 2 audio blocks plus USB
// jitter (a few ms).

#include <Arduino.h>
#include <Wire.h>
#include <SD.h>
#include <AudioStream_F32.h>
#include <AudioSettings_F32.h>
#include <AudioMixer_F32.h>
#include <AudioSDPlayer_F32.h>
#include "AudioOutputTDM_F32.h"
#include "AudioInputUSB_F32.h"
#include "AudioOutputUSB_F32.h"
#include "tac5212_regs.h"

// SHDNZ for the TAC5212 (and the 6140 ADC, which shares this pin on this
// board). Per project_shdnz_pin memory: this pin may be toggled exactly
// ONCE at boot; after that, drivers must use SW_RESET via I2C only.
const int TAC5212_EN_PIN = 35;

AudioInputUSB_F32   usbIn;     // unused in SD test; left as orphan
AudioOutputUSB_F32  usbOut;    // unused in SD test; left as orphan
AudioSDPlayer_F32   sdPlayer;
AudioMixer4_F32     mixL;
AudioMixer4_F32     mixR;
AudioOutputTDM_F32  tdmOut;

// SD WAV L -> mixer L -> TDM slot 0
AudioConnection_F32 patchSdL_mix (sdPlayer, 0, mixL,   0);
AudioConnection_F32 patchMixL_tdm(mixL,     0, tdmOut, 0);

// SD WAV R -> mixer R -> TDM slot 1
AudioConnection_F32 patchSdR_mix (sdPlayer, 1, mixR,   0);
AudioConnection_F32 patchMixR_tdm(mixR,     0, tdmOut, 1);

// Raw-register I2C write to the TAC5212. Mirrors the production project's
// writeReg() helper (projects/t-dsp_tac5212_audio_shield_adaptor/src/main.cpp)
// rather than going through the typed lib/TAC5212 API for boot config.
//
// Why raw writes: production calls this the "Phase 1 hand-rolled path,
// preserved as the working audio config" (production main.cpp:2000) and
// explicitly notes "Never call g_codec.begin() — that would SW-reset the
// chip and wipe the hand-rolled init" (production main.cpp:4503-4506).
// The typed API is mature for runtime register access (used by Tac5212Panel)
// but the hand-rolled register sequence is the validated boot path. The
// spike mirrors it exactly to take the codec config out of the variable set.
static void writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// M4g: read a single register for diagnostic purposes (matches the production
// readReg helper in t-dsp_tac5212_audio_shield_adaptor/src/main.cpp).
static uint8_t readReg(uint8_t reg) {
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_I2C_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

// M4j: software-shutdown the TLV320ADC6140 ADC chip. SHDNZ is shared with
// the TAC5212 so the 6140 powers up alongside; the spike doesn't capture
// audio and doesn't init the 6140, leaving it in POR-default state. Per
// project_6140_buffer_contention memory the 6140 board has a 74LVC1G125
// always-on buffer that fights the TAC5212 on slots 0-3. Even in TX-side
// contention the shared analog supply / ground may couple noise into the
// TAC5212 DAC output. Hypothesis: SW-resetting the 6140 quiets it.
//
// SW_RESET puts the 6140 back at POR which is sleep mode (REG_SLEEP_CFG
// bit 0 SLEEP_ENZ = 0). Chip stops driving anything afterward.
static constexpr uint8_t ADC6140_I2C_ADDR = 0x4C;
static void shutdownAdc6140() {
    Wire.beginTransmission(ADC6140_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("ADC6140 not on I2C (skipping shutdown).");
        return;
    }
    Wire.beginTransmission(ADC6140_I2C_ADDR);
    Wire.write(0x01);  // REG_SW_RESET
    Wire.write(0x01);  // SW_RESET_TRIGGER
    Wire.endTransmission();
    delay(20);
    Serial.println("ADC6140 SW reset (now in sleep).");
}

// M4g: codec status snapshot taken once at end of setupCodec() and printed
// by the diag loop. DEV_STS0 = channel power status; DEV_STS1 = PLL/mode.
static uint8_t g_codec_sts0 = 0xFF;
static uint8_t g_codec_sts1 = 0xFF;

// DAC-only subset of production's setupCodecHandRolled(). Skips:
//   codecConfigurePdmMic       — no PDM mic on the spike
//   codecConfigureAdcInputs    — no analog input capture
//   codecEnableDspAvddSel      — only matters with the DSP limiter on
//   codecApplyEarFatigueDefaults — biquad EQ polish, optional
static void setupCodec() {
    // SW reset + wake from sleep.
    writeReg(REG_SW_RESET, 0x01);
    delay(100);
    writeReg(REG_SLEEP_CFG, SLEEP_CFG_WAKE);
    delay(100);

    // Serial interface: TDM, 32-bit slot, 24-bit data left-justified,
    // BCLK inverted (matches the SAI1 config in AudioOutputTDM_F32).
    // PASI offset = 1 BCLK = standard 1-bit FSYNC width / FSE.
    writeReg(REG_PASI_CFG0,    PASI_CFG0_TDM_24_BCLK_INV);
    writeReg(REG_INTF_CFG1,    INTF_CFG1_DOUT_PASI);
    writeReg(REG_INTF_CFG2,    INTF_CFG2_DIN_ENABLE);
    writeReg(REG_PASI_RX_CFG0, PASI_OFFSET_1);
    writeReg(REG_PASI_TX_CFG1, PASI_OFFSET_1);

    // Slot mappings: TDM slot 0 -> RX CH1 (DAC L), slot 1 -> RX CH2 (DAC R).
    // TX paths kept symmetric in case the user ever wants ADC capture later.
    writeReg(REG_RX_CH1_SLOT, slot(0));
    writeReg(REG_RX_CH2_SLOT, slot(1));
    writeReg(REG_TX_CH1_SLOT, slot(0));
    writeReg(REG_TX_CH2_SLOT, slot(1));

    // M4m: configure ADC inputs to single-ended INP-only mode.
    writeReg(REG_ADC_CH1_CFG0, ADC_CFG0_SE_INP_ONLY);
    writeReg(REG_ADC_CH2_CFG0, ADC_CFG0_SE_INP_ONLY);

    // M4n: PDM mic pin config (production main.cpp:2040-2046). We don't have
    // a PDM mic, but production sets these pins to known states. Maybe the
    // POR-default GPIO state has some side-effect on the analog stage.
    writeReg(REG_GPIO1_CFG, GPIO1_PDM_CLK);
    writeReg(REG_GPI1_CFG,  GPI1_INPUT);
    writeReg(REG_INTF_CFG4, INTF_CFG4_GPI1_PDM_3_4);
    writeReg(REG_TX_CH3_SLOT, slot(2));
    writeReg(REG_TX_CH4_SLOT, slot(3));

    // OUT1 / OUT2: source = DAC, mono SE-positive routing, headphone driver
    // at 0 dB level. Same config the production project ships.
    writeReg(REG_OUT1_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT1_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT1_CFG2, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG0, OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P);
    writeReg(REG_OUT2_CFG1, OUT_CFG1_HP_0DB);
    writeReg(REG_OUT2_CFG2, OUT_CFG1_HP_0DB);

    // Boot-gate: power up DAC at -inf so we don't pop on PWR_CFG enable.
    writeReg(REG_DAC_L1_VOL, 0);
    writeReg(REG_DAC_R1_VOL, 0);
    writeReg(REG_DAC_L2_VOL, 0);
    writeReg(REG_DAC_R2_VOL, 0);

    // Power up: enable all channel slots, then PWR_CFG = ADC+DAC+MICBIAS.
    writeReg(REG_CH_EN,   CH_EN_ALL);
    writeReg(REG_PWR_CFG, PWR_CFG_ADC_DAC_MICBIAS);
    delay(100);

    // M4o: production has ~hundreds of ms between PWR_CFG and unmute (filled
    // by ADC6140 init / audio memory / synth init). Spike races straight
    // to unmute. Maybe the chip needs the extra settling time.
    delay(1000);

    // M4m: unmute restored after we confirmed mute = silence.
    writeReg(REG_DAC_L1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R1_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_L2_VOL, DAC_VOL_0DB);
    writeReg(REG_DAC_R2_VOL, DAC_VOL_0DB);

    // M4n: codecEnableDspAvddSel (production main.cpp:2086-2093). Sets bit 1
    // of MISC_CFG0 on PAGE 1 (= 0x2D). Production's comment says this is
    // needed for the DSP Limiter, BOP, and DRC blocks. We don't use those,
    // but the production memory note says "POR is 'Reserved' (0)" — and
    // Reserved POR values are exactly the kind of thing that produce
    // unspecified behavior including analog noise.
    //
    // Page select uses register 0 = page #. After writing page 1, all
    // register accesses target page 1 until we switch back to page 0.
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(0x00); Wire.write(0x01);  // select page 1
    Wire.endTransmission();

    // RMW MISC_CFG0 (page 1 reg 0x2D): set bit 1 (DSP_AVDD_SEL).
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(0x2D);
    Wire.endTransmission(false);
    Wire.requestFrom(TAC5212_I2C_ADDR, (uint8_t)1);
    uint8_t misc_cfg0 = Wire.available() ? Wire.read() : 0;
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(0x2D); Wire.write(misc_cfg0 | 0x02);  // DSP_AVDD_SEL = 1
    Wire.endTransmission();

    // Switch back to page 0 for any subsequent reads (e.g. dumpCodecState).
    Wire.beginTransmission(TAC5212_I2C_ADDR);
    Wire.write(0x00); Wire.write(0x00);
    Wire.endTransmission();

    // M4g: snapshot codec channel-power and PLL-lock status after init.
    delay(10);  // give the chip a moment to update its status registers
    g_codec_sts0 = readReg(REG_DEV_STS0);
    g_codec_sts1 = readReg(REG_DEV_STS1);
}

// M4h: read back every register we wrote and print as REG=ACTUAL/EXPECTED.
// Any mismatch means an I2C write was silently rejected or the chip is in
// an unexpected page, etc.
static void dumpCodecState() {
    struct R { const char *name; uint8_t reg; uint8_t expect; };
    const R checks[] = {
        {"PASI_CFG0   ", REG_PASI_CFG0,    PASI_CFG0_TDM_24_BCLK_INV},
        {"INTF_CFG1   ", REG_INTF_CFG1,    INTF_CFG1_DOUT_PASI},
        {"INTF_CFG2   ", REG_INTF_CFG2,    INTF_CFG2_DIN_ENABLE},
        {"PASI_RX_CFG0", REG_PASI_RX_CFG0, PASI_OFFSET_1},
        {"PASI_TX_CFG1", REG_PASI_TX_CFG1, PASI_OFFSET_1},
        {"RX_CH1_SLOT ", REG_RX_CH1_SLOT,  slot(0)},
        {"RX_CH2_SLOT ", REG_RX_CH2_SLOT,  slot(1)},
        {"OUT1_CFG0   ", REG_OUT1_CFG0,    (uint8_t)(OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P)},
        {"OUT1_CFG1   ", REG_OUT1_CFG1,    OUT_CFG1_HP_0DB},
        {"OUT1_CFG2   ", REG_OUT1_CFG2,    OUT_CFG1_HP_0DB},
        {"OUT2_CFG0   ", REG_OUT2_CFG0,    (uint8_t)(OUT_SRC_DAC | OUT_ROUTE_MONO_SE_P)},
        {"OUT2_CFG1   ", REG_OUT2_CFG1,    OUT_CFG1_HP_0DB},
        {"OUT2_CFG2   ", REG_OUT2_CFG2,    OUT_CFG1_HP_0DB},
        {"DAC_L1_VOL  ", REG_DAC_L1_VOL,   DAC_VOL_0DB},
        {"DAC_R1_VOL  ", REG_DAC_R1_VOL,   DAC_VOL_0DB},
        {"DAC_L2_VOL  ", REG_DAC_L2_VOL,   DAC_VOL_0DB},
        {"DAC_R2_VOL  ", REG_DAC_R2_VOL,   DAC_VOL_0DB},
        {"CH_EN       ", REG_CH_EN,        CH_EN_ALL},
        {"PWR_CFG     ", REG_PWR_CFG,      PWR_CFG_ADC_DAC_MICBIAS},
    };
    Serial.println("--- TAC5212 register readback ---");
    for (const auto &r : checks) {
        const uint8_t actual = readReg(r.reg);
        const char *mark = (actual == r.expect) ? "  " : "!!";
        Serial.printf("%s %s: 0x%02X (expect 0x%02X)\n",
                      mark, r.name, actual, r.expect);
    }
    Serial.printf("   DEV_STS0    : 0x%02X\n", g_codec_sts0);
    Serial.printf("   DEV_STS1    : 0x%02X\n", g_codec_sts1);
    Serial.println("--- end ---");
}

// M4e: codec ack stash. Set once in setup(), printed by the diag loop.
static uint8_t g_codec_ack = 0;

// M4q: visual diagnostic that doesn't depend on the serial monitor.
// LED blink period encodes SD / WAV state:
//   200 ms (fast)  : WAV is playing  (sdPlayer.isPlaying())
//   500 ms (slow)  : alive but WAV NOT playing (file missing, ended, etc.)
//   100 ms (panic) : SD.begin() failed at boot
static volatile bool g_sd_init_ok = false;

void setup() {
    // SHDNZ one-shot. Powers the codec LDO and brings it out of HW reset.
    // Per project_shdnz_pin memory, this is the ONLY allowed toggle of
    // pin 35 across the whole firmware lifetime.
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);
    delay(5);                            // > 100 us SHDNZ low spec, generous
    digitalWrite(TAC5212_EN_PIN, HIGH);
    delay(10);                           // settle internal supplies before I2C

    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);

    // Wait up to 2 s for the host to attach to the USB Serial endpoint
    // before printing the boot banner; otherwise the banner is lost.
    const uint32_t serial_wait_start = millis();
    while (!Serial && (millis() - serial_wait_start) < 2000) {
        delay(10);
    }

    Serial.println("=== F32 spike boot ===");
    Serial.printf("AUDIO_SAMPLE_RATE_EXACT=%f\n", AUDIO_SAMPLE_RATE_EXACT);
    Serial.printf("AUDIO_BLOCK_SAMPLES=%d\n", AUDIO_BLOCK_SAMPLES);
    Serial.printf("AUDIO_SUBSLOT_SIZE=%d\n", AUDIO_SUBSLOT_SIZE);

    AudioMemory_F32(48);   // bumped a bit for SD player overhead

    // Built-in SD slot on Teensy 4.1 uses SDIO; just SD.begin(BUILTIN_SDCARD).
    if (!SD.begin(BUILTIN_SDCARD)) {
        Serial.println("SD init FAILED");
        g_sd_init_ok = false;
    } else {
        Serial.println("SD init OK");
        g_sd_init_ok = true;
    }
    sdPlayer.begin();

    // Production doesn't call Wire.setClock(); leave the bus at the Arduino
    // default (~100 kHz on Teensy 4.x). Verified by grep on the production
    // project — the only setClock matches are unrelated (MIDI clock plumbing).
    Wire.begin();

    // M4j: shut the 6140 first so it isn't fighting on the TDM bus while
    // we configure the TAC5212.
    shutdownAdc6140();

    Wire.beginTransmission(TAC5212_I2C_ADDR);
    if (Wire.endTransmission() != 0) {
        g_codec_ack = 0;
        Serial.println("TAC5212 not found on I2C!");
    } else {
        g_codec_ack = 1;
        setupCodec();
        Serial.println("TAC5212 init done.");
        dumpCodecState();
    }

    mixL.gain(0, 1.0f);
    mixR.gain(0, 1.0f);

    // Kick off the WAV file. Loop will retry every 500 ms if not playing.
    if (sdPlayer.play("TEST.WAV")) {
        Serial.println("Playing TEST.WAV");
    } else {
        Serial.println("TEST.WAV play() returned false (file missing?)");
    }
}

void loop() {
    const uint32_t now = millis();

    // M4q: LED rate encodes SD/WAV state for monitor-less diagnostics.
    //   100 ms (very fast) : SD.begin failed at boot
    //   200 ms (fast)      : WAV is playing
    //   500 ms (slow)      : alive but WAV not playing
    static uint32_t last_blink = 0;
    uint32_t blink_period = 500;
    if (!g_sd_init_ok)               blink_period = 100;
    else if (sdPlayer.isPlaying())   blink_period = 200;
    if (now - last_blink >= blink_period) {
        last_blink = now;
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    }

    // Retry playback if the file ended (or never started).
    static uint32_t last_play_retry = 0;
    if (!sdPlayer.isPlaying() && (now - last_play_retry >= 1000)) {
        last_play_retry = now;
        sdPlayer.play("TEST.WAV");
    }

    // 1 Hz diagnostic status print. Counters are volatile uint32_t,
    // single-word reads are atomic on Cortex-M7.
    static uint32_t last_status = 0;
    if (now - last_status >= 1000) {
        last_status = now;
        const auto s = AudioInputUSB_F32::getStatus();
        Serial.printf("[t=%lu] usb_rx=%lu usb_tx=%lu rx_over=%lu tx_under=%lu tdm_isr=%lu codec_ack=%u\n",
            (unsigned long)now,
            (unsigned long)usb_audio_f32_rx_packets,
            (unsigned long)usb_audio_f32_tx_packets,
            (unsigned long)s.rx_overruns,
            (unsigned long)s.tx_underruns,
            (unsigned long)AudioOutputTDM_F32::getIsrCount(),
            (unsigned)g_codec_ack);
        Serial.printf("        ui_upd=%lu ui_pop=%lu tdm_upd=%lu tdm_data_chs=%lu sts0=0x%02X sts1=0x%02X peak0=0x%08lX\n",
            (unsigned long)AudioInputUSB_F32::updates,
            (unsigned long)AudioInputUSB_F32::pop_ok,
            (unsigned long)AudioOutputTDM_F32::getUpdateCalls(),
            (unsigned long)AudioOutputTDM_F32::getIsrDataChs(),
            (unsigned)g_codec_sts0,
            (unsigned)g_codec_sts1,
            (unsigned long)AudioOutputTDM_F32::readAndClearPeakSlot0());
    }
}
