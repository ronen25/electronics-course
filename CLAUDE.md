# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

The product is `esp32_synth/`, a 3-oscillator additive synthesizer, built with PlatformIO (not Arduino IDE / arduino-cli). Targets a specific user's board: an **"ESP32-C3 SuperMini with 0.42in OLED"** (purple PCB, generic/unbranded — identified by web lookup, not the vendor's own docs). PlatformIO has no dedicated board definition for it, so `platformio.ini` uses the closest generic match, `board = esp32-c3-devkitm-1`. `esp32_synth/src/main.cpp` is the entire firmware.

The C3 has no DAC peripheral, so audio goes out over I2S to an external MAX98357A mono amp (I2S DAC + Class-D amp in one chip): LRC→GPIO9, BCLK→GPIO7, DIN→GPIO10, sent as 32-bit words (not 16 -- see below), amp SD tied directly to 3V3 (enable, mono output), GAIN left floating (9dB), amp GND/Vin → board GND/3V3. This replaced an earlier LEDC-PWM-into-RC-filter approach — see git history for that version if it's ever needed again. None of these 3 pins are GPIO8 (onboard LED) or GPIO0-5 (the 6 pots), so there's no interference with either.

Hard-won lessons from getting this actually working (audio was silent through several distinct root causes, found one at a time):
- **GPIO8 carries this board's onboard LED.** A continuously-toggling signal (I2S BCLK) placed there visibly lit the LED solid (toggling too fast to see as blinking) and corrupted the clock enough to kill audio entirely -- confirmed by moving BCLK off GPIO8 and watching the LED go back to off. Never put a fast/continuous digital signal on GPIO8.
- **The MAX98357A's SD pin defaults to shutdown if left floating** (internal pull-down) -- it must be tied to 3V3, not just intended to be. This was missed initially because a wire was plugged into a nearby-but-different pin.
- **A breadboard's `+`/`-` rail only carries power if something else feeds it** -- tying SD to the rail didn't help until the rail was confirmed to actually have its own wire back to 3V3.
- **16-bit stereo (32x BCLK/LRCLK ratio) is a known-flaky auto-detect case for the MAX98357A**; 32-bit words (64x ratio) are more reliable even when only ~16 bits of actual precision are used. Switched to 32-bit as a precaution during bring-up.
- **The actual final root cause, after all of the above were fixed and audio was still silent, was mechanical**: the speaker wire wasn't fully seated in the amp's screw terminal. No amount of firmware/pin correctness fixes that -- when signal-chain debugging is fully exhausted (correct clock via `i2s_get_clk()`, `ESP_OK` on every call, continuous non-stalling writes) and there's still no sound, check the physical output connection next.
- A minimal standalone bring-up sketch (no synth logic, no display, no pots -- just a hardcoded tone) was far more effective for isolating these issues than debugging inside `esp32_synth` directly. Kept as `esp32_i2s_test/` alongside `esp32c3_blink_test/` for future hardware bring-up.

`esp32c3_blink_test/` is a bring-up scaffold, not part of the product, but it's also the current source of truth for this board's confirmed LED/OLED pinout (see Hardware notes below) — check it before re-deriving pin assumptions. Treat it as disposable/reworkable otherwise; don't build product features on it without checking whether it's still wanted first. `esp32_i2s_test/` is the same kind of scaffold, for I2S/MAX98357A audio bring-up — a minimal hardcoded-tone sketch with no synth/display/pot logic, kept separate because isolating hardware issues there was much faster than debugging inside `esp32_synth` directly (see the I2S/MAX98357A lessons under Hardware notes).

## Commands

Run from inside whichever project directory (the one containing `platformio.ini`):

```
pio run                    # build
pio run --target upload    # build + flash to a connected board
pio device monitor         # serial monitor (115200 baud, matches monitor_speed)
pio run --target clean     # clean build artifacts (.pio/build)
```

There is no test suite, linter, or CI config in this repo.

### Toolchain notes for this machine

- PlatformIO is installed via Homebrew (`brew install platformio`), not pip — pyenv's active Python here is newer than PlatformIO's installer supports.
- `esp32_synth/platformio.ini`, `esp32c3_blink_test/platformio.ini`, and `esp32_i2s_test/platformio.ini` all pin `platform = espressif32`, `board = esp32-c3-devkitm-1`, `framework = arduino`, and require `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` — required for `Serial` to bind to this board's native USB-Serial/JTAG peripheral, since it has no separate CP210x/CH340 bridge chip.
- PlatformIO resolved an older `framework-arduinoespressif32` (pre-3.x) that uses the **legacy** Arduino-ESP32 APIs, not the modern ones:
  - Timer: `timerBegin(num, divider, countUp)` / `timerAttachInterrupt(timer, fn, edge)` / `timerAlarmWrite` + `timerAlarmEnable` (not `timerBegin(frequency)` / `timerAlarm(...)`).
  - LEDC/PWM: `ledcSetup(channel, freq, resolution_bits)` + `ledcAttachPin(pin, channel)` + `ledcWrite(channel, duty)` (not the newer pin-addressed `ledcAttach(pin, freq, resolution)` / `ledcWrite(pin, duty)`).
  - Any new timer or PWM code must use these legacy signatures, matching what's already in `main.cpp`.

### Hardware notes

- The ESP32-C3 chip has no DAC peripheral at all — confirmed by the absence of `SOC_DAC_SUPPORTED` in the vendor's `soc_caps.h` for this chip, and `dacWrite()` in the Arduino core is compiled out entirely for C3 targets. Audio output must go through LEDC PWM (or an external DAC chip, if one is added).
- **Board identity and confirmed pinout** (sourced from a working example for this exact board, github.com/peff74/ESP32-C3_OLED, plus physical confirmation on the user's unit):
  - LED: GPIO8, plain on/off (not WS2812) — confirmed by this board's "IO8" silkscreen label and a working `digitalWrite()` blink.
  - OLED: SSD1306-compatible, I2C address `0x3C`, SDA=GPIO5, SCL=GPIO6. Visible glass is 72x40, but the controller addresses a 128x64 RAM window and no dedicated 72x40 constructor exists in U8g2 (the display library now in use, in `esp32c3_blink_test/`) — drive it as 128x64 and offset every draw by (30, 12) to land on the visible area. This is implemented and confirmed working in `esp32c3_blink_test/src/main.cpp`.
  - **`esp32_synth/src/main.cpp` has not been updated to match** — it still assumes Adafruit_SSD1306 (not U8g2), SDA/SCL=GPIO8/9 (wrong — GPIO8 is the LED, and GPIO5/6 are the real I2C pins), and a native 72x40 constructor (doesn't exist in either library it could use). Its `VOLUME_PINS` also uses GPIO5 for a potentiometer, which conflicts with the now-confirmed OLED SDA pin. Treat `esp32_synth`'s display/pin code as stale until it's ported to match these confirmed facts.

## Architecture

Everything lives in `esp32_synth/src/main.cpp`. There's no audio-rate ISR — the legacy ESP-IDF I2S driver's own DMA/hardware clock paces sample output, so both synthesis and control-rate work happen from the Arduino `loop()`:

- **Audio block generation (`writeAudioBlock()`)** — called every `loop()` iteration. For each of the 3 oscillators it advances a 32-bit phase accumulator (`phaseAcc`) by a per-oscillator phase increment (`phaseInc`), looks up the top 8 bits of the phase in a precomputed 256-entry sine wavetable (`sineTable`), scales by that oscillator's volume (`oscVolume`, 0-255), and packs the summed/averaged mix into a 128-sample stereo buffer (mono duplicated onto both I2S channels). `i2s_write()` blocks until the DMA queue has room, which is what naturally paces `loop()` close to real-time now — there's no separate hardware timer.
- **Control-rate (`loop()` → `readControls()`)** — reads 3 pitch potentiometers and 3 volume potentiometers via `analogRead` (12-bit ADC, on the C3's 6 ADC-capable pins GPIO0-5), maps pitch to `FREQ_MIN..FREQ_MAX` (40-1200Hz) and volume to 0-255, and writes the results into `phaseInc[]`/`oscVolume[]`. Gated to run roughly every 80ms via a `millis()` check inside `loop()`, alongside `writeAudioBlock()`. Since everything now runs single-threaded from `loop()` (no ISR), these arrays don't need `volatile`.
- **Display (`updateDisplay()`)** — draws a compact single-line-per-oscillator layout on a 72x40 OLED over I2C, but is currently only called once in `setup()` (a static "Synth / booting..." screen), not from `loop()` -- see the I2S/I2C conflict note below. Uses the Adafruit_GFX/Adafruit_SSD1306 libraries (pulled via `lib_deps` in `platformio.ini`).

Pin assignments and full wiring are documented in the file header comment of `main.cpp` — read that before changing pin numbers.

### Constraints that shape the code

- `writeAudioBlock()` calls `i2s_write(..., portMAX_DELAY)`, which blocks until DMA buffer space frees up — this is what keeps `loop()` roughly synced to `SAMPLE_RATE` without a timer ISR. Don't add slow/blocking work ahead of it in `loop()`, or audio will underrun.
- Forward declarations for helper functions are written explicitly at the top of `main.cpp` rather than relying on Arduino's `.ino` auto-prototyping, since this is a plain `.cpp` file under PlatformIO.
- `i2s_pin_config_t` has an `mck_io_num` field that isn't obvious from typical I2S examples — it defaults to GPIO0 if left unset via `= {}`, which would silently collide with the pitch-1 pot. Always set it to `I2S_PIN_NO_CHANGE` explicitly.
- **Never put a continuously-toggling signal (I2S BCLK, PWM, etc.) on GPIO8** — it's hardwired to this board's onboard LED. The LED+resistor load is a real electrical load on that pin, not just a cosmetic side effect: assigning BCLK there made the LED appear solidly lit (toggling too fast to see as blinking) and corrupted the clock signal badly enough that the MAX98357A produced no audio at all, despite every other connection (GND, Vin/SD power, I2S data/WS pins, speaker) being correct. Found by physically tracing wiring photos and noticing the "always on" LED as the tell.
- **The I2S driver and the Arduino `Wire` (I2C) library cannot both be active on this chip** — confirmed by direct measurement (per-call timing showed `updateDisplay()` jumping from 1ms to a flat 6000ms the moment `setupI2S()` ran) and matches a known, still-open Arduino-ESP32 core bug: [espressif/arduino-esp32#4686](https://github.com/espressif/arduino-esp32/issues/4686). Whichever peripheral initializes first works; the other then stalls every transaction for several seconds. Since `writeAudioBlock()` and any `Wire`-based `updateDisplay()` call both run from the same single-threaded `loop()`, a stalled I2C call would silence audio for its duration too — not just a display cosmetic issue. Current workaround: don't call `updateDisplay()` from `loop()` at all (see above). The documented real fix is dropping `Wire` for the OLED and talking to it via the raw ESP-IDF `driver/i2c.h` API instead (per a maintainer comment on that issue) — not yet done here.
- `analogRead(GPIO5)` (the osc3 volume pot) always fails with `ESP_ERR_INVALID_ARG` — GPIO5 maps to ADC2 on the C3, which this Arduino core has disabled (WiFi coexistence limitation upstream, not fixable from application code). Pre-existing, unrelated to the I2S work above.
