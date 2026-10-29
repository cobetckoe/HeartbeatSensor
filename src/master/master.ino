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
#include <stdarg.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Preferences.h>
#include "HLK_LD2450.h"
#include "OokReceiver.h"
#include "ZoneManager.h"
#include "RadarDisplay.h"
#include "DebugLog.h"
#include "TuyaLink.h"

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

// ── Vibration Sensor (GPIO8, active HIGH via MOSFET driver) ──
#define VIBRATION_PIN  8
#define OOK_RX_PIN     20
#define BOOT_PIN       9

//  注：433M 接收的接线自检（开机扫脚 / 全脚实时监视 / 输入阻抗测试 /
//      原始抓包 / 诊断长按手势）已在量产版中整体移除，
//      如需再次排查请参考 docs/legacy-snapshots/ 中的历史版本。

bool zoneFlags[4] = {false, false, false, false};
bool zoneEnabled[4] = {true, true, true, true};

// ── Objects ──
// ★ 硬件 SPI（原来用软件 SPI 位模拟，整帧 40KB 要 150-300ms → 严重卡顿）
//   ⚠️ 必须先 SPI.begin(显式引脚) 再 initR()，否则驱动内部的无参 begin()
//      会把 SCLK 重映射到 C3 变体默认的 GPIO4 → 必然花屏。
// ★ 为什么是 20MHz 而不是更高：
//   40MHz 下 SPI 瞬变会在 433MHz 模块 DATA 脚上把比特打翻（实测 crc>0）；
//   20MHz 实测 crc=0，代价只是推屏慢一倍 —— 而推屏本身已被预算限到 3000px，
//   单次只花 ~2.4ms，完全够用。不要为了帧率把频率提回去。
#define TFT_SPI_HZ  20000000UL   // 20MHz（40MHz 会把比特打翻，crc>0）

Adafruit_ST7735 display(&SPI, TFT_CS, TFT_DC, TFT_RST);
RadarDisplay radarDisplay(&display);
// 涂鸦 TuyaLink（MQTT 标准协议）。★ 独立 FreeRTOS 任务跑 WiFi/TLS/MQTT，
// 主循环只喂状态快照 —— TLS 握手是秒级阻塞，绝不能放进 loop()。
// 默认 WiFi 关闭（Preferences 持久化），三击 BOOT 键切换。
TuyaLink tuya;
Preferences prefs;
OokReceiver ookReceiver;
ZoneManager zoneManager;

HLK_LD2450 radar1(Serial1);
HLK_LD2450 *radar = nullptr;

bool screenOn = true;
bool pairingMode = false;
bool pairingSearchActive = false;
unsigned long pairingSearchStarted = 0;
const unsigned long PAIR_TIMEOUT_MS = 15000UL;

// ★ 配对模式【整体】空闲超时 —— 与上面「搜索中」的 15s 超时相互独立。
//   原来配对模式只在「正在搜索」时才有超时；一旦单击切防区，
//   pairingSearchActive 被清零 → 15s 超时永远不会再触发，
//   配对模式就只能靠双击退出。现场表现为「切完防区后卡在配对模式出不来」。
//   现在配对模式自身也有超时：长时间没有任何操作 → 自动退出。
const unsigned long PAIRING_IDLE_TIMEOUT_MS = 30000UL;
unsigned long pairingModeStarted = 0;   // 最近一次配对模式下的用户操作时刻

// ── 震动提示时长：检测到有人 → 震这么久就停（不再常亮）──
#ifndef VIBRATE_MS
#define VIBRATE_MS 3000UL
#endif

// ── 报警复位去抖：判据已迁到 ZoneManager.h 的 ZONE_CLEAR_FRAMES ──
//   ★★ 这里【故意不再定义】 ZONE_CLEAR_HOLD_MS。
//      历史上这个宏在 master.ino 和 ZoneManager.h 各有一份（6000 / 12000），
//      因为 ZoneManager.h 用的是 #ifndef，实际生效的永远是【先被 include 的
//      那一份】，也就是 ZoneManager.h —— 改 master.ino 这份完全不生效，
//      而且不会有任何报错。这种「同一个常量两个值」的重复定义是最典型的
//      静默失效陷阱，必须只留一处。
//
//   ★ 现在的判据是「连续收到 N 帧明确无人」（ZONE_CLEAR_FRAMES，默认 3），
//     不再依赖墙钟时间。理由见 ZoneManager.h 里的完整说明，核心是：
//     从机状态变化时连发 3 帧，正常离开 ~450ms 就能解锁；
//     而单个假无人帧只能让计数 +1，挡得住抖动。
//   ★ 曾经取 2000ms，实测「每 2~3 秒震一次」；抬到 6000ms 仍误复位；
//     再抬到 12000ms 则变成「P3.3 拉低好几秒主机还在报警」。
//     时间窗这个方向本身是错的 —— 不要再往回调。

// ── 「同一突发重复帧」时间窗 —— 已废弃，留作历史记录 ──
//   ★★ 这个常量现在【没有被任何代码使用】，但故意保留：
//     曾经用它放行「解出第 1 帧后 600ms 内无条件推屏」，
//     而那正是端到端延迟 2~3 秒的元凶（详见主循环里的说明）。
//     保留定义是为了让后来调参的人看得见这个坑，不要重新引入。
//   ⸻ 原始注释 ⸻
//   从机每次上报连发 2~3 帧，帧间隔 20~59ms，帧长 ~146ms。
//   既然已解出第 1 帧，后面的重复帧即使被整屏 SPI 推送（~10ms）打散，
//   也不丢任何功能信息（内容完全相同，只是 seq 不同）。
//   ⸻ 为什么这个推理是错的 ⸻
//   它对心跳帧成立，对【状态变化帧】是致命的：状态变化那 3 帧是主机
//   得知「有人了」的唯一机会，没有应答重传。被牺牲掉就只能等下一次
//   心跳（3s）。实测证据：11 分钟 ook=209（应约 440，命中率 47.5%），
//   以及逐帧时间戳里 dms=190 / 757 / 3400 三种延迟的分化。
// ── 【已删除】OOK_REPEAT_WINDOW_MS 600UL / DBG_LOG ──
//   · OOK_REPEAT_WINDOW_MS 是早期「600ms 内同 ID 同 seq 就去重」的方案，
//     已被 OokReceiver 内部基于 900ms 重复帧窗口 + 帧内否决的方案取代
//     （去重逻辑收进接收器，主循环不再需要知道窗口长度）。
//     留着一个写着「未使用，勿重新引入」的宏，反而在 grep 时误导人。
//   · DBG_LOG 曾经用来开关 1Hz 周期性日志（"T:..." / "[OOK-STAT]"），
//     那两类日志现在都不存在了（改成每 60s 一条 [HEALTH]），
//     宏本身零引用。现在的日志策略见 DebugLog.h：
//     全部走 dbgLog()，224 字节栈缓冲、零堆分配、非阻塞发送。

// ══════════════════════════════════════════════════════
//  BOOT 按键手势（原「长按进配对」与「长按复位」撞车，已重排）
// ══════════════════════════════════════════════════════
//   非配对模式：  单击 = 开关屏幕
//                 双击 = 进入配对（立即生效）
//                 长按 2s   = WiFi 开关（涂鸦接入，见 TuyaLink.h）
//                 长按 5s   = 系统复位（同时清掉已配的 WiFi）
//   配对模式内：  单击 = 切防区 A→B→C→D
//                 长按 1.5s = 对当前防区开始配对（防区闪烁）
//                 双击 = 退出配对模式
//   配对成功 / 超时 → 自动停止闪烁并退出配对模式
//
//   ★ 为什么不用三击：三击的前两击与双击完全同形，必须先等 700ms
//     判断有没有第三击，于是「双击进配对」要延迟 700ms 才生效，手感发滞；
//     而且连按三下很容易在无意中触发。改成【长按 2 秒】后：
//       · 双击立即进配对，无延迟
//       · 开 WiFi 是明确的「按住不放」，不会误触，也不会与配对混淆
//       · 与 5 秒复位形成 2s / 5s 两档，语义清晰
#define BTN_CLICK_MAX_MS     400UL    // 按下 < 400ms 视为单击
#define BTN_DBL_GAP_MS       350UL    // 两次单击间隔 < 350ms → 双击
#define BTN_LONG_WIFI_MS     2000UL   // 非配对模式长按 2s → WiFi 开关
#define BTN_LONG_PAIR_MS     1500UL   // 配对模式内长按 → 开始配对
#define BTN_LONG_RESET_MS    5000UL   // 非配对模式长按 5s → 系统复位
#define BTN_DEBOUNCE_MS        25UL   // 电平需稳定这么久才承认（消抖）
#define BTN_ARM_DELAY_MS     1500UL   // 开机后这么久内忽略按键

static bool     s_btnDown        = false;
static uint32_t s_btnDownMs      = 0;
static bool     s_pendingClick   = false;   // 已松开但还在等第二次点击
static uint32_t s_pendingClickMs = 0;
static bool     s_longFired      = false;   // 本次长按已处理过
static bool     s_wifiLongFired  = false;   // 2s 的 WiFi 档已触发（5s 复位仍要能触发）

// ── 按键消抖 ──
//   ★ 必须做：BOOT(GPIO9) 是 C3 的 strapping 脚，上电/复位时 USB 握手、
//     以及主机侧 DTR/RTS 翻转都会在它上面产生 10~100ms 的毛刺。
//     实测把串口 DTR/RTS 拉低就让设备"自己"连发了
//     Enter pairing → Select zone 2/3/4 → Exit 的四次单击序列。
//     不加消抖的话，现场只要有人插拔充电线/按复位，就可能误进配对模式。
static bool     s_btnRaw         = false;   // 原始电平
static uint32_t s_btnRawMs       = 0;       // 原始电平最后一次变化时刻
static uint32_t s_bootMs         = 0;       // 开机时刻（setup 里赋值）
static bool     s_btnArmed       = false;   // 已过开机静默期

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

// ── 进入配对模式（双击触发）──
static void enterPairingMode(unsigned long now) {
  pairingMode = true;
  pairingSearchActive = false;          // 先不搜索，等用户选定防区后长按激活
  pairingModeStarted = now;             // ★ 启动配对模式整体空闲计时
  zoneManager.setPairTarget(-1);
  zoneManager.setPairing(false);
  radarDisplay.setPairingMode(true);    // ★ 选中防区显示白框 + 警示底色
  radarDisplay.setPairingFlash(false);
  Serial.printf("[BTN] Enter pairing mode (zone %d selected)\n",
                zoneManager.activeIndex() + 1);
}

// ── 退出配对模式（统一出口：双击 / 配对成功 / 超时 / 复位）──
static void exitPairingMode(const char *why) {
  pairingMode = false;
  pairingSearchActive = false;
  pairingModeStarted = 0;
  zoneManager.setPairTarget(-1);
  zoneManager.setPairing(false);
  radarDisplay.setPairingMode(false);
  radarDisplay.setPairingFlash(false);
  Serial.printf("[BTN] Exit pairing mode (%s)\n", why);
}

// ── 对当前选定防区开始搜索（长按触发）──
static void startPairSearch(unsigned long now) {
  zoneManager.setPairTarget(zoneManager.activeIndex());  // 精确写入本防区
  zoneManager.setPairing(true);
  pairingSearchActive = true;
  pairingSearchStarted = now;
  pairingModeStarted = now;             // ★ 有操作 → 刷新整体空闲计时
  radarDisplay.setPairingMode(true);    // 白框保持
  radarDisplay.setPairingFlash(true);   // 再加闪烁
  Serial.printf("[BTN] Zone %d start pairing search (timeout %lus)\n",
                zoneManager.activeIndex() + 1, PAIR_TIMEOUT_MS / 1000UL);
}

// 非配对模式下的单击动作
static void onClickNormal(unsigned long now) {
  screenOn = !screenOn;
  digitalWrite(TFT_BLK, screenOn ? HIGH : LOW);
  Serial.printf("[BTN] Screen %s\n", screenOn ? "ON" : "OFF");
}

// 配对模式下的单击动作：切到下一个防区
static void onClickInPairing(unsigned long now) {
  int idx = (zoneManager.activeIndex() + 1) % ZONE_MAX;
  zoneManager.setActiveIndex(idx);
  // 换防区 = 放弃当前搜索，回到「已选定、待长按激活」状态
  pairingSearchActive = false;
  pairingModeStarted = now;             // ★ 有操作 → 刷新整体空闲计时
  zoneManager.setPairing(false);
  zoneManager.setPairTarget(idx);
  radarDisplay.setPairingMode(true);    // 仍在配对模式：白框跟到新防区
  radarDisplay.setPairingFlash(false);  // ★ 必须先停闪，否则红闪会跟到新防区
  Serial.printf("[BTN] Select zone %d (long-press to pair)\n", idx + 1);
}

// ── 单击（在双击等待窗结束后才确定）──
static void fireSingleClick(unsigned long now) {
  if (pairingMode) onClickInPairing(now);
  else             onClickNormal(now);
}

// ══════════════════════════════════════════════════════
//  BOOT 键手势处理（非阻塞）
// ══════════════════════════════════════════════════════
void handleBootButton() {
  unsigned long now = millis();

  // ── 开机静默期：GPIO9 是 strapping 脚，上电和 USB 枚举期间会有毛刺 ──
  if (!s_btnArmed) {
    if (now - s_bootMs < BTN_ARM_DELAY_MS) {
      s_btnRaw   = (digitalRead(BOOT_PIN) == LOW);
      s_btnRawMs = now;
      s_btnDown  = false;
      return;
    }
    s_btnArmed = true;
    s_btnRaw   = (digitalRead(BOOT_PIN) == LOW);
    s_btnRawMs = now;
  }

  // ── 消抖：原始电平必须稳定 BTN_DEBOUNCE_MS 才承认 ──
  bool raw = (digitalRead(BOOT_PIN) == LOW);
  if (raw != s_btnRaw) {
    s_btnRaw   = raw;
    s_btnRawMs = now;
    return;                              // 还在抖，本轮不做任何判定
  }
  bool down = s_btnRaw;                  // 下面是"已消抖"后的稳定电平
  if ((now - s_btnRawMs) < BTN_DEBOUNCE_MS) return;

  // ── 双击判定：松开后等 BTN_DBL_GAP_MS，期间无第二次按下 → 确认为单击 ──
  if (!down && s_pendingClick && (now - s_pendingClickMs >= BTN_DBL_GAP_MS)) {
    s_pendingClick = false;
    fireSingleClick(now);
  }

  if (down) {
    if (!s_btnDown) {                    // ── 下降沿 ──
      // 若正好落在双击窗口内 → 这是一次双击
      if (s_pendingClick) {
        s_pendingClick = false;
        if (pairingMode) {
          exitPairingMode("double-click");
        } else {
          enterPairingMode(now);         // ★ 立即进配对，不再等第三击
        }
        s_longFired = true;              // 本次按下不再触发长按
        s_btnDown = true;
        s_btnDownMs = now;
        return;
      }
      s_btnDown = true;
      s_btnDownMs = now;
      s_longFired = false;
      s_wifiLongFired = false;
      return;
    }

    // ── 按住中 ──
    unsigned long held = now - s_btnDownMs;

    if (pairingMode) {
      if (!s_longFired && held >= BTN_LONG_PAIR_MS) {   // 长按 → 开始配对
        s_longFired = true;
        startPairSearch(now);
      }
      return;
    }

    // 非配对模式：2s 开/关 WiFi，5s 复位（两档独立，先把 2s 打完再等 5s）
    if (!s_wifiLongFired && held >= BTN_LONG_WIFI_MS) {
      s_wifiLongFired = true;
      tuya.toggleWifi();
      Serial.printf("[BTN] WiFi %s (long-press 2s)\n", tuya.wifiEnabled() ? "ON" : "OFF");
      // 注意：不 return，也不置 s_longFired —— 继续按住到 5s 仍可复位
    }
    if (!s_longFired && held >= BTN_LONG_RESET_MS) {   // 长按 5s → 系统复位
      s_longFired = true;
      Serial.println("[BTN] Factory reset");
      zoneManager.clearStored();
      zoneManager.clearAll();
      tuya.forgetWifi();                 // ★ 一并清掉 WiFi 凭据，下次开 WiFi 直接进配网
      exitPairingMode("factory-reset");
    }
    return;
  }

  // ── 松开 ──
  if (!s_btnDown) return;
  s_btnDown = false;
  unsigned long held = now - s_btnDownMs;

  if (s_longFired) return;               // 长按/双击已处理过，不再计单击

  if (held < BTN_CLICK_MAX_MS) {
    s_pendingClick = true;               // 可能是单击，也可能是双击的前半
    s_pendingClickMs = now;
  }
  // 介于 400ms 与长按阈值之间：忽略（防误触）
}

// ══════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════
void setup() {
  s_bootMs = millis();          // 按键静默期起点（见 handleBootButton）
  Serial.begin(115200);
  // ★★ USB CDC 发送超时 —— 这一行曾经是「偶然死机」的真凶，改动前务必读完 ★★
  //   【历史】原来写的是 setTxTimeoutMs(0)，本意是「缓冲区满就丢，不等待」。
  //   实际后果是 Arduino core 的 HWCDC::write() 里 `tries--` 发生无符号下溢：
  //     tx_timeout_ms = 0 → tries 一开始就是 0 → 一旦环形缓冲区满、送不出数据，
  //     立刻走 `tries--; delay(1);` → tries 变成 4294967295 →
  //     `if (tries == 0) connected = false;` 这条唯一的逃生门永不成立 →
  //     死循环。loop() 永不返回 → 5s 后 TASK_WDT → panic 重启。
  //   现场「烧录后偶然死机」的核心转储就是这么来的（task_wdt_isr → abort，
  //   而 loopTask 卡在 HWCDC::write 的 delay(1)）。
  //   ⚠️ 只有 USB 插着、但主机没人取数据时才会走进这条阻塞分支，
  //      所以它表现为「偶发」，且拔掉 USB 单独跑时不复现。
  //
  //   【现在】给一个【非 0】的小超时：tries 从 10 开始递减，最多卡 10ms 就把
  //   connected 置 false 退出，之后改走 flushTXBuffer() 的纯非阻塞路径。
  //   最坏情况是每行日志多花 10ms（1Hz 日志 → 占空比 1%），肉眼不可见；
  //   换来的是「无论如何都会退出」，这是必须的安全边界。
  //   ★ 不要再改回 0。配套的容量前置检查在 DebugLog.h 的 dbgLog() 里。
  Serial.setTxTimeoutMs(10);
  delay(500);
  Serial.println("\n=== HEARTBEAT v7 C3 ===");
  // ★ 必须第一行就打印复位原因：
  //   现场说「死机」后重启，靠这一条就能区分
  //   BROWNOUT（供电被拉垮）/ TASK_WDT（主循环被阻塞）/ PANIC（程序异常）。
  //   三者修法完全不同，不能靠猜。
  //   raw = ROM 原始码，用于 esp_reset_reason() 返回 UNKNOWN 时进一步区分：
  //     0x03 软件复位 / 0x0B CPU0 主看门狗 / 0x0F 掉电 / 0x15 USB 串口（烧录后）
  dbgLog("[BOOT] reset=%s raw=0x%02X(%s) heap=%lu\n",
         resetReasonStr(), resetRawCode(), resetRawStr(),
         (unsigned long)ESP.getFreeHeap());

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  pinMode(VIBRATION_PIN, OUTPUT);
  digitalWrite(VIBRATION_PIN, LOW);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(OOK_RX_PIN, INPUT);

  // ★ 硬件 SPI 必须在面板驱动之前初始化，且必须显式传引脚。
  //   SCLK=GPIO6 / MOSI=GPIO7 正是 ESP32-C3 的 SPI2 IO_MUX 专用引脚（零延迟直连）。
  //   SCLK/MOSI 显式指定、MISO 传 -1（本屏只写不读）。
  //   ⚠️ 只用引脚版 begin() 即可：若先调无参 SPI.begin()，C3 变体文件里的默认
  //      SCK 是旧的 GPIO4，会把 SCLK 从 GPIO6 挪走 → 花屏。
  //
  //   ⚠️ C3 的 spiAttachMISO(-1) 会打一条 log_e("SPI Does not have default
  //      pins on ESP32C3!")。本屏是只写设备、MISO 根本不用，这条 [E] 无害，
  //      但成品开机日志里出现 [E] 会让现场误以为故障。
  //   ★ 注意：Arduino HAL 的 log_e 走的是 log_printf()（ARDUHAL_LOG_FORMAT），
  //      **不经过 IDF 日志系统**，所以 esp_log_level_set("spi", ...) 对它无效。
  //      唯一有效的办法是编译期把 ARDUHAL_LOG_LEVEL 设为 0
  //      （见 platformio.ini 的 -DCORE_DEBUG_LEVEL=0），此时所有 log_x 都是空语句。
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

  zoneManager.begin();
  zoneManager.setPairing(false);
  ookReceiver.begin(OOK_RX_PIN);

  // ══════════════════════════════════════════════════════
  //  ★ 开机打印已保存的防区 ID（非常重要，别删）
  // ══════════════════════════════════════════════════════
  //  为什么要专门打这一条：
  //    从机换了芯片 / 换了整块板子之后，STC8 的【出厂唯一 ID 变了】。
  //    主机 EEPROM 里存的还是旧 ID，于是 matchMac() 全部不命中 →
  //    updateFromRadioEx() 返回 OOK_RX_NONE → 防区永不更新 →
  //    lastSeenMs 不刷新 → 10s 后判离线 → 报警反复复位/恢复。
  //
  //    现场表现极具误导性：
  //      · 串口里能看到 [OOK] xx flags=01（因为那行是无条件打印的）
  //      · 屏幕上防区一直闪（正因为判了离线）
  //      · 看起来像「采样慢 / 延时大 / 从机重启」，实际是【没配对】
  //    只看到 [OOK] 行就以为收到了，是最容易踩的坑。
  //    所以必须把「主机认为的 ID」打在启动日志里，和从机实测 ID 直接对照。
  {
    int cnt = zoneManager.count();
    dbgLog("[ZONE] stored=%d\n", cnt);
    for (int i = 0; i < cnt; i++) {
      char idbuf[24];
      ookIdToStr(zoneManager.zoneMac(i), idbuf, sizeof(idbuf));
      dbgLog("[ZONE]  #%d id=%s name=%s en=%d\n",
             i, idbuf, zoneManager.zoneName(i),
             (int)zoneManager.zoneEnabled(i));
    }
    if (cnt == 0) dbgLog("[ZONE] (empty - press PAIR on the slave)\n");
  }

  radarDisplay.begin();   // LOGO 结束 → 直接雷达界面

  // ── 涂鸦云接入：只加载持久化开关 + 起后台任务，绝不阻塞开机 ──
  //   WiFi 默认关（除非上次用三击打开过并被记住），
  //   所以这一步对启动时间与射频初始化零影响。
  tuya.begin();

  // ══════════════════════════════════════════════════════
  //  ★ 主循环看门狗：把「卡死」变成「自动重启 + 留下证据」
  // ══════════════════════════════════════════════════════
  //  Arduino-ESP32 的 loopTask 每轮都是先 esp_task_wdt_reset() 再 loop()，
  //  但 loopTaskWDTEnabled 默认是 false —— 也就是说主循环【从来没被监控】。
  //  结果就是：一旦某处真卡住，画面就永远停在那里，
  //  既不报警也不重启，现场只能拔电。
  //
  //  本工程 SDK 配置：CONFIG_ESP_TASK_WDT=y / TIMEOUT_S=5 / PANIC=y
  //  → 启用后，主循环超过 5 秒没转回来就 panic 重启，
  //    并且下次启动的 [BOOT] reset= 会明确写成 TASK_WDT（raw=0x0B），
  //    直接把「死机」变成可诊断事件，而不是一团迷雾。
  //
  //  ⚠️ 必须在 setup() 的【最后】调用：
  //     setup 里有 showBootLogo(1500) / deepUartScan() 等长阻塞，
  //     提前启用会在开机阶段就触发重启，变成无限重启。
  //     稳态实测 loop_max 只有 13~14ms，离 5 秒有 300 倍余量，
  //     不会误触发。
  enableLoopWDT();
  // ★ enableLoopWDT() 内部失败时只调 log_e，而本工程 CORE_DEBUG_LEVEL=0
  //   → 那句 log_e 被编译成空，失败也静默。所以必须自己回查状态：
  //   esp_task_wdt_status() 返回 ESP_OK 才代表主循环真的被监控上了。
  //   若这里打 FAILED，说明主循环处于【无看门狗】状态，
  //   此时「死机」会表现为画面永久停住且不重启（只能拔电），而不是 panic 重启。
  dbgLog("[BOOT] loop WDT %s (5s)\n",
         esp_task_wdt_status(NULL) == ESP_OK ? "armed" : "FAILED - loop unmonitored!");
  dbgLog("[OK] %s\n", detectedBaud > 0 ? "RUNNING" : "NO DATA");
}

// ══════════════════════════════════════════════════════
//  雷达航迹跟踪：关联 / 平滑 / 短时保持
// ══════════════════════════════════════════════════════
//  ★ 为什么不能直接把 radar->targets[] 拿去画：
//    1) 传感器内部只有 3 个「槽位」，同一个人稍微一动就可能被换到另一个
//       槽位；而屏幕颜色是按槽位取的（1=青 2=黄 3=红），于是同一个人会
//       【毫无理由地换色】，现场看着就是「显示不准」。
//    2) LD2450 输出约 10Hz，屏幕 50Hz：同一次采样被重复画 5 帧，
//       采样与采样之间的跳动被原样保留 → 光点原地发抖。
//    3) 传感器偶尔漏一次（该槽位 x=y=0），直接画就是【闪一下消失】。
//    4) 云端取 tDist[0] 的注释写的是「最近目标」，但原代码根本没排序，
//       取到的是传感器槽位顺序里的第一个 —— 屏幕与云端的「主目标」
//       都可能不是最近的那一个。
//
//  这里用一个 3 槽【航迹池】解决：把本帧采样按极坐标最近邻关联到已有
//  航迹（颜色/编号因此稳定），关联上就做 EMA 平滑，没关联上则在
//  TRACK_HOLD_MS 内保持原值（吃掉漏采），超时才删除。
#define TRACK_SLOTS     3
#define TRACK_HOLD_MS   350UL   // 漏采保持时长：> 1 个传感器周期(100ms)，仍小于人走开的视觉容忍
#define TRACK_EMA       0.45f   // 新值权重：越小越稳但越滞后；0.45 ≈ 0.2s 时间常数
#define TRACK_GATE_M    0.90f   // 关联门限：距离差(米)。人走路 10Hz 位移约 0.15m，留足余量
#define TRACK_GATE_DEG  30.0f   // 关联门限：角度差(度)
#define TRACK_MIN_M     0.15f   // 小于此距离视为传感器噪声（LD2450 近端有效区 0.2m）
#define TRACK_MAX_M     8.0f    // 大于此距离直接丢弃，不让它污染航迹

// ── 推屏强制放行阀值（防止 WiFi 干扰导致门控永久不放行 → 屏幕冻结）──
//   SOFT：不在帧内、但安静窗（≡10ms 无跳变）一直不成立 → 120ms 后放行
//   HARD：连 _inFrame 都长期为真（持续自激）→ 400ms 后硬放行
//   两个值都取得足够大：只有【真的推不上去】才会触发，正常突发不会误触。
#define DEFER_SOFT_MS   120UL
#define DEFER_HARD_MS   400UL

// 强制放行计数（健康日志用；直接量出「门控被 WiFi 干扰卡住多久」）
static uint32_t s_forceSoft = 0;
static uint32_t s_forceHard = 0;

struct RadarTrack {
  bool     active;
  float    dist;     // m，已平滑
  float    angle;    // deg，已平滑
  uint32_t seenMs;   // 最近一次被采样命中的时刻
};
static RadarTrack s_tracks[TRACK_SLOTS];

// 角度差归一化到 (-180, 180]
static inline float angDiffDeg(float a, float b) {
  float d = a - b;
  while (d > 180.0f) d -= 360.0f;
  while (d <= -180.0f) d += 360.0f;
  return d;
}

// 按距离升序做插入排序（最多 3 个元素，插入排序在这里最快也最省栈）
//   ★ 目前只在 trackRadar() 的输出阶段隐式使用（靠 nearestIdx 比较取最小），
//     保留该函数供将来需要「多目标同时排序显示」时直接使用。

// ── 主入口 ──
//   outDist/outAngle 按【航迹槽位】顺序写出，长度固定 TRACK_SLOTS，
//   空槽写 0 —— 保持槽位顺序是为了让屏幕颜色/编号不随其它目标的出现
//   或消失而错位（画面里靠 dist>0 跳过空槽）。
//   返回值 = 存活航迹数；*nearestIdx = 其中最近的那个槽位（无目标时 -1）。
static int trackRadar(unsigned long now, float *outDist, float *outAngle, int *nearestIdx) {
  // ── 0) 取本帧所有在量程内的原始采样 ──
  float rDist[3], rAngle[3];
  int   rn = 0;
  if (radar) {
    for (int i = 0; i < 3; i++) {
      if (!radar->targets[i].valid) continue;
      float dist  = radar->targets[i].distance_m;
      float angle = radar->targets[i].angle_deg;
      if (!(dist > TRACK_MIN_M) || dist > TRACK_MAX_M) continue;
      if (angle < -95.0f || angle > 95.0f) continue;
      rDist[rn] = dist;
      rAngle[rn] = angle;
      rn++;
    }
  }

  // ── 1) 关联：每条已有航迹贪心匹配最近的采样（最多 3×3 次比较）──
  bool usedRaw[3] = {false, false, false};
  for (int t = 0; t < TRACK_SLOTS; t++) {
    if (!s_tracks[t].active) continue;
    int   best = -1;
    float bestScore = 0.0f;
    for (int i = 0; i < rn; i++) {
      if (usedRaw[i]) continue;
      float dd = fabsf(rDist[i] - s_tracks[t].dist);
      float da = fabsf(angDiffDeg(rAngle[i], s_tracks[t].angle));
      if (dd > TRACK_GATE_M || da > TRACK_GATE_DEG) continue;
      float score = dd / TRACK_GATE_M + da / TRACK_GATE_DEG;  // 距离权重更高
      if (best < 0 || score < bestScore) { best = i; bestScore = score; }
    }
    if (best >= 0) {
      usedRaw[best] = true;
      s_tracks[t].dist  += TRACK_EMA * (rDist[best] - s_tracks[t].dist);
      s_tracks[t].angle += TRACK_EMA * angDiffDeg(rAngle[best], s_tracks[t].angle);
      s_tracks[t].seenMs = now;
    }
  }

  // ── 2) 没被关联上的采样 → 占用一个空槽（或已超时的旧槽），不做平滑直接落位 ──
  for (int i = 0; i < rn; i++) {
    if (usedRaw[i]) continue;
    for (int t = 0; t < TRACK_SLOTS; t++) {
      bool stale = s_tracks[t].active &&
                   (long)(now - s_tracks[t].seenMs) > (long)TRACK_HOLD_MS;
      if (!s_tracks[t].active || stale) {
        s_tracks[t].active = true;
        s_tracks[t].dist   = rDist[i];
        s_tracks[t].angle  = rAngle[i];
        s_tracks[t].seenMs = now;
        break;
      }
    }
  }

  // ── 3) 输出：超时航迹删除，存活航迹写回自己的槽位 ──
  int alive = 0;
  *nearestIdx = -1;
  float nearestDist = 0.0f;
  for (int t = 0; t < TRACK_SLOTS; t++) {
    uint32_t age = now - s_tracks[t].seenMs;
    if (s_tracks[t].active && age > TRACK_HOLD_MS) s_tracks[t].active = false;
    if (!s_tracks[t].active) {
      outDist[t]  = 0.0f;
      outAngle[t] = 0.0f;
      continue;
    }
    outDist[t]  = s_tracks[t].dist;
    outAngle[t] = s_tracks[t].angle;
    alive++;
    if (*nearestIdx < 0 || s_tracks[t].dist < nearestDist) {
      nearestDist = s_tracks[t].dist;
      *nearestIdx = t;
    }
  }
  return alive;
}

// ══════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  handleBootButton();
  // ★ 必须重新取样：handleBootButton() 内部会再调一次 millis() 并写入
  //   pairingSearchStarted / pairingModeStarted，那个时刻 >= 本轮循环开始时的 now。
  //   若不刷新，下面 (now - pairingSearchStarted) 会因无符号下溢变成 0xFFFFFFFF，
  //   使 (>= PAIR_TIMEOUT_MS) 恒成立 —— 表现为「刚开始配对就立刻超时退出」，
  //   整个配对窗口形同不存在。
  now = millis();
  ookReceiver.poll();

  // ── 涂鸦：落地 APP 下发的开关请求（内部几个字节的检查，永不阻塞；
  //    真正的 WiFi/TLS/MQTT 全在 tuya 自己的任务里跑）──
  tuya.loop();

  // ★★ 报警状态变化 → 必须【立即】推屏，不受 busy() 让路窗阻挡。
  //   为什么需要这个许可（这是「报警启动慢」的直接原因，已实测定位）：
  //     从机状态变化时连发 3 帧，第 1 帧解出时 busy() 必然为真
  //     （让路窗刚被推到帧尾，还剩 152ms-146ms≈6ms，紧接着 6ms 突发窗）。
  //     若无条件等 busy() 放行，屏幕要等这一整轮突发走完才更新 —— 最长 500ms+。
  //   为什么放行不会丢帧：
  //     这 3 帧内容【完全相同】，第 1 帧已经拿到了全部信息（present 已更新），
  //     后 2 帧纯属冗余。此刻推屏即使把后 2 帧打散也毫无损失。
  //   ★ 与之前被删除的 OOK_REPEAT_WINDOW_MS 例外的本质区别：
  //     那个例外是「解出任何帧后 600ms 内无条件放行」——
  //     它会在【下一次】突发的前导码上推屏，把新的状态帧本身打掉；
  //     而且 600ms 远超帧长，跨过了帧间隙，等于长期压制接收。
  //     本许可只生效【一次】、只在 flags 真的变化时置位，
  //     推完立刻清零，绝不会覆盖下一轮突发。
  OokFrame frame;
  while (ookReceiver.pop(frame)) {
    int rx = zoneManager.updateFromRadioEx(frame);
    if (pairingMode && pairingSearchActive) {
      if (rx == OOK_RX_PAIRED) {
        // ★ 只有「新写入」才算配对完成。仅凭 count 变化会漏判覆盖写，
        //   也会把「更新已登记防区」误判成配对成功。
        zoneManager.save();
        char idbuf[24];
        ookIdToStr(frame.id, idbuf, sizeof(idbuf));
        Serial.printf("[BTN] Pairing complete: %s -> Zone %d\n",
                      idbuf, zoneManager.activeIndex() + 1);
        exitPairingMode("paired");        // 自动停闪 + 退出
      } else if (rx == OOK_RX_KNOWN) {
        Serial.println("[OOK] Already-known device seen during pairing");
      }
    }
    // ★ 逐帧打日志会每秒写 18 次串口（从机连发 2~3 帧）。
    //   USB CDC 在主机不读取时会阻塞，直接造成动画抽帧。
    //   改为「首次见到该设备」或「flags 变化」才打印：
    //   配对/有人/无人这些关键事件一条不少，重复帧不再占串口。
    static uint8_t s_logId[7] = {0};
    static int     s_logFlags = -1;
    static bool    s_logValid = false;
    bool sameId = s_logValid && memcmp(s_logId, frame.id, 7) == 0;
    if (!sameId || (int)frame.flags != s_logFlags) {
      memcpy(s_logId, frame.id, 7);
      s_logFlags = (int)frame.flags;
      s_logValid = true;
      Serial.printf("[OOK] %02X:%02X:%02X:%02X:%02X:%02X:%02X flags=%02X seq=%u%s\n",
                    frame.id[0], frame.id[1], frame.id[2], frame.id[3], frame.id[4], frame.id[5], frame.id[6],
                    frame.flags, frame.seq,
                    (frame.flags & OOK_FLAG_PAIRREQ) ? " PAIRREQ" : "");
    }
  }

  // ★ 一律用有符号差值比较（long），杜绝 unsigned 下溢把「刚发生」判成「早就超时」
  if (pairingMode && pairingSearchActive &&
      (long)(now - pairingSearchStarted) >= (long)PAIR_TIMEOUT_MS) {
    exitPairingMode("timeout");           // 自动停闪 + 退出
  } else if (pairingMode && !pairingSearchActive && pairingModeStarted != 0 &&
             (long)(now - pairingModeStarted) >= (long)PAIRING_IDLE_TIMEOUT_MS) {
    // ★ 配对模式整体空闲超时：选中防区后一直没长按 → 自动退出，
    //   避免「切完防区就卡在配对模式里出不来」。
    exitPairingMode("idle-timeout");
  }

  if (radar) radar->update();

  // ── 防区看门狗 + 报警锁存（全部在 ZoneManager 内部按槽位独立完成）──
  //   ★ 这里只做一件事：调用 tick()，然后把每个槽位自己的状态取出来用。
  //     主循环不再持有任何 s_alarmLatched / s_clearSinceMs 全局数组 ——
  //     那正是过去「一个防区离线把全部防区一起复位」的根源：
  //     两套状态（ZoneManager 的 occupied 和 loop 里的锁存）互相打架，
  //     而锁存又被写成了 4 个全局变量，任何一处判定都会影响整体显示。
  int zOffline = zoneManager.tick();
  if (zOffline > 0) {
    // ★ 只打印【本轮刚刚掉线】的防区（zoneJustOffline），不是所有当前离线防区。
    //   旧写法在 zOffline>0 时遍历所有 zoneOffline(i) 并打印，
    //   于是只要有一台掉线，另一台【早就掉线、一直没恢复】的也会被
    //   重复打印一遍 —— 现场看到 "每 30 秒 #2 又 offline 一次"，
    //   会误判成 #2 在反复掉线，进而去查射频/供电，其实它从来没回来。
    //   现在这条日志严格对应一次 1→0 的下降沿，可以直接数掉线次数。
    for (int i = 0; i < zoneManager.count(); i++) {
      if (zoneManager.zoneJustOffline(i)) {
        Serial.printf("[ZONE] #%d %s offline (no frame for %lus) - only this zone affected\n",
                      i + 1, zoneManager.zoneName(i), ZONE_OFFLINE_MS / 1000UL);
      }
    }
  }

  for (int i = 0; i < 4; i++) {
    zoneFlags[i] = i < zoneManager.count() && zoneManager.zoneOccupied(i);
    zoneEnabled[i] = i < zoneManager.count() && zoneManager.zoneEnabled(i);
  }
  // ★ 第四个参数换成「在线」而不是「有人」：
  //   旧代码把 occupied 直连显示，于是「一直无人」和「从机掉线」在屏幕上
  //   长得一模一样，完全无法区分。现在每防区三态独立：离线 / 无人 / 有人。
  bool zoneOnline[4];
  bool zoneLatched[4];
  for (int i = 0; i < 4; i++) {
    zoneOnline[i]  = zoneManager.zoneOnline(i);
    zoneLatched[i] = zoneManager.zoneAlarmLatched(i);
  }
  // ★ 防区标签固定为 A/B/C/D。
  //   不要用 zoneManager.zoneName()：EEPM 里存的是 "Z1"~"Z4"，
  //   直接拿来显示会出现「A 配完就变成 Z1」的怪异现象。
  static const char *kZoneLetters[4] = {"A", "B", "C", "D"};
  radarDisplay.setZoneLayout(kZoneLetters, zoneEnabled, zoneFlags, zoneManager.activeIndex(), 4);
  radarDisplay.setZoneHealth(zoneOnline, zoneLatched);

  // ── 涂鸦：喂防区状态快照（4bit 掩码写入，纳秒级）+ 屏幕徽标 ──
  //   presence = 原始「有人」（zoneFlags），online = 链路在线。
  //   云端语义：APP 里防区有人/无人 = presence，防区掉线 = online=false。
  tuya.updateZones(zoneFlags, zoneOnline, zoneManager.count());
  radarDisplay.setWifiState((uint8_t)tuya.uiState());

  // ══════════════════════════════════════════════════════
  //  AP 配网二维码：进 AP 那一刻整屏画一次
  // ══════════════════════════════════════════════════════
  //  ★ 为什么只在「刚进 AP」这一次画、而不是每帧画：
  //    showApQr() 是整屏 pushCanvas()，40960 字节连续 SPI ≈ 21ms，
  //    这段时间 433MHz 接收机彻底致盲（下面推屏预算那段有实测数据）。
  //    AP 配网期雷达画面本来也没人看，画一次就够了；屏幕自己会保持，
  //    不需要刷（ST7735 有自己的显存，不刷新不会灭）。
  //  ★ 内容固定为标准 WiFi 入网码（详见 TuyaConfig.h 里那段说明）。
  //    不再提供 http://192.168.4.1 的网址码：微信能打开网址，但扫码那一刻
  //    手机还挂在自家路由器上，192.168.4.1 根本不可达，用户得先手动连热点
  //    再扫 —— 比相机扫多一步，所以这条分支已移除。
  static bool s_apQrShown = false;
  bool apNow = tuya.apMode();
  if (apNow && !s_apQrShown) {
    s_apQrShown = true;                 // 无论成败都置位，免得失败后每帧重试拖死主循环
    char payload[80];
    // 标准 WiFi 二维码格式：T=加密方式，S=SSID，P=密码（开放热点无 P）
    //   末尾两个分号不能少，少了部分扫码器不认。
    //   ★ SSID 里若有 \ ; , : " 需要转义，我们的热点名只含字母数字和 '-'，无需处理。
    //
    // ★★ 载荷必须跟着【实际加密方式】走，不能写死 nopass（这是原来的 bug）：
    //   手机扫到 T:nopass 会按「开放网络、无需密码」去关联；
    //   如果热点实际是 WPA2，关联一定失败，用户看到的就是
    //   「扫了但连不上」。反过来 T:WPA 配开放热点也一样连不上。
    //   所以这里用 TUYA_AP_OPEN 编译期开关把两者绑死，改一处即可。
    //
    // ★ 关于「扫码后只打开 WLAN 设置页、不自动连」（现场反馈）：
    //   这是手机 OS 的策略，不是载荷写错了 ——
    //     · 开放热点：系统会提示「此网络不安全」，必须人工确认一次，
    //       而且很多机型（含 HyperOS/MIUI）检测到「无互联网」后
    //       干脆只把用户丢到 WLAN 列表，让用户自己点。
    //     · 加密热点：系统会把密码预填进连接弹窗，仍是点一次，但
    //       不会因为「不安全」再拦一道，实际少一次犹豫。
    //   两种都要「点一次」，区别只在提示强度。想换加密方式就把
    //   TuyaConfig.h 里的 TUYA_AP_OPEN 改成 0（下面这段会自动跟着变）。
#if TUYA_AP_OPEN
    snprintf(payload, sizeof(payload), "WIFI:T:nopass;S:%s;;", tuya.apSsid());
#else
    snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;",
             tuya.apSsid(), TUYA_AP_PASS);
#endif
    if (radarDisplay.showApQr(payload, tuya.apSsid())) {
      Serial.printf("[QR] AP qr shown: %s\n", payload);
    } else {
      Serial.println("[QR] payload too long / canvas missing, qr skipped");
    }
  } else if (!apNow && s_apQrShown) {
    // 退出 AP：什么都不用做 —— showApQr() 已经把 _prevValid 作废，
    // 下一次能推屏时 pushDirty() 会自动走整屏重绘，一步把二维码擦干净。
    s_apQrShown = false;
    Serial.println("[QR] AP qr cleared, back to radar");
  }

  // ── 震动提示：每个防区「一次报警事件」只震一次 ──
  //   规则（现场要求）：
  //     防区第一次「无人 → 有人」        → 震 VIBRATE_MS
  //     之后一直有人（无论多久）          → 不再震，只有防区闪烁提示
  //     连续 ZONE_CLEAR_HOLD_MS 无人      → 本防区报警复位，解除锁存
  //     复位后再检测到有人                → 算新的一次，再震
  //   ★ 事件的判定已经完全交给 ZoneManager（每槽位独立计数），
  //     这里只比较累计事件数是否变化 —— 主循环不再自己维护锁存数组，
  //     因此任一防区的事件不可能再影响另一防区。
  static uint16_t s_vibSeenEvents = 0;
  static unsigned long s_vibUntil = 0;
  uint16_t events = zoneManager.alarmEvents();
  if (events != s_vibSeenEvents) {
    s_vibSeenEvents = events;
    s_vibUntil = now + VIBRATE_MS;         // 新的一次报警事件 → 震一次
  }

  bool anyLatched = zoneManager.anyLatched();
  digitalWrite(VIBRATION_PIN,
               (anyLatched && (long)(now - s_vibUntil) < 0) ? HIGH : LOW);

  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;

    // ── 取本帧目标（已关联 + 平滑 + 漏采保持，见 trackRadar()）──
    //   ★ 数组按【航迹槽位】排列、长度恒为 3，空槽 dist=0；
    //     槽位顺序稳定 ⇒ 屏幕颜色不会因为别的目标出现/消失而错位。
    float tDist[TRACK_SLOTS], tAngle[TRACK_SLOTS];
    int   nearestIdx  = -1;
    int   targetCount = trackRadar(now, tDist, tAngle, &nearestIdx);

    // ── Radar frame + bottom 4-zone strip ──
    // ★★ 推屏与 433MHz 收帧的互斥关系（实测结论，勿再把预算调回整屏）：
    //   OOK 一帧有 ~201 个边沿、历时 ~146ms，每位宽只有 ~200µs 量级。
    //   SPI 传输期间长时间占用总线会让接收机「瞎」一段时间 —— 不是丢掉
    //   一个边沿那么简单，而是 RMT 硬件录到一段 ~10ms 的平坦电平，
    //   解码时被当成一个超长段，整帧结构随即作废（[XFR] 实测 len=229 却
    //   只有 4 段 / len=223 SHORT）。
    //   实测三组对照：
    //     (a) 收帧期间全禁推屏       → crc=0，ok≈12~16/5s   ✅ 但 maxGapMs≈200
    //     (b) 完全放开推屏           → crc=1~8，ok≈0~2      ❌
    //     (c) 像素预算限流（本次方案） → crc=0，ok≈2/5s      ✅ maxGapMs≈40
    //   关键不是「推多久」，而是「单次连续 SPI 是否长到能塞进一个假段」。
    //   整屏推 = 20480px = 40960 字节，20MHz 下理论 16ms（实测含开销 ~21ms）→ 必然致命；
    //   3000px ≈ 6KB ≈ 2.4ms → 就算撞上帧体也只让 1~2 段略变长，CRC 仍能过。
    //   所以：① SPI 必须 20MHz（40MHz 会比特打翻，crc>0）
    //         ② 单次推屏预算 3000 脏像素
    //         ③ 只在帧间隙 + 距上个边沿 ≥10ms 的安静窗里推

    // ★★ 雷达数据上报给涂鸦 —— 必须在推屏门控【之外】做。
    //   原因：下面 canPush 为假时（收帧中/AP 模式）整块会被跳过，
    //   而雷达数据与推屏完全无关，不该被屏幕的时序拖着走。
    //   之前「主机雷达信息没有推送」的根因就是雷达值只喂了
    //   radarDisplay.updateFrame()，从来没往 tuya 传过。
    //   ★ 取【真正最近的那个目标】：nearestIdx 由 trackRadar() 在并存航迹
    //     里比较得出（旧代码直接取 tDist[0]，而它只是传感器槽位顺序里的
    //     第一个，并非最近）。无目标时传 0 目标，updateRadar() 内部归零。
    tuya.updateRadar(nearestIdx >= 0 ? tDist[nearestIdx] : 0.0f,
                     nearestIdx >= 0 ? tAngle[nearestIdx] : 0.0f,
                     targetCount);

    bool rxBusy = ookReceiver.inFrame() || ookReceiver.runSegs() > 0;
    bool pushOk = ookReceiver.pushSafe();

    // ══════════════════════════════════════════════════
    //  推屏门控 + 【有上限的强制放行】
    // ══════════════════════════════════════════════════
    //  ★ 为什么必须加“上限”：
    //     门控条件是「不在帧内 + 距上个边沿 ≥10ms」。这套条件对「从机每 2s
    //     发一次的标准突发」完全成立，但【一旦 WiFi 打开】，433MHz 超再生
    //     接收模块会被 2.4GHz 频段压得持续自激，边沿一个接一个不断，
    //     _inFrame 长期为真 → canPush 长期为假 → 屏幕【无限期冻结】。
    //     这就是现场反馈的「联网之后主机偶然卡顿」。
    //  ★ 为什么强制放行是安全的（跟以前被删掉的 OOK_REPEAT_WINDOW_MS 不同）：
    //     · 只在【已经持续推不上去】之后才触发，而不是无条件预开一个窗口；
    //     · 第一级强制只在 !rxBusy（绝不在真帧内）时生效，只是打破 10ms
    //       安静窗的约束；第二级即便 rxBusy 也放行，但把像素预算降到 600，
    //       单次 SPI 仅 ~0.5ms，最多把一个假段拉长一丁点，CRC 仍能过；
    //     · 从机每突发发 2~3 帧相同内容，牺牲 1 帧不影响任何功能。
    //  ★ 像素预算分级：强制推屏时用更小的预算 —— 宁可一帧只更新几条行带，
    //    也绝不长时间连续占 SPI（那是 433MHz 接收致盲的根因）。
    static unsigned long s_deferSince = 0;
    // ── 自适应基线预算 ──
    //   逻辑：正常放行 → 慢慢加回去（+20/帧，恢复很快）；
    //         硬强制   → 立刻大砍（-300，下限 1500）。
    //   于是「WiFi 干扰越重 → 单次 SPI 越短 → 对 433 接收机干扰越小 →
    //   门控越容易放行」形成负反馈，画面自动收敛到不冻也不丢帧的状态，
    //   不需要现场调参。上限 3000 就是经过实测的安全值。
    static uint32_t s_budgetBase = 3000UL;

    bool canPush = false;
    uint32_t budget = s_budgetBase;

    if (!rxBusy && pushOk) {
      canPush = true;
      s_deferSince = 0;                    // 正常放行 → 延后计时器清零
      // ★ 恢复速度 +20/帧（50FPS → 约 1.5 秒从下限爬回 3000），
      //   必须夹住上限，否则会慢慢越过 3000 这个实测安全值。
      if (s_budgetBase < 3000UL) {
        s_budgetBase += 20UL;
        if (s_budgetBase > 3000UL) s_budgetBase = 3000UL;
      }
    } else {
      if (s_deferSince == 0) s_deferSince = now;
      unsigned long deferred = now - s_deferSince;
      if (!rxBusy && deferred >= DEFER_SOFT_MS) {
        canPush = true; budget = 1200UL; s_forceSoft++;
        s_deferSince = 0;
      } else if (rxBusy && deferred >= DEFER_HARD_MS) {
        canPush = true; budget = 600UL;  s_forceHard++;
        s_deferSince = 0;
        s_budgetBase = (s_budgetBase > 1800UL) ? (s_budgetBase - 300UL) : 1500UL;
      }
    }
    if (budget > s_budgetBase) budget = s_budgetBase;
    radarDisplay.setPushBudget(budget);

    // ★ AP 模式下屏幕上挂着二维码，绝不能让雷达帧把它盖掉：
    //   一旦 updateFrame() 跑起来就会把画布从 _bg 重绘并把整屏推回去。
    if (canPush && !apNow) {
      radarDisplay.updateFrame(now, tDist, tAngle, TRACK_SLOTS);
      // ★ 不调用 ookReceiver.onPushDone()：RMT 在硬件里已经把边沿录完，
      //   清空捕获只会丢掉真边沿。门控（帧间隙 + 安静窗）已保证推屏不落在帧内。
    }
  }

  // ══════════════════════════════════════════════════════
  //  厂测健康日志 —— 每 60 秒一行（占 CPU < 0.1%）
  // ══════════════════════════════════════════════════════
  //  ★ 为什么成品里还留这一条：
  //    「长时间不关机不变卡」这条要求，只能靠「看堆是否单调下降、
  //     看主循环最坏耗时是否上爬、看有没有 WDT 复位」来证明，
  //    没有这行数据就无从验证，只能靠感觉。
  //  ★ 为什么它不影响流畅：每 60s 才 1 条、约 110 字节，
  //    且 setup 里已把 USB CDC 设为非阻塞发送（缓冲区满直接丢），
  //    主循环绝不会因为打印而卡住。
  //  ★ 判读方法（现场只需看这三个数）：
  //      heap  —— 必须稳定在 20 万以上且不单调下跌（跌到 4 万以下才会出问题）
  //      loop  —— 主循环最坏耗时，必须远小于 WDT 的 5000ms（正常 10~20ms）
  //      up    —— 已连续运行分钟数，用来确认真的没重启过
  static unsigned long s_healthNext = 60000UL;
  static uint32_t s_loopMaxUs = 0;
  static unsigned long s_loopMark = 0;
  {
    // 主循环最坏耗时采样：只做两次减法，成本可忽略
    unsigned long dt = micros() - s_loopMark;
    s_loopMark = micros();
    if (dt > s_loopMaxUs && dt < 1000000UL) s_loopMaxUs = (uint32_t)dt;
  }
  if ((long)(now - s_healthNext) >= 0) {
    s_healthNext = now + 60000UL;
    // ★ rst = 从机重启次数，由 OOK_FLAG_BOOT 确定性统计（不靠 seq 猜）。
    //   rbt = 距上次重启的秒数（-1 表示从未观察到重启）。
    //     nz  = 接收机检出的噪声突发数。如果 nz 很大而 ook 很小，
    //           说明 433 接收模块一直在自激（超再生被干扰/被占空干扰），
    //           帧头根本立不起来 —— 这是射频问题，调固件无用。
    //           如果 nz 很小（近乎只有从机的帧），说明接收机很干净，
    //           那就是信号真的弱（功率/天线/距离），或者从机发得不对。
    //     ovf = RMT 环形缓冲溢出次数。>0 说明主机端边沿都来不及读，
    //           帧是被【主机】丢掉的，跟射频无关。
    //     age = 当前帧龄（距最近一帧成功解码多久），最直观反映丢帧程度。
    //     ★ 判读 rst / rbt：
    //         rst 不涨、rbt 很大或 -1   → 从机运行正常，问题在射频/供电
    //         rst 持续上涨、rbt 很小    → 从机在反复重启（最常见：433 发射
    //                                     瞬间把 Vcc 拉塌导致欠压复位）
    //         rst 涨一次就停            → 只是上电启动，正常
    //     zid = 主机 EEPROM 里 0 号防区【已保存的】ID。
    //           ★ 这一项是排查「配不上对」的决定性证据，别删：
    //           把它和上面 [OOK] 行里从机实际发的 ID 逐个字节对照，
    //           不一致 = 主机根本不认这台从机（换芯片/换板后必然如此），
    //           此时 age 会一路涨到等于开机时长、防区永远离线、
    //           报警反复复位——看起来却像「延时大 / 从机重启」。
    //           和 [OOK] 行直接上下对比，一眼就能确定。
    //   ══════════════════════════════════════════════════
    //   ★★ 防区槽位必须【逐个】打印，绝不能只看 0 号槽。
    //      这是踩过的真实误判：防区实际存在 3 号槽（0/1/2 是空的），
    //      而诊断只取了 zoneAgeMs(0) —— 空槽的 lastSeenMs 恒为 0，
    //      于是 age 天生就等于「开机时长」，看起来像「从来没配对成功」。
    //      据此得出的「ID 不匹配」结论完全是假的，白查了一大圈。
    //      教训：诊断指标的下标必须和真实数据所在位置一致；
    //      多槽结构一律全打印，不要假设 0 号槽就是当前使用的那个。
    dbgLog("[ZONE ] zn=%d act=%d\n", zoneManager.count(), zoneManager.activeIndex());
    // ★ 60 秒窗口翻转必须在打印之前做，这样打印的是【刚过去这一分钟】的收帧数。
    zoneManager.rollFrameWindow();
    for (int zi = 0; zi < zoneManager.count() && zi < 4; zi++) {
      char zidbuf[24];
      ookIdToStr(zoneManager.zoneMac(zi), zidbuf, sizeof(zidbuf));
      // ★ f= 该防区最近 60s 收到的有效帧数，on= 是否在线。
      //   判读方法（多防区链路质量，这才是「通信可靠性」的唯一硬指标）：
      //   ★★ 心跳已从 3000ms 改成 2000ms，判读基准必须跟着改，
      //      否则现场会把「正常」误判成「丢帧明显」。
      //     心跳设计值 = 2000ms/次 × 2 帧 = 60 帧/分钟（无人时）
      //     f ≈ 45~60 → 链路正常
      //     f ≈ 15~30 → 丢帧明显（天线/距离/供电/相位碰撞）
      //     f =  0     → 该防区链路已断（或从未配对），看 nz/age 判断是
      //                  射频问题还是 ID 不匹配问题
      //   ★ 四个防区的 f 相差很大 = 典型的相位碰撞或某台天线差；
      //     四个都低 = 主机侧问题（接收模块/干扰/推屏门控）。
      dbgLog("[ZONE ]  #%d id=%s name=%s en=%d on=%d f=%u age=%lums\n",
             zi, zidbuf, zoneManager.zoneName(zi),
             (int)zoneManager.zoneEnabled(zi),
             (int)zoneManager.zoneOnline(zi),
             (unsigned)zoneManager.zoneFramesWindow(zi),
             (unsigned long)zoneManager.zoneAgeMs(zi));
    }
    // ★ stk / tstk = 两个任务的【栈高水位】（单位：字节，剩余量）。
    //   为什么要看：主循环栈 8KB、tuya 任务栈 TUYA_TASK_STACK=16KB，
    //   而 TLS 握手是栈消耗大户。一旦剩余量掉到几百字节，迟早会踩穿栈，
    //   表现为无规律的 PANIC —— 和看门狗死机长得不一样，必须分开判断。
    //   判读：stk > 1500 且 tstk > 3000 → 安全；
    //         任一 < 500 → 立即加大对应任务的栈（tuya 改 TUYA_TASK_STACK）。
    dbgLog("[HEALTH] up=%lum heap=%lu min=%lu loop=%lums stk=%u tstk=%u ook=%lu crc=%lu nz=%lu ovf=%lu T=%uus rst=%u rbt=%lds wdt=%s did=%s\n",
           now / 60000UL,
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)(s_loopMaxUs / 1000UL),
           (unsigned)uxTaskGetStackHighWaterMark(NULL),
           tuya.taskHandle() ? (unsigned)uxTaskGetStackHighWaterMark(tuya.taskHandle()) : 0u,
           (unsigned long)ookReceiver.okFrames(),
           (unsigned long)ookReceiver.crcErrors(),
           (unsigned long)ookReceiver.noiseBursts(),
           (unsigned long)ookReceiver.overflows(),
           (unsigned)ookReceiver.lastUnitUs(),
           (unsigned)zoneManager.seqDrops(),
           zoneManager.seqDrops() ? (long)(zoneManager.rebootAgeMs() / 1000UL) : -1L,
           esp_task_wdt_status(NULL) == ESP_OK ? "armed" : "FAILED",
           tuya.deviceId());
    // ★ fs/fh = 推屏门控【强制放行】次数（本分钟）。
    //   判读：
    //     fs=fh=0        → 门控从未卡死（WiFi 关闭或干扰很轻），画面一直流畅
    //     fs 有值、fh=0  → 只是安静窗偶尔不满足，柔性强制已足够
    //     fh 持续增长    → 433 接收机被 2.4GHz 压得持续自激（真帧可能也丢），
    //                      表现为「联网后偶发卡顿」；此时应优先查天线/换安装位置。
    dbgLog("[GATE] fs=%lu fh=%lu\n",
           (unsigned long)s_forceSoft, (unsigned long)s_forceHard);
    s_forceSoft = 0;
    s_forceHard = 0;
    // ★ [RADAR] —— 雷达 UART 链路是否真的有数据进来。
    //   这三个数以前是「只写不读」的死计数器（HLK_LD2450.h 里 ++ 了但没人打），
    //   而 README 的排障表却写着「串口看 RAW: 计数」—— 那条日志根本不存在。
    //   现在把它接上，排障表说的那件事就真的能做了：
    //     raw = UART1 上累计收到的字节数
    //     ok  = 校验通过并解析成功的完整帧数
    //     bad = 帧头对上了但帧尾不对的帧数（线路噪声 / 波特率不匹配）
    //   ★ 判读（这是「雷达不出数」的第一个分叉点）：
    //     raw 一直不涨           → 物理层没通：TX/RX 接反、传感器没供电、
    //                              传感器还在启动（LD2450 上电要 ~1s）
    //     raw 在涨、ok 不涨      → 波特率不对（本工程按 256000 探测）或帧被截断
    //     ok 在涨但屏幕上没点     → 已进入算法层：看防区/跟踪门限，与 UART 无关
    dbgLog("[RADAR] ok=%lu bad=%lu raw=%lu\n",
           (unsigned long)radar1.totalFrames,
           (unsigned long)radar1.badFrames,
           (unsigned long)radar1.rawBytes);
    // ★ 解码失败原因直方图 —— 定位「丢帧到底丢在哪道闸门」的决定性数据。
    //   一次 60 秒就能给出确定答案，不必再猜：
    //     F11 占绝大多数  → 位判错了（段长都在合法窗口内，只是长短判反）
    //                        根因是接收机 AGC 在某几个位上把边沿拉偏 ~250us；
    //                        调 OOK_BIT_MIN/MAX 无用（本来就没越界）。
    //     F2/F3/F7/F8 多  → 有段长越界 → 才是窗口问题，需扩 OOK_BIT_MIN/MAX
    //     F5 多           → 两簇分不开 → 该帧形状整体坏，通常是干扰/推屏致盲
    //     F1/F6 多        → 载荷段数不够 → 帧尾被帧间隔吞掉的比例变了
    //     F4 多           → 有效样本太少 → 帧体大面积丢失
    //     F10 多          → 出现等长段 → 极可能是噪声凑出来的假帧
    dbgLog("[FAIL] f1=%u f2=%u f3=%u f4=%u f5=%u f6=%u f7=%u f8=%u f10=%u f11=%u\n",
           (unsigned)ookReceiver.failCode(1),  (unsigned)ookReceiver.failCode(2),
           (unsigned)ookReceiver.failCode(3),  (unsigned)ookReceiver.failCode(4),
           (unsigned)ookReceiver.failCode(5),  (unsigned)ookReceiver.failCode(6),
           (unsigned)ookReceiver.failCode(7),  (unsigned)ookReceiver.failCode(8),
           (unsigned)ookReceiver.failCode(10), (unsigned)ookReceiver.failCode(11));
    // ★ 单比特纠错成效 —— 这一行直接量化「救回了多少」，以及错位是否有规律。
    //   fixed 大 → 纠错在起作用，通信可靠性实际已提升；
    //   wbit    → 出错最集中的比特位。若长期固定在某一位（尤其 79 号末位），
    //             说明该位判定方法本身有系统偏差（末位缺配对段，只能靠全帧
    //             中点门限），应针对该位单独优化，而不是整体调门限。
    {
      int wb = ookReceiver.worstFixBit();
      dbgLog("[FIXB] fixed=%lu wbit=%d wcnt=%u\n",
             (unsigned long)ookReceiver.bitsFixed(),
             wb, (unsigned)(wb >= 0 ? ookReceiver.fixBitCount(wb) : 0));
    }
    s_loopMaxUs = 0;
  }
}
