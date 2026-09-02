# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

The product is `esp32_synth/`, a 3-oscillator additive synthesizer, built with PlatformIO (not Arduino IDE / arduino-cli). Targets a specific user's board: an **"ESP32-C3 SuperMini with 0.42in OLED"** (purple PCB, generic/unbranded — identified by web lookup, not the vendor's own docs). PlatformIO has no dedicated board definition for it, so `platformio.ini` uses the closest generic match, `board = esp32-c3-devkitm-1`. `esp32_synth/src/main.cpp` is the entire firmware.

The C3 has no DAC peripheral, so audio is currently synthesized via LEDC PWM into an external RC low-pass filter. The user intends to add a real DAC to the hardware later — when that happens, the PWM output block in `onAudioTimer()`/`setup()` is what needs to change.

`esp32c3_blink_test/` is a bring-up scaffold, not part of the product, but it's also the current source of truth for this board's confirmed LED/OLED pinout (see Hardware notes below) — check it before re-deriving pin assumptions. Treat it as disposable/reworkable otherwise; don't build product features on it without checking whether it's still wanted first.

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
- Both `esp32_synth/platformio.ini` and `esp32c3_blink_test/platformio.ini` pin `platform = espressif32`, `board = esp32-c3-devkitm-1`, `framework = arduino`, and require `build_flags = -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` — required for `Serial` to bind to this board's native USB-Serial/JTAG peripheral, since it has no separate CP210x/CH340 bridge chip.
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

Everything lives in `esp32_synth/src/main.cpp`. The design splits work between a hardware timer ISR (audio-rate) and the Arduino `loop()` (control-rate):

- **Audio ISR (`onAudioTimer`)** — fires at `SAMPLE_RATE` (20kHz) via a hardware timer. For each of the 3 oscillators it advances a 32-bit phase accumulator (`phaseAcc`) by a per-oscillator phase increment (`phaseInc`), looks up the top 8 bits of the phase in a precomputed 256-entry sine wavetable (`sineTable`), scales by that oscillator's volume (`oscVolume`, 0-255), and writes the summed/averaged mix out as a PWM duty cycle (`ledcWrite`, GPIO7). This is the entire synthesis engine — no separate audio buffer or task, just this ISR.
- **Control-rate (`loop()` → `readControls()`)** — reads 3 pitch potentiometers and 3 volume potentiometers via `analogRead` (12-bit ADC, on the C3's 6 ADC-capable pins GPIO0-5), maps pitch to `FREQ_MIN..FREQ_MAX` (40-1200Hz) and volume to 0-255, and writes the results into `phaseInc[]`/`oscVolume[]`. These arrays are the only communication channel between `loop()` and the ISR; they're `volatile` but not otherwise synchronized — safe here only because `loop()` writes at a much lower rate than the ISR reads.
- **Display (`updateDisplay()`)** — redraws a 72x40 OLED over I2C each `loop()` iteration (~every 80ms), in a compact single-line-per-oscillator layout sized for the small panel. Uses the Adafruit_GFX/Adafruit_SSD1306 libraries (pulled via `lib_deps` in `platformio.ini`).

Pin assignments and full wiring are documented in the file header comment of `main.cpp` — read that before changing pin numbers.

### Constraints that shape the code

- The audio ISR must stay allocation-free and fast (`IRAM_ATTR`) since it runs at 20kHz; any new per-sample work belongs there, while anything that can tolerate ~12ms latency (control reads, UI) belongs in `loop()`.
- Forward declarations for the ISR and helper functions are written explicitly at the top of `main.cpp` rather than relying on Arduino's `.ino` auto-prototyping, since this is a plain `.cpp` file under PlatformIO.
