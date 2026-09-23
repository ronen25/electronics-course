// Temporary bypass test: proves the drum machine's actual synthesis and
// sequencer logic works correctly, completely independent of the MAX98357A
// amp module -- see docs/DRUM_MACHINE_PLAN.md's "Hardware bring-up" section
// for why this exists (that amp is suspected dead after exhaustive
// debugging, and this sketch exists to confirm the ESP32/firmware side is
// not also at fault before waiting on a replacement amp).
//
// This is NOT a product -- it's a throwaway diagnostic, same spirit as
// esp32_i2s_test/. It reuses esp32_drum_machine's actual Voice/waveform/
// envelope/sequencer code (copied, not shared, to keep this standalone) but
// swaps I2S output for the same LEDC-PWM-into-RC-filter approach this
// project's very first esp32_synth prototype used (see git history --
// commit ac5efc7's esp32_synth/src/main.cpp -- before it moved to I2S).
// No WiFi/web UI here: the default pattern just starts playing on boot.
//
// Wiring: connect the speaker directly to GPIO6 (through a resistor -- use
// whatever you have; 220 ohm is a reasonable current-limiter) and GND,
// BYPASSING the MAX98357A entirely. Disconnect the speaker from the amp's
// screw terminal first. GPIO6 is unused by every other pin assignment in
// this repo, isn't a strapping pin, and isn't the onboard LED (GPIO8).
//
// If you hear the default kick/snare/hihat pattern looping at 120bpm, the
// ESP32 + firmware logic side is confirmed good, and the amp module is the
// thing to replace. If it's silent too, something more fundamental (this
// exact GPIO/speaker/resistor combination, or the board itself) needs
// another look.

#include <Arduino.h>
#include <math.h>

// ---------- Audio ----------
#define SAMPLE_RATE 20000
#define PWM_PIN 6
#define PWM_CHANNEL 0
#define PWM_FREQ 312500 // = 80MHz APB / 2^8, exact at 8-bit resolution
#define PWM_RESOLUTION_BITS 8

static uint8_t sineTable[256];
static float phaseIncPerHz;

enum Waveform { WAVE_SINE = 0, WAVE_SQUARE = 1, WAVE_SAW = 2 };

uint8_t computeWaveSample(Waveform w, uint32_t phaseAcc) {
  uint8_t idx = phaseAcc >> 24;
  switch (w) {
    case WAVE_SQUARE: return idx < 128 ? 255 : 0;
    case WAVE_SAW: return idx;
    case WAVE_SINE:
    default: return sineTable[idx];
  }
}

// ---------- Sequencer (fixed pattern/tempo, no WiFi/UI for this test) ----------
#define NUM_TRACKS 3
#define NUM_STEPS 16
enum { TRACK_KICK = 0, TRACK_SNARE = 1, TRACK_HIHAT = 2 };

static bool pattern[NUM_TRACKS][NUM_STEPS];
static const float BPM = 120.0f;
static volatile int currentStep = -1;
static uint32_t samplesPerStep;
static volatile uint32_t stepSampleCounter = 0;

// ---------- Voice: one-shot enveloped oscillator, per track ----------
static const float DECAY_MIN_S = 0.02f;
static const float DECAY_MAX_S = 0.8f;

struct Voice {
  Waveform waveform;
  float pitchSemitones;
  float decayPercent;
  float volumePercent;
  float baseFreq;

  bool active;
  uint32_t phaseAcc;
  uint32_t phaseInc;
  float envAmp;
  float ampDecayCoeff;
};

// Not volatile: only ever touched from inside onAudioTimer() (the ISR
// itself) -- loop() only reads the separately-volatile currentStep/
// stepSampleCounter for its own diagnostic print, never voices[] directly.
static Voice voices[NUM_TRACKS];

void recomputeVoiceDecay(int t) {
  float dp = voices[t].decayPercent;
  float time = DECAY_MIN_S + (dp / 100.0f) * (DECAY_MAX_S - DECAY_MIN_S);
  voices[t].ampDecayCoeff = powf(0.0001f, 1.0f / (time * SAMPLE_RATE));
}

void IRAM_ATTR triggerVoice(int track) {
  float freq = voices[track].baseFreq * powf(2.0f, voices[track].pitchSemitones / 12.0f);
  voices[track].phaseInc = (uint32_t)(freq * phaseIncPerHz);
  voices[track].phaseAcc = 0;
  voices[track].envAmp = 1.0f;
  voices[track].active = true;
}

int16_t IRAM_ATTR renderVoice(int t) {
  if (!voices[t].active) return 0;
  voices[t].phaseAcc += voices[t].phaseInc;
  int16_t centered = (int16_t)computeWaveSample(voices[t].waveform, voices[t].phaseAcc) - 128;
  int16_t out = (int16_t)(centered * voices[t].envAmp * (voices[t].volumePercent / 100.0f));
  voices[t].envAmp *= voices[t].ampDecayCoeff;
  if (voices[t].envAmp < 0.001f) voices[t].active = false;
  return out;
}

void buildSineTable() {
  for (int i = 0; i < 256; i++) {
    float angle = (2.0f * PI * i) / 256.0f;
    sineTable[i] = static_cast<uint8_t>(128.0f + 127.0f * sinf(angle));
  }
}

void IRAM_ATTR tickSequencer() {
  if (stepSampleCounter == 0) {
    currentStep = (currentStep + 1) % NUM_STEPS;
    for (int t = 0; t < NUM_TRACKS; t++) {
      if (pattern[t][currentStep]) triggerVoice(t);
    }
    stepSampleCounter = samplesPerStep;
  }
  stepSampleCounter--;
}

hw_timer_t *audioTimer = NULL;

void IRAM_ATTR onAudioTimer() {
  tickSequencer();
  int32_t mix = renderVoice(0) + renderVoice(1) + renderVoice(2);
  int32_t out = 128 + mix;
  if (out < 0) out = 0;
  if (out > 255) out = 255;
  ledcWrite(PWM_CHANNEL, static_cast<uint32_t>(out));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("PWM amp-bypass test starting -- default pattern loops at 120bpm");

  buildSineTable();
  phaseIncPerHz = 4294967296.0f / SAMPLE_RATE;
  samplesPerStep = (uint32_t)((SAMPLE_RATE * 60.0f) / (BPM * 4.0f));

  // Same defaults as esp32_drum_machine/src/main.cpp.
  voices[TRACK_KICK] = {WAVE_SINE, -6, 62, 90, 60.0f, false, 0, 0, 0, 1.0f};
  voices[TRACK_SNARE] = {WAVE_SQUARE, 0, 40, 75, 200.0f, false, 0, 0, 0, 1.0f};
  voices[TRACK_HIHAT] = {WAVE_SAW, 9, 18, 60, 800.0f, false, 0, 0, 0, 1.0f};
  for (int t = 0; t < NUM_TRACKS; t++) recomputeVoiceDecay(t);

  static const bool DEFAULT_KICK[NUM_STEPS] = {1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0};
  static const bool DEFAULT_SNARE[NUM_STEPS] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,1,0};
  static const bool DEFAULT_HIHAT[NUM_STEPS] = {1,0,1,0, 1,0,1,1, 1,0,1,0, 1,0,1,1};
  for (int s = 0; s < NUM_STEPS; s++) {
    pattern[TRACK_KICK][s] = DEFAULT_KICK[s];
    pattern[TRACK_SNARE][s] = DEFAULT_SNARE[s];
    pattern[TRACK_HIHAT][s] = DEFAULT_HIHAT[s];
  }

  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION_BITS);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);

  // Legacy Arduino-ESP32 timer API (this core predates timerBegin(freq)):
  // timer 0, divider 80 against the 80MHz APB clock -> 1MHz (1us) tick.
  audioTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(audioTimer, &onAudioTimer, true);
  timerAlarmWrite(audioTimer, 1000000 / SAMPLE_RATE, true);
  timerAlarmEnable(audioTimer);
}

void loop() {
  static int lastStep = -1;
  int step = currentStep;
  if (step != lastStep) {
    lastStep = step;
    Serial.printf("step %d\n", step);
  }
  delay(20);
}
