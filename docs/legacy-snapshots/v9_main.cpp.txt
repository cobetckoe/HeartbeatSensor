/*
 * ESP32 HLK-LD2450 Radar - Semicircle Sweep v8 (unified S3/C3)
 *
 * * EEPROM memory: once locked, never re-scans
 * * No boot splash: radar UI is on screen within ~100ms of power-on
 * * No status bar -> full-screen semicircle radar (R=96)
 * * Smooth reciprocal sweep beam (left->right->left, no jumps)
 * * Incremental refresh: only changed rows are pushed (no tearing)
 * * ECG at bottom, NATO phosphor green palette
 *
 * Pin map (ESP32-S3 SuperMini):
 *   TFT:  CS=1  DC=2  RST=3  MOSI=6  SCLK=7  BLK=8
 *   Radar: Serial0 GPIO43/44 (auto-detect with TX/RX swap)
 *   Vibration: GPIO9 (optional, active LOW)
 *
 * Pin map (ESP32-C3 SuperMini):
 *   TFT:  CS=1  DC=2  RST=3  MOSI=7  SCLK=6  BLK=10
 *   Radar: UART1 GPIO4(RX) GPIO5(TX) - only UART1 available
 *          (UART0 = USB Serial, shared with debug)
 *   Vibration: GPIO8 (optional, active LOW)
 *
 * Board selected by build flag: -DBOARD_S3 (env:esp32s3) / -DBOARD_C3 (env:esp32c3)
 * Falls back to ARDUINO_ESP32C3 detection if no explicit flag is given.
 *
 * NOTE: C3 only has UART0 (USB) + UART1. No UART2.
 *       Radar must be on UART1 (GPIO4/5). If GPIO4/5 conflict with other
 *       peripherals, remap RADAR_RX/RADAR_TX below to any two free GPIOs.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include "HLK_LD2450.h"
#include "RadarDisplay.h"

// ---------------------------------------------------------------------------
// Board selection
// ---------------------------------------------------------------------------
#if !defined(BOARD_S3) && !defined(BOARD_C3)
  #if defined(ARDUINO_ESP32C3_DEV) || defined(ARDUINO_ESP32C3_UNORDERED)
    #define BOARD_C3
  #else
    #define BOARD_S3
  #endif
#endif

// -- TFT Pins (common) --
#define TFT_CS    1
#define TFT_DC    2
#define TFT_RST   3

#if defined(BOARD_C3)
  #define BOARD_NAME     "C3"
  #define TFT_MOSI       7   // C3: MOSI on GPIO7
  #define TFT_SCLK       6   // C3: SCLK on GPIO6
  #define TFT_BLK        10  // C3: Backlight on GPIO10
  #define VIBRATION_PIN  8
  #define RADAR_RX       4
  #define RADAR_TX       5
#else
  #define BOARD_NAME     "S3"
  #define TFT_MOSI       6
  #define TFT_SCLK       7
  #define TFT_BLK        8
  #define VIBRATION_PIN  9
#endif

// -- Objects --
// Hardware SPI (VSPI/FSPI). Passing the SPIClass* selects TFT_HARD_SPI, which
// uses the ESP32 SPI peripheral DMA-less bulk path (writePixels -> one
// spiTransferBytes). The previous 5-pin constructor selected TFT_SOFT_SPI,
// i.e. bit-banged GPIO toggling, which is ~10x slower and was the real cause
// of the choppy sweep. Pins are bound in setup() via SPI.begin().
Adafruit_ST7735 display(&SPI, TFT_CS, TFT_DC, TFT_RST);
RadarDisplay radarDisplay(&display);
Preferences prefs;

#if defined(BOARD_C3)
  // C3: only UART1 (UART0 is the USB CDC debug port)
  HLK_LD2450 radar1(Serial1);
#else
  HLK_LD2450 radarA(Serial0);
  HLK_LD2450 radarB(Serial1);
  HLK_LD2450 radarC(Serial2);
#endif
HLK_LD2450 *radar = nullptr;

// -- Timing --
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 20; // ~50 FPS

int detectedBaud = 0;
HardwareSerial *activeSerial = nullptr;

// ---------------------------------------------------------------------------
//  EEPROM CONFIG - save once, never scan again
// ---------------------------------------------------------------------------
// Key IDs for UART config:
//   0=Serial0, 1=Serial1, 2=Serial2
//   -1=not saved yet
// (C3 always uses UART1; legacy C3 saves without the "uart" key default to 1)

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
#if defined(BOARD_C3)
  if (cfg.uartId < 0) cfg.uartId = 1;  // legacy C3 saves lack the "uart" key
#endif
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

// ---------------------------------------------------------------------------
//  UART SCAN (fallback when no saved config)
// ---------------------------------------------------------------------------

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
#if defined(BOARD_C3)
  HardwareSerial &uart = Serial1;
#else
  if (cfg.uartId < 0 || cfg.uartId > 2) return false;
  HardwareSerial *ports[] = {&Serial0, &Serial1, &Serial2};
  HardwareSerial &uart = *ports[cfg.uartId];
#endif
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

// Bind the HLK_LD2450 wrapper to the serial port detected or saved
void bindRadar() {
#if defined(BOARD_C3)
  radar = &radar1;  // C3: always Serial1
#else
  if (activeSerial == &Serial0)      radar = &radarA;
  else if (activeSerial == &Serial1) radar = &radarB;
  else if (activeSerial == &Serial2) radar = &radarC;
#endif
}

void deepUartScan() {
#if defined(BOARD_C3)
  Serial.println("[SCAN] Scanning UART1...");
  // C3: only UART1 (GPIO4/5). Try both pin orders.
  if (scanUart(Serial1, RADAR_RX, RADAR_TX, 1, false)) return;
  if (scanUart(Serial1, RADAR_TX, RADAR_RX, 1, true))  return;
#else
  Serial.println("[SCAN] Auto-detect...");
  // S3: Serial0(43/44), Serial1(4/5), Serial2(16/17) - try both pin orders
  if (scanUart(Serial0, 43, 44, 0, false)) return;
  if (scanUart(Serial0, 44, 43, 0, true))  return;
  if (scanUart(Serial1, 4, 5, 1, false))    return;
  if (scanUart(Serial1, 5, 4, 1, true))     return;
  if (scanUart(Serial2, 16, 17, 2, false))  return;
  if (scanUart(Serial2, 17, 16, 2, true))   return;
#endif
  Serial.println("[SCAN] NOT FOUND");
}

// ---------------------------------------------------------------------------
//  SETUP
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== RADAR SWEEP v8 " BOARD_NAME " ===");

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  pinMode(VIBRATION_PIN, INPUT_PULLUP);

  // Explicit hardware SPI init on the custom pins BEFORE the panel driver runs.
  // Without this the Adafruit driver may fall back to bit-banged SPI, which is
  // far too slow for smooth animation on the ESP32-C3.
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);

  display.initR(INITR_BLACKTAB);
  display.setRotation(1);
  // 8MHz：杜邦线 + 面板时序的安全值。增量渲染每帧只推小块，带宽绰绰
  // 有余，动画流畅度不受影响。
  display.setSPISpeed(8000000);

  // -- 自定义开机 LOGO（时长/文字/颜色改 RadarDisplay.h 里的 LOGO_* 宏）--
  radarDisplay.showBootLogo();

  // Radar UI on screen immediately - no boot splash, no config text.
  radarDisplay.begin();

  // Silent UART bind (saved config first, full scan as fallback).
  UartConfig saved;
  if (loadConfig(saved) && trySavedConfig(saved)) {
    bindRadar();
  } else {
    clearConfig();
    deepUartScan();
    bindRadar();
  }

  Serial.printf("[OK] %s\n", detectedBaud > 0 ? "RUNNING" : "NO DATA");
}

// ---------------------------------------------------------------------------
//  LOOP
// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  if (radar) radar->update();

  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;

    // -- Collect valid targets --
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

    // -- Semicircle sweep display (incremental refresh, tear-free) --
    // Only the changed pixels are pushed to the TFT, so no full-frame
    // tearing appears in the middle of the screen.
    radarDisplay.updateFrame(now, tDist, tAngle, targetCount);

    // -- Debug 1Hz --
    static unsigned long lastDbg = 0;
    if (now - lastDbg >= 1000) {
      lastDbg = now;
      uint32_t raw = radar ? radar->rawBytes : 0;
      uint32_t tot = radar ? radar->totalFrames : 0;
      Serial.printf("T:%d F:%lu RAW:%lu BAUD:%d\n", targetCount, tot, raw, detectedBaud);
    }
  }
}
