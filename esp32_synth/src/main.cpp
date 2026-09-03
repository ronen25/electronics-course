// ESP32-C3 3-oscillator synth (PlatformIO build)
// - 3 software sine oscillators, mixed and output as audio
// - 3 potentiometers set pitch, 3 potentiometers set volume, read via ADC
// - SSD1306-family OLED shows live frequency/volume per oscillator
//
// Targets a specific user's ESP32-C3 board with a built-in ~0.4" OLED.
// The C3 has no DAC peripheral, so audio goes out over I2S to an external
// MAX98357A mono amp (I2S DAC + Class-D amp in one chip), replacing the
// earlier LEDC-PWM-into-RC-filter approach.
//
//   Audio: I2S -> MAX98357A -> speaker
//     LRC (word select) -> GPIO9, BCLK (bit clock) -> GPIO7, DIN (data) -> GPIO10
//     Amp SD tied directly to 3V3 (enable, mono (L+R)/2 output); GAIN left
//     floating (9dB, MAX98357A default); amp GND/Vin -> board GND/3V3.
//     Samples are sent as 32-bit words (64x BCLK/LRCLK ratio) even though
//     only ~16 bits of audio precision are used: the MAX98357A's automatic
//     clock-ratio auto-detect is known to be unreliable at the 32x (16-bit)
//     ratio and solid at 64x.
//     None of GPIO7/9/10 are this board's onboard LED (GPIO8 -- see CLAUDE.md
//     hardware notes; a continuously-toggling signal there visibly lit the
//     LED solid and corrupted the clock badly enough to kill audio) or its
//     ADC-capable pins (GPIO0-5, all spoken for by the 6 pots below), so
//     there's no interference between I2S and either of those. GPIO9 is a
//     strapping pin but safe to reuse: it's driven by the ESP32 (as an I2S
//     output) into a high-impedance amp input, so nothing pulls it during
//     boot the way a pull-down/up load would.
//   Pots:  wiper -> ADC pin, outer legs -> 3V3 and GND
//     (C3 only exposes 6 ADC-capable pins total: GPIO0-5)
//     Pitch pots:  osc1 GPIO0, osc2 GPIO1, osc3 GPIO2
//     Volume pots: osc1 GPIO3, osc2 GPIO4, osc3 GPIO5
//   OLED: this firmware's display pin/library assumptions are stale and
//     unrelated to the I2S pins above -- see CLAUDE.md's hardware notes for
//     the confirmed OLED pinout (GPIO5/6) and what still needs porting here.

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <driver/i2s.h>

// ---------- Display ----------
#define SCREEN_WIDTH 72
#define SCREEN_HEIGHT 40
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Audio ----------
#define SAMPLE_RATE 20000
#define NUM_OSC 3

// No DAC on this chip -- audio goes out over I2S to an external MAX98357A.
#define I2S_PORT I2S_NUM_0
#define I2S_PIN_LRC 9   // word select
#define I2S_PIN_BCLK 7  // bit clock -- NOT GPIO8: that's this board's onboard LED pin,
                         // whose LED+resistor load corrupts a fast toggling signal
#define I2S_PIN_DOUT 10 // data out -> amp DIN
#define I2S_BLOCK_SAMPLES 128
static const uint8_t PITCH_PINS[NUM_OSC] = {0, 1, 2};
static const uint8_t VOLUME_PINS[NUM_OSC] = {3, 4, 5};

#define FREQ_MIN 40.0f
#define FREQ_MAX 1200.0f

// 256-entry sine wavetable, unsigned 8-bit (0..255, centered at 128)
static uint8_t sineTable[256];

// Phase accumulators / increments. Both writeAudioBlock() and readControls()
// now run from loop() (no more ISR), so plain (non-volatile) shared state is
// fine -- there's no concurrent access to synchronize against.
uint32_t phaseAcc[NUM_OSC] = {0, 0, 0};
uint32_t phaseInc[NUM_OSC] = {0, 0, 0};
uint8_t oscVolume[NUM_OSC] = {200, 200, 200}; // 0..255

// UI state (updated in loop)
float oscFreq[NUM_OSC] = {0, 0, 0};
uint8_t oscVolPct[NUM_OSC] = {0, 0, 0};

// I2S TX buffer: interleaved stereo frames, mono signal duplicated to both
// channels (the amp sums/selects channels per its SD pin strapping). 32-bit
// words (not 16): see the bits_per_sample comment in setupI2S().
static int32_t i2sBuf[I2S_BLOCK_SAMPLES * 2];

void setupI2S();
void writeAudioBlock();
uint32_t freqToPhaseInc(float freq);
void buildSineTable();
void readControls();
void updateDisplay();

void setupI2S() {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = SAMPLE_RATE;
  // 32-bit (not 16): the MAX98357A's automatic BCLK/LRCLK ratio detection is
  // known to be unreliable at 32x (16-bit stereo) but solid at 64x. Audio
  // content below still only needs ~16 bits; the low bits are left at zero.
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 4;
  i2sConfig.dma_buf_len = I2S_BLOCK_SAMPLES;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  esp_err_t installErr = i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  if (installErr != ESP_OK) {
    Serial.printf("i2s_driver_install failed: %s\n", esp_err_to_name(installErr));
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE; // unused; defaults to GPIO0 if left unset
  pinConfig.bck_io_num = I2S_PIN_BCLK;
  pinConfig.ws_io_num = I2S_PIN_LRC;
  pinConfig.data_out_num = I2S_PIN_DOUT;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE; // TX only
  esp_err_t pinErr = i2s_set_pin(I2S_PORT, &pinConfig);
  if (pinErr != ESP_OK) {
    Serial.printf("i2s_set_pin failed: %s\n", esp_err_to_name(pinErr));
  }
}

// Fills one block of samples from the oscillators and blocks on i2s_write()
// until the DMA queue accepts it -- this paces the loop close to real-time,
// taking over the job the hardware timer ISR used to do for PWM.
void writeAudioBlock() {
  for (int n = 0; n < I2S_BLOCK_SAMPLES; n++) {
    int32_t mix = 0;
    for (int i = 0; i < NUM_OSC; i++) {
      phaseAcc[i] += phaseInc[i];
      uint8_t sample = sineTable[phaseAcc[i] >> 24]; // top 8 bits -> table index
      int16_t centered = static_cast<int16_t>(sample) - 128; // -128..127
      mix += (centered * oscVolume[i]) / 255;
    }
    int32_t out = (mix / NUM_OSC) << 24; // -128..127 -> MSB-justified in 32-bit word
    i2sBuf[2 * n] = out;
    i2sBuf[2 * n + 1] = out; // duplicate mono onto both I2S channels
  }
  size_t bytesWritten;
  i2s_write(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesWritten, portMAX_DELAY);
}

uint32_t freqToPhaseInc(float freq) {
  // phaseInc = freq * 2^32 / SAMPLE_RATE
  return static_cast<uint32_t>((freq * 4294967296.0f) / SAMPLE_RATE);
}

void buildSineTable() {
  for (int i = 0; i < 256; i++) {
    float angle = (2.0f * PI * i) / 256.0f;
    sineTable[i] = static_cast<uint8_t>(128.0f + 127.0f * sinf(angle));
  }
}

void readControls() {
  for (int i = 0; i < NUM_OSC; i++) {
    int rawPitch = analogRead(PITCH_PINS[i]);   // 0..4095
    int rawVol = analogRead(VOLUME_PINS[i]);    // 0..4095

    float freq = FREQ_MIN + (FREQ_MAX - FREQ_MIN) * (rawPitch / 4095.0f);
    uint8_t vol = static_cast<uint8_t>((rawVol * 255UL) / 4095UL);

    oscFreq[i] = freq;
    oscVolPct[i] = static_cast<uint8_t>((rawVol * 100UL) / 4095UL);

    phaseInc[i] = freqToPhaseInc(freq);
    oscVolume[i] = vol;
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Compact single-line-per-oscillator layout for the small onboard panel
  // (screen size itself is unverified -- see the wiring comment up top).
  display.setCursor(0, 0);
  display.println("OSC SYNTH");
  for (int i = 0; i < NUM_OSC; i++) {
    display.setCursor(0, 8 * (i + 1));
    display.print(i + 1);
    display.print(':');
    display.print(static_cast<int>(oscFreq[i]));
    display.print("Hz ");
    display.print(oscVolPct[i]);
    display.print('%');
  }

  display.display();
}

void setup() {
  Serial.begin(115200);

  // ADC pins are input-only on the C3, no pinMode needed for analogRead.
  analogReadResolution(12);

  buildSineTable();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 not found, continuing without display");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 8);
    display.println("Synth");
    display.println("booting...");
    display.display();
  }

  readControls();

  setupI2S();
}

void loop() {
  // Keeps the I2S DMA queue fed; i2s_write() blocking is what paces this
  // loop to roughly real-time now that there's no audio-rate timer ISR.
  writeAudioBlock();

  static uint32_t lastControlUpdate = 0;
  uint32_t now = millis();
  if (now - lastControlUpdate >= 80) {
    lastControlUpdate = now;
    readControls();
    // updateDisplay() is NOT called here: on this chip, having the I2S
    // driver active at the same time as any I2C (Wire) transaction causes
    // multi-second bus stalls (both directions) -- a known Arduino-ESP32
    // core bug, not a wiring issue: see
    // github.com/espressif/arduino-esp32/issues/4686. The one Wire
    // transaction that happens before setupI2S() in setup() (the static
    // "Synth / booting..." screen) is unaffected and still works.
  }
}
