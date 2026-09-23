// WiFi + I2S coexistence bring-up test -- no synth, no display, no pots.
// Purpose: find out whether running WiFi (AP mode) + ESPAsyncWebServer +
// WebSockets alongside a continuous I2S audio loop causes audible glitches
// or dropped/delayed i2s_write() calls on this single-core ESP32-C3, before
// wiring a web UI into the real drum machine. See DRUM_MACHINE_PLAN.md
// ("Key risk: audio/WiFi CPU contention") for why this exists.
//
// What it does:
//   - Plays a continuous 440Hz tone out over I2S to the MAX98357A (same
//     wiring as esp32_i2s_test/).
//   - Hosts a WiFi AP ("drum-machine-test", password below) and a tiny
//     ESPAsyncWebServer with:
//       GET /        -> plain text status page (exercises HTTP handling)
//       WS  /ws      -> WebSocket that echoes back any text it receives and
//                        also gets a broadcast counter pushed to it once per
//                        second (exercises WS send from loop(), same pattern
//                        the real drum machine will use for step/playhead
//                        broadcasts).
//   - Measures the worst-case i2s_write() call duration in each 1-second
//     window and prints it over serial, plus whether bytesWritten ever came
//     back short (a real underrun, not just slow).
//
// How to use it: flash this, connect a phone/laptop to the AP, open
// http://192.168.4.1/ in a browser (and optionally hit it with repeated
// requests / open the page and leave it loading) while listening to the
// tone. Watch the serial monitor for:
//   - "worst i2s_write" spikes much above the ~6.4ms nominal block time
//     (128 samples / 20000 Hz) -- indicates WiFi is stalling the audio loop.
//   - Any "SHORT WRITE" lines -- indicates an actual dropped-sample underrun.
//   - Audible clicks/glitches in the tone while a page is loading or a
//     WebSocket message arrives.
// If it's clean, WiFi + I2S can coexist and the real drum machine can wire
// the web UI directly into the audio loop the same way this test does. If
// not, next things to try: lower WiFi TX power, batch/rate-limit WS sends
// further, or move networking off the hot path some other way.
//
// Wiring: identical to esp32_i2s_test/ -- LRC->GPIO3, BCLK->GPIO7,
// DIN->GPIO10, amp SD->3V3 directly, GAIN floating, amp GND->board GND,
// amp Vin->board 5V pin (NOT 3V3 -- see esp32_i2s_test/src/main.cpp's
// STATUS note: this module needs 5V in practice to produce output).

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <driver/i2s.h>

#define SAMPLE_RATE 20000
#define TONE_HZ 440
#define I2S_PORT I2S_NUM_0
#define I2S_PIN_LRC 3
#define I2S_PIN_BCLK 7
#define I2S_PIN_DOUT 10
#define BLOCK_SAMPLES 128

static const char *AP_SSID = "drum-machine-test";
static const char *AP_PASSWORD = "drumtest123"; // WPA2 min length is 8 chars

static int32_t buf[BLOCK_SAMPLES * 2]; // interleaved L/R

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

void setupI2S() {
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
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  } else if (type == WS_EVT_DATA) {
    client->text("echo: " + String((char *)data, len));
  }
}

void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "WiFi+I2S coexistence test OK\n");
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("WiFi + I2S coexistence test starting");

  setupI2S();
  setupWeb();

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

  uint32_t t0 = micros();
  size_t bytesWritten = 0;
  esp_err_t err = i2s_write(I2S_PORT, buf, sizeof(buf), &bytesWritten, portMAX_DELAY);
  uint32_t elapsedUs = micros() - t0;

  static uint32_t worstUs = 0;
  if (elapsedUs > worstUs) worstUs = elapsedUs;
  if (err != ESP_OK || bytesWritten != sizeof(buf)) {
    Serial.printf("SHORT WRITE: err=%s bytesWritten=%u/%u\n", esp_err_to_name(err),
                  (unsigned)bytesWritten, (unsigned)sizeof(buf));
  }

  static uint32_t wsBroadcastCounter = 0;
  static uint32_t lastLog = 0;
  uint32_t now = millis();
  if (now - lastLog >= 1000) {
    lastLog = now;
    Serial.printf("worst i2s_write this window: %lu us (nominal ~%lu us), heap=%u, ws clients=%u\n",
                  (unsigned long)worstUs,
                  (unsigned long)(1000000UL * BLOCK_SAMPLES / SAMPLE_RATE),
                  (unsigned)ESP.getFreeHeap(), (unsigned)ws.count());
    worstUs = 0;

    // Exercises WS send from loop(), same pattern the real drum machine
    // will use to push step/playhead updates to connected browsers.
    ws.textAll(String("tick:") + String(wsBroadcastCounter++));
    ws.cleanupClients();
  }
}
