// ESP32-C3 drum machine (PlatformIO build)
// - 4 synthesized one-shot oscillator voices (selectable sine/square/saw
//   waveform, pitch/decay/volume per voice), mixed and output over I2S --
//   same signal chain as esp32_synth/.
// - 16-step x 4-track sequencer (kick/snare/hihat/ride), sample-accurate
//   step timing, no hardware timer ISR (same "loop() paced by blocking
//   i2s_write()" pattern as the synth -- see CLAUDE.md's Architecture
//   section for why that works here).
// - Programmed entirely from a browser: the board hosts its own WiFi AP and
//   serves the step-sequencer UI itself over plain HTTP (ESPAsyncWebServer),
//   with live control over a WebSocket on the same server. No pots, no
//   display -- see DRUM_MACHINE_PLAN.md for the design discussion this came
//   out of, and docs/Drum Machine.dc.html for the visual design this UI is
//   ported from (that file is a Claude Design Canvas artboard -- it depends
//   on the design tool's own runtime/`_ds_bundle.js` and can't run standalone,
//   so the HTML/CSS/JS below is a from-scratch, dependency-free
//   reimplementation of its layout and controls, not a copy of its markup).
//
// Wiring (audio only -- no pots, no OLED in this project):
//   I2S -> MAX98357A -> speaker, same pins/config as esp32_synth:
//     LRC (word select) -> GPIO9, BCLK (bit clock) -> GPIO7, DIN (data) -> GPIO10
//     Amp SD -> 3V3 directly (enable), GAIN floating (9dB), amp GND -> board GND,
//     amp Vin -> board 5V pin (NOT 3V3 -- see STATUS below).
//     32-bit I2S words (64x BCLK/LRCLK ratio) -- MAX98357A auto-detect is
//     flaky at 32x/16-bit, reliable at 64x. See esp32_synth/src/main.cpp and
//     CLAUDE.md for the full history behind this pin/format choice.
//
// STATUS 2026-09-16: RESOLVED -- the amp module is not dead. The 2026-09-11
// "likely-dead amp" conclusion (see docs/DRUM_MACHINE_PLAN.md for the full
// elimination log) was wrong: Vin was wired to 3V3, and this module needs 5V
// in practice to produce output even though 3.3V is within the MAX98357A's
// datasheet range. Confirmed working once Vin moved to the board's 5V pin.
//
// WiFi: AP mode (board hosts "drum-machine" network, see AP_SSID/AP_PASSWORD
// below) rather than joining an existing network -- meant to work as a
// standalone instrument. Connect and open http://192.168.4.1/.
//
// Coexistence risk: running WiFi + I2S together on this single-core chip was
// flagged as an open risk in DRUM_MACHINE_PLAN.md (by analogy to the
// confirmed I2S/I2C conflict documented in CLAUDE.md). It was smoke-tested
// in esp32_wifi_i2s_test/ first; the mitigation carried over here: sliders
// only send a WebSocket message on release (`change`, not `input`), not
// continuously while dragging, and step-boundary playhead broadcasts are a
// few times a second, not per-block. Still worth watching for audio glitches
// on real hardware.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>

// ---------- Audio ----------
#define SAMPLE_RATE 20000
#define I2S_PORT I2S_NUM_0
#define I2S_PIN_LRC 9
#define I2S_PIN_BCLK 7
#define I2S_PIN_DOUT 10
#define I2S_BLOCK_SAMPLES 128

static int32_t i2sBuf[I2S_BLOCK_SAMPLES * 2]; // interleaved stereo, mono duplicated
static uint8_t sineTable[256];
static float phaseIncPerHz; // freq(Hz) * phaseIncPerHz -> 32-bit phase increment

enum Waveform { WAVE_SINE = 0, WAVE_SQUARE = 1, WAVE_SAW = 2, WAVE_NOISE = 3 };

// Same 0..255 (centered on 128) sample format for all four waveforms, so
// downstream envelope/mix code doesn't need to care which one is active.
uint8_t computeWaveSample(Waveform w, uint32_t phaseAcc) {
  uint8_t idx = phaseAcc >> 24; // top 8 bits of phase -> 0..255 position in the cycle
  switch (w) {
    case WAVE_SQUARE: return idx < 128 ? 255 : 0;
    case WAVE_SAW: return idx;
    case WAVE_NOISE: {
      // White noise, not phase-derived like the other three waveforms --
      // a free-running xorshift32 PRNG advanced once per call, so pitch has
      // no effect on it (there's no fundamental frequency to tune) and
      // successive hits don't repeat the same "random" sequence the way a
      // phaseAcc reset to 0 on every triggerVoice() would.
      static uint32_t noiseState = 0x1234567u;
      noiseState ^= noiseState << 13;
      noiseState ^= noiseState >> 17;
      noiseState ^= noiseState << 5;
      return (uint8_t)(noiseState & 0xFF);
    }
    case WAVE_SINE:
    default: return sineTable[idx];
  }
}

// ---------- Sequencer ----------
#define NUM_TRACKS 4
#define NUM_STEPS 16
enum { TRACK_KICK = 0, TRACK_SNARE = 1, TRACK_HIHAT = 2, TRACK_RIDE = 3 };

// Written from the WebSocket callback (a separate AsyncTCP FreeRTOS task)
// and read from loop()/writeAudioBlock() -- plain bool/float/struct field
// reads and writes are single-word on this chip so torn values aren't a
// real risk, and a rare stale read just means an edit lands a few
// milliseconds late. Not worth a mutex for a hobby step sequencer.
static bool pattern[NUM_TRACKS][NUM_STEPS] = {};
static bool playing = false;
static float bpm = 120.0f;
static int currentStep = -1;
static uint32_t samplesPerStep = 0;
static uint32_t stepSampleCounter = 0;
static volatile bool stepChanged = false;
static float masterVolumePercent = 78.0f;

void recomputeStepTiming() {
  // 16th notes: 4 steps per beat.
  samplesPerStep = (uint32_t)((SAMPLE_RATE * 60.0f) / (bpm * 4.0f));
  if (samplesPerStep < 1) samplesPerStep = 1;
}

// ---------- Voice: one-shot enveloped oscillator, per track ----------
// Decay knob (0..100) maps onto this time range for the amplitude envelope.
static const float DECAY_MIN_S = 0.02f;
static const float DECAY_MAX_S = 0.8f;

struct Voice {
  Waveform waveform;
  float pitchSemitones;  // -24..24, tunes baseFreq
  float decayPercent;    // 0..100 -> DECAY_MIN_S..DECAY_MAX_S envelope time
  float volumePercent;   // 0..100
  float baseFreq;        // untransposed center frequency for this voice
  bool muted;            // silences output only -- envelope/trigger logic still runs,
                          // so unmuting mid-decay resumes rather than restarting

  bool active;
  uint32_t phaseAcc;
  uint32_t phaseInc; // latched at trigger time from pitch+baseFreq
  float envAmp;
  float ampDecayCoeff;
};

static Voice voices[NUM_TRACKS];

void recomputeVoiceDecay(Voice &v) {
  float t = DECAY_MIN_S + (v.decayPercent / 100.0f) * (DECAY_MAX_S - DECAY_MIN_S);
  v.ampDecayCoeff = powf(0.0001f, 1.0f / (t * SAMPLE_RATE));
}

void triggerVoice(int track) {
  Voice &v = voices[track];
  float freq = v.baseFreq * powf(2.0f, v.pitchSemitones / 12.0f);
  v.phaseInc = (uint32_t)(freq * phaseIncPerHz);
  v.phaseAcc = 0;
  v.envAmp = 1.0f;
  v.active = true;
}

int16_t renderVoice(Voice &v) {
  if (!v.active) return 0;
  v.phaseAcc += v.phaseInc;
  int16_t centered = (int16_t)computeWaveSample(v.waveform, v.phaseAcc) - 128; // -128..127
  int16_t out = v.muted ? 0 : (int16_t)(centered * v.envAmp * (v.volumePercent / 100.0f));
  v.envAmp *= v.ampDecayCoeff;
  if (v.envAmp < 0.001f) v.active = false;
  return out;
}

void buildSineTable() {
  for (int i = 0; i < 256; i++) {
    float angle = (2.0f * PI * i) / 256.0f;
    sineTable[i] = static_cast<uint8_t>(128.0f + 127.0f * sinf(angle));
  }
}

// Advances the sequencer by exactly one sample, triggering step 0..15 voices
// at sample-accurate step boundaries. Called once per output sample so step
// timing doesn't inherit I2S block-size jitter (128 samples / ~6.4ms).
void tickSequencer() {
  if (!playing) return;
  if (stepSampleCounter == 0) {
    currentStep = (currentStep + 1) % NUM_STEPS;
    for (int t = 0; t < NUM_TRACKS; t++) {
      if (pattern[t][currentStep]) triggerVoice(t);
    }
    stepSampleCounter = samplesPerStep;
    stepChanged = true;
  }
  stepSampleCounter--;
}

void writeAudioBlock() {
  for (int n = 0; n < I2S_BLOCK_SAMPLES; n++) {
    tickSequencer();
    int32_t mix = renderVoice(voices[0]) + renderVoice(voices[1]) + renderVoice(voices[2]) + renderVoice(voices[3]);
    mix = (int32_t)(mix * (masterVolumePercent / 100.0f));
    // No fixed headroom divide here -- the master knob is the only global
    // attenuation. Simultaneous voices at full envelope can hard-clip on
    // the two lines below; that only happens for an instant at shared
    // trigger points and reads as a bit of punch, not distortion, at these
    // levels -- much better than everything being quiet all the time.
    if (mix > 127) mix = 127;
    if (mix < -128) mix = -128;
    int32_t out = mix << 24; // MSB-justified in the 32-bit I2S word
    i2sBuf[2 * n] = out;
    i2sBuf[2 * n + 1] = out;
  }
  size_t bytesWritten;
  i2s_write(I2S_PORT, i2sBuf, sizeof(i2sBuf), &bytesWritten, portMAX_DELAY);
}

void setupI2S() {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  i2sConfig.sample_rate = SAMPLE_RATE;
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
  pinConfig.mck_io_num = I2S_PIN_NO_CHANGE; // must be explicit -- defaults to GPIO0 otherwise
  pinConfig.bck_io_num = I2S_PIN_BCLK;
  pinConfig.ws_io_num = I2S_PIN_LRC;
  pinConfig.data_out_num = I2S_PIN_DOUT;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;
  esp_err_t pinErr = i2s_set_pin(I2S_PORT, &pinConfig);
  if (pinErr != ESP_OK) {
    Serial.printf("i2s_set_pin failed: %s\n", esp_err_to_name(pinErr));
  }
}

// ---------- Flash persistence (Save/Load buttons) ----------
// Single-slot save: one packed blob in NVS via the Preferences wrapper, no
// extra library needed beyond what's already in the Arduino-ESP32 core.
struct __attribute__((packed)) SavedVoice {
  uint8_t waveform;
  int8_t pitch;
  uint8_t decay;
  uint8_t volume;
  uint16_t steps; // bit i = step i on/off
  uint8_t muted;
};
struct __attribute__((packed)) SavedState {
  uint32_t magic;
  float bpm;
  uint8_t master;
  SavedVoice voice[NUM_TRACKS];
};
static const uint32_t SAVE_MAGIC = 0x44524d32; // "DRM2" -- bumped from DRM1 when ride (4th voice) was added, so a
                                                // differently-sized old save is rejected outright instead of
                                                // partially matching.

Preferences prefs;

void saveStateToFlash() {
  SavedState s;
  s.magic = SAVE_MAGIC;
  s.bpm = bpm;
  s.master = (uint8_t)masterVolumePercent;
  for (int t = 0; t < NUM_TRACKS; t++) {
    s.voice[t].waveform = (uint8_t)voices[t].waveform;
    s.voice[t].pitch = (int8_t)voices[t].pitchSemitones;
    s.voice[t].decay = (uint8_t)voices[t].decayPercent;
    s.voice[t].volume = (uint8_t)voices[t].volumePercent;
    s.voice[t].muted = voices[t].muted ? 1 : 0;
    uint16_t bits = 0;
    for (int st = 0; st < NUM_STEPS; st++)
      if (pattern[t][st]) bits |= (1u << st);
    s.voice[t].steps = bits;
  }
  prefs.begin("drum", false);
  prefs.putBytes("state", &s, sizeof(s));
  prefs.end();
}

bool loadStateFromFlash() {
  SavedState s;
  prefs.begin("drum", true);
  size_t n = prefs.getBytes("state", &s, sizeof(s));
  prefs.end();
  if (n != sizeof(s) || s.magic != SAVE_MAGIC) return false;

  bpm = s.bpm;
  masterVolumePercent = s.master;
  recomputeStepTiming();
  for (int t = 0; t < NUM_TRACKS; t++) {
    voices[t].waveform = (Waveform)s.voice[t].waveform;
    voices[t].pitchSemitones = s.voice[t].pitch;
    voices[t].decayPercent = s.voice[t].decay;
    voices[t].volumePercent = s.voice[t].volume;
    voices[t].muted = s.voice[t].muted != 0;
    recomputeVoiceDecay(voices[t]);
    for (int st = 0; st < NUM_STEPS; st++)
      pattern[t][st] = (s.voice[t].steps >> st) & 1u;
  }
  return true;
}

// ---------- Web UI ----------
static const char *AP_SSID = "drum-machine";
static const char *AP_PASSWORD = "drumbeats"; // WPA2 min length is 8 chars

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

static const char INDEX_HTML[] PROGMEM = R"HTMLDOC(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DRUM . C3</title>
<style>
  :root {
    --bg: #121214; --panel: #1a1a1e; --text: #e8e8ec; --muted: #8a8a94;
    --divider: #2c2c33; --divider-strong: #4a4a55; --accent: #ff7a45; --accent-ink: #1a0d05;
  }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--text); font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; padding:16px; }
  h1 { font-size:1em; letter-spacing:0.08em; margin:0; }
  .tag { font-size:9px; letter-spacing:0.05em; text-transform:uppercase; border:1px solid var(--divider-strong); border-radius:3px; padding:2px 6px; color:var(--muted); }
  button { font:inherit; }
  .btn { background:var(--panel); color:var(--text); border:1px solid var(--divider-strong); border-radius:6px; padding:6px 14px; cursor:pointer; }
  .btn-primary { background:var(--accent); border-color:var(--accent); color:var(--accent-ink); font-weight:600; }
  .btn-primary.playing { background:#e64545; border-color:#e64545; color:#fff; }
  .btn-ghost { background:transparent; }
  .topbar { display:flex; align-items:center; gap:16px; flex-wrap:wrap; padding-bottom:12px; border-bottom:2px solid var(--divider); margin-bottom:14px; }
  .field { display:flex; align-items:center; gap:6px; }
  .mini-label { font-size:9px; letter-spacing:0.05em; text-transform:uppercase; color:var(--muted); white-space:nowrap; }
  input[type=number] { width:56px; background:var(--panel); color:var(--text); border:1px solid var(--divider-strong); border-radius:4px; padding:4px 6px; }
  input[type=range] { accent-color:var(--accent); }
  #master { width:90px; }
  .spacer { margin-left:auto; }
  .osc-strip { display:grid; grid-template-columns:70px 96px 200px 1fr; align-items:center; gap:16px; padding:10px 0; border-top:2px solid var(--divider); }
  .osc-strip.muted { opacity:0.45; }
  .osc-name-row { display:flex; align-items:center; gap:6px; }
  .osc-name { font-weight:700; font-size:0.95em; }
  .osc-voice { font-size:10px; color:var(--muted); letter-spacing:0.05em; }
  .mute-btn { background:var(--panel); color:var(--muted); border:1.5px solid var(--divider-strong); border-radius:4px; width:20px; height:20px; line-height:1; padding:0; cursor:pointer; font-size:10px; font-weight:700; }
  .mute-btn.active { background:#e64545; border-color:#e64545; color:#fff; }
  .waveseg { display:flex; border:1.5px solid var(--divider-strong); border-radius:4px; overflow:hidden; width:fit-content; }
  .wave-opt { padding:5px 8px; cursor:pointer; color:var(--muted); display:flex; }
  .wave-opt.active { background:var(--accent); color:var(--accent-ink); }
  .knobs { display:flex; gap:14px; }
  .knob-col { flex: 0 0 56px; width:56px; }
  .knob-col input[type=range] { display:block; width:56px; }
  .knob-val { font-size:9px; color:var(--muted); white-space:nowrap; }
  .steps { display:grid; grid-template-columns:repeat(16,1fr); gap:4px; }
  .step { width:100%; aspect-ratio:1; border:1.5px solid var(--divider); background:var(--panel); border-radius:3px; padding:0; cursor:pointer; }
  .step.on { background:var(--accent); border-color:var(--accent); }
  .step.beat { border-left-color:var(--divider-strong); border-left-width:2px; }
  .step.playhead { outline:2px solid #ffce54; outline-offset:1px; }
  #status { font-size:11px; color:var(--muted); }
</style>
</head>
<body>
<div class="topbar">
  <h1>DRUM&nbsp;&middot;&nbsp;C3</h1>
  <span class="tag">ESP32-C3</span>
  <button id="playBtn" class="btn btn-primary">Play</button>
  <div class="field"><span class="mini-label">BPM</span><input id="bpm" type="number" min="40" max="300" value="120"></div>
  <div class="field"><span class="mini-label">Master</span><input id="master" type="range" min="0" max="100" value="78"></div>
  <div class="spacer"></div>
  <button id="saveBtn" class="btn btn-ghost">Save</button>
  <button id="loadBtn" class="btn btn-ghost">Load</button>
  <span id="status"></span>
</div>
<div id="grid"></div>
<script>
const TRACKS = [
  { name: "OSC 1", voice: "KICK" },
  { name: "OSC 2", voice: "SNARE" },
  { name: "OSC 3", voice: "HI-HAT" },
  { name: "OSC 4", voice: "RIDE" },
];
const WAVE_ICONS = [
  '<svg width="16" height="10" viewBox="0 0 20 12" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M1 6 C3 1 5 1 7 6 C9 11 11 11 13 6 C15 1 17 1 19 6"/></svg>',
  '<svg width="16" height="10" viewBox="0 0 20 12" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M1 9 L1 3 L7 3 L7 9 L13 9 L13 3 L19 3"/></svg>',
  '<svg width="16" height="10" viewBox="0 0 20 12" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M1 9 L7 3 L7 9 L13 3 L13 9 L19 3"/></svg>',
  '<svg width="16" height="10" viewBox="0 0 20 12" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M1 7 L3 3 L5 9 L7 2 L9 10 L11 5 L13 8 L15 1 L17 9 L19 4"/></svg>',
];
const NUM_STEPS = 16;

let ws;
const rows = []; // per-track DOM refs

const grid = document.getElementById("grid");
TRACKS.forEach((tr, t) => {
  const strip = document.createElement("div");
  strip.className = "osc-strip";

  const nameCol = document.createElement("div");
  const muteBtn = document.createElement("button");
  muteBtn.className = "mute-btn";
  muteBtn.textContent = "M";
  muteBtn.title = "Mute " + tr.name;
  muteBtn.onclick = () => send({ cmd: "mute", track: t });
  const nameRow = document.createElement("div");
  nameRow.className = "osc-name-row";
  const nameEl = document.createElement("div");
  nameEl.className = "osc-name";
  nameEl.textContent = tr.name;
  nameRow.appendChild(nameEl);
  nameRow.appendChild(muteBtn);
  nameCol.appendChild(nameRow);
  const voiceEl = document.createElement("div");
  voiceEl.className = "osc-voice";
  voiceEl.textContent = tr.voice;
  nameCol.appendChild(voiceEl);
  strip.appendChild(nameCol);

  const waveseg = document.createElement("div");
  waveseg.className = "waveseg";
  const waveEls = [];
  WAVE_ICONS.forEach((svg, w) => {
    const opt = document.createElement("div");
    opt.className = "wave-opt";
    opt.innerHTML = svg;
    opt.onclick = () => send({ cmd: "wave", track: t, value: w });
    waveseg.appendChild(opt);
    waveEls.push(opt);
  });
  strip.appendChild(waveseg);

  const knobs = document.createElement("div");
  knobs.className = "knobs";
  function makeKnob(label, min, max, onChange) {
    const col = document.createElement("div");
    col.className = "knob-col";
    const valEl = document.createElement("div");
    valEl.className = "knob-val";
    valEl.textContent = label;
    const input = document.createElement("input");
    input.type = "range"; input.min = min; input.max = max; input.value = 0;
    input.oninput = () => { valEl.textContent = label + " " + input.value; };
    input.onchange = () => onChange(parseInt(input.value, 10));
    col.appendChild(valEl);
    col.appendChild(input);
    knobs.appendChild(col);
    return { input, valEl, label };
  }
  const pitchKnob = makeKnob("Pitch", -24, 24, (v) => send({ cmd: "pitch", track: t, value: v }));
  const decayKnob = makeKnob("Decay", 0, 100, (v) => send({ cmd: "decay", track: t, value: v }));
  const volumeKnob = makeKnob("Vol", 0, 100, (v) => send({ cmd: "volume", track: t, value: v }));
  strip.appendChild(knobs);

  const stepsDiv = document.createElement("div");
  stepsDiv.className = "steps";
  const stepEls = [];
  for (let s = 0; s < NUM_STEPS; s++) {
    const el = document.createElement("div");
    el.className = "step" + (s % 4 === 0 ? " beat" : "");
    el.onclick = () => send({ cmd: "toggle", track: t, step: s });
    stepsDiv.appendChild(el);
    stepEls.push(el);
  }
  strip.appendChild(stepsDiv);

  grid.appendChild(strip);
  rows.push({ strip, muteBtn, waveEls, pitchKnob, decayKnob, volumeKnob, stepEls });
});

const playBtn = document.getElementById("playBtn");
const saveBtn = document.getElementById("saveBtn");
const loadBtn = document.getElementById("loadBtn");
const bpmInput = document.getElementById("bpm");
const masterInput = document.getElementById("master");
const statusEl = document.getElementById("status");

playBtn.onclick = () => send({ cmd: playBtn.classList.contains("playing") ? "stop" : "play" });
saveBtn.onclick = () => send({ cmd: "save" });
loadBtn.onclick = () => send({ cmd: "load" });
bpmInput.onchange = () => send({ cmd: "bpm", value: parseInt(bpmInput.value, 10) });
masterInput.onchange = () => send({ cmd: "master", value: parseInt(masterInput.value, 10) });

function send(msg) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(msg)); }

function setKnob(knob, value) {
  knob.input.value = value;
  knob.valEl.textContent = knob.label + " " + value;
}

function applyState(st) {
  if (document.activeElement !== bpmInput) bpmInput.value = st.bpm;
  if (document.activeElement !== masterInput) masterInput.value = st.master;
  playBtn.textContent = st.playing ? "Stop" : "Play";
  playBtn.classList.toggle("playing", st.playing);

  st.oscillators.forEach((osc, t) => {
    const row = rows[t];
    row.waveEls.forEach((el, w) => el.classList.toggle("active", w === osc.wave));
    setKnob(row.pitchKnob, osc.pitch);
    setKnob(row.decayKnob, osc.decay);
    setKnob(row.volumeKnob, osc.volume);
    row.muteBtn.classList.toggle("active", !!osc.muted);
    row.strip.classList.toggle("muted", !!osc.muted);
    osc.steps.forEach((on, s) => row.stepEls[s].classList.toggle("on", !!on));
  });
}

function applyStep(n) {
  rows.forEach((row) => row.stepEls.forEach((el, s) => el.classList.toggle("playhead", s === n)));
}

function connect() {
  ws = new WebSocket("ws://" + location.host + "/ws");
  ws.onopen = () => { statusEl.textContent = "connected"; send({ cmd: "state" }); };
  ws.onclose = () => { statusEl.textContent = "disconnected, retrying..."; setTimeout(connect, 1000); };
  ws.onerror = () => ws.close();
  ws.onmessage = (evt) => {
    const msg = JSON.parse(evt.data);
    if (msg.type === "state") applyState(msg);
    else if (msg.type === "step") applyStep(msg.step);
  };
}
connect();
</script>
</body>
</html>
)HTMLDOC";

void buildStateJson(String &out) {
  JsonDocument doc;
  doc["type"] = "state";
  doc["bpm"] = (int)bpm;
  doc["playing"] = playing;
  doc["master"] = (int)masterVolumePercent;
  JsonArray oscillators = doc["oscillators"].to<JsonArray>();
  for (int t = 0; t < NUM_TRACKS; t++) {
    JsonObject osc = oscillators.add<JsonObject>();
    osc["wave"] = (int)voices[t].waveform;
    osc["pitch"] = (int)voices[t].pitchSemitones;
    osc["decay"] = (int)voices[t].decayPercent;
    osc["volume"] = (int)voices[t].volumePercent;
    osc["muted"] = voices[t].muted;
    JsonArray steps = osc["steps"].to<JsonArray>();
    for (int st = 0; st < NUM_STEPS; st++) steps.add(pattern[t][st] ? 1 : 0);
  }
  serializeJson(doc, out);
}

void broadcastState() {
  String s;
  buildStateJson(s);
  ws.textAll(s);
}

void handleCommand(JsonDocument &doc, AsyncWebSocketClient *client) {
  const char *cmd = doc["cmd"];
  if (!cmd) return;
  Serial.printf("[ws cmd] %s\n", cmd); // cheap serial visibility into what the UI is sending

  if (strcmp(cmd, "toggle") == 0) {
    int track = doc["track"], step = doc["step"];
    if (track >= 0 && track < NUM_TRACKS && step >= 0 && step < NUM_STEPS) {
      pattern[track][step] = !pattern[track][step];
      broadcastState();
    }
  } else if (strcmp(cmd, "play") == 0) {
    playing = true;
    currentStep = -1; // so the next tick lands on step 0
    stepSampleCounter = 0;
    broadcastState();
  } else if (strcmp(cmd, "stop") == 0) {
    playing = false;
    broadcastState();
  } else if (strcmp(cmd, "bpm") == 0) {
    float v = doc["value"];
    if (v >= 40 && v <= 300) {
      bpm = v;
      recomputeStepTiming();
      broadcastState();
    }
  } else if (strcmp(cmd, "wave") == 0) {
    int track = doc["track"], v = doc["value"];
    if (track >= 0 && track < NUM_TRACKS && v >= 0 && v <= 3) {
      voices[track].waveform = (Waveform)v;
      broadcastState();
    }
  } else if (strcmp(cmd, "pitch") == 0) {
    int track = doc["track"];
    float v = doc["value"];
    if (track >= 0 && track < NUM_TRACKS && v >= -24 && v <= 24) {
      voices[track].pitchSemitones = v;
      broadcastState();
    }
  } else if (strcmp(cmd, "decay") == 0) {
    int track = doc["track"];
    float v = doc["value"];
    if (track >= 0 && track < NUM_TRACKS && v >= 0 && v <= 100) {
      voices[track].decayPercent = v;
      recomputeVoiceDecay(voices[track]);
      broadcastState();
    }
  } else if (strcmp(cmd, "volume") == 0) {
    int track = doc["track"];
    float v = doc["value"];
    if (track >= 0 && track < NUM_TRACKS && v >= 0 && v <= 100) {
      voices[track].volumePercent = v;
      broadcastState();
    }
  } else if (strcmp(cmd, "mute") == 0) {
    int track = doc["track"];
    if (track >= 0 && track < NUM_TRACKS) {
      voices[track].muted = !voices[track].muted;
      broadcastState();
    }
  } else if (strcmp(cmd, "master") == 0) {
    float v = doc["value"];
    if (v >= 0 && v <= 100) {
      masterVolumePercent = v;
      broadcastState();
    }
  } else if (strcmp(cmd, "save") == 0) {
    saveStateToFlash();
  } else if (strcmp(cmd, "load") == 0) {
    if (loadStateFromFlash()) broadcastState();
  } else if (strcmp(cmd, "state") == 0) {
    String s;
    buildStateJson(s);
    client->text(s);
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    String s;
    buildStateJson(s);
    client->text(s);
  } else if (type == WS_EVT_DATA) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, data, len);
    if (!err) handleCommand(doc, client);
  }
}

void setupWeb() {
  // WiFi.persistent(false): AP SSID/password are set fresh from code every
  // boot, so there's no need for the core to also write them to NVS on
  // every softAP() call -- avoids unnecessary flash wear and keeps WiFi's
  // NVS entries independent of the Preferences ("drum" namespace) writes.
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  // Lower TX power than the default max (~19.5dBm) -- this is a short-range
  // AP (phone/laptop right next to the board), so full range isn't needed,
  // and WiFi TX bursts are among the highest peak-current events on this
  // chip. Suspected (not yet fully confirmed) to be sagging the shared
  // power rail enough to disrupt the radio once the MAX98357A is also
  // continuously switching for real (see the LRC pin history above) --
  // this rail contention is a new instance of the same "two peripherals
  // competing for a marginal shared resource" pattern as the documented
  // I2S/I2C conflict in CLAUDE.md, just power instead of bus arbitration.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

  // softAP() returning false was going unnoticed before -- softAPIP()
  // returns the configured IP regardless of whether the radio actually
  // came up, so a silent failure here looked identical to success in the
  // serial log. Retry a few times and say clearly if it never comes up.
  bool apOk = false;
  for (int attempt = 1; attempt <= 3 && !apOk; attempt++) {
    apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
    if (!apOk) {
      Serial.printf("WiFi.softAP() failed (attempt %d/3), retrying...\n", attempt);
      delay(300);
    }
  }
  Serial.printf("WiFi AP %s: SSID=\"%s\" IP=%s\n", apOk ? "up" : "FAILED TO START",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML);
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  buildSineTable();
  phaseIncPerHz = 4294967296.0f / SAMPLE_RATE;

  // Defaults for kick/snare/hihat mirror docs/Drum Machine.dc.html's mockup
  // values. Ride is new (not in that mockup) -- noise waveform (pitch/baseFreq
  // are inert for it, see computeWaveSample()'s WAVE_NOISE case) with a much
  // longer decay than the hihat, to read as a sustained wash/ping rather than
  // the hihat's short chick.
  voices[TRACK_KICK] = {WAVE_SINE, -24, 9, 90, 60.0f, false};
  voices[TRACK_SNARE] = {WAVE_SQUARE, 0, 40, 75, 200.0f, false};
  voices[TRACK_HIHAT] = {WAVE_SAW, 9, 18, 60, 800.0f, false};
  voices[TRACK_RIDE] = {WAVE_NOISE, 0, 65, 50, 500.0f, false};
  for (int t = 0; t < NUM_TRACKS; t++) recomputeVoiceDecay(voices[t]);

  // Exact default patterns from docs/Drum Machine.dc.html's mockup data.
  static const bool DEFAULT_KICK[NUM_STEPS] = {1,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0};
  static const bool DEFAULT_SNARE[NUM_STEPS] = {0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,1,0};
  static const bool DEFAULT_HIHAT[NUM_STEPS] = {1,0,1,0, 1,0,1,1, 1,0,1,0, 1,0,1,1};
  // Ride: not in the mockup -- a plain quarter-note pattern (every downbeat).
  static const bool DEFAULT_RIDE[NUM_STEPS] = {1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0};
  for (int s = 0; s < NUM_STEPS; s++) {
    pattern[TRACK_KICK][s] = DEFAULT_KICK[s];
    pattern[TRACK_SNARE][s] = DEFAULT_SNARE[s];
    pattern[TRACK_HIHAT][s] = DEFAULT_HIHAT[s];
    pattern[TRACK_RIDE][s] = DEFAULT_RIDE[s];
  }

  recomputeStepTiming();

  setupI2S();
  setupWeb();

  // AsyncTCP-esphome's own service task runs at a hardcoded priority of 3
  // (see _start_async_task() in AsyncTCP.cpp), while Arduino's loopTask --
  // which runs writeAudioBlock()/i2s_write() -- defaults to priority 1. That
  // means AsyncTCP can freely preempt the audio loop for routine network
  // housekeeping (TCP ACKs, keep-alives) even with no visible WebSocket
  // command, which showed up as intermittent audio crackling. Raising
  // loopTask's own priority above AsyncTCP's (but nowhere near WiFi/lwIP's
  // own much higher, ~18-23, system priorities) flips who preempts whom,
  // without touching anything WiFi-critical.
  vTaskPrioritySet(NULL, 4);
}

void loop() {
  // Keeps the I2S DMA queue fed; i2s_write() blocking is what paces this
  // loop close to real-time (same pattern as esp32_synth -- no audio ISR).
  writeAudioBlock();

  if (stepChanged) {
    stepChanged = false;
    JsonDocument doc;
    doc["type"] = "step";
    doc["step"] = currentStep;
    String s;
    serializeJson(doc, s);
    ws.textAll(s);
  }

  static uint32_t lastCleanup = 0;
  uint32_t now = millis();
  if (now - lastCleanup >= 1000) {
    lastCleanup = now;
    ws.cleanupClients();
  }

  // Cheap heartbeat so AP/station health is checkable over serial alone --
  // no need to have a phone in hand to tell whether anything ever joined.
  static uint32_t lastWifiLog = 0;
  if (now - lastWifiLog >= 5000) {
    lastWifiLog = now;
    Serial.printf("[heartbeat] AP stations=%d IP=%s playing=%d step=%d voices_active=%d,%d,%d,%d\n",
                  WiFi.softAPgetStationNum(), WiFi.softAPIP().toString().c_str(), playing,
                  currentStep, voices[0].active, voices[1].active, voices[2].active, voices[3].active);
  }
}
