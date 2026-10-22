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

//  注：314/315M 接收的接线自检（开机扫脚 / 全脚实时监视 / 输入阻抗测试 /
//      原始抓包 / 诊断长按手势）已在量产版中整体移除，
//      如需再次排查请参考 docs/legacy-snapshots/ 中的历史版本。

bool zoneFlags[4] = {false, false, false, false};
bool zoneEnabled[4] = {true, true, true, true};

// ── Objects ──
// ★ 硬件 SPI（原来用软件 SPI 位模拟，整帧 40KB 要 150-300ms → 严重卡顿）
//   ⚠️ 必须先 SPI.begin(显式引脚) 再 initR()，否则驱动内部的无参 begin()
//      会把 SCLK 重映射到 C3 变体默认的 GPIO4 → 必然花屏。
// ★ 为什么是 20MHz 而不是更高：
//   40MHz 下 SPI 瞬变会在 315MHz 模块 DATA 脚上把比特打翻（实测 crc>0）；
//   20MHz 实测 crc=0，代价只是推屏慢一倍 —— 而推屏本身已被预算限到 3000px，
//   单次只花 ~2.4ms，完全够用。不要为了帧率把频率提回去。
#define TFT_SPI_HZ  20000000UL   // 20MHz（40MHz 会把比特打翻，crc>0）

Adafruit_ST7735 display(&SPI, TFT_CS, TFT_DC, TFT_RST);
RadarDisplay radarDisplay(&display);
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

// ── 「同一突发重复帧」时间窗 —— 已废弃，留作历史记录 ──
//   ★★ 这个常量现在【没有被任何代码使用】，但故意保留：
//     曾经用它放行「解出第 1 帧后 600ms 内无条件推屏」，
//     而那正是端到端延迟 2~3 秒的元凶（详见主循环里的说明）。
//     保留定义是为了让后来调参的人看得见这个坑，不要重新引入。
//   ⸻ 原始注释 ⸻
//   从机每次上报连发 2~3 帧，帧间隔 20~59ms，帧长 ~163ms。
//   既然已解出第 1 帧，后面的重复帧即使被整屏 SPI 推送（~10ms）打散，
//   也不丢任何功能信息（内容完全相同，只是 seq 不同）。
//   ⸻ 为什么这个推理是错的 ⸻
//   它对心跳帧成立，对【状态变化帧】是致命的：状态变化那 3 帧是主机
//   得知「有人了」的唯一机会，没有应答重传。被牺牲掉就只能等下一次
//   心跳（3s）。实测证据：11 分钟 ook=209（应约 440，命中率 47.5%），
//   以及逐帧时间戳里 dms=190 / 757 / 3400 三种延迟的分化。
#define OOK_REPEAT_WINDOW_MS 600UL   // （未使用，勿重新引入）

// ── 诊断日志总开关 ──
//   0 = 关闭 1Hz 周期性日志（"T:..." / "[OOK-STAT]"）。
//   事件类日志（配对成功/退出、工厂复位、按键）始终保留 —— 它们很少发生。
//   ★ 周期性日志即使开着也不会再拖慢主循环：setup 里已把 USB CDC
//     设为非阻塞发送（setTxTimeoutMs(0)），缓冲区满就丢，不等待。
#define DBG_LOG 1

// ══════════════════════════════════════════════════════
//  BOOT 按键手势（原「长按进配对」与「长按复位」撞车，已重排）
// ══════════════════════════════════════════════════════
//   非配对模式：  单击 = 开关屏幕   双击 = 进入配对   长按 5s = 系统复位
//   配对模式内：  单击 = 切防区 A→B→C→D
//                 长按 1.5s = 对当前防区开始配对（防区闪烁）
//                 双击 = 退出配对模式
//   配对成功 / 超时 → 自动停止闪烁并退出配对模式
#define BTN_CLICK_MAX_MS     400UL    // 按下 < 400ms 视为单击
#define BTN_DBL_GAP_MS       350UL    // 两次单击间隔 < 350ms → 双击
#define BTN_LONG_PAIR_MS     1500UL   // 配对模式内长按 → 开始配对
#define BTN_LONG_RESET_MS    5000UL   // 非配对模式长按 → 系统复位

static bool     s_btnDown        = false;
static uint32_t s_btnDownMs      = 0;
static bool     s_pendingClick   = false;   // 已松开但还在等第二次点击
static uint32_t s_pendingClickMs = 0;
static bool     s_longFired      = false;   // 本次长按已处理过

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
  bool down = (digitalRead(BOOT_PIN) == LOW);
  unsigned long now = millis();

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
          enterPairingMode(now);
        }
        s_longFired = true;              // 本次按下不再触发长按
        s_btnDown = true;
        s_btnDownMs = now;
        return;
      }
      s_btnDown = true;
      s_btnDownMs = now;
      s_longFired = false;
      return;
    }

    // ── 按住中 ──
    unsigned long held = now - s_btnDownMs;
    if (s_longFired) return;

    if (pairingMode) {
      if (held >= BTN_LONG_PAIR_MS) {     // 长按 → 开始配对
        s_longFired = true;
        startPairSearch(now);
      }
    } else {
      if (held >= BTN_LONG_RESET_MS) {    // 长按 5s → 系统复位
        s_longFired = true;
        Serial.println("[BTN] Factory reset");
        zoneManager.clearStored();
        zoneManager.clearAll();
        exitPairingMode("factory-reset");
      }
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
  Serial.begin(115200);
  // ★ USB CDC 非阻塞发送：
  //   主机没打开串口监视器时，CDC 的 write() 会一直阻塞到默认
  //   tx_timeout（250ms）才丢数据。每秒两条诊断日志就足以让
  //   屏幕出现周期性抽帧 —— 这就是「偶然还是卡顿」的来源之一。
  //   设为 0：缓冲区满立即丢弃，打印永远不会拖住主循环。
  Serial.setTxTimeoutMs(0);
  delay(500);
  Serial.println("\n=== HEARTBEAT v7 C3 ===");
  // ★ 必须第一行就打印复位原因：
  //   现场说「死机」后重启，靠这一条就能区分
  //   BROWNOUT（供电被拉垮）/ TASK_WDT（主循环被阻塞）/ PANIC（程序异常）。
  //   三者修法完全不同，不能靠猜。
  //   raw = ROM 原始码，用于 esp_reset_reason() 返回 UNKNOWN 时进一步区分：
  //     0x03 软件复位 / 0x0B CPU0 主看门狗 / 0x0F 掉电 / 0x15 USB 串口（烧录后）
  dbgLog("[BOOT] reset=%s raw=0x%02X heap=%lu\n",
         resetReasonStr(), resetRawCode(),
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

  radarDisplay.begin();   // LOGO 结束 → 直接雷达界面

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
  dbgLog("[BOOT] loop WDT enabled (5s)\n");
  dbgLog("[OK] %s\n", detectedBaud > 0 ? "RUNNING" : "NO DATA");
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

  // ★★ 报警状态变化 → 必须【立即】推屏，不受 busy() 让路窗阻挡。
  //   为什么需要这个许可（这是「报警启动慢」的直接原因，已实测定位）：
  //     从机状态变化时连发 3 帧，第 1 帧解出时 busy() 必然为真
  //     （让路窗刚被推到帧尾，还剩 170ms-163ms≈7ms，紧接着 6ms 突发窗）。
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

  // ── 防区离线看门狗 ──
  //   从机直接关机 / 断联后不再发帧。没有这一步，occupied 永远是 true，
  //   报警会一直闪到主机重启（现场已复现）。
  int zOffline = zoneManager.tick();
  if (zOffline > 0) {
    Serial.printf("[ZONE] %d zone(s) offline (no frame for %lus) -> alarm cleared\n",
                  zOffline, ZONE_OFFLINE_MS / 1000UL);
  }

  for (int i = 0; i < 4; i++) {
    zoneFlags[i] = i < zoneManager.count() && zoneManager.zoneOccupied(i);
    zoneEnabled[i] = i < zoneManager.count() && zoneManager.zoneEnabled(i);
  }
  // ★ 防区标签固定为 A/B/C/D。
  //   不要用 zoneManager.zoneName()：EEPM 里存的是 "Z1"~"Z4"，
  //   直接拿来显示会出现「A 配完就变成 Z1」的怪异现象。
  static const char *kZoneLetters[4] = {"A", "B", "C", "D"};
  radarDisplay.setZoneLayout(kZoneLetters, zoneEnabled, zoneFlags, zoneManager.activeIndex(), 4);

  // ── 震动提示：仅在「有人」上升沿触发一次，震 VIBRATE_MS 就停 ──
  //   原来写的是「只要有人就常亮」→ 有人后一直震不停。
  static unsigned long s_vibUntil = 0;
  static bool s_vibPrevPresent = false;
  bool presentNow = zoneManager.anyPresent() && !zoneManager.isSilent();
  if (presentNow && !s_vibPrevPresent) s_vibUntil = now + VIBRATE_MS;
  s_vibPrevPresent = presentNow;
  digitalWrite(VIBRATION_PIN, (presentNow && (long)(now - s_vibUntil) < 0) ? HIGH : LOW);

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

    // ── Radar frame + bottom 4-zone strip ──
    // ★★ 推屏与 315MHz 收帧的互斥关系（实测结论，勿再把预算调回整屏）：
    //   OOK 一帧有 ~225 个边沿、历时 ~163ms，每位宽只有 ~200µs 量级。
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
    bool rxBusy = ookReceiver.inFrame() || ookReceiver.runSegs() > 0;
    radarDisplay.setPushBudget(3000UL);

    bool canPush = !rxBusy && ookReceiver.pushSafe();

    if (canPush) {
      radarDisplay.updateFrame(now, tDist, tAngle, targetCount);
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
    dbgLog("[HEALTH] up=%lum heap=%lu min=%lu loop=%lums ook=%lu crc=%lu\n",
           now / 60000UL,
           (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)(s_loopMaxUs / 1000UL),
           (unsigned long)ookReceiver.okFrames(),
           (unsigned long)ookReceiver.crcErrors());
    s_loopMaxUs = 0;
  }
}
