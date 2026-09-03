// Minimal I2S audio bring-up test for the MAX98357A amp -- no synth, no
// display, no pots. Just outputs a loud 440Hz square wave continuously.
// Purpose: isolate whether the I2S -> amp -> speaker signal chain works at
// all, independent of esp32_synth's complexity.
//
// Wiring (freshly rewired from scratch):
//   LRC (word select) -> GPIO3
//   BCLK (bit clock)  -> GPIO7
//   DIN (data)        -> GPIO10
//   SD  -> 3V3 directly (enable)
//   GAIN -> not connected (9dB default)
//   GND -> board GND
//   Vin -> 3V3 directly
//   Speaker on the amp's output terminal.
// None of these GPIOs are strapping pins (2/8/9) or the onboard LED (8).

#include <Arduino.h>
#include <driver/i2s.h>

#define SAMPLE_RATE 20000
#define TONE_HZ 440
#define I2S_PORT I2S_NUM_0
#define I2S_PIN_LRC 3
#define I2S_PIN_BCLK 7
#define I2S_PIN_DOUT 10
#define BLOCK_SAMPLES 128

// 32-bit samples (not 16): the MAX98357A auto-detects its BCLK/LRCLK ratio
// from what it sees on the bus, and its detection is known to be flaky at
// the 32x ratio (16-bit stereo) but reliable at 64x (32-bit stereo). Actual
// audio content still only needs ~16 bits of precision; the low bits are
// just left at zero.
static int32_t buf[BLOCK_SAMPLES * 2]; // interleaved L/R

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("I2S bring-up test starting");

  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = BLOCK_SAMPLES;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  Serial.printf("i2s_driver_install: %s\n", esp_err_to_name(err));

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.bck_io_num = I2S_PIN_BCLK;
  pinConfig.ws_io_num = I2S_PIN_LRC;
  pinConfig.data_out_num = I2S_PIN_DOUT;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(I2S_PORT, &pinConfig);
  Serial.printf("i2s_set_pin: %s\n", esp_err_to_name(err));

  Serial.printf("i2s_get_clk: %f Hz (requested %d)\n", i2s_get_clk(I2S_PORT), SAMPLE_RATE);
  Serial.println("Writing tone now...");
}

void loop() {
  static uint32_t phase = 0;
  const uint32_t samplesPerHalfCycle = SAMPLE_RATE / (TONE_HZ * 2);

  for (int n = 0; n < BLOCK_SAMPLES; n++) {
    int32_t sample = ((phase / samplesPerHalfCycle) % 2 == 0) ? 0x7FFFFFFF : 0x80000000;
    buf[2 * n] = sample;
    buf[2 * n + 1] = sample;
    phase++;
  }

  size_t bytesWritten = 0;
  esp_err_t err = i2s_write(I2S_PORT, buf, sizeof(buf), &bytesWritten, portMAX_DELAY);

  static uint32_t lastLog = 0;
  if (millis() - lastLog >= 1000) {
    lastLog = millis();
    Serial.printf("writing: err=%s bytesWritten=%u/%u\n", esp_err_to_name(err),
                  (unsigned)bytesWritten, (unsigned)sizeof(buf));
  }
}
