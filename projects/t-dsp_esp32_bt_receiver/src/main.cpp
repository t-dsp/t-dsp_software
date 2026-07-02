// T-DSP ESP32 Bluetooth Receiver
// ---------------------------------------------------------------------------
// Bluetooth Classic A2DP sink on the ESP32-DevKitC of the
// teensy41_digital_audio_board. Decodes A2DP audio and streams it out over
// I2S (ESP32 as I2S MASTER) into the Teensy 4.1's SAI2 slave input.
//
// I2S pin map is fixed by the board's #ESP32_I2S1 header:
//   ESP32 GPIO26 (BCK)  -> Teensy pin 4  (BCLK2)
//   ESP32 GPIO16 (WS)   -> Teensy pin 3  (LRCLK2)
//   ESP32 GPIO25 (DOUT) -> Teensy pin 5  (IN2)   <-- audio to the Teensy
//   ESP32 GPIO33 (DIN)  <- Teensy pin 2  (OUT2)  (unused here; future BT TX)
//   ESP32 GPIO0  (MCLK) <> Teensy pin 33 (MCLK2) (unused; Teensy slave needs no MCLK)
//
// A2DP is always 44.1 kHz / 16-bit stereo; the Teensy resamples to the 48 kHz
// F32 graph. Config mirrors the proven JayShoe/esp32_T4_bt_music_receiver.

#include <Arduino.h>

#include "BluetoothA2DPSink.h"

// Discoverable Bluetooth name your phone will pair with.
static constexpr char BT_DEVICE_NAME[] = "T-DSP";

BluetoothA2DPSink a2dp_sink;

void setup() {
  Serial.begin(115200);
  Serial.println("T-DSP ESP32 BT receiver: starting A2DP sink...");

  // I2S peripheral: ESP32 is master and transmits the decoded audio.
  // APLL on for accurate 44.1 kHz; 8x128-byte DMA buffers.
  static const i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 44100,
      .bits_per_sample = (i2s_bits_per_sample_t)16,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = true,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };
  a2dp_sink.set_i2s_config(i2s_config);

  // Board I2S pin map (see header above). MCLK left unconnected: the Teensy
  // runs as an I2S slave and does not need a master clock.
  static const i2s_pin_config_t pin_config = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = 26,   // BCK  -> Teensy BCLK2 (pin 4)
      .ws_io_num = 16,    // WS   -> Teensy LRCLK2 (pin 3)
      .data_out_num = 25, // DOUT -> Teensy IN2 (pin 5)
      .data_in_num = I2S_PIN_NO_CHANGE,
  };
  a2dp_sink.set_pin_config(pin_config);

  a2dp_sink.start(BT_DEVICE_NAME);
  Serial.printf("A2DP sink up. Pair with \"%s\".\n", BT_DEVICE_NAME);
}

void loop() {
  // The A2DP sink runs in its own FreeRTOS task; nothing to do here.
  delay(1000);
}
