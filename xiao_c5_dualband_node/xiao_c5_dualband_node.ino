/*
 * ============================================================================
 *  IoT Bhai — XIAO ESP32-C5 Dual-Band AUTO Comparison Node
 * ----------------------------------------------------------------------------
 *  Board : Seeed Studio XIAO ESP32-C5  (dual-band Wi-Fi 6)
 *  Base  : Seeed Studio Expansion Board Base for XIAO (SSD1306 OLED @ 0x3C)
 *
 *  What it does — fully automatic, no re-flashing between bands:
 *    1. Connects LOCKED to 2.4 GHz (SSID "IoT Bhai")  -> benchmarks it
 *    2. Connects LOCKED to 5   GHz (SSID "IoT Bhai_5G")-> benchmarks it
 *    3. Shows a SIDE-BY-SIDE comparison on the OLED (Mbps / RSSI / CH +
 *       a "Faster" line), holds it on screen for filming, then repeats.
 *    Each band result is also published to MQTT.
 *
 *  Split-SSID routers: this router uses one name per band, so the matching
 *  SSID is selected automatically for whichever band is being tested.
 *
 *  >>> Reality check for the video <<<
 *  Raw Mbps on the ESP32-C5 is capped by the CHIP (~15-40 Mbps), not the air
 *  link, so 5 GHz may NOT look dramatically faster than 2.4 GHz. The honest,
 *  visible difference is RSSI/range (2.4 reaches further; 5 is faster only up
 *  close) and channel/congestion. For the 5 GHz run, move near the router.
 *
 *  Toolchain:
 *    - arduino-esp32 core v3.1.0+   Board: "XIAO_ESP32C5"
 *    - Libraries (Library Manager): U8g2, PubSubClient
 * ============================================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "esp_wifi.h"        // esp_wifi_set_band_mode()
#include "arduino_secrets.h" // your Wi-Fi credentials (git-ignored; copy from
                             // arduino_secrets.h.example and fill in your own)

// ============================== USER CONFIG =================================

// --- Wi-Fi: split SSIDs, one name per band (values live in arduino_secrets.h) ---
const char* WIFI_SSID_24 = SECRET_WIFI_SSID_24;   // 2.4 GHz network name
const char* WIFI_SSID_5  = SECRET_WIFI_SSID_5;    // 5 GHz  network name
const char* WIFI_PASS    = SECRET_WIFI_PASS;      // assumes SAME password on both

// --- MQTT (optional logging; failures are harmless) ---
const char* MQTT_HOST      = "mqtt.iotbhai.io";
const uint16_t MQTT_PORT   = 1883;
const char* MQTT_USER      = "";             // leave "" if broker is open
const char* MQTT_PASS      = "";
const char* MQTT_TOPIC     = "iotbhai/c5/bench";
const char* MQTT_CLIENT_ID = "xiao-c5-node";

// --- Throughput test (file served from a PC on the SAME router) ---
const char* THROUGHPUT_URL = "http://192.168.0.197:8000/testfile.bin";
const bool  RUN_THROUGHPUT = true;

// --- Timing ---
const uint32_t CONNECT_TIMEOUT_MS = 20000;   // give up connecting after this
const uint32_t THROUGHPUT_MAX_MS  = 120000;  // hard stop per download (100 MB @
                                             // ~15 Mbps ≈ 55 s; 120 s is safe)
const uint32_t COMPARE_HOLD_MS    = 20000;   // how long the result screen stays
const uint32_t BAND_SETTLE_MS     = 400;     // pause while radio switches band

// ===========================================================================

#define BAND_24   1
#define BAND_5    2

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R2, U8X8_PIN_NONE, /*SCL=*/24, /*SDA=*/23);
WiFiClient   netClient;
PubSubClient mqtt(netClient);

// One benchmark result for a single band.
struct BandResult {
  bool     ok;
  int      band;          // BAND_24 / BAND_5
  const char* label;      // "2.4G" / "5G"
  int      channel;
  long     rssi;
  uint32_t connectMs;
  float    mbps;
  char     ip[20];
};

BandResult r24, r5;

// -------------------------------------------------------------- helpers -----

const char* ssidForBand(int band)  { return band == BAND_5 ? WIFI_SSID_5 : WIFI_SSID_24; }
const char* labelForBand(int band) { return band == BAND_5 ? "5G"        : "2.4G"; }

void setBandMode(int band) {
  wifi_band_mode_t mode = (band == BAND_5) ? WIFI_BAND_MODE_5G_ONLY
                                           : WIFI_BAND_MODE_2G_ONLY;
  esp_err_t err = esp_wifi_set_band_mode(mode);
  Serial.printf("[wifi] set_band_mode(%s) -> %s\n",
                labelForBand(band), esp_err_to_name(err));
}

// format a value into a fixed column, or "--" when that band failed
void fmtF(char* out, size_t n, bool ok, float v) {
  if (ok) snprintf(out, n, "%.1f", v); else snprintf(out, n, "--");
}
void fmtI(char* out, size_t n, bool ok, long v) {
  if (ok) snprintf(out, n, "%ld", v); else snprintf(out, n, "--");
}

// ------------------------------------------------------------- OLED ---------

void oledLines(const String& l1, const String& l2 = "",
               const String& l3 = "", const String& l4 = "") {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 12, l1.c_str());
  if (l2.length()) oled.drawStr(0, 26, l2.c_str());
  if (l3.length()) oled.drawStr(0, 40, l3.c_str());
  if (l4.length()) oled.drawStr(0, 54, l4.c_str());
  oled.sendBuffer();
}

void oledTesting(int band, const char* phase) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tf);
  char t[24];
  snprintf(t, sizeof(t), "Testing %s", labelForBand(band));
  oled.drawStr(0, 16, t);
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 34, phase);
  oled.drawStr(0, 50, ssidForBand(band));
  oled.sendBuffer();
}

// Live download screen: band + % on top, current Mbps, and a progress bar.
void oledProgress(int band, int pct, float mbps) {
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  oled.clearBuffer();

  oled.setFont(u8g2_font_7x14B_tf);
  char t[24];
  snprintf(t, sizeof(t), "%s  %d%%", labelForBand(band), pct);
  oled.drawStr(0, 15, t);

  oled.setFont(u8g2_font_7x14B_tf);
  char m[24];
  snprintf(m, sizeof(m), "%.1f Mbps", mbps);
  oled.drawStr(0, 36, m);

  // progress bar
  oled.drawFrame(0, 46, 128, 14);
  int w = (pct * 124) / 100;
  if (w > 0) oled.drawBox(2, 48, w, 10);

  oled.sendBuffer();
}

// Side-by-side 2.4 vs 5 comparison. 128x64, 6x12 font ≈ 21 chars/line.
void oledCompare(const BandResult& a /*2.4*/, const BandResult& b /*5*/) {
  char line[26], av[8], bv[8];
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);

  //            label   col-2.4    col-5
  snprintf(line, sizeof(line), "%-5s %6s %6s", "",     a.label, b.label);
  oled.drawStr(0, 10, line);

  fmtF(av, sizeof(av), a.ok, a.mbps);
  fmtF(bv, sizeof(bv), b.ok, b.mbps);
  snprintf(line, sizeof(line), "%-5s %6s %6s", "Mbps", av, bv);
  oled.drawStr(0, 23, line);

  fmtI(av, sizeof(av), a.ok, a.rssi);
  fmtI(bv, sizeof(bv), b.ok, b.rssi);
  snprintf(line, sizeof(line), "%-5s %6s %6s", "dBm", av, bv);
  oled.drawStr(0, 36, line);

  fmtI(av, sizeof(av), a.ok, (long)a.channel);
  fmtI(bv, sizeof(bv), b.ok, (long)b.channel);
  snprintf(line, sizeof(line), "%-5s %6s %6s", "CH", av, bv);
  oled.drawStr(0, 49, line);

  // Winner (by Mbps). Highlighted bar across the bottom row.
  const char* faster = "--";
  if (a.ok && b.ok) faster = (b.mbps >= a.mbps) ? "5G" : "2.4G";
  else if (a.ok)    faster = "2.4G";
  else if (b.ok)    faster = "5G";
  snprintf(line, sizeof(line), " Faster: %s", faster);
  oled.setDrawColor(1);
  oled.drawBox(0, 54, 128, 10);       // highlighted bar
  oled.setDrawColor(0);               // knock text out in black
  oled.drawStr(2, 62, line);
  oled.setDrawColor(1);               // restore for next frame

  oled.sendBuffer();
}

// ---------------------------------------------------------- throughput ------

// Times a download; counts bytes as they pass through, stores nothing.
// Updates the OLED live with running Mbps + percentage while it downloads.
float measureThroughput(int band) {
  if (!RUN_THROUGHPUT) return 0.0f;

  HTTPClient http;
  if (!http.begin(netClient, THROUGHPUT_URL)) {
    Serial.println("[thru] http.begin failed");
    return 0.0f;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[thru] HTTP %d\n", code);
    http.end();
    return 0.0f;
  }

  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  uint32_t total = 0;
  uint32_t t0 = millis();
  uint32_t lastDraw = 0;

  oledProgress(band, 0, 0.0f);

  while (http.connected() && (len < 0 || total < (uint32_t)len)) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(buf, min(avail, sizeof(buf)));
      total += n;
    }

    uint32_t now = millis();
    if (now - lastDraw >= 300) {                 // refresh OLED ~3x/sec
      lastDraw = now;
      uint32_t dt = now - t0;
      float mbps = dt ? (total * 8.0f) / (dt * 1000.0f) : 0.0f;
      int pct = (len > 0) ? (int)((uint64_t)total * 100 / (uint32_t)len) : 0;
      oledProgress(band, pct, mbps);
    }

    if (millis() - t0 > THROUGHPUT_MAX_MS) break;
    yield();
  }
  uint32_t dt = millis() - t0;
  http.end();

  if (dt == 0) return 0.0f;
  float mbps = (total * 8.0f) / (dt * 1000.0f);   // bits / seconds / 1e6
  int pct = (len > 0) ? (int)((uint64_t)total * 100 / (uint32_t)len) : 100;
  oledProgress(band, pct, mbps);                  // final frame (100%)
  Serial.printf("[thru] %u bytes in %u ms = %.2f Mbps\n", total, dt, mbps);
  return mbps;
}

// ---------------------------------------------------------------- MQTT ------

void mqttPublish(const BandResult& r) {
  if (!mqtt.connected()) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    bool ok = (strlen(MQTT_USER) > 0)
                ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)
                : mqtt.connect(MQTT_CLIENT_ID);
    Serial.printf("[mqtt] connect %s\n", ok ? "OK" : "FAIL");
  }
  if (!mqtt.connected()) return;

  char payload[220];
  snprintf(payload, sizeof(payload),
    "{\"band\":\"%s\",\"ok\":%s,\"channel\":%d,\"rssi\":%ld,"
    "\"connect_ms\":%u,\"mbps\":%.2f,\"ip\":\"%s\"}",
    r.label, r.ok ? "true" : "false", r.channel, r.rssi,
    r.connectMs, r.mbps, r.ip);
  bool ok = mqtt.publish(MQTT_TOPIC, payload);
  Serial.printf("[mqtt] publish %s : %s\n", ok ? "OK" : "FAIL", payload);
}

// ------------------------------------------------------- benchmark one ------

BandResult benchmarkBand(int band) {
  BandResult r;
  r.ok = false; r.band = band; r.label = labelForBand(band);
  r.channel = 0; r.rssi = 0; r.connectMs = 0; r.mbps = 0.0f; r.ip[0] = '\0';

  Serial.printf("\n===== BENCH %s  (SSID=%s) =====\n", r.label, ssidForBand(band));
  oledTesting(band, "connecting...");

  // Clean switch off the previous band, then lock the new one.
  WiFi.disconnect(true, true);
  delay(BAND_SETTLE_MS);
  WiFi.mode(WIFI_STA);
  delay(100);
  setBandMode(band);
  WiFi.begin(ssidForBand(band), WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > CONNECT_TIMEOUT_MS) {
      Serial.printf("[wifi] %s connect TIMEOUT\n", r.label);
      oledTesting(band, "FAILED - skipped");
      delay(1500);
      return r;                       // r.ok stays false
    }
    delay(100);
  }

  r.connectMs = millis() - t0;
  r.channel   = WiFi.channel();
  r.rssi      = WiFi.RSSI();
  WiFi.localIP().toString().toCharArray(r.ip, sizeof(r.ip));
  Serial.printf("[wifi] %s connected in %u ms  ch=%d rssi=%ld ip=%s\n",
                r.label, r.connectMs, r.channel, r.rssi, r.ip);

  oledTesting(band, "downloading...");
  r.mbps = measureThroughput(band);
  r.ok   = true;

  mqttPublish(r);
  return r;
}

// ---------------------------------------------------------------- setup -----

void setup() {
  Serial.begin(115200);
  delay(300);

  Wire.begin(23, 24);           // XIAO Expansion Base I2C: D4=GPIO23(SDA), D5=GPIO24(SCL)
  oled.begin();
  oledLines("IoT Bhai", "XIAO C5", "2.4 vs 5 GHz", "auto compare");
  delay(1500);
}

// ---------------------------------------------------------------- loop ------

void loop() {
  mqtt.loop();

  // Benchmark both bands back-to-back.
  r24 = benchmarkBand(BAND_24);
  r5  = benchmarkBand(BAND_5);

  // Show the comparison and hold it long enough to film.
  Serial.printf("\n##### COMPARE  2.4G: %s %.2f Mbps / %ld dBm   |   5G: %s %.2f Mbps / %ld dBm #####\n",
                r24.ok ? "ok" : "FAIL", r24.mbps, r24.rssi,
                r5.ok  ? "ok" : "FAIL", r5.mbps,  r5.rssi);
  oledCompare(r24, r5);

  uint32_t hold0 = millis();
  while (millis() - hold0 < COMPARE_HOLD_MS) {
    mqtt.loop();
    delay(50);
  }
}
