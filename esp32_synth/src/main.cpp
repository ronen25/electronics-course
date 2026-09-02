// ESP32-C3 3-oscillator synth (PlatformIO build)
// - 3 software sine oscillators, mixed and output as audio
// - 3 potentiometers set pitch, 3 potentiometers set volume, read via ADC
// - SSD1306-family OLED shows live frequency/volume per oscillator
//
// Targets a specific user's ESP32-C3 board with a built-in ~0.4" OLED.
// The C3 has no DAC peripheral, so audio is currently synthesized via LEDC
// PWM (duty cycle = sample value) into an external RC low-pass filter; the
// plan is to switch this to a real DAC once one is added to the hardware --
// see the audio-output block in onAudioTimer()/setup() when that happens.
//
//   Audio: PWM output -> GPIO7 -> (RC low-pass / cap) -> amp/speaker, GND shared
//   Pots:  wiper -> ADC pin, outer legs -> 3V3 and GND
//     (C3 only exposes 6 ADC-capable pins total: GPIO0-5)
//     Pitch pots:  osc1 GPIO0, osc2 GPIO1, osc3 GPIO2
//     Volume pots: osc1 GPIO3, osc2 GPIO4, osc3 GPIO5
//   OLED: this firmware assumes SDA/SCL at this board's Arduino defaults
//     (GPIO8/GPIO9) and a 72x40 panel -- UNVERIFIED against the actual
//     hardware; this board has a built-in ~0.4" OLED of unconfirmed exact
//     resolution/pinout. Confirm both against that board's silkscreen/vendor
//     docs before flashing. GPIO7 for PWM avoids the I2C pins and the C3's
//     strapping pins (GPIO2/8/9 -- 2 is reused here as an ADC input, which
//     is fine; 8/9 are already spoken for by I2C).

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- Display ----------
#define SCREEN_WIDTH 72
#define SCREEN_HEIGHT 40
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------- Audio ----------
#define SAMPLE_RATE 20000
#define NUM_OSC 3

// No DAC on this chip (yet) -- audio is synthesized via PWM duty cycle.
#define PWM_PIN 7
#define PWM_CHANNEL 0
#define PWM_FREQ 312500     // = 80MHz APB / 2^8, exact at 8-bit resolution
#define PWM_RESOLUTION_BITS 8
static const uint8_t PITCH_PINS[NUM_OSC] = {0, 1, 2};
static const uint8_t VOLUME_PINS[NUM_OSC] = {3, 4, 5};

#define FREQ_MIN 40.0f
#define FREQ_MAX 1200.0f

// 256-entry sine wavetable, unsigned 8-bit (0..255, centered at 128)
static uint8_t sineTable[256];

hw_timer_t *audioTimer = NULL;

// Phase accumulators / increments, shared between ISR and loop().
// Frequency/volume are updated from loop() at a much lower rate than the
// audio ISR runs, so plain volatile reads/writes are sufficient here.
volatile uint32_t phaseAcc[NUM_OSC] = {0, 0, 0};
volatile uint32_t phaseInc[NUM_OSC] = {0, 0, 0};
volatile uint8_t oscVolume[NUM_OSC] = {200, 200, 200}; // 0..255

// UI state (updated in loop, not ISR)
float oscFreq[NUM_OSC] = {0, 0, 0};
uint8_t oscVolPct[NUM_OSC] = {0, 0, 0};

void IRAM_ATTR onAudioTimer();
uint32_t freqToPhaseInc(float freq);
void buildSineTable();
void readControls();
void updateDisplay();

void IRAM_ATTR onAudioTimer() {
  int32_t mix = 0;
  for (int i = 0; i < NUM_OSC; i++) {
    phaseAcc[i] += phaseInc[i];
    uint8_t sample = sineTable[phaseAcc[i] >> 24]; // top 8 bits -> table index
    int16_t centered = static_cast<int16_t>(sample) - 128; // -128..127
    mix += (centered * oscVolume[i]) / 255;
  }
  int32_t out = 128 + (mix / NUM_OSC);
  if (out < 0) out = 0;
  if (out > 255) out = 255;
  ledcWrite(PWM_CHANNEL, static_cast<uint32_t>(out));
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

  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);

  // Legacy Arduino-ESP32 timer API (this core predates timerBegin(freq)):
  // timer 0, divider 80 against the 80MHz APB clock -> 1MHz (1us) tick.
  audioTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(audioTimer, &onAudioTimer, true);
  timerAlarmWrite(audioTimer, 1000000 / SAMPLE_RATE, true); // fire every 1/SAMPLE_RATE sec
  timerAlarmEnable(audioTimer);
}

void loop() {
  readControls();
  updateDisplay();
  delay(80);
}
