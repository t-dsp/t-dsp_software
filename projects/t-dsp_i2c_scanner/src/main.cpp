// T-DSP I2C Scanner — Teensy 4.0 / 4.1
//
// Confirms which I2C devices are present on the default Wire bus (SCL pin 19,
// SDA pin 18), INCLUDING devices hidden behind a TCA9544A 4-channel I2C
// multiplexer.
//
// Why the mux matters: a TCA9544A is a switch, not a buffer. Anything wired to
// one of its 4 downstream channels is invisible on the main bus until that
// channel is explicitly selected by writing a control byte to the mux. A naive
// scanner only sees the mux itself (it ACKs at its base address 0x70..0x77),
// never the codec behind it. So after scanning the main segment we detect any
// TCA9544A and re-scan through each of its 4 channels.
//
// On the teensy41_digital_audio_board the TAC5212 sits behind this mux, so it
// will only appear in the per-channel scan, never on the main bus.
//
// SHDNZ: pin 35 is the shared active-low shutdown for the TAC5212 / ADC6140.
// It is pulsed LOW->HIGH once at boot BEFORE Wire.begin() so the codecs are out
// of hardware shutdown before any I2C transaction runs.
//
// Known T-DSP device addresses (annotated in the report):
//   0x4C  TLV320ADC6140  (4-ch XLR preamp)
//   0x51  TAC5212        (codec)
//   0x70  TCA9544A       (4-ch I2C mux)
//
// If a codec reads as missing everywhere: power-cycle the board FIRST (the
// codec can latch a stuck state that survives a soft reboot), then re-run.

#include <Arduino.h>
#include <Wire.h>

// Shared SHDNZ pin for the TAC5212 + on-board TLV320ADC6140. Active-low.
constexpr int TAC5212_EN_PIN = 35;

// TCA9544A: base address (A2..A0 = 0 -> 0x70), and the highest possible
// strapped address (0x77). The control byte selects ONE downstream channel:
//   bit 2 (0x04) = enable, bits 1..0 = channel (0..3).
// Writing 0x00 disables all channels (downstream isolated from the main bus).
constexpr uint8_t TCA9544A_BASE      = 0x70;
constexpr uint8_t TCA9544A_MAX       = 0x77;
constexpr uint8_t TCA9544A_CH_ENABLE = 0x04;

// Known T-DSP device addresses, for friendly annotation in the report.
struct KnownDevice {
    uint8_t     addr;
    const char *name;
};
constexpr KnownDevice kKnownDevices[] = {
    {0x4C, "TLV320ADC6140 (XLR preamp)"},
    {0x51, "TAC5212 (codec)"},
    {0x70, "TCA9544A (4-ch I2C mux)"},
};

static const char *lookupName(uint8_t addr) {
    for (const KnownDevice &d : kKnownDevices) {
        if (d.addr == addr) return d.name;
    }
    return "unknown device";
}

static bool isMuxAddr(uint8_t addr) {
    return addr >= TCA9544A_BASE && addr <= TCA9544A_MAX;
}

// Probe a single address. Returns true if a device ACKs.
static bool probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// Select one channel on a TCA9544A (or pass disable=true to isolate all).
// Returns true if the mux ACKed the control write.
static bool muxSelect(uint8_t muxAddr, uint8_t channel, bool disable = false) {
    Wire.beginTransmission(muxAddr);
    Wire.write(disable ? 0x00 : (uint8_t)(TCA9544A_CH_ENABLE | (channel & 0x03)));
    return Wire.endTransmission() == 0;
}

// Scan 0x08..0x77, calling onFound() for each ACK. Skips the mux address range
// when skipMux is true (so a per-channel scan doesn't re-report the mux itself).
template <typename Fn>
static uint8_t scanRange(bool skipMux, Fn onFound) {
    uint8_t found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (skipMux && isMuxAddr(addr)) continue;
        if (probe(addr)) { onFound(addr); found++; }
    }
    return found;
}

// Flags from the most recent full scan, used for the closing summary.
static bool g_sawTac5212 = false;
static uint8_t g_muxAddrs[8];
static uint8_t g_muxCount = 0;

static void scanMainBus() {
    Serial.println("Main bus (0x08..0x77):");
    g_muxCount = 0;

    uint8_t found = scanRange(/*skipMux=*/false, [](uint8_t addr) {
        Serial.printf("  ACK  0x%02X  %s\n", addr, lookupName(addr));
        if (addr == 0x51) g_sawTac5212 = true;
        if (isMuxAddr(addr) && g_muxCount < 8) g_muxAddrs[g_muxCount++] = addr;
    });

    if (found == 0) Serial.println("  (no devices on main bus)");
}

static void scanBehindMux(uint8_t muxAddr) {
    Serial.printf("TCA9544A mux 0x%02X - scanning 4 channels:\n", muxAddr);

    for (uint8_t ch = 0; ch < 4; ch++) {
        if (!muxSelect(muxAddr, ch)) {
            Serial.printf("  ch%u: mux did not ACK channel-select write\n", ch);
            continue;
        }

        uint8_t found = scanRange(/*skipMux=*/true, [muxAddr, ch](uint8_t addr) {
            Serial.printf("  ch%u  ACK  0x%02X  %s\n", ch, addr, lookupName(addr));
            if (addr == 0x51) g_sawTac5212 = true;
        });

        if (found == 0) Serial.printf("  ch%u: (nothing)\n", ch);
    }

    muxSelect(muxAddr, 0, /*disable=*/true); // isolate downstream when done
}

static void scanAll() {
    g_sawTac5212 = false;

    scanMainBus();
    for (uint8_t i = 0; i < g_muxCount; i++) scanBehindMux(g_muxAddrs[i]);

    // Closing verdict for the device we actually care about.
    if (g_sawTac5212) {
        Serial.println("=> TAC5212 (0x51): PRESENT");
    } else {
        Serial.println("=> TAC5212 (0x51): NOT FOUND on main bus or any mux channel");
        Serial.println("   If expected: power-cycle (not just reset), then re-run;");
        Serial.println("   check SHDNZ (pin 35) reaches the codec and SDA/SCL pull-ups.");
    }
}

void setup() {
    // SHDNZ pulse — exactly once, before Wire.begin(), so the codecs are out
    // of hardware shutdown before we probe them.
    pinMode(TAC5212_EN_PIN, OUTPUT);
    digitalWrite(TAC5212_EN_PIN, LOW);
    delay(5);  // > 100 us SHDNZ low spec, generous
    digitalWrite(TAC5212_EN_PIN, HIGH);
    delay(10); // let internal supplies settle

    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    while (!Serial && millis() < 3000) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        delay(100);
    }

    Serial.println("================================");
    Serial.println("  T-DSP I2C Scanner (mux-aware)");
    Serial.println("  Wire bus: SCL=19 SDA=18");
    Serial.println("================================");

    Wire.begin();
    Wire.setClock(100000); // conservative 100 kHz for probing
}

void loop() {
    scanAll();
    Serial.println();
    delay(3000); // re-scan every 3 s so you can wiggle connectors and watch
}
