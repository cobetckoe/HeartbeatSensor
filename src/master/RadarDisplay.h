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

  // ── 推屏像素预算 ──
  //   ★ 为什么需要「预算」而不是简单的「推/不推」：
  //     SPI 传输期间中断被关闭，OOK 边沿会在这段时间彻底丢失。
  //     实测单次整屏推屏最长 13ms，而 OOK_FRAME_GAP_US = 12000：
  //     一旦这段中断关闭时间超过 12ms，ISR 回来的第一个边沿会被判成
  //     「帧间隔」→ _head 清零 → 整帧作废（实测 crc 从 0 涨到 3）。
  //     但脏矩形推送只有 1~3ms，远低于 12ms，收帧期间推它是安全的。
  //   所以收帧期间不是「完全不推」，而是「只推得起小脏块」。
  //   0xFFFFFFFF = 无限制（空闲时的默认值）。
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
  struct Ping { unsigned long born; bool active; };
  Ping  _pings[PING_SLOTS];
  unsigned long _lastPing;
  const char *_zoneNames[4];
  bool _zoneEnabled[4];
  bool _zoneOccupied[4];
  bool _pairingFrame;           // 处于配对模式（静态白框提示）
  bool _pairingFlash;           // 正在搜索（闪烁）
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
  //     2) 脏像素超过预算 / 超过整屏 40% → 本帧不推（有限预算模式下绝不整屏推，
  //        因为 16ms 连续 SPI 会致盲 315MHz 接收机，见下方注释）
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

    // ── 第一遍：逐行求出「变化像素」的最小/最大列，并统计脏像素总数 ──
    int rX0[SCREEN_H], rX1[SCREEN_H];
    uint32_t dirtyPx = 0;
    for (int y = 0; y < SCREEN_H; y++) {
      const uint16_t *a = _prev + (size_t)y * SCREEN_W;
      const uint16_t *b = cur + (size_t)y * SCREEN_W;
      if (memcmp(a, b, (size_t)SCREEN_W * 2) == 0) {
        rX0[y] = 1; rX1[y] = 0;              // 空行标记
        continue;
      }
      int lo = 0, hi = SCREEN_W - 1;
      while (lo <= hi && a[lo] == b[lo]) lo++;
      while (hi >= lo && a[hi] == b[hi]) hi--;
      rX0[y] = lo; rX1[y] = hi;
      dirtyPx += (uint32_t)(hi - lo + 1);
    }
    if (dirtyPx == 0) return;                // 画面完全没变 → 一个字节都不发

    // ★ 预算超了 → 本帧不推，且【不更新 _prev】。
    //   这样下一帧的差异会继续累积，等预算放开时一次推出去，画面不会丢内容。
    if (dirtyPx > _pushBudgetPx) return;

    // 安全网 2：脏面积太大 → 整屏推一次更省
    //   ★ 但整屏推 = 20480px = 40960 字节，20MHz 下理论 16ms（实测含开销 ~21ms）
    //     连续 SPI，会稳定致盲 315MHz 接收机
    //     （实测帧体内被插入 ~10ms 假段 → 整帧作废）。
    //     所以当上层设了有限预算时，绝不整屏推，直接跳过本帧等下次。
    if (dirtyPx > (uint32_t)SCREEN_W * SCREEN_H * 4UL / 10UL) {
      if (_pushBudgetPx != 0xFFFFFFFFUL) return;   // 有限预算模式：宁可不推
      pushCanvas();
      memcpy(_prev, cur, (size_t)SCREEN_W * SCREEN_H * 2);
      return;
    }

    // ── 第二遍：把相邻脏行合并成尽量贴合的矩形，一次开窗推出去 ──
    _d->startWrite();
    int y = 0;
    while (y < SCREEN_H) {
      if (rX1[y] < rX0[y]) { y++; continue; }

      int yStart = y, r0 = rX0[y], r1 = rX1[y], lastDirty = y;
      int yy = y + 1;
      while (yy < SCREEN_H) {
        if (rX1[yy] < rX0[yy]) {             // 空行
          if (yy - lastDirty > 2) break;     // 连续 2 行以上没变 → 收口
          yy++;
          continue;
        }
        // 合并后矩形比当前宽多少：斜向波前每行只外移 1~2px，
        // 放宽 6px 能让一条斜线合成一个矩形；跨度突然变大就断（如防区条）。
        int n0 = (rX0[yy] < r0) ? rX0[yy] : r0;
        int n1 = (rX1[yy] > r1) ? rX1[yy] : r1;
        if ((n1 - n0) - (r1 - r0) > 6) break;
        r0 = n0; r1 = n1; lastDirty = yy; yy++;
      }

      int h = lastDirty + 1 - yStart;
      // 安全网 3：起点必须 4 字节对齐。
      //   缓冲区内偏移 = (y*160 + x) 个像素 → 字节 = 该值*2；
      //   160 的倍数恒为偶，所以只要 x 取偶，指针就 4 字节对齐。
      //   spiWritePixelsNL() 内部把指针强转成 uint32_t*，未对齐会触发
      //   RISC-V LoadStoreAlignment 异常（直接 panic 重启）。
      if (r0 & 1) { r0--; }
      int w = r1 - r0 + 1;
      if (r0 + w > SCREEN_W) w = SCREEN_W - r0;   // 防止越界

      _d->setAddrWindow((int16_t)r0, (int16_t)yStart, (int16_t)w, (int16_t)h);
      if (r0 == 0 && w == SCREEN_W) {
        // 整宽行带：画布里天然连续 → 一次 writePixels 推完，省掉逐行调用开销
        size_t off = (size_t)yStart * SCREEN_W;
        _d->writePixels(cur + off, (uint32_t)w * h);
        memcpy(_prev + off, cur + off, (size_t)w * h * 2);
      } else {
        for (int k = 0; k < h; k++) {
          size_t off = (size_t)(yStart + k) * SCREEN_W + (size_t)r0;
          _d->writePixels(cur + off, (uint32_t)w);
          memcpy(_prev + off, cur + off, (size_t)w * 2);
        }
      }
      y = yy;
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
    // 对齐到 2° 栅格后查表 —— 目标点每帧通常 ≤5 个，
    // 但统一走整型路径可彻底消除浮点三角函数。
    int d = (int)(angle_deg + (angle_deg >= 0 ? 0.5f : -0.5f));
    if (d < -90) d = -90; else if (d > 90) d = 90;
    d = (d >> 1) << 1;
    sx = RADAR_CX + mul1024((int)r, cosDeg1024(d));
    sy = RADAR_CY + mul1024((int)r, sinDeg1024(d));
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
    rangeLabel(RING_2M, "2");
    rangeLabel(RING_4M, "4");
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
    // A=绿 / B=黄 / C=红 / D=亮天蓝
    //   ★ D 原为 0x001F（纯蓝, R=0 G=0 B=31），是四色里最暗的一个，
    //     在纯黑底上做字母色偏弱 → 提亮为 0x041F（R=0 G=32 B=31, 亮天蓝）。
    //     保持「蓝色」语义不变，但对比明显增强。
    static const uint16_t alert[4] = {0x07E0, 0xFFE0, 0xF800, 0x041F};
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
      bool occupied  = i < _zoneCount && _zoneOccupied[i] && _zoneEnabled[i];
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

      // ── 触发（有人）：整块反色 —— 防区色底 + 黑字 ──
      //   ★ 配对模式下绝不闪：此时闪的只应该是「正在搜索」那个红闪，
      //     否则别的防区报警会被误认成新防区在闪，看着像卡在报警态。
      if (occupied && !searching && !selected && !pairingUI && ((millis() / 160) & 1)) {
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

  void rangeLabel(int r, const char* t) {
    int ly = RADAR_CY - r - 7;
    if (ly > 2 && ly < RADAR_CY - 4) {
      _cv.setCursor(RADAR_CX - 3, ly);
      _cv.setTextColor(COL_TEXT_DIM);
      _cv.print(t);
    }
  }
};

#endif // RADAR_DISPLAY_H
