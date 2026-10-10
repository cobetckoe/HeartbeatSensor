/*
 * ESP32-S3 HLK-LD2450 Radar — Military Heartbeat v6
 *
 * ★ EEPROM memory: once locked, never re-scans
 * ★ No status bar → full-screen radar (R=96)
 * ★ ECG at bottom, NATO phosphor green palette
 *
 * Pin map (ESP32-S3 SuperMini):
 *   TFT:  CS=1  DC=2  RST=3  MOSI=6  SCLK=7  BLK=8
 *   Radar: Serial0 GPIO43/44 (auto-detect with TX/RX swap)
 *   Vibration: GPIO9 (optional, active LOW)
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include "HLK_LD2450.h"
#include "RadarDisplay.h"

// ── TFT Pins ──
#define TFT_CS    1
#define TFT_DC    2
#define TFT_RST   3
#define TFT_MOSI  6
#define TFT_SCLK  7
#define TFT_BLK   8

// ── Vibration Sensor (active LOW) ──
#define VIBRATION_PIN  9

// ── Objects ──
Adafruit_ST7735 display(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);
RadarDisplay radarDisplay(&display);
Preferences prefs;

HLK_LD2450 radarA(Serial0);
HLK_LD2450 radarB(Serial1);
HLK_LD2450 radarC(Serial2);
HLK_LD2450 *radar = nullptr;

// ── Timing ──
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 20; // ~50 FPS

// ── Display: ping-ring heartbeat (rings expand from center, no sweep line) ──
// Targets are collected each tick and handed to RadarDisplay::updateFrame()

int detectedBaud = 0;
HardwareSerial *activeSerial = nullptr;

// ══════════════════════════════════════════════════════
//  EEPROM CONFIG — save once, never scan again
// ══════════════════════════════════════════════════════
// Key IDs for UART config:
//   0=Serial0, 1=Serial1, 2=Serial2
//   -1=not saved yet

struct UartConfig {
  int uartId;   // 0/1/2
  int rxPin;
  int txPin;
  bool swapped;
  long baudRate;
};

bool loadConfig(UartConfig &cfg) {
  prefs.begin("radar", true);  // read-only
  cfg.uartId   = prefs.getInt("uart", -1);
  cfg.rxPin    = prefs.getInt("rx", -1);
  cfg.txPin    = prefs.getInt("tx", -1);
  cfg.swapped  = prefs.getBool("swap", false);
  cfg.baudRate = prefs.getLong("baud", 0);
  prefs.end();
  return (cfg.uartId >= 0 && cfg.baudRate > 0);
}

void saveConfig(int uartId, int rxPin, int txPin, bool swapped, long baudRate) {
  prefs.begin("radar", false);
  prefs.putInt("uart", uartId);
  prefs.putInt("rx", rxPin);
  prefs.putInt("tx", txPin);
  prefs.putBool("swap", swapped);
  prefs.putLong("baud", baudRate);
  prefs.end();
  Serial.printf("[EEPROM] Saved: UART%d %ld baud %s\n", uartId, baudRate, swapped ? "(swapped)" : "");
}

void clearConfig() {
  prefs.begin("radar", false);
  prefs.clear();
  prefs.end();
}

// ══════════════════════════════════════════════════════
//  UART SCAN (fallback when no saved config)
// ══════════════════════════════════════════════════════

bool scanUart(HardwareSerial &uart, int rxPin, int txPin, int uartId, bool swapped) {
  long bauds[] = {256000, 115200, 460800, 9600};

  for (int b = 0; b < 4; b++) {
    long baud = bauds[b];
    uart.end();
    delay(50);
    uart.begin(baud, SERIAL_8N1, rxPin, txPin);
    delay(150);
    while (uart.available()) uart.read();
    delay(20);

    unsigned long t0 = millis();
    int byteCount = 0;
    uint8_t buf[16];

    while (millis() - t0 < 1200) {
      if (uart.available()) {
        uint8_t b = uart.read();
        if (byteCount < 16) buf[byteCount] = b;
        byteCount++;
      }
    }

    if (byteCount == 0) continue;

    for (int i = 0; i <= byteCount - 4; i++) {
      if (buf[i]==0xAA && buf[i+1]==0xFF && buf[i+2]==0x03 && buf[i+3]==0x00) {
        Serial.printf("[SCAN] Found! UART%d %ld baud %s\n", uartId, baud, swapped ? "(swapped)" : "");
        uart.end();
        delay(30);
        uart.begin(baud, SERIAL_8N1, rxPin, txPin);
        delay(100);
        while (uart.available()) uart.read();
        detectedBaud = baud;
        activeSerial = &uart;
        saveConfig(uartId, rxPin, txPin, swapped, baud);
        return true;
      }
    }
  }
  return false;
}

// Try saved config first (fast, 1-second test)
bool trySavedConfig(const UartConfig &cfg) {
  HardwareSerial *ports[] = {&Serial0, &Serial1, &Serial2};
  if (cfg.uartId < 0 || cfg.uartId > 2) return false;

  HardwareSerial &uart = *ports[cfg.uartId];
  Serial.printf("[EEPROM] Trying saved: UART%d %ld baud\n", cfg.uartId, cfg.baudRate);

  uart.end();
  delay(30);
  uart.begin(cfg.baudRate, SERIAL_8N1, cfg.rxPin, cfg.txPin);
  delay(100);
  while (uart.available()) uart.read();
  delay(20);

  unsigned long t0 = millis();
  int byteCount = 0;
  uint8_t buf[16];

  while (millis() - t0 < 1000) {
    if (uart.available()) {
      uint8_t b = uart.read();
      if (byteCount < 16) buf[byteCount] = b;
      byteCount++;
    }
  }

  if (byteCount == 0) return false;

  for (int i = 0; i <= byteCount - 4; i++) {
    if (buf[i]==0xAA && buf[i+1]==0xFF && buf[i+2]==0x03 && buf[i+3]==0x00) {
      Serial.printf("[EEPROM] OK! Running\n");
      detectedBaud = cfg.baudRate;
      activeSerial = &uart;
      return true;
    }
  }
  Serial.printf("[EEPROM] Stale, scanning...\n");
  return false;
}

void deepUartScan() {
  Serial.println("[SCAN] Auto-detect...");
  // S3: Serial0(43/44), Serial1(4/5), Serial2(16/17) — try both pin orders
  if (scanUart(Serial0, 43, 44, 0, false)) return;
  if (scanUart(Serial0, 44, 43, 0, true))  return;
  if (scanUart(Serial1, 4, 5, 1, false))    return;
  if (scanUart(Serial1, 5, 4, 1, true))     return;
  if (scanUart(Serial2, 16, 17, 2, false))  return;
  if (scanUart(Serial2, 17, 16, 2, true))   return;
  Serial.println("[SCAN] NOT FOUND");
}

// ══════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HEARTBEAT v6 S3 ===");

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  pinMode(VIBRATION_PIN, INPUT_PULLUP);

  display.initR(INITR_BLACKTAB);
  display.setRotation(1);
  display.setSPISpeed(40000000);  // fast full-frame push (lower to 24000000 if glitchy)
  display.fillScreen(0x0000);

  display.setTextColor(0x07E0);
  display.setTextSize(1);
  display.setCursor(20, 50);
  display.println(F("HEARTBEAT v6"));

  // ── Try EEPROM first (instant) ──
  UartConfig saved;
  if (loadConfig(saved)) {
    display.setTextColor(0x03E0);
    display.setCursor(15, 70);
    display.println(F("Checking saved config..."));

    if (trySavedConfig(saved)) {
      display.fillScreen(0x0000);
      display.setTextColor(0x07E0);
      display.setCursor(20, 50);
      display.printf("LOCKED %ld", detectedBaud);
      delay(400);
      display.fillScreen(0x0000);
      radarDisplay.begin();
      return; // Done! Skip full scan
    }
    // Saved config stale → clear and full scan
    clearConfig();
  }

  // ── Full scan (first boot or stale config) ──
  display.setTextColor(0x8410);
  display.setCursor(10, 70);
  display.println(F("Scanning UART..."));
  deepUartScan();

  if (activeSerial == &Serial0)      radar = &radarA;
  else if (activeSerial == &Serial1) radar = &radarB;
  else if (activeSerial == &Serial2) radar = &radarC;

  display.fillScreen(0x0000);
  if (detectedBaud > 0) {
    display.setTextColor(0x07E0);
    display.setCursor(20, 50);
    display.printf("LOCKED %ld", detectedBaud);
    delay(400);
  } else {
    display.setTextColor(0xF800);
    display.setCursor(25, 40);
    display.println(F("NO SIGNAL"));
    display.setTextColor(0x8410);
    display.setCursor(10, 60);
    display.println(F("Check 5V GND TX RX"));
    delay(2000);
  }

  display.fillScreen(0x0000);
  radarDisplay.begin();
  Serial.printf("[OK] %s\n", detectedBaud > 0 ? "RUNNING" : "NO DATA");
}

// ══════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  if (radar) radar->update();

  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;

    // ── Collect valid targets ──
    float tDist[3], tAngle[3];
    int targetCount = 0;
    if (radar) {
      for (int i = 0; i < 3; i++) {
        if (radar->targets[i].valid) {
          float dist = radar->targets[i].distance_m;
          float angle = radar->targets[i].angle_deg;
          if (dist > 0.05 && dist < 8.0 && angle >= -95.0 && angle <= 95.0) {
            tDist[targetCount] = dist;
            tAngle[targetCount] = angle;
            targetCount++;
          }
        }
      }
    }

    // ── Heartbeat ping-ring display (double-buffered, flicker-free) ──
    radarDisplay.updateECG(targetCount > 0);
    radarDisplay.updateFrame(now, tDist, tAngle, targetCount);

    // ── Debug 1Hz ──
    static unsigned long lastDbg = 0;
    if (now - lastDbg >= 1000) {
      lastDbg = now;
      uint32_t raw = radar ? radar->rawBytes : 0;
      uint32_t tot = radar ? radar->totalFrames : 0;
      Serial.printf("T:%d F:%lu RAW:%lu BAUD:%d\n", targetCount, tot, raw, detectedBaud);
    }
  }
}
