// Bring-up test for this ESP32-C3 board (purple PCB, "ESP32-C3 SuperMini
// with 0.42in OLED" -- identified by web lookup, not guessed). Confirms
// the toolchain, USB-JTAG/serial link, flashing, the onboard LED, and the
// onboard OLED all work end-to-end. Not the synth firmware, see
// ../esp32_synth.
//
// LED: GPIO8, confirmed by this board's "IO8" silkscreen label and a
// working plain digitalWrite() blink. Plain on/off LED, not WS2812.
//
// OLED: this board's actual, sourced (not guessed) display config:
//   SSD1306-compatible controller, 0x3C, I2C on SDA=GPIO5 SCL=GPIO6.
//   Visible glass is 72x40, but the controller itself addresses a 128x64
//   RAM window and no dedicated 72x40 constructor exists in U8g2, so this
//   uses the standard community workaround: drive it as a 128x64 SSD1306
//   and offset every draw by (30, 12) -- ((128-72)/2, (64-40)/2) -- to
//   land content on the visible window.
// Source: github.com/peff74/ESP32-C3_OLED (working example for this exact
// board).

#include <Arduino.h>
#include <U8g2lib.h>

#define LED_PIN 8

#define OLED_SDA 5
#define OLED_SCL 6
#define OLED_VISIBLE_WIDTH 72
#define OLED_VISIBLE_HEIGHT 40
#define OLED_X_OFFSET 30 // (128 - 72) / 2
#define OLED_Y_OFFSET 12 // (64 - 40) / 2

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(255);
  u8g2.setFont(u8g2_font_9x15B_tr);

  const char *text = "RONEN";
  int textWidth = u8g2.getStrWidth(text);
  int x = OLED_X_OFFSET + (OLED_VISIBLE_WIDTH - textWidth) / 2;
  int y = OLED_Y_OFFSET + (OLED_VISIBLE_HEIGHT + u8g2.getAscent()) / 2;

  u8g2.clearBuffer();
  u8g2.drawStr(x, y, text);
  u8g2.sendBuffer();
}

void loop() {
  static uint32_t count = 0;
  digitalWrite(LED_PIN, HIGH);
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  delay(1000);
  Serial.printf("alive: %lu\n", (unsigned long)count++);
}
