# Drum Machine + Web Interface — Plan

Status: implemented, including a rework to match `docs/Drum Machine.dc.html`'s
visual/control design (see "Design pass" below), and flashed to real
hardware. **Audio bring-up is now RESOLVED (2026-09-16): the amp module is
not dead — see item 10 in "Hardware bring-up" below. Root cause was Vin
wired to 3V3 instead of 5V.** See "Hardware bring-up: audio signal chain
debugging" below before touching pin assignments or gain code again. WiFi/AP
bring-up (a separate, now-resolved issue) is also covered there. Captured
from a design discussion on 2026-09-09; implemented 2026-09-09; reworked to
match the added design on 2026-09-09; hardware bring-up on 2026-09-10/11,
resolved 2026-09-16.

## Hardware bring-up: audio signal chain debugging (2026-09-10/11)

Flashed to real hardware for the first time and worked through a long chain
of issues, roughly in the order hit:

1. **WiFi AP silently failing to start.** `WiFi.softAP()`'s return value
   wasn't checked; `softAPIP()` returns the configured IP regardless of
   whether the radio actually came up, so a failure looked identical to
   success in the serial log. Fixed with a checked/retried `softAP()` call
   in `setupWeb()` (`esp32_drum_machine/src/main.cpp`) — confirmed working
   after this fix.
2. **Software gain was double-attenuated.** `writeAudioBlock()` multiplied
   by the master-volume knob *and* divided by a fixed headroom factor,
   stacking to ~39% of full scale even at default settings. Removed the
   fixed divide; the master knob is now the only global attenuation.
3. **GAIN-pin rabbit hole (dead end).** A user report that the MAX98357A's
   GAIN pin was wired to a GPIO (not floating) led to a firmware fix driving
   that GPIO low for a defined 12dB. This had zero effect, because the
   amp's SD pin is hardwired straight to 3V3 — GAIN/SD are latched once at
   the shutdown→enabled transition, which happens before the ESP32 finishes
   booting, so nothing done in software after boot can change it. **The
   GAIN report itself turned out to be wrong anyway** (see next point) — the
   pin in question was actually LRC, and GAIN really was floating the whole
   time. The GPIO-drive code was reverted. Net lesson: GAIN pin changes on
   this amp need an actual hardware rewire (tie to GND directly, or via a
   ~100kΩ ±5% resistor to GND for the datasheet max of 15dB — smaller
   resistors like 1k/220Ω/10kΩ all behave identically to a direct short,
   12dB, since 100kΩ is what the datasheet's tolerance is specified around).
4. **LRC pin was wrong — but which pin took three tries to pin down.**
   Physical inspection first reported LRC on GPIO3 (correcting the initial
   assumption of GPIO9, copied from `esp32_synth` without re-verification).
   Fixed in firmware, but WiFi then stopped broadcasting entirely.
5. **WiFi/I2S power contention.** A/B testing (revert LRC pin, WiFi comes
   back; reapply it, WiFi drops) confirmed real contention once the amp
   started actually switching for real, most likely a power-rail sag during
   WiFi TX bursts (among the highest peak-current events on this chip)
   coinciding with the amp's now-continuous Class-D switching current.
   Mitigated by lowering WiFi TX power (`WiFi.setTxPower(WIFI_POWER_8_5dBm)`
   in `setupWeb()`) — confirmed this restored stable WiFi with audio-driving
   pins active.
6. **Still silent after the GPIO3 fix.** Added serial diagnostics
   (`[ws cmd]` log, extended `[heartbeat]` with playing/step/voice-active
   state) and confirmed the entire software stack was working correctly:
   WebSocket commands arriving, sequencer stepping on schedule, voices
   triggering and decaying right on schedule. The bug was entirely in the
   physical signal chain, not code.
7. **LRC pin, take two.** A user photo, traced wire-by-wire against the
   ESP32 board's silkscreened pin numbers, showed LRC was actually on
   **GPIO5**, not GPIO3 (the GPIO3 report was itself wrong). BCLK (GPIO7)
   and DIN (GPIO10) were confirmed correct by the same photo. Moved LRC to
   GPIO5 in firmware and physically — still silent.
8. **Systematic elimination.** With LRC now tried on 3, 5, *and* 9 (the
   last being `esp32_synth`'s own historically-proven pin, wired up as a
   direct A/B test) and all three silent, attention shifted to the rest of
   the chain: speaker verified good via a AAA-battery click test done in
   complete isolation from the amp; amp confirmed powered (onboard LED lit);
   SD re-seated and confirmed solid; amp moved to a fresh breadboard slot.
   All good, all still silent.
9. **Conclusion by elimination (superseded by item 10 below)**: every other
   link in the chain (ESP32/I2S driver, WiFi, software, wiring pins, speaker,
   amp power/enable) had been individually verified. The MAX98357A amp
   module itself was the one remaining, untested-in-isolation component, and
   was concluded most likely dead. The wiring pattern documented in both
   `esp32_drum_machine/src/main.cpp` and `esp32_i2s_test/src/main.cpp`
   (LRC→GPIO9, BCLK→GPIO7, DIN→GPIO10, SD→3V3, GND→GND, GAIN floating) was
   otherwise the confirmed-correct one.
10. **RESOLVED (2026-09-16): the amp was never dead — Vin was under-powered.**
    Every step above wired amp Vin to the board's 3V3 rail (same as the SD
    enable pin), which is within the MAX98357A's datasheet supply range
    (2.5-5.5V) but not enough in practice for this module to actually
    produce output. Moving Vin alone to the board's 5V pin (SD stays on
    3V3 — it's a separate logic-level enable pin, unaffected by this) fixed
    it immediately, no other changes needed. Lesson for next time: datasheet
    "minimum voltage" and "voltage a specific real module needs to actually
    work" are not the same thing — when a device is silent and every digital
    signal/pin/connection has checked out, question the supply voltage
    itself even if it's nominally in-range, before condemning the module.

## Design pass (2026-09-09)

## Design pass (2026-09-09)

`docs/Drum Machine.dc.html` is a Claude Design Canvas artboard — it depends
on the design tool's own runtime (`support.js`, `_ds_bundle.js`, `{{ }}`
templating) and can't run standalone, so it was never going to be droppable
into the firmware verbatim. `esp32_drum_machine/src/main.cpp`'s embedded
`INDEX_HTML` is a from-scratch, dependency-free port of its layout and
control surface: title/tag header, Play, BPM, Master volume, Save/Load, and
per-oscillator (not per-fixed-drum-type) strips with a sine/square/saw
waveform selector plus pitch/decay/volume sliders and a 16-step row.

Porting the control surface meant reworking the audio engine to match: the
old kick/snare/hihat-specific DSP (pitch-swept sine kick, highpassed-noise
snare/hihat) is gone, replaced by one generic one-shot enveloped oscillator
type (`Voice`, sine/square/saw selectable) used for all three tracks — the
mockup's controls (waveform, pitch, decay, volume, no noise/sweep knobs)
only make sense against a single uniform voice model. Default waveform/pitch
per track (kick=sine, snare=square, hihat=saw, with the mockup's exact
pitch/decay/volume/step-pattern values) approximates the old hand-tuned
sounds but will need re-tuning by ear on real hardware — a pure square-wave
"snare" and saw-wave "hihat" with no noise component will sound more
tonal/synthy than the old noise-based versions did.

New in this pass, driven by the mockup's Save/Load buttons: single-slot
pattern+settings persistence to flash via the Arduino-ESP32 core's
`Preferences` (NVS) wrapper — no new library dependency. Communication with
the browser was also switched from the earlier hand-rolled comma-separated
text protocol to JSON (`bblanchon/ArduinoJson`), since per-oscillator nested
state (wave/pitch/decay/volume/steps × 3) made manual string parsing
error-prone. Sliders send their WebSocket update on release (`change`, not
`input`) rather than continuously while dragging, both to avoid flooding the
WebSocket during a drag and to avoid the server's state echo fighting the
slider's position mid-drag.

## Goal

Add a drum machine to the ESP32-C3 board (same hardware as `esp32_synth/`), programmable
from a web browser (step-sequencer style UI) rather than the onboard pots.

## Hardware constraints (confirmed)

- Chip: ESP32-C3, single-core RISC-V, 160MHz. No PSRAM support on this chip at all.
- RAM: 320KB usable SRAM (`maximum_ram_size` in the board def), of which `esp32_synth`'s
  existing 3-oscillator synth uses only ~15.7KB (4.8%) — confirmed via `pio run` size report.
  Plenty of headroom for either approach below.
- Flash: 4MB physical, but the default PlatformIO partition table only gives the app
  ~1.3MB — the rest needs repartitioning to use.
- WiFi/Bluetooth are available on this chip (per the board's `connectivity` field), untested
  in this project so far.

## Drum sound generation: two options

1. **Synthesized drums** (recommended starting point)
   - Kick = sine + pitch/amplitude envelope (pitch sweep down).
   - Snare/hihat = filtered noise burst + envelope.
   - RAM cost: negligible (a few bytes of envelope state per voice) — same order of
     magnitude as the existing oscillator state.
   - No flash/partition changes needed.
   - Architecturally similar to the existing oscillator code in `esp32_synth/src/main.cpp`,
     just swapping continuous tone generation for one-shot enveloped voices.

2. **Sample-based drums** (real recorded hits)
   - RAM is not the binding constraint (samples get streamed from flash, not loaded
     whole into RAM — SPI flash read speed comfortably exceeds PCM streaming needs).
   - The real constraint is **flash partitioning**: default app partition is only 1.3MB,
     so this needs a custom partition table (e.g. a LittleFS/SPIFFS region) to store
     sample data, up to most of the 4MB chip.
   - More setup cost than option 1; revisit if synthesized drums aren't satisfying enough.

Decision: start with synthesized drums, revisit samples later if wanted.

## Web interface

- Serve a small step-sequencer grid UI (HTML/JS) from the ESP32, likely via
  **ESPAsyncWebServer** + **WebSockets** for real-time step toggling (avoid blocking
  HTTP request handling from stalling audio).
- WiFi mode: likely AP mode (board hosts its own network) rather than joining an
  existing WiFi network, since this is meant to be a standalone instrument — open
  question, revisit when implementing.
- Pattern state lives in a small RAM array (steps × tracks) — trivial memory footprint.
- Persistence (patterns surviving reboot) not yet decided — likely flash (NVS or
  LittleFS) if wanted, low priority initially.

## Key risk: audio/WiFi CPU contention

This project already hit one confirmed peripheral-coexistence bug on this exact chip:
I2S and Arduino `Wire` (I2C) cannot both be active without one stalling the other for
seconds at a time (see `CLAUDE.md` — Arduino-ESP32 core issue #4686). The audio loop
(`writeAudioBlock()`) is single-threaded and timing-critical: `i2s_write()` blocking is
what paces sample output close to real-time, with no separate audio ISR.

Adding WiFi introduces the same category of risk: the C3 is single-core, so WiFi's
stack running interrupts/scheduling alongside the tight audio loop could glitch audio
the same way the I2C conflict did.

**Plan: prove WiFi + I2S coexist cleanly in a throwaway bring-up sketch before wiring
the web server into the real synth/drum machine code.** Follow the same pattern as
`esp32_i2s_test/` (a minimal hardcoded-tone sketch used to isolate the earlier I2S
bring-up issues) — e.g. a new `esp32_wifi_i2s_test/` scaffold that plays a continuous
tone while an async web server handles simple requests, and check for audible glitches
or dropped I2S writes.

## Next steps

1. **Done.** `esp32_wifi_i2s_test/` — WiFi AP + ESPAsyncWebServer + WebSocket
   alongside a continuous I2S tone, with per-block timing instrumentation
   (worst-case `i2s_write()` duration and short-write detection printed over
   serial once a second). Compiles clean. **Not yet flashed/run on hardware —
   the actual coexistence question (audible glitches, i2s_write timing under
   WiFi/HTTP load) is still unverified.** See the file header for how to run it.
2. **Done, reworked in the design pass.** 3 one-shot enveloped oscillator
   voices (`Voice` in `esp32_drum_machine/src/main.cpp`), sine/square/saw
   selectable, `esp32_synth`-style wavetable sine plus square/saw derived
   from the same phase accumulator. Superseded the original kick/snare/hihat-
   specific DSP (pitch-swept sine kick, highpass-noise snare/hihat) — see
   "Design pass" above for why. No manual per-track trigger UI in this
   design (the mockup doesn't have one); triggering only happens from the
   sequencer or by pattern edits.
3. **Done, reworked in the design pass.** 16-step x 3-track sequencer with
   sample-accurate step timing (advanced once per output sample inside
   `writeAudioBlock()`, not per block), plus a step-sequencer web UI ported
   from `docs/Drum Machine.dc.html` (play/stop, BPM, master volume,
   per-oscillator waveform/pitch/decay/volume, per-step toggle, live
   playhead highlight, save/load) served from the ESP32 itself over WiFi AP
   mode, updated over a JSON WebSocket protocol (switched from the original
   plain-text one — see "Design pass" above for why).
4. **Done** (combined with 3): all UI controls wired directly into the
   sequencer and voice engine over the WebSocket.
5. (Optional, later) Repartition flash and add sample-based drum sound support as an
   alternative/addition to synthesized drums.
6. **Done, single-slot only.** Save/Load buttons persist bpm, master volume,
   and all 3 oscillators' waveform/pitch/decay/volume/pattern to flash via
   `Preferences` (NVS) — one slot, no auto-load on boot (only on pressing
   Load). Multi-slot / auto-restore-on-boot not implemented; revisit if
   wanted.

### What's left before this is "real"

- **Flash `esp32_wifi_i2s_test/` and confirm no audio glitches** while a
  browser is connected and hitting `/` or the WebSocket — this was the
  plan's key risk and is still unverified on real hardware. If it glitches,
  try: lower WiFi TX power, less frequent `ws.cleanupClients()`, or (bigger
  change) moving audio to its own FreeRTOS task pinned away from WiFi's.
- **Flash `esp32_drum_machine/` and listen** — the mockup's default
  waveform/pitch/decay/volume values per track were carried over as-is and
  are untested against a real speaker; a pure square-wave "snare" and
  saw-wave "hihat" with no noise component will likely sound more
  tonal/synthy than the old noise-based versions did, and may want retuning
  (or a return to noise-based snare/hihat, if the design's uniform-oscillator
  model turns out not to sound percussive enough in practice).
- Websocket commands (toggle/wave/pitch/decay/volume/etc.) touch the same
  voice/pattern state that `loop()` reads every sample, from a different
  FreeRTOS task (AsyncTCP's). No mutex is used (see the comment above
  `pattern` in `esp32_drum_machine/src/main.cpp`) — treated as an acceptable
  hobby-project race, but worth knowing if an edit ever causes an audible
  click.

## Resolved design decisions

- **AP mode**, not joining home WiFi — board hosts `drum-machine` /
  `drumbeats`, browser connects to it directly at `192.168.4.1`. Simpler and
  matches "standalone instrument" framing; revisit if it turns out to be
  annoying to have to switch WiFi networks to use it.
- **New sibling project** `esp32_drum_machine/`, not folded into
  `esp32_synth/` — different enough product (sequencer + web UI vs. pots +
  continuous oscillators) that sharing one `main.cpp` would mostly add
  branching, not reuse. They do share the I2S pin assignments/config and the
  wavetable-sine approach.
- **Tempo/clock**: no separate timer. The step counter decrements once per
  output *sample* inside `writeAudioBlock()`'s existing per-sample loop, so
  step boundaries are sample-accurate and ride on the same `i2s_write()`
  pacing the synth already relies on — no new ISR or RTOS timer needed.
- **HTTP, not just WebSocket**: the ESP32 runs a real `ESPAsyncWebServer` on
  port 80 serving the UI over plain HTTP GET `/`; the WebSocket at `/ws`
  (an HTTP-upgraded connection on that same server) is only the live-control
  channel layered on top, not a replacement for it.
