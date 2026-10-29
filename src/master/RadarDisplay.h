/*
 * Radar Display — Military Tactical v7
 *
 * ★ Heartbeat ping rings: a SINGLE thin halo pulses outward from the radar
 *   center (sonar-style ripple — replaces the old rotating sweep line)
 *   v4: 只保留一层弧线 (原先 4 层拖尾太乱), 并放慢扩散节奏 → 看得清
 * ★ Full-frame double-buffer (GFXcanvas16) → flicker-free redraw
 * ★ ECG at bottom only
 * ★ 最高对比度配色 (v4): 纯黑底 + 亮绿 + 纯白高光
 * ★ ESP32-C3 pin-compatible (header only, pins set in .ino)
 *
 * Layout (160×128 landscape):
 *   y=0..96   Semicircle radar (center y=96, R=96)
 *   y=97      Separator line
 *   y=98..127 ECG heartbeat (center y=113, amplitude ±10)
 */

#ifndef RADAR_DISPLAY_H
#define RADAR_DISPLAY_H

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "HLK_LD2450.h"

// ── 图片 LOGO：存在同目录 LogoImage.h 时自动启用 ──
// 由 utils/logo_converter.py 从你自己的 PNG/JPG 生成（自导入替换）
#if defined(__has_include)
#if __has_include("LogoImage.h")
#define LOGO_IMAGE_AVAILABLE 1
#include "LogoImage.h"
#endif
#endif

// ── 配网二维码：存在 qrcode 库（ricmoo/QRCode）时自动启用 ──
//   库没装也不会编译失败，只是退化成一屏纯文字提示。
//   刻意【不】定义 LOCK_VERSION：那样省几百字节 flash，但会把版本锁死，
//   而我们希望「内容短就用小版本（模块更大更易扫）、内容长自动升版本」。
#if defined(__has_include)
#if __has_include("qrcode.h")
#define RADAR_QR_AVAILABLE 1
#include "qrcode.h"
#endif
#endif

// ── 二维码页上的中文提示点阵 ──
//   ★ GFX 自带字体只有 ASCII，屏上要显示「系统相机 / 扫码入网 / 勿用微信」
//     就必须把汉字预渲染成点阵。由 utils/make_qr_text.py 生成，
//     三行合计约 350 字节 —— 比塞一个完整中文字库（几十 KB）划算太多。
//     点阵存的是 1bit 掩膜，颜色在绘制时指定，所以同一份数据能画成
//     金色 / 黑色 / 红色，不用为每种颜色各存一份。
#if defined(__has_include)
#if __has_include("QrTextImage.h")
#include "QrTextImage.h"
#endif
#endif
#ifndef QR_TEXT_IMAGE_AVAILABLE
#define QR_TEXT_IMAGE_AVAILABLE 0
#endif

#define SCREEN_W 160
#define SCREEN_H 128

// ── Semicircle geometry (expanded: no status bar) ──
#define RADAR_CX  80
#define RADAR_CY  96
#define RADAR_R   96   // 恢复完整大半圆，保持前一版显示完整

#define RING_2M  (int)(RADAR_R * 2.0 / 6.0)
#define RING_4M  (int)(RADAR_R * 4.0 / 6.0)
#define RING_6M  RADAR_R

// ── Ping-ring (heartbeat ripple) tuning ──
// SLOTS=1 → 屏上永远只有一条波纹, 干净不杂乱。
//   慢版 (LIFE=3000) 像卡住；快版 (LIFE=1400) 又太急促 → 这里取中间值。
#define PING_SLOTS    1
#define PING_LIFE_MS  2000  // 一条波纹从中心扩散到边缘的时长
#define PING_IDLE_MS  1200  // 静默时的生成间隔
#define PING_BEAT_MS  1000  // 检测到目标时的生成间隔

// ★ 波纹抬起半径：波前刚生成时半径很小，drawArc(5) 画出来就是
//   基点上的一个小半圆，且每秒重新生成一次 → 看着就是「基点总在抖」。
//   半径小于此值不画，并在之后 PING_FADE_R 像素内渐显，
//   波纹就是一条从中心平稳推开的干净弧线。
#define PING_MIN_R    12
#define PING_FADE_R   12

// ── 脏矩形：每行最多登记多少个变化段 ──
//   波纹外移时同一行会同时出现「旧弧左/新弧左/新弧右/旧弧右」共 4 段，
//   再加上目标小框、防区条文字，留 6 段余量。
#define PUSH_MAXSP    6

// ═══════════════════════════════════════════════════
//  最高对比度配色 (v4) — 纯黑底 + 亮绿 + 纯白高光
//  原方案绿色过暗 (0x0108 = G4, 0x0120 = G9), 强光下几乎看不见。
//  这里整体提亮并按重要性分级:
//    纯白 (0xFFFF)  = 中心十字 / HUD / 分隔线 / ECG 峰值  ← 最强对比
//    满绿 (0x07E0)  = 6M 边界 / ECG 波形 / 刻度数字
//    亮绿 (0x05E0)  = 2M / 4M 刻度圈
//    中绿 (0x03E0)  = 方位网格线
//    暗绿 (0x0320)  = 扇区边 / ECG 基线 (结构辅助, 刻意压低)
// ═══════════════════════════════════════════════════
#define COL_BG          0x0000   // 纯黑底
#define COL_RING        0x05E0   // 2M / 4M 刻度圈 (原 0x0108, G8 → G47)
#define COL_RING_KEY    0x07E0   // 6M 最外边界 (满亮绿)
#define COL_GRID_LINE   0x03E0   // 方位网格线 (G31)
#define COL_CONE        0x0320   // ±60° 扇区边 (G25, 结构辅助)
#define COL_CROSS       0xFFFF   // 中心十字 (纯白, 最高对比)
#define COL_SWEEP       0x07E0
#define COL_SWEEP_MID   0x04E0
#define COL_SWEEP_DIM   0x02C0
#define COL_SWEEP_FAINT 0x0180
#define COL_SWEEP_GHOST 0x00C0
#define COL_TARGET_1    0x07FF   // Cyan
#define COL_TARGET_2    0xFFE0   // Amber
#define COL_TARGET_3    0xF800   // Red
#define COL_HUD         0xFFFF   // HUD 文字 / 分隔线 (纯白)
#define COL_TEXT_DIM    0x07E0   // 刻度数字 (原 0x0180 过暗, 直接拉满绿)
#define COL_ALERT       0xF800
#define COL_ECG_BASE    0x0320   // ECG 基线 (原 0x0120)
#define COL_ECG_LINE    0x07E0   // ECG 波形 (满亮绿)
#define COL_ECG_PEAK    0xFFFF   // ECG 峰值 (纯白)

// ── 二维码页的强调色 ──
//   屏是白底的，所以这里要挑在白色上够刺眼的深色，
//   而不是雷达页那种在黑底上用的亮色。
//   RGB565 换算：R=5bit(G>>3)<<11, G=6bit(G>>2)<<5, B=5bit(B>>3)
//     #FFD800 亮金 → R31,G54,B0 → 0xF800 | 0x06C0 = 0xFEC0
//     #E60012 正红 → R28,G0,B2  → 0xE000 | 0x0000 | 0x0002 = 0xE002
#define COL_QR_GOLD     0xFEC0   // 亮金 (强调「系统相机」)
#define COL_QR_RED      0xE002   // 正红 (警告「勿用微信」)

#define MAX_RANGE_M 6.0

// ═══════════════════════════════════════════════════
//  开机 LOGO 时长（毫秒）
// ═══════════════════════════════════════════════════
#ifndef LOGO_HOLD_MS
#define LOGO_HOLD_MS 1500UL
#endif

class RadarDisplay {
public:
  RadarDisplay(Adafruit_ST7735 *d)
      : _d(d), _cv(SCREEN_W, SCREEN_H), _lastPing(0), _zoneCount(4), _activeZone(0),
        _pairingFrame(false), _pairingFlash(false), _pairingFlashStart(0) {
    for (int i = 0; i < 4; i++) {
      _zoneNames[i] = (i == 0) ? "A" : (i == 1) ? "B" : (i == 2) ? "C" : "D";
      _zoneEnabled[i] = true;
      _zoneOccupied[i] = false;
      // 上电默认当作「在线」，避免开机瞬间四格全灰；
      // 真正的在线状态由 zoneManager.tick() 在首个心跳内校正。
      _zoneOnline[i] = true;
      _zoneLatched[i] = false;
    }
    for (int i = 0; i < PING_SLOTS; i++) {
      _pings[i].born = 0;
      _pings[i].active = false;
    }
  }

  void begin() {
    if (_cv.getBuffer() == nullptr) {  // allocation failed — degrade gracefully
      _d->fillScreen(COL_BG);
      return;
    }
    // ★ 脏矩形比较用的「上一帧」缓存：只分配一次，永不释放 → 长时间运行不产生堆碎片。
    //   分配失败则自动退回整屏推送（功能不受影响，只是慢一点）。
    if (_prev == nullptr) {
      _prev = (uint16_t *)malloc((size_t)SCREEN_W * SCREEN_H * 2);
    }
    // ★ 静态背景（黑底 + 圆环 + 角线 + 量程标签）预烘焙一次。
    //   原来每帧重做 fillScreen(129µs) + drawGrid(783µs) ≈ 912µs，
    //   占整帧合成时间(1.5ms) 的六成 —— 脏矩形做好之后它就成了新瓶颈。
    //   现在每帧只需一次 40KB memcpy（约 60µs），省下约 850µs/帧。
    if (_bg == nullptr) {
      _bg = (uint16_t *)malloc((size_t)SCREEN_W * SCREEN_H * 2);
    }
    if (_bg != nullptr) {
      _cv.fillScreen(COL_BG);
      drawGrid();
      memcpy(_bg, _cv.getBuffer(), (size_t)SCREEN_W * SCREEN_H * 2);
      _bgValid = true;
    }
    _prevValid = false;
    updateFrame(millis(), nullptr, nullptr, 0);
  }

  // ── 开机 LOGO ──
  // ★ 用与雷达帧完全相同的「整帧画布 + 一次 writePixels」路径推送，
  //    不用 v9 那种「分行 setAddrWindow + 32px 分片」（窗口尺寸不匹配会溢出 → 整屏花）。
  // 有 LogoImage.h → 贴你的图；没有 → 画个简易矢量标识。仅开机调一次。
  void showBootLogo(unsigned long holdMs = LOGO_HOLD_MS) {
#if LOGO_IMAGE_AVAILABLE
    int dw = (LOGO_W > SCREEN_W) ? SCREEN_W : LOGO_W;
    int dh = (LOGO_H > SCREEN_H) ? SCREEN_H : LOGO_H;
    int ox = (SCREEN_W - dw) / 2;
    int oy = (SCREEN_H - dh) / 2;
    _cv.fillScreen(COL_BG);
    for (int y = 0; y < dh; y++) {
      for (int x = 0; x < dw; x++) {
        _cv.drawPixel(ox + x, oy + y,
                      pgm_read_word(&LOGO_BITMAP[(size_t)y * LOGO_W + x]));
      }
    }
    pushCanvas();
#else
    _cv.fillScreen(COL_BG);
    _cv.fillCircle(SCREEN_W / 2, SCREEN_H / 2 - 10, 26, COL_RING_KEY);
    _cv.drawCircle(SCREEN_W / 2, SCREEN_H / 2 - 10, 18, COL_BG);
    _cv.setTextColor(COL_HUD);
    _cv.setTextSize(1);
    _cv.setCursor(26, 88);
    _cv.print(F("KANCHUAN INDUSTRY"));
    pushCanvas();
#endif
    delay(holdMs);
  }

  // ══════════════════════════════════════════════════════════════
  //  AP 配网二维码（整屏覆盖）
  // ──────────────────────────────────────────────────────────────
  //  payload: 二维码内容
  //     "WIFI:T:nopass;S:Kanchuan-HB-xxxx;;"  → 系统相机扫，一键入网
  //     "http://192.168.4.1"                 → 微信/浏览器扫，打开配网页
  //  label:   屏幕顶部那行字（一般给热点名），让用户不扫码也能手动连
  //
  //  ★★ 这是【整屏 pushCanvas()】：20480 px = 40960 字节，20MHz 下约 21ms
  //     连续 SPI —— 期间 433MHz 接收机完全致盲（见 master.ino 里那段实测注释）。
  //     所以只能在 AP 配网期调用：那时雷达画面本来就停更、也没有现场报警要收。
  //     绝不能塞进正常帧，也不要每帧调。
  //
  //  ★ 画完把 _prevValid 作废：画面已经从「雷达」变成「二维码」，
  //     若还拿旧 _prev 做脏矩形比较，退出 AP 时要分好几帧、每次 3000px
  //     预算才能把屏幕擦回雷达图 —— 中途会看到残缺的二维码残影。
  //     作废后下一帧直接整屏推一次，一步到位。
  //
  //  返回 false = payload 太长（超过 v4 容量）或画布没分配成功，画面未改动。
  bool showApQr(const char *payload, const char *label) {
    if (_cv.getBuffer() == nullptr) return false;

    const uint16_t QR_LIGHT = 0xFFFF;   // 底色（白）
    const uint16_t QR_DARK  = 0x0000;   // 码点（黑）

#if !RADAR_QR_AVAILABLE
    // 库没装：退化成一屏文字，至少还能照着读写热点名和地址
    _cv.fillScreen(QR_DARK);
    _cv.setTextColor(QR_LIGHT);
    _cv.setTextSize(1);
    _cv.setCursor(4, 30); _cv.print(F("AP: "));
    if (label) _cv.print(label);
    _cv.setCursor(4, 46); _cv.print(F("open, no password"));
    _cv.setCursor(4, 62); _cv.print(F("http://192.168.4.1"));
    _cv.setCursor(4, 90); _cv.print(F("QR lib not installed"));
    pushCanvas();
    _prevValid = false;
    return true;
#else
    // ── 选最小能装下 payload 的版本（1..4）──
    //   ★ qrcode.c 里明写着一句「@TODO: Return error if data is too big」——
    //     也就是说【内容超长时它不返回错误，会直接越界写调用者的缓冲区】。
    //     所以必须自己先用容量表卡住，绝不能靠 initText 的返回值来判断。
    //     下表是 ECC_LOW / BYTE 模式的容量，取最保守的一档
    //     （内容若恰好全是数字/大写字母，库会自动换更省的模式，能装更多；
    //      这里按 BYTE 算就一定不会越界）。
    //     v1=21×21, v2=25×25, v3=29×29, v4=33×33 模块。
    static const uint8_t kByteCap[5] = {0, 17, 32, 53, 78};
    const int len = (int)strlen(payload);
    uint8_t ver = 0;
    for (uint8_t v = 1; v <= 4; v++) {
      if (len <= kByteCap[v]) { ver = v; break; }
    }
    if (ver == 0) return false;         // 超过 v4 容量，别动屏幕

    QRCode qr;
    if (qrcode_initText(&qr, _qrBuf, ver, ECC_LOW, payload) != 0) return false;

    const int mods = (int)qr.size;      // 4*version + 17
    const int LINE = 8;                 // setTextSize(1) 的行高
    const int CW   = 6;                 // setTextSize(1) 的字宽

    // ── 缩放 ──
    //   ★ 静区【不再算进 qrPx】。整屏白底本身就是静区，黑区外面那一圈白
    //     远比规范要求的宽，所以只需保证「文字不贴到黑区边上」。
    //   ★ 上下仍给足 4 个模块的纯白：扫码器靠左上/右上/左下三个定位图案找码，
    //     文字若压进静区会被当成数据模块，直接扫不出。
    //   ★ 横向同样留 4 个模块，右栏再放 48px 宽的汉字 —— 正好放得下，
    //     所以不需要像最初设想的那样把静区压到 3 个模块。
    int scale = 0;
    for (int s = 4; s >= 1; s--) {
      int px = mods * s;
      //   纵向：黑区 + 上下各 4 模块静区 + 底部 1 行热点名
      //   横向：黑区 + 左右各 4 模块静区 + 右栏（48px，四个 12px 汉字）
      if (px + 2 * (s * 4) + LINE <= SCREEN_H &&
          px + 2 * (s * 4) + 48 <= SCREEN_W) {
        scale = s;
        break;
      }
    }
    if (scale == 0) return false;

    const int qrPx = mods * scale;
    const int qzY  = scale * 4;         // 上下静区（规范 4 模块）
    const int qzX  = scale * 4;         // 左右静区（同样 4 模块）

    // ── 排布：二维码【靠左】，文字全部集中在右侧一栏 ──
    //   用户反馈「二维码不用放中心，文字说明放在同一侧」——
    //   左右各挂一串竖排字确实乱，集中到一侧后视觉重心清楚得多。
    const int ox = qzX;                          // 左边距 = 左静区
    const int oy = (SCREEN_H - qrPx) / 2;        // 垂直居中
    const int tx0 = ox + qrPx + qzX;             // 右栏起点（= 右静区之后）

    // ── 画：白底 + 黑模块（扫码器只认「浅底深点」，反色是扫不出的）──
    _cv.fillScreen(QR_LIGHT);
    for (int y = 0; y < mods; y++) {
      for (int x = 0; x < mods; x++) {
        if (!qrcode_getModule(&qr, (uint8_t)x, (uint8_t)y)) continue;
        _cv.fillRect(ox + x * scale, oy + y * scale, scale, scale, QR_DARK);
      }
    }

    // ── 右栏中文提示（点阵，由 utils/make_qr_text.py 预渲染）──
    //   文字整体在右栏内【垂直居中】，和二维码的中线对齐。
    //   三行按重要度给不同颜色：
    //     系统相机 = 金色  ← 用户要求「彩色字体」+ 强调正确的扫法
    //     扫码入网 = 黑色  ← 正向说明，正常正文
    //     勿用微信 = 红色  ← 反面警告，用最刺眼的颜色
#if QR_TEXT_IMAGE_AVAILABLE
    {
      const int gap = 6;                          // 行间距
      const int h1 = QRT_HINT1_H, h2 = QRT_HINT2_H, h3 = QRT_HINT3_H;
      const int total = h1 + h2 + h3 + gap * 2;
      int ty = oy + (qrPx - total) / 2;
      if (ty < 0) ty = 0;

      drawMask(tx0, ty, QRT_HINT1, QRT_HINT1_W, QRT_HINT1_H, COL_QR_GOLD);
      ty += h1 + gap;
      drawMask(tx0, ty, QRT_HINT2, QRT_HINT2_W, QRT_HINT2_H, QR_DARK);
      ty += h2 + gap;
      drawMask(tx0, ty, QRT_HINT3, QRT_HINT3_W, QRT_HINT3_H, COL_QR_RED);
    }
#endif

    // ── 底部：WiFi 名（带「WiFi:」前缀，居中）──
    //   用户要求删掉「OPEN NO PASSWORD」那行 —— 开放热点本来就不用密码，
    //   写出来反而让人以为要先输点什么，纯属干扰。
    //   ★ 但热点名必须保留并加上 "WiFi: " 前缀：
    //     不扫码时用户要照着去手机 WLAN 列表里手动找这个名，
    //     光秃秃一串 "Kanchuan-HB-1B60" 看不出这是要搜索的 WiFi 名，
    //     容易被当成设备型号或序列号。
    //   ★ 宽度核算：GFX 默认字体 6×8，只有 ASCII，不能写中文。
    //     "WiFi: Kanchuan-HB-1B60" = 22 字符 × 6px = 132px ≤ 160px ✓
    //     （最长前缀 TUYA_AP_SSID_PREFIX 是 "Kanchuan-HB-"，加 4 位 MAC
    //       共 16 字符；再留 6 字符余量给将来改前缀，仍不会溢出。）
    {
      char line[40];
      snprintf(line, sizeof(line), "WiFi: %s", (label && label[0]) ? label : "?");
      int tw = (int)strlen(line) * CW;
      int bx = (SCREEN_W - tw) / 2;
      if (bx < 0) bx = 0;
      // 二维码下方的静区之外才画，避免压进静区
      int by = oy + qrPx + qzY;
      if (by > SCREEN_H - LINE) by = SCREEN_H - LINE;
      _cv.setTextColor(QR_DARK, QR_LIGHT);
      _cv.setTextSize(1);
      _cv.setCursor(bx, by);
      _cv.print(line);
    }

    pushCanvas();
    _prevValid = false;   // 下一帧普通画面整屏重绘
    return true;
#endif
  }

  void setZoneLayout(const char *names[4], const bool *enabled, const bool *occupied,
                    int activeZone, int zoneCount = 4) {
    _zoneCount = (zoneCount < 0) ? 0 : ((zoneCount > 4) ? 4 : zoneCount);
    _activeZone = activeZone;
    for (int i = 0; i < 4; i++) {
      _zoneNames[i] = names && names[i] ? names[i] : ((i == 0) ? "A" : (i == 1) ? "B" : (i == 2) ? "C" : "D");
      _zoneEnabled[i] = enabled ? enabled[i] : true;
      _zoneOccupied[i] = occupied ? occupied[i] : false;
    }
  }

  // ── 每防区独立健康状态（在线 / 报警锁存）──
  //   ★ 为什么必须单独传这一层：
  //     旧界面只拿到 occupied，于是一个「一直无人」的防区和一台
  //     「从机掉线」的防区显示完全一样，现场无法区分「没人」和「坏了」。
  //     现在三态独立呈现：离线（灰）/ 无人（暗绿）/ 有人（亮绿闪烁）。
  void setZoneHealth(const bool *online, const bool *latched) {
    for (int i = 0; i < 4; i++) {
      _zoneOnline[i]  = online  ? online[i]  : true;
      _zoneLatched[i] = latched ? latched[i] : false;
    }
  }

  // ── 推屏像素预算 ──
  //   ★ 为什么需要「预算」而不是简单的「推/不推」：
  //     SPI 传输期间中断被关闭，OOK 边沿会在这段时间彻底丢失。
  //     实测单次整屏推屏最长 13ms，而 OOK_FRAME_GAP_US = 12000：
  //     一旦这段中断关闭时间超过 12ms，ISR 回来的第一个边沿会被判成
  //     「帧间隔」→ _head 清零 → 整帧作废（实测 crc 从 0 涨到 3）。
  //     但脏矩形推送只有 1~3ms，远低于 12ms，收帧期间推它是安全的。
  //   所以收帧期间不是「完全不推」，而是「只推得起小脏块」。
  //   0xFFFFFFFF = 无限制（空闲时的默认值）。
  //
  //   ★ 预算的单位是「实际推送的像素数」（矩形宽×高之和），不是脏像素数：
  //     pushDirty() 会把脏行合并成矩形，矩形内未变化的像素同样要占用 SPI 时间。
  //     超预算时本帧推满预算即停手，剩余部分留到下一帧 —— 绝不整帧丢弃，
  //     否则 _prev 不更新会让差异只增不减，画面永久冻结（光环卡死的老 bug）。
  void setPushBudget(uint32_t px) { _pushBudgetPx = px; }
  // ★ 必须与「正在搜索」(setPairingFlash)分开：
  //   进配对模式只是「已选中某防区、待长按」，此时应给出清晰但静态的提示；
  //   长按后才开始闪烁。原来只有一个 _pairingFlash，导致平时看不出区别。
  void setPairingMode(bool enabled) { _pairingFrame = enabled; }
  bool pairingMode() const { return _pairingFrame; }

  // ── 正在搜索（长按后 → 防区闪烁）──
  void setPairingFlash(bool enabled) {
    _pairingFlash = enabled;
    if (enabled) _pairingFlashStart = millis();
  }
  bool pairingFlash() const { return _pairingFlash; }

  // ── WiFi/云状态徽标（右上角）──
  //   state 取值见 TuyaLink.h 的 TuyaUiState：
  //   0=关 1=连WiFi/对时 2=连云 3=在线 4=失败背退
  //   每帧整画布本来就从 _bg 重绘，徽标跟随重画；
  //   未变化时脏矩形 diff 不产生推送 → 零额外 SPI 开销。
  void setWifiState(uint8_t state) {
    if (_wifiState == state) return;
    _wifiState = state;      // 变了也只是下一帧多推那十几个像素
  }
  uint8_t wifiState() const { return _wifiState; }

  // Compose one full frame into the canvas, then push it to the TFT
  void updateFrame(unsigned long ms, const float *dist, const float *angle, int count) {
    if (_cv.getBuffer() == nullptr) return;

    // ── Spawn a new ping ring on cadence (faster while tracking targets) ──
    //   ★ _lastPing 必须在「真的生成了」之后才重置。
    //     原先一到间隔就重置，哪怕槽位还被上一条波占着、根本没生成 ——
    //     节拍基准被白白推走，波纹周期变成 2.4s 且与显示节奏错拍，
    //     看着就是「波纹总在抖」。现在节拍只由真实生成事件推进。
    unsigned long interval = (count > 0) ? PING_BEAT_MS : PING_IDLE_MS;
    if (ms - _lastPing >= interval) {
      for (int i = 0; i < PING_SLOTS; i++) {
        if (!_pings[i].active) {
          _pings[i].born = ms;
          _pings[i].active = true;
          _lastPing = ms;
          break;
        }
      }
    }

    // ── Compose the frame in RAM ──
    //   ★ 背景（黑底 + 圆环 + 角线 + 量程标签）是静态的，直接 memcpy 预烘焙
    //     好的 _bg（约 60µs），不再每帧 fillScreen + drawGrid（约 912µs）。
    if (_bgValid) {
      memcpy(_cv.getBuffer(), _bg, (size_t)SCREEN_W * SCREEN_H * 2);
    } else {
      _cv.fillScreen(COL_BG);
      drawGrid();
    }
    // ★ 原在此处画「源点闪光」：fillCircle(RADAR_CX, RADAR_CY - 2, 3, 0xDFFF)。
    //   它由外层 newPing 标志驱动，而 newPing 在「间隔到点、但波纹槽仍被
    //   占用」时同样为真 —— 那一刻其实并没有新波生成。结果这个亮点按
    //   间隔节奏闪，波纹却按自己的生命周期重启，两者不同步，
    //   看上去就是「基点老是在抖」。已移除：波纹本身就是源点提示。

    // Expanding ping rings (光环由内向外扩散)
    for (int i = 0; i < PING_SLOTS; i++) {
      if (!_pings[i].active) continue;
      float p = (ms - _pings[i].born) / (float)PING_LIFE_MS;
      if (p >= 1.0f) { _pings[i].active = false; continue; }
      if (p > 0.93f) continue;                // fades out before reaching the edge
      float rEdge = 5.0f + p * ((float)RADAR_R + 4.0f - 5.0f);
      // ★ 单层波纹: 只画波前这一条弧 (原先 k=0..3 的 4 层拖尾太乱)
      //   且必须等它离开基点（PING_MIN_R）才开始画，否则波前就是
      //   基点上的一个小半圆；叠加 PING_FADE_R 像素的渐显，
      //   避免在半径 12px 处「啡」地整条冒出。
      if (rEdge >= PING_MIN_R) {
        uint16_t c = pingColor(p, 0);
        float f = (rEdge - PING_MIN_R) / (float)PING_FADE_R;
        if (f < 1.0f) c = blend565(COL_BG, c, f);
        if (c) drawArc((int)(rEdge + 0.5f), c);
      }
    }

    // Targets
    for (int i = 0; i < count; i++) {
      if (dist[i] > 0.0f) plotTarget(dist[i], angle[i], i, ms);
    }

    drawZoneStrip();
    drawWifiBadge();

    // ── Push canvas → TFT ──
    //   ★ 只推送「相对上一帧真正变化了的行带」：典型一帧只有 10~30 行在动
    //     （波纹前缘 1~2 行 + 上次波纹位置 + 目标小框 + 防区条），
    //     推屏从 11.3ms(整屏 40KB) 降到 ~1.5~3ms → 帧率与手感明显更顺。
    //     画布仍是「整帧重绘」，所以画面永远自洽，不会残留旧像素。
    pushDirty();
  }


private:
  Adafruit_ST7735 *_d;
  GFXcanvas16     _cv;           // offscreen frame buffer (flicker-free)
  uint16_t       *_prev = nullptr;  // 上一帧画布（脏矩形比较用），一次性分配
  bool            _prevValid = false;
  uint16_t       *_bg = nullptr;    // 静态背景预烘焙（圆环/角线/标签），一次性分配
  bool            _bgValid = false;
  // QR 编码缓冲区（放成员里而不是栈上：随对象一起进 .bss，
  //   loop 任务栈只有 8KB，不宜再吃 200+ 字节的局部大数组）。
  //   v4 需要 ((33*33)+7)/8 = 137 字节，留 240 足够。
  uint8_t         _qrBuf[240];
  struct Ping { unsigned long born; bool active; };
  Ping  _pings[PING_SLOTS];
  unsigned long _lastPing;
  const char *_zoneNames[4];
  bool _zoneEnabled[4];
  bool _zoneOccupied[4];
  bool _zoneOnline[4];          // 该防区最近是否收得到从机帧（离线灰显）
  bool _zoneLatched[4];         // 该防区独立的报警锁存（诊断/后续扩展用）
  bool _pairingFrame;           // 处于配对模式（静态白框提示）
  bool _pairingFlash;           // 
  uint8_t _wifiState = 0;       // WiFi/云状态徽标（TuyaUiState）正在搜索（闪烁）
  bool    _wifiDrawn = false;   // 上一帧是否画过徽标（用于 1→0 时擦除一次）
  unsigned long _pairingFlashStart;
  uint32_t _pushBudgetPx = 0xFFFFFFFFUL;   // 单帧允许推送的最大脏像素数
  int _zoneCount;
  int _activeZone;

  // 唯一允许的推屏路径：一次开窗 + 一次连续 writePixels（已被实测证明零花屏）
  void pushCanvas() {
    _d->startWrite();
    _d->setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
    _d->writePixels(_cv.getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
    _d->endWrite();
  }

  // ── 把一个 1bit 掩膜点阵画到画布上，指定颜色 ──────────────
  //   mask:   PROGMEM 里的点阵，MSB first（与 utils/make_qr_text.py 一致）
  //   w,h:    点阵尺寸
  //   color:  RGB565
  //   ★ 逐像素 fillRect(1,1) 在 53x18 = 954 像素的规模下完全够快，
  //     而且这里是整屏一次性绘制（AP 配网期，不进主循环），
  //     没必要为它写更复杂的行合并逻辑。
  //   ★ 只画「有墨」的像素，底色（白）由调用方的 fillScreen 提供，
  //     这样反白/彩色切换都不用重画背景。
  void drawMask(int x, int y, const uint8_t *mask, int w, int h, uint16_t color) {
    for (int yy = 0; yy < h; yy++) {
      for (int xx = 0; xx < w; xx++) {
        int idx = yy * w + xx;
        if (pgm_read_byte(&mask[idx >> 3]) & (0x80 >> (idx & 7))) {
          _cv.fillRect(x + xx, y + yy, 1, 1, color);
        }
      }
    }
  }

  // ── 推送一个「脏段列表」：把相邻行的段合并成尽量贴合的矩形 ──────
  //   x0[y]/x1[y] 是第 y 行该段的起止列，has[y] 表示该行有这一段。
  //   ★ 每推出一块就立即同步该块的 _prev，所以「推到哪里」和「_prev
  //     记到哪里」永远一致，不会出现差异累积或死锁。
  //   ★★ 预算必须按【真实脏像素数】核算，不能按矩形面积 w*h！
  //      一条弧在纵向连续 100 行、每行只有 2~3 px，合并后的矩形是
  //      4×100 = 400 面积，而真实脏像素只有 ~250。按面积核算会让
  //      每帧预算被放大数倍，仿真实测：budget=3000 时 88 帧里 51 帧
  //      超预算，被截断的行恒在 y=20~59（屏幕中部）——波纹每帧只画到
  //      一半就停手，下一帧从断点继续，屏上就是新旧弧叠加 = 扭曲反转。
  //      改为按段内真实变化像素计数后，同样的 3000 预算可覆盖整条弧。
  void pushSpanList(const int16_t (*sp0)[PUSH_MAXSP],
                    const int16_t (*sp1)[PUSH_MAXSP],
                    const uint8_t *spN, int k, uint32_t &spent) {
    uint16_t *cur = _cv.getBuffer();
    if (cur == nullptr) return;
    const bool limited = (_pushBudgetPx != 0xFFFFFFFFUL);

    int y = 0;
    while (y < SCREEN_H) {
      if (spN[y] <= k) { y++; continue; }

      int yStart = y, a0 = sp0[y][k], a1 = sp1[y][k], lastDirty = y;
      // 本块「真实变化像素数」：逐行按该行第 k 段的长度累加，
      // 而不是矩形面积（矩形内的空白/未变像素不该占预算）。
      uint32_t realPx = (uint32_t)(a1 - a0 + 1);
      int yy = y + 1;
      while (yy < SCREEN_H) {
        if (spN[yy] <= k) {                   // 空行
          if (yy - lastDirty > 2) break;      // 连续 2 行以上没变 → 收口
          yy++;
          continue;
        }
        // 合并后矩形比当前宽多少：斜向弧每行只外移 1~2px，
        // 放宽 6px 能让一条斜线合成一个矩形；跨度突然变大就断。
        int n0 = (sp0[yy][k] < a0) ? sp0[yy][k] : a0;
        int n1 = (sp1[yy][k] > a1) ? sp1[yy][k] : a1;
        if ((n1 - n0) - (a1 - a0) > 6) break;
        realPx += (uint32_t)(sp1[yy][k] - sp0[yy][k] + 1);
        a0 = n0; a1 = n1; lastDirty = yy; yy++;
      }

      int hFull = lastDirty + 1 - yStart;
      int h = hFull;
      // 安全网 3：起点必须 4 字节对齐。
      //   缓冲区内偏移 = (y*160 + x) 个像素 → 字节 = 该值*2；
      //   160 的倍数恒为偶，所以只要 x 取偶，指针就 4 字节对齐。
      //   spiWritePixelsNL() 内部把指针强转成 uint32_t*，未对齐会触发
      //   RISC-V LoadStoreAlignment 异常（直接 panic 重启）。
      if (a0 & 1) { a0--; }
      int w = a1 - a0 + 1;
      if (a0 + w > SCREEN_W) w = SCREEN_W - a0;   // 防止越界
      if (w <= 0) { y = yStart + hFull; continue; }

      if (limited) {
        uint32_t left = (_pushBudgetPx > spent) ? (_pushBudgetPx - spent) : 0u;
        if (left < (uint32_t)w) break;            // 连一行都放不下 → 收手
        // ★ 按真实脏像素数判断是否放得下；放不下就按比例截短行数。
        if (realPx > left) {
          int keep = (int)((uint64_t)hFull * left / realPx);
          if (keep <= 0) break;
          h = keep;
        }
      }
      if (h <= 0) break;
      // ★ 记账用「本块真实脏像素」，不是 w*h。
      //   截短时按行数比例折算，保证预算语义始终是「真实变化像素数」。
      uint32_t useReal = (h == hFull)
                             ? realPx
                             : (uint32_t)((uint64_t)realPx * h / hFull);
      spent += useReal;

      _d->setAddrWindow((int16_t)a0, (int16_t)yStart, (int16_t)w, (int16_t)h);
      if (a0 == 0 && w == SCREEN_W) {
        // 整宽行带：画布里天然连续 → 一次 writePixels 推完
        size_t off = (size_t)yStart * SCREEN_W;
        _d->writePixels(cur + off, (uint32_t)w * h);
        memcpy(_prev + off, cur + off, (size_t)w * h * 2);
      } else {
        for (int k = 0; k < h; k++) {
          size_t off = (size_t)(yStart + k) * SCREEN_W + (size_t)a0;
          _d->writePixels(cur + off, (uint32_t)w);
          memcpy(_prev + off, cur + off, (size_t)w * 2);
        }
      }
      // ★ 必须用 yStart + h（而不是合并循环算出的 yy）：本块可能被预算
      //   截短，截短后要从截断处接着处理，否则中间那几行会被永久跳过。
      y = yStart + h;
    }
  }

  // ── 脏矩形增量推送 ──────────────────────────────────────────
  // 画布仍然是「整帧重绘」，所以画面永远自洽、不会残留旧像素；
  // 这里只把「相对上一帧真正变了的那几块矩形」推给屏。
  //
  //   ★ 收益来源：雷达界面每帧只有波前圆弧（约 1~3 px/行，随半径外移）、
  //     目标小框、底部防区条在变；圆环/网格/锥面是静态的。
  //     整屏推送 = 40960 字节 ≈ 16ms@20MHz；脏矩形通常只有 1~4KB ≈ 0.3~1ms。
  //
  //   ★ 三重安全网：
  //     1) _prev 分配失败 / 首帧没有参照 → 整屏推（只此一次，开机时无碍）
  //     2) 预算按【实际推送矩形面积】核算；超了就推满预算后停手，
  //        剩下的留给下一帧（有限预算模式下绝不整屏推，因为 16ms 连续
  //        SPI 会致盲 433MHz 接收机，见下方注释）
  //     3) 矩形起点强制 4 字节对齐（下方有说明，RISC-V 未对齐访问会触发异常）
  void pushDirty() {
    uint16_t *cur = _cv.getBuffer();
    if (cur == nullptr) return;

    if (_prev == nullptr || !_prevValid) {
      pushCanvas();
      if (_prev != nullptr) {
        memcpy(_prev, cur, (size_t)SCREEN_W * SCREEN_H * 2);
        _prevValid = true;
      }
      return;
    }

    // ── 第一遍：逐行把「所有变化段」登记成段表（每行最多 PUSH_MAXSP 段）──
    //   ★★ 这是「波纹扭曲/反转」的真正根因修复。
    //      波纹半径是整数（drawArc 收 int），所以波纹不是连续移动，而是
    //      每 ~21ms 整条跳到下一个半径：那一瞬【全部约 190 行同时变脏】，
    //      但每行只变 1~4 个像素，真实变化总量只有 ~300~800 px。
    //      旧代码每行取 [最左变化列 … 最右变化列] 当脏矩形 → 每行横跨
    //      整个圆内部（150~190 px），190 行 ≈ 28500 px，预算 3000 只够
    //      18 行 → 一个环要跨十几帧才画完；那几帧里屏上是「新弧 + 上一帧
    //      旧弧」叠着 → 看着就是拉长、扭曲、像倒着走。
    //      现在按【段序号】登记：第 k 段在所有行里天然是同一条竖向带
    //      （左弧 / 右弧 / 目标框边），可以纵向合并成又高又窄的矩形，
    //      面积接近真实脏像素，一个环一帧就能推完。
    //   ★ 数组用 static：6 段 × 128 行 × 2 表 × 2 字节 ≈ 3KB，
    //     放栈上有溢出风险（Arduino loop 任务栈仅 8KB），放 .bss 零代价。
    static int16_t sp0[SCREEN_H][PUSH_MAXSP];
    static int16_t sp1[SCREEN_H][PUSH_MAXSP];
    static uint8_t spN[SCREEN_H];
    int      maxSpans = 0;
    uint32_t dirtyPx  = 0;

    for (int y = 0; y < SCREEN_H; y++) {
      const uint16_t *a = _prev + (size_t)y * SCREEN_W;
      const uint16_t *b = cur  + (size_t)y * SCREEN_W;
      spN[y] = 0;
      if (memcmp(a, b, (size_t)SCREEN_W * 2) == 0) continue;

      int n = 0, i = 0;
      while (i < SCREEN_W) {
        while (i < SCREEN_W && a[i] == b[i]) i++;
        if (i >= SCREEN_W) break;
        if (n >= PUSH_MAXSP) {
          // 段数超上限：把最后一段的右端拉到行尾，宁可多推也不漏推。
          sp1[y][PUSH_MAXSP - 1] = (int16_t)(SCREEN_W - 1);
          break;
        }
        int s = i;
        while (i < SCREEN_W && a[i] != b[i]) i++;
        sp0[y][n] = (int16_t)s;
        sp1[y][n] = (int16_t)(i - 1);
        dirtyPx += (uint32_t)(i - s);
        n++;
      }
      spN[y] = (uint8_t)n;
      if (n > maxSpans) maxSpans = n;
    }
    if (dirtyPx == 0) return;                // 画面完全没变 → 一个字节都不发

    // ── 安全网 2（仅「无限预算」时生效）：脏面积太大 → 整屏推一次更省 ──
    //   ★ 有限预算模式下【绝不能】整屏推：整屏 = 40960 字节，20MHz 下理论
    //     16ms（实测含开销 ~21ms）连续 SPI，会稳定致盲 433MHz 接收机
    //     （实测帧体内被插入 ~10ms 假段 → 整帧作废）。
    if (_pushBudgetPx == 0xFFFFFFFFUL &&
        dirtyPx > (uint32_t)SCREEN_W * SCREEN_H * 4UL / 10UL) {
      pushCanvas();
      memcpy(_prev, cur, (size_t)SCREEN_W * SCREEN_H * 2);
      return;
    }

    // ── 第二遍：左弧、右弧各自合并推送 ──
    //   ★ 预算按【实际推送矩形面积】核算，且【推一块就同步一块 _prev】。
    //       旧实现用「逐行跨度之和」当预算、超了就整帧不推且不更新 _prev，
    //       导致两个叠加的致命问题：
    //         1) 光环这类细弧的跨度之和随半径平方增长（≈πr²），代入预算
    //            3000 得 r≈31px —— 弧度推到 1/3 就必然越界，即「光环推到
    //            一半就不推了」；
    //         2) _prev 不更新让差异只增不减，而 setPushBudget(3000) 每帧都
    //            在调、预算永不放开 → 之后每帧都越界 → 整屏永久冻结。
    //       现在按真实段推送、只推一部分其余下一帧继续：既保证每帧都有
    //       进展，又绝不死锁。
    uint32_t spent = 0;
    _d->startWrite();
    for (int k = 0; k < maxSpans; k++) {
      pushSpanList(sp0, sp1, spN, k, spent);
    }
    _d->endWrite();
  }


  float toRad(int deg) {
    return (deg - 90.0) * PI / 180.0;
  }

  // ══════════════════════════════════════════════════
  //  三角函数定点查表（-90°..90°，步进 2°，定点 ×1024）
  // ══════════════════════════════════════════════════
  //  ★ 这是帧率修复的核心。ESP32-C3 没有硬件 FPU，cos()/sin() 是
  //    软件双精度实现（实测约 20us/次调用）。而 drawGrid() 每帧要算
  //    3 条弧（各 91 点 ×2）+ 7 条角线 ×2 ≈ 560 次，drawArc() 每条
  //    弧再加 182 次 → 合计约 740 次三角函数 / 帧，实测吃掉 11.8ms
  //    (grid) + 2.9ms (ping)，而整帧预算只有 20ms。
  //    改成整型查表后只剩乘加，绘制段从 ~15.5ms 掉到 ~1ms。
  //    ★ 表只在第一次调用时用浮点算一次（91×2 次），之后纯整型。
  static const int16_t *trigTbl() {
    static int16_t t[182];        // [0..90] = cos, [91..181] = sin
    static bool ready = false;
    if (!ready) {
      for (int i = 0; i < 91; i++) {
        int deg = -90 + 2 * i;
        float rad = (float)(deg - 90) * (float)PI / 180.0f;
        float c = cosf(rad), s = sinf(rad);
        t[i]      = (int16_t)(c * 1024.0f + (c >= 0 ? 0.5f : -0.5f));
        t[91 + i] = (int16_t)(s * 1024.0f + (s >= 0 ? 0.5f : -0.5f));
      }
      ready = true;
    }
    return t;
  }
  // deg 自动夹取到 -90..90，并按 2° 栅格取整（调用方本来就是偶数度）
  static int cosDeg1024(int deg) {
    int i = (deg + 90) >> 1;
    if (i < 0) i = 0; else if (i > 90) i = 90;
    return trigTbl()[i];
  }
  static int sinDeg1024(int deg) {
    int i = (deg + 90) >> 1;
    if (i < 0) i = 0; else if (i > 90) i = 90;
    return trigTbl()[91 + i];
  }
  // (v * c) / 1024 带四舍五入；v 恒为正，符号来自 c
  static inline int mul1024(int v, int c) {
    int p = v * c;
    return (p >= 0) ? ((p + 512) >> 10) : -(((-p) + 512) >> 10);
  }

  void polarToScreen(float dist_m, float angle_deg, int &sx, int &sy) {
    float r = (dist_m / MAX_RANGE_M) * RADAR_R;
    if (!(r >= 0.0f)) r = 0.0f;
    // ★ 原实现把角度【直接对齐到 2° 栅格】（d = (d>>1)<<1）后再查表，
    //   于是半径 96px 处横向误差可达 ±1.65px，再叠加目标点半径 2px 的
    //   填充圆，现场看就是「光点左右位置和实际对不上」。而且对负数
    //   右移是向 -∞ 取整（-5>>1 = -3），还带固定 -1° 偏向。
    //   现在改为【2° 表 + 线性插值】：只多两次乘加（每帧 ≤3 个目标，
    //   开销为零），角度量化误差降到 ~0.02°，彻底消除栅格感。
    float a = angle_deg;
    if (a < -90.0f) a = -90.0f; else if (a > 90.0f) a = 90.0f;
    float fi = (a + 90.0f) * 0.5f;
    int   i0 = (int)fi;
    if (i0 < 0) i0 = 0; else if (i0 > 89) i0 = 89;   // 保证 i0+1 ≤ 90
    float f = fi - (float)i0;
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;

    const int16_t *t = trigTbl();
    // 表值是「cos/sin × 1024」，插值后仍需除回 1024
    float c = ((float)t[i0]      + f * (float)(t[i0 + 1]      - t[i0]))      * (1.0f / 1024.0f);
    float s = ((float)t[91 + i0] + f * (float)(t[92 + i0] - t[91 + i0])) * (1.0f / 1024.0f);

    float rc = r * c;
    float rs = r * s;
    sx = RADAR_CX + (int)(rc >= 0 ? (rc + 0.5f) : (rc - 0.5f));
    sy = RADAR_CY + (int)(rs >= 0 ? (rs + 0.5f) : (rs - 0.5f));
  }

  // Halo color ramp: young ping = white/cyan → aging = green → faint tail
  uint16_t pingColor(float p, int k) {
    static const uint16_t tbl[4][4] = {
      {0xFFFF, 0xCFFF, 0x87FF, 0x5FFB},  // k=0 leading edge (white → cyan)
      {0x87FF, 0x5FFB, 0x2FF7, 0x17A6},  // k=1
      {0x5FFB, 0x2FF7, 0x17A6, 0x0F7F},  // k=2
      {0x2FF7, 0x17A6, 0x0F7F, 0x075F},  // k=3 faint tail
    };
    int a = (p < 0.22f) ? 0 : (p < 0.45f) ? 1 : (p < 0.70f) ? 2 : 3;
    return tbl[k][a];
  }

  void drawGrid() {
    // 让外圆弧不再被硬裁，线条在边界内保留完整轮廓。
    //   ★ 三条弧 + 角度线全部改为整型查表（原实现每帧 ~560 次
    //     软件 cos/sin，实测占 11.8ms —— 见 trigTbl() 注释）。
    drawArc(RING_2M, COL_RING);
    drawArc(RING_4M, COL_RING);
    drawArc(RING_6M, COL_RING_KEY);

    for (int a = -90; a <= 90; a += 30) {
      int c = cosDeg1024(a), s = sinDeg1024(a);
      int x1 = RADAR_CX + mul1024(8, c);
      int y1 = RADAR_CY - 1;
      int x2 = RADAR_CX + mul1024(RADAR_R, c);
      int y2 = RADAR_CY + mul1024(RADAR_R, s);
      if (y2 < RADAR_CY)
        _cv.drawLine(x1, y1, x2, y2, (a == 0) ? COL_HUD : COL_GRID_LINE);
    }

    // 60° 扇区边已由网格角线提供，不再重复绘制，避免最外侧斜线重叠。

    // Center crosshair
    _cv.drawLine(RADAR_CX - 4, RADAR_CY, RADAR_CX + 4, RADAR_CY, COL_CROSS);
    _cv.drawLine(RADAR_CX, RADAR_CY - 4, RADAR_CX, RADAR_CY + 4, COL_CROSS);

    // Separator between radar and ECG
    _cv.drawLine(0, 97, SCREEN_W - 1, 97, COL_HUD);

    _cv.setTextSize(1);
    ringLabel(RING_2M, "2m");
    ringLabel(RING_4M, "4m");
    ringLabel(RING_6M, "6m");

    // ── 方位角刻度标注 ──
    //   ★ 现场要求：「网格上要有角度标识」。
    //     原来只画了网格线却一个数字都没有，用户只能靠「数第几条线」猜，
    //     这本身就是「显示不够精确」的直接来源。
    //   ★ 只标这 4 个角度是有原因的，不是漏了：
    //     · ±90°：drawGrid() 里这两条线的终点 y2 = RADAR_CY + R*sin(0) = 96，
    //       判据是 `y2 < RADAR_CY`，等于 96 时【整条线被跳过】——屏幕上
    //       根本没有 ±90° 的网格线，标上去等于凭空多出两个刻度。
    //       半圆左右端点在屏幕外（CX − 96 = −16），边界由 0° 轴线自己暗示。
    //     · 0°：就是正上方那条白色主方位线，用户一眼就能认出来，
    //       标个「0°」纯属占地方（现场明确要求删掉）。
    //   ★ 这些字全部在 begin() 里随 _bg 预烘焙一次，运行时零开销。
    angleLabel(-60, 76);
    angleLabel(-30, 78);
    angleLabel( 30, 78);
    angleLabel( 60, 76);
  }

  // ── 方位角刻度标注（数字 + 手画度符号）──
  //   ★ 角度约定（与 trigTbl() 完全一致，不要改）：
  //        deg = -90 → 屏幕【左侧水平】  (cos=-1, sin= 0)
  //        deg =   0 → 屏幕【正上方】    (cos= 0, sin=-1)
  //        deg = +90 → 屏幕【右侧水平】  (cos=+1, sin= 0)
  //     表里存的是 cos/sin(deg-90)，所以 0° 才是朝上，符合雷达直觉。
  //   ★ 为什么度符号要用 drawCircle 现画、不用 print("°")：
  //     GFX 内置的经典 5×7 字体只覆盖 ASCII 0x20..0x7F，0xB0（°）及
  //     任何 ≥0x80 的字节都是越界/空白，直接印会出乱码甚至拉出垃圾字形。
  //   ★ 先用黑底抹掉该处网格线，否则斜线从字中间穿过去，字就糊了。
  void angleLabel(int deg, int r) {
    char t[6];
    snprintf(t, sizeof(t), "%d", deg);          // -60 / -30 / 0 / 30 / 60

    int w  = (int)strlen(t) * 6;      // GFX 默认字体 6px/字符
    int tw = w + 4;                   // 再给度符号留位
    int cx = RADAR_CX + mul1024(r, cosDeg1024(deg));
    int cy = RADAR_CY + mul1024(r, sinDeg1024(deg));
    int x  = cx - tw / 2;
    int y  = cy - 4;
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x + tw > SCREEN_W - 1) x = SCREEN_W - 1 - tw;

    _cv.fillRect(x - 1, y - 1, tw + 2, 10, COL_BG);
    _cv.setTextColor(COL_TEXT_DIM);
    _cv.setCursor(x, y);
    _cv.print(t);
    _cv.drawCircle(x + w + 3, y + 2, 1, COL_TEXT_DIM);   // 度符号
  }

  void plotTarget(float dist_m, float angle_deg, int idx, unsigned long ms) {
    if (dist_m > MAX_RANGE_M) dist_m = MAX_RANGE_M;
    int tx, ty;
    polarToScreen(dist_m, angle_deg, tx, ty);

    // Pulsing ring
    int phase = (ms / 300 + idx * 127) % 300;
    int rR = 3 + phase / 30;
    _cv.drawCircle(tx, ty, rR, COL_HUD);

    // Bright dot
    uint16_t c = (idx == 0) ? COL_TARGET_1 :
                 (idx == 1) ? COL_TARGET_2 : COL_TARGET_3;
    _cv.fillCircle(tx, ty, 2, c);
  }

  uint16_t zoneAlertColor(int idx) const {
    // A=青 / B=黄 / C=红 / D=品红
    //
    // ★★ 为什么 A 从「绿」改成「青」—— 现场实测反馈「A 和 B 看起来都是黄的」。
    //   这不是代码串色（zoneAlertColor(i) 按槽位取色，索引没错），
    //   而是【这块屏的色域问题】：
    //     · A 原来是 0x07E0 = R0 G63 B0（纯绿）；
    //     · B 是 0xFFE0 = R31 G63 B0（黄）。
    //   廉价 ST7735 模块的绿色 LED 是黄绿色（≈570nm），G 拉满时肉眼就是
    //   「发黄的绿」。再叠加报警态是【整块填充 + 2 倍字号黑字】，
    //   大面积的 R0G63B0 在黄绿背光下和 R31G63B0 几乎分不开 ——
    //   这正是「A、B 都成黄色」的来源。调暗、调亮度都救不了，只能换色相。
    //   → A 改用青色（R0 G63 B31）：蓝通道拉满后色相明显偏冷，
    //     和 B 的黄形成冷暖对立，小面积字母、大面积填充两种状态下都能分辨。
    //
    // ★ D 原为 0x001F（纯蓝, R0 G0 B31），是四色里最暗的一个，黑底上做
    //   字母色偏弱；曾提亮成 0x041F（R0 G32 B31, 亮天蓝），但它和 A 的新
    //   青色同属冷色系且都带 B31，报警整块填充时会互相靠拢。
    //   → D 改用品红（R31 G0 B31）：四个色相 R 通道组合各不相同
    //     （A:0/0、B:1/0、C:1/0 无蓝、D:1/1），彻底拆开。
    //
    //   ★ 四色相分布：青(冷) / 黄(暖) / 红(暖) / 品红(冷+红)
    //     相邻槽位永远是冷暖或明暗对立，不会再出现两格同色。
    static const uint16_t alert[4] = {0x07FF, 0xFFE0, 0xF800, 0xF81F};
    return alert[idx % 4];
  }

  // RGB565 按通道整体压暗：n=1 → 约半亮，n=2 → 约 1/4 亮。
  // 用于「存在但不报警」的降级显示（未启用防区的字母）。
  static uint16_t dim565(uint16_t c, uint8_t n) {
    uint16_t r = (uint16_t)((c >> 11) & 0x1F);
    uint16_t g = (uint16_t)((c >> 5)  & 0x3F);
    uint16_t b = (uint16_t)( c        & 0x1F);
    return (uint16_t)(((r >> n) << 11) | ((g >> n) << 5) | (b >> n));
  }

  // RGB565 线性插值：t=0 取 a，t=1 取 b。用于波纹渐显，
  // 避免波前在最小半径处整条「啡」地冒出来（那种硬边看着就像抖）。
  static uint16_t blend565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r  = ar + (int)((br - ar) * t);
    int g  = ag + (int)((bg - ag) * t);
    int bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
  }

  void drawZoneStrip() {
    const int TOP_Y = 104;
    const int H = 22;
    const int SLOT_W = SCREEN_W / 4;
    // ★ 配对模式全屏禁闪：否则「别的防区正在报警」看起来和
    //   「刚选中的新防区在闪」一模一样，用户无法分辨，也会误以为
    //   配对模式没退出。配对模式下改为静态暗底表示「已占用」。
    const bool pairingUI = _pairingFrame;
    for (int i = 0; i < 4; i++) {
      int x = i * SLOT_W;
      bool enabledZ  = (i < _zoneCount) && _zoneEnabled[i];
      bool online    = (i < _zoneCount) && _zoneOnline[i];
      // ★★ 报警态 = 「原始 PRESENCE」或「本防区锁存」，两者取或。
      //   为什么不能只看 _zoneOccupied（原始单帧 PRESENCE）：
      //     433M 单向 OOK 实测丢帧率可达 40%+，而 occupied 完全由
      //     「最近解出来的那一帧的 flags」决定 —— 一旦这一帧丢了、
      //     下一帧要 3 秒后才来，中间这段时间 occupied 是不变的；
      //     但更糟的是「从机重启/误发 flags=00」会立刻把 occupied 打成 0，
      //     屏幕上就表现为「报警自己灭一下又亮」。
      //     ZoneManager 里本来就有每防区独立的 alarmLatched（连续
      //     ZONE_CLEAR_FRAMES 帧「无人」才解除），它才是「报警」的真值。
      //   ★ 之前 _zoneLatched[] 只被 setZoneHealth() 写入、渲染时从未使用，
      //     等于每防区锁存白算了一遍 —— 现在真正接通，防区之间互不影响。
      //   ★ 但必须再与 online 相与：离线防区按设计【不】解锁存
      //     （避免丢几帧就报警闪断），可是屏幕上绝不能把一个已经失联
      //     的探头画成「正在报警」—— 那和「真有人」完全无法区分。
      //     离线优先：先判链路，再判报警。
      bool occupied  = enabledZ && online && (_zoneOccupied[i] || _zoneLatched[i]);
      bool active = (i == _activeZone);
      bool searching = _pairingFlash && active;      // 已长按 → 闪烁搜索
      bool selected  = _pairingFrame && active;      // 配对模式中的选中防区
      uint16_t alert  = zoneAlertColor(i);

      // ── 平时（未触发）：纯黑底 + 防区色字母 ──
      //   ★ 字母颜色就是防区色，必须和「触发时整块填充的底色」完全一致，
      //     这样触发时整块反色（防区色底 + 黑字）才看得出是同一个防区。
      //     不要对未启用的防区做 dim565 压暗 —— 那会让字母色偏离防区色，
      //     同一个防区平时和报警时对不上号。
      uint16_t bg     = COL_BG;
      uint16_t border = 0x0000;
      uint16_t text   = alert;

      // ── 离线：字母压暗 + 灰色虚线边框 ──
      //   ★ 这是「各防区独立」的直观体现：某台从机掉电时，
      //     只有它自己变灰，其余防区照常工作，绝不会一起复位。
      //     压暗用 dim565(alert, 3) 而不是纯灰，保留防区色便于对号。
      if (enabledZ && !online) {
        bg     = COL_BG;
        text   = dim565(alert, 3);
        border = 0x4208;          // 暗灰：与「选中/搜索」的亮框明确区分
      }

      // ── 触发（有人）：整块反色 —— 防区色底 + 黑字 ──
      //   ★ 配对模式下绝不闪：此时闪的只应该是「正在搜索」那个红闪，
      //     否则别的防区报警会被误认成新防区在闪，看着像卡在报警态。
      //   ★★ 闪烁相位必须【按防区错开】：原来所有防区共用
      //     (millis()/160)&1，于是同时报警的 A/B/C/D 会完全同步地一起亮、
      //     一起灭 —— 视觉上是一整条黄块在闪，根本数不出有几个防区在报警，
      //     而且进一步加重了「A 和 B 颜色一样」的错觉（同亮同灭时两块
      //     大面积同色相邻，肉眼只会记成一个色块）。
      //     给每个槽位加 i*80ms 的相位偏移后，四格闪烁彼此错开，
      //     每格的颜色、节奏都能单独辨认。
      const bool flashOn = (((millis() + (uint32_t)i * 80UL) / 160UL) & 1U) != 0;
      if (occupied && !searching && !selected && !pairingUI && flashOn) {
        bg = alert;
        text = 0x0000;
      } else if (occupied && !searching && !selected && pairingUI) {
        // 配对模式：已占用 → 静态暗底 + 防区色字母（不闪）。
        //   ★ 不能用「暗底 + 黑字」：D 区天蓝压暗后近乎全黑，字母看不见。
        //   用 dim565(alert, 2) 做底、原色做字，四个防区都清晰可辨。
        bg = dim565(alert, 2);
        text = alert;
      }

      // ── 配对模式：选中防区加白框 + 警示底色 ──
      //   ★ 未占用时用橙黄警示底（不是几乎看不见的 0x0010 暗底），
      //     这样「我现在选中了哪个防区」一眼可辨。
      if (selected) {
        border = 0xFFFF;
        bg = occupied ? 0xFE60 : 0xFC00;   // 已占用=浅黄，未占用=橙黄
        text = 0x0000;
      }

      // ── 正在搜索：红/暗红闪烁，优先级最高 ──
      if (searching) {
        bool on = ((millis() / 150) & 1) != 0;
        bg = on ? 0xF800 : 0x7000;
        text = 0xFFFF;
        border = 0xFFFF;
      }

      _cv.fillRect(x + 1, TOP_Y, SLOT_W - 2, H, bg);
      _cv.drawRect(x, TOP_Y, SLOT_W, H, border);
      _cv.setTextColor(text);
      _cv.setTextSize(2);
      const char *name = _zoneNames[i];
      int16_t x1 = x + (SLOT_W - (strlen(name) * 12)) / 2;
      int16_t y1 = TOP_Y + 3;
      _cv.setCursor(x1, y1);
      _cv.print(name);
    }
    // ★ 原版在此画了一条整宽纯白线 (y = TOP_Y - 1)。
    //   它紧贴防区条顶边，和各防区自己的 drawRect 边框重复，
    //   视觉上就是一条「难看的白线」→ 已移除。
  }

  // ── 左上角 WiFi/云状态徽标 ──
  //   ★ 为什么从右上角挪到左上角：
  //     右上角 (133..160, 2..12) 虽然也不占圆弧，但它是「大角度扇区」那一侧，
  //     视线习惯先看右上；而左上角这块 (2..29, 1..11) 是整屏最干净的
  //     死角 —— 外圆弧(R=96, CX=80, CY=96) 在 x=29 处才降到 y≈15，
  //     在 x=2 处已到 y≈43，所以 y≤11 这一条全程在圆环外侧。
  //     同时它不与 2m/4m 量程标签(x=77) 冲突，也不压防区条(y=104)。
  //   state 见 TuyaLink.h 的 TuyaUiState：
  //     0=关(不画) 1=连WiFi/扫描(琥珀闪) 2=连云(琥珀) 3=在线(绿)
  //     4=失败(红) 5=配网热点(蓝闪)
  //   文案长度不同（"ON" vs "WIFI"），所以每帧先清底再画，避免残字。
  //   状态不变时画布与 _prev 一致 → 脏矩形不推 → 零额外 SPI 开销；
  //   闪烁态每 400ms 只推一块 27×10 的小矩形，对 OOK 收帧无感。
  void drawWifiBadge() {
    const int bx = 2, by = 1, bw = 28, bh = 10;
    // ★ 关 WiFi（state=0）时必须把上一次画的擦掉，不能直接 return：
    //   否则「长按 2s 关掉 WiFi」后琥珀色的旧字样会永远留在屏上，
    //   现场会以为 WiFi 还开着。只在 1→0 时擦一次，之后零开销。
    if (_wifiState == 0) {
      if (_wifiDrawn) {
        _cv.fillRect(bx, by, bw, bh, COL_BG);
        _wifiDrawn = false;
      }
      return;
    }
    _wifiDrawn = true;
    _cv.fillRect(bx, by, bw, bh, COL_BG);
    uint16_t c;
    const char *txt;
    switch (_wifiState) {
      case 1:                                   // 正在扫描 / 连 WiFi / NTP 对时
        txt = "WIFI";
        c = ((millis() / 400) & 1) ? 0xFC00 : dim565(0xFC00, 2);
        break;
      case 2:                                   // 正在连涂鸦云（MQTT/TLS）
        txt = "MQTT";
        c = 0xFC00;
        break;
      case 3:                                   // 已在线
        txt = "ON";
        c = 0x07E0;
        break;
      case 5:                                   // 配网热点已开，等手机连
        txt = "AP";
        c = ((millis() / 400) & 1) ? 0x04DF : dim565(0x04DF, 2);
        break;
      default:                                  // 反复失败（背退中）
        txt = "ERR";
        c = 0xF800;
        break;
    }
    _cv.setTextColor(c);
    _cv.setTextSize(1);
    _cv.setCursor(bx + (bw - (int)strlen(txt) * 6) / 2, by + 1);
    _cv.print(txt);
  }

  // ★ 原「左上角两行数值读数」(3.2m / A-15) 已删除 —— 现场觉得字丑且占地方。
  //   精度需求改由量程圈上的 "2m/4m/6m" 标注满足（见 ringLabel）。
  //   要恢复的话，画在 (2,13) 与 (2,22)，与 WiFi 徽标 (y1..10) 不冲突。

  // Semicircle arc drawn into the canvas (upper half only)
  //   ★ 角度固定为 -90..90 步进 2°，正好与 trigTbl() 的栅格对齐，
  //     全程整型查表，不再调用 cos()/sin()。
  void drawArc(int radius, uint16_t color) {
    int px = -1, py = -1;
    for (int i = 0; i < 91; i++) {
      int x = RADAR_CX + mul1024(radius, cosDeg1024(-90 + 2 * i));
      int y = RADAR_CY + mul1024(radius, sinDeg1024(-90 + 2 * i));
      if (y > RADAR_CY) y = RADAR_CY;
      if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        px = -1; py = -1; continue;
      }
      if (px >= 0 && (x != px || y != py)) {
        _cv.drawLine(px, py, x, y, color);
      }
      px = x; py = y;
    }
  }

  void drawConeEdge(int deg) {
    int x2 = RADAR_CX + mul1024(RADAR_R, cosDeg1024(deg));
    int y2 = RADAR_CY + mul1024(RADAR_R, sinDeg1024(deg));
    if (y2 > RADAR_CY) y2 = RADAR_CY;
    _cv.drawLine(RADAR_CX, RADAR_CY, x2, y2, COL_CONE);
  }

  // ── 量程圈上的数字+单位标注 ──
  //   ★ 为什么标在圈【内侧】并居中在竖直轴上：
  //     旧版只写 "2" / "4" 且贴在轴线左侧（x = CX - 3），既没有单位，
  //     又被正上方那条白色 0° 方位线直接穿过 —— 现场看就是几个脏点。
  //     现在每圈内侧居中写 "2m/4m/6m"（6m 圈原来因为 ly<0 根本画不出来），
  //     并先用黑底把该处的轴线抹掉，文字干净、不糊。
  //   ★ 这部分在 begin() 里随 _bg 一起预烘焙，每帧只 memcpy，零额外开销。
  //     文字宽度 = 字符数 × 6（GFX 默认字体），"2m" = 12px。
  void ringLabel(int r, const char *t) {
    int w = (int)strlen(t) * 6;
    int x = RADAR_CX - w / 2;
    int y = RADAR_CY - r + 3;          // 圈内侧，紧贴弧线下方
    if (y < 1) y = 1;
    if (x < 0) x = 0;
    if (x + w > SCREEN_W) x = SCREEN_W - w;
    // 黑底遮住该处的 0° 白色方位线，否则竖线穿过字形
    _cv.fillRect(x - 1, y - 1, w + 2, 10, COL_BG);
    _cv.setTextColor(COL_TEXT_DIM);
    _cv.setCursor(x, y);
    _cv.print(t);
  }
};

#endif // RADAR_DISPLAY_H
