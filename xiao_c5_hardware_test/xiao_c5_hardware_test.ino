/*
 * ============================================================================
 *  IoT Bhai — XIAO ESP32-C5 First-Upload Hardware Test  (I2C pins fixed)
 * ----------------------------------------------------------------------------
 *  Fix: the XIAO ESP32-C5's Expansion Base puts the OLED on D4(SDA)/D5(SCL),
 *       NOT the chip's default I2C pins. We now set the pins explicitly.
 *
 *  Toolchain: arduino-esp32 v3.1.0+  Board "XIAO_ESP32C5"  Library: U8g2
 *
 *  PASS: LED blinks, Serial shows "device found at 0x3C", OLED shows text.
 * ============================================================================
 */

#include <Wire.h>
#include <U8g2lib.h>

#define I2C_SDA  23        // XIAO D4 = GPIO23 (SDA)
#define I2C_SCL  24        // XIAO D5 = GPIO24 (SCL)

// Pass the SCL/SDA pins straight into the U8g2 hardware-I2C constructor
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R2, U8X8_PIN_NONE, I2C_SCL, I2C_SDA);

uint32_t counter = 0;

void i2cScan() {
  Serial.printf("[i2c] scanning on SDA=D4 SCL=D5 ...\n");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c] device found at 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("[i2c] none found - reseat the XIAO on the Base, check orientation");
  else            Serial.printf("[i2c] %d device(s) total\n", found);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== IoT Bhai XIAO ESP32-C5 hardware test ===");

  pinMode(LED_BUILTIN, OUTPUT);      // user LED = GPIO27

  Wire.begin(I2C_SDA, I2C_SCL);      // <-- the fix: use D4/D5, not chip default
  i2cScan();

  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(0, 14, "IoT Bhai");
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 30, "XIAO C5 OK");
  oled.drawStr(0, 44, "OLED @ 0x3C");
  oled.sendBuffer();

  Serial.println("[ok] setup done - watch the LED and OLED");
  delay(1500);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH); delay(500);
  digitalWrite(LED_BUILTIN, LOW);  delay(500);

  counter++;
  Serial.printf("[alive] uptime %lus  count=%lu\n", millis() / 1000, counter);

  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tf);
  oled.drawStr(0, 14, "XIAO C5 OK");
  oled.setFont(u8g2_font_6x12_tf);
  oled.drawStr(0, 32, "Hardware test");
  char line[24];
  snprintf(line, sizeof(line), "count: %lu", counter);
  oled.drawStr(0, 50, line);
  oled.sendBuffer();
}
