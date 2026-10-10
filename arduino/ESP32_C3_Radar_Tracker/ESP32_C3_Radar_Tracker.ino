/*
 * ESP32-C3 SuperMini HLK-LD2450 Radar — Military Heartbeat v6
 *
 * ★ Same display logic as S3 version
 * ★ EEPROM memory: once locked, never re-scans
 * ★ No status bar → full-screen radar (R=96)
 * ★ ECG at bottom, NATO phosphor green palette
 *
 * Pin map (ESP32-C3 SuperMini):
 *   TFT:  CS=1  DC=2  RST=3  MOSI=7  SCLK=6  BLK=10
 *   Radar: UART1 GPIO4(RX) GPIO5(TX) — only UART1 available
 *          (UART0 = USB Serial, shared with debug)
 *   Vibration: GPIO8 (optional, active LOW)
 *
 * NOTE: C3 only has UART0 (USB) + UART1.
 *       No UART2. Radar must be on UART1 (GPIO4/5).
 *       If GPIO4/5 conflict with other peripherals,
 *       you can remap radar to any two free GPIOs.
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include "HLK_LD2450.h"
#include "RadarDisplay.h"

// ── TFT Pins (C3 SuperMini) ──
#define TFT_CS    1
#define TFT_DC    2
#define TFT_RST   3
#define TFT_MOSI  7   // C3: MOSI on GPIO7
#define TFT_SCLK  6   // C3: SCLK on GPIO6
#define TFT_BLK   10  // C3: Backlight on GPIO10

// ── Radar UART (C3 only has UART1) ──
#define RADAR_RX  4
#define RADAR_TX  5

// ── Vibration Sensor (active LOW) ──
#define VIBRATION_PIN  8

// ── Objects ──
// ★ 硬件 SPI（原来用软件 SPI 位模拟，整帧 40KB 要 150-300ms → 严重卡顿）
//   实测：1-40MHz 全频段整屏推送均无花屏。20MHz → 19.4ms/帧 ≈ 44fps。
//   ⚠️ 必须先 SPI.begin(显式引脚) 再 initR()，否则驱动内部的无参 begin()
//      会把 SCLK 重映射到 C3 变体默认的 GPIO4 → 必然花屏。
#define TFT_SPI_HZ  20000000UL   // 流畅度不够就降 16000000 / 12000000 / 8000000

Adafruit_ST7735 display(&SPI, TFT_CS, TFT_DC, TFT_RST);
RadarDisplay radarDisplay(&display);
Preferences prefs;

HLK_LD2450 radar1(Serial1);
HLK_LD2450 *radar = nullptr;

// ── Timing ──
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 20; // ~50 FPS

// ── Display: ping-ring heartbeat (rings expand from center, no sweep) ──

int detectedBaud = 0;
HardwareSerial *activeSerial = nullptr;

// ══════════════════════════════════════════════════════
//  EEPROM CONFIG — save once, never scan again
// ══════════════════════════════════════════════════════

struct UartConfig {
  int rxPin;
  int txPin;
  bool swapped;
  long baudRate;
};

bool loadConfig(UartConfig &cfg) {
  prefs.begin("radar", true);
  cfg.rxPin    = prefs.getInt("rx", -1);
  cfg.txPin    = prefs.getInt("tx", -1);
  cfg.swapped  = prefs.getBool("swap", false);
  cfg.baudRate = prefs.getLong("baud", 0);
  prefs.end();
  return (cfg.rxPin >= 0 && cfg.baudRate > 0);
}

void saveConfig(int rxPin, int txPin, bool swapped, long baudRate) {
  prefs.begin("radar", false);
  prefs.putInt("rx", rxPin);
  prefs.putInt("tx", txPin);
  prefs.putBool("swap", swapped);
  prefs.putLong("baud", baudRate);
  prefs.end();
  Serial.printf("[EEPROM] Saved: %ld baud %s\n", baudRate, swapped ? "(swapped)" : "");
}

void clearConfig() {
  prefs.begin("radar", false);
  prefs.clear();
  prefs.end();
}

// ══════════════════════════════════════════════════════
//  UART SCAN — C3 only has Serial1 (UART1)
// ══════════════════════════════════════════════════════

bool scanUart(HardwareSerial &uart, int rxPin, int txPin, bool swapped) {
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
        Serial.printf("[SCAN] Found! %ld baud %s\n", baud, swapped ? "(swapped)" : "");
        uart.end();
        delay(30);
        uart.begin(baud, SERIAL_8N1, rxPin, txPin);
        delay(100);
        while (uart.available()) uart.read();
        detectedBaud = baud;
        activeSerial = &uart;
        saveConfig(rxPin, txPin, swapped, baud);
        return true;
      }
    }
  }
  return false;
}

bool trySavedConfig(const UartConfig &cfg) {
  Serial.printf("[EEPROM] Trying saved: %ld baud\n", cfg.baudRate);

  Serial1.end();
  delay(30);
  Serial1.begin(cfg.baudRate, SERIAL_8N1, cfg.rxPin, cfg.txPin);
  delay(100);
  while (Serial1.available()) Serial1.read();
  delay(20);

  unsigned long t0 = millis();
  int byteCount = 0;
  uint8_t buf[16];

  while (millis() - t0 < 1000) {
    if (Serial1.available()) {
      uint8_t b = Serial1.read();
      if (byteCount < 16) buf[byteCount] = b;
      byteCount++;
    }
  }

  if (byteCount == 0) return false;

  for (int i = 0; i <= byteCount - 4; i++) {
    if (buf[i]==0xAA && buf[i+1]==0xFF && buf[i+2]==0x03 && buf[i+3]==0x00) {
      Serial.printf("[EEPROM] OK! Running\n");
      detectedBaud = cfg.baudRate;
      activeSerial = &Serial1;
      return true;
    }
  }
  Serial.printf("[EEPROM] Stale, scanning...\n");
  return false;
}

void deepUartScan() {
  Serial.println("[SCAN] Scanning UART1...");
  // C3: Only UART1 (GPIO4/5). Try both pin orders.
  if (scanUart(Serial1, RADAR_RX, RADAR_TX, false)) return;
  if (scanUart(Serial1, RADAR_TX, RADAR_RX, true))  return;
  Serial.println("[SCAN] NOT FOUND");
}

// ══════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== HEARTBEAT v6 C3 ===");

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  pinMode(VIBRATION_PIN, INPUT_PULLUP);

  // ★ 硬件 SPI 必须在面板驱动之前初始化，且必须显式传引脚。
  //   SCLK=GPIO6 / MOSI=GPIO7 正是 ESP32-C3 的 SPI2 IO_MUX 专用引脚（零延迟直连）。
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);

  display.initR(INITR_BLACKTAB);
  display.setRotation(1);
  display.setSPISpeed(TFT_SPI_HZ);
  display.fillScreen(0x0000);

  // ── 开机 LOGO（内置点阵，用你自己的图替换：见 utils/logo_converter.py）──
  radarDisplay.showBootLogo(1500);

  // ══════════════════════════════════════════════════════
  //  ★ LOGO 之后直接进雷达界面，屏幕上不再出现任何检测/状态页。
  //     UART 探测逻辑照旧，但全部静默进行（只写串口日志，不写屏幕）。
  // ══════════════════════════════════════════════════════
  UartConfig saved;
  if (loadConfig(saved)) {
    if (!trySavedConfig(saved)) clearConfig();   // 记忆失效 → 清掉，走全扫描
  }
  if (detectedBaud == 0) deepUartScan();         // 仅在没锁上时才扫

  radar = &radar1;  // C3: always Serial1

  // 没锁上也先把 UART1 起在 256000：屏幕立刻进雷达界面，
  // 之后传感器若上电/复位，数据还能被接住（全程无任何状态页）。
  if (detectedBaud == 0) {
    Serial1.end();
    delay(20);
    Serial1.begin(256000, SERIAL_8N1, RADAR_RX, RADAR_TX);
  }

  radarDisplay.begin();   // LOGO 结束 → 直接雷达界面
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

    // ── Heartbeat ping-ring display (double-buffered) ──
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
