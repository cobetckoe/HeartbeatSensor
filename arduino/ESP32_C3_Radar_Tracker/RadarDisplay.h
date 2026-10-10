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
#define RADAR_R   96   // was 78, now fills full upper screen

#define RING_2M  (int)(RADAR_R * 2.0 / 6.0)
#define RING_4M  (int)(RADAR_R * 4.0 / 6.0)
#define RING_6M  RADAR_R

// ── Ping-ring (heartbeat ripple) tuning ──
// v4: 波纹「单层 + 慢速」。SLOTS=1 → 屏上永远只有一条波纹, 干净不杂乱。
//     一条波纹走完 (LIFE) 之后才生成下一条, 节奏舒缓, 看得清。
#define PING_SLOTS    1     // 同时存在的波纹数 (原 4 → 现在永远只有 1 条)
#define PING_LIFE_MS  3000  // 一条波纹从中心扩散到边缘的时长 (原 2400, 越慢越清晰)
#define PING_IDLE_MS  2200  // 静默时的生成间隔 (原 1200)
#define PING_BEAT_MS  1800  // 检测到目标时的生成间隔 (原 750)

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

// ═══════════════════════════════════════════════════
//  ECG waveform — clean P-QRS-T, 90 samples
// ═══════════════════════════════════════════════════
static const int8_t ecgWave[90] = {
  // Flat baseline (25)
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0,
  // P wave (8)
   0, 1, 2, 3, 3, 2, 1, 0,
  // PR segment (4)
   0, 0, 0, 0,
  // Q dip (3)
  -1, -2, 0,
  // R spike (5)
   4, 10, 10, 4, 0,
  // S dip (3)
  -1, -3, -1,
  // ST segment (4)
   0, 0, 0, 0,
  // T wave (8)
   0, 1, 2, 3, 3, 2, 1, 0,
  // Flat to next beat (30)
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

class RadarDisplay {
public:
  RadarDisplay(Adafruit_ST7735 *d)
      : _d(d), _cv(SCREEN_W, SCREEN_H), _ecgPhase(0), _ecgWriteIdx(0),
        _ecgActive(false), _lastPing(0) {
    memset(_ecgBuf, 0, sizeof(_ecgBuf));
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

  // Advance the ECG ring buffer only (rendering happens in updateFrame)
  void updateECG(bool active) {
    _ecgActive = active;
    int amp = 0;
    if (active) {
      amp = ecgWave[_ecgPhase];
      _ecgPhase = (_ecgPhase + 1) % 90;
    }
    _ecgBuf[_ecgWriteIdx] = amp;
    _ecgWriteIdx = (_ecgWriteIdx + 1) % SCREEN_W;
  }

  // Compose one full frame into the canvas, then push it to the TFT
  void updateFrame(unsigned long ms, const float *dist, const float *angle, int count) {
    if (_cv.getBuffer() == nullptr) return;

    // ── Spawn a new ping ring on cadence (faster while heartbeat active) ──
    bool newPing = false;
    unsigned long interval = _ecgActive ? PING_BEAT_MS : PING_IDLE_MS;
    if (ms - _lastPing >= interval) {
      _lastPing = ms;
      newPing = true;
      for (int i = 0; i < PING_SLOTS; i++) {
        if (!_pings[i].active) {
          _pings[i].born = ms;
          _pings[i].active = true;
          break;
        }
      }
    }

    // ── Compose the frame in RAM ──
    _cv.fillScreen(COL_BG);
    drawGrid();
    if (newPing) _cv.fillCircle(RADAR_CX, RADAR_CY - 2, 3, 0xDFFF);  // source flash

    // Expanding ping rings (光环由内向外扩散)
    for (int i = 0; i < PING_SLOTS; i++) {
      if (!_pings[i].active) continue;
      float p = (ms - _pings[i].born) / (float)PING_LIFE_MS;
      if (p >= 1.0f) { _pings[i].active = false; continue; }
      if (p > 0.93f) continue;                // fades out before reaching the edge
      float rEdge = 5.0f + p * ((float)RADAR_R + 4.0f - 5.0f);
      // ★ 单层波纹: 只画波前这一条弧 (原先 k=0..3 的 4 层拖尾太乱)
      if (rEdge >= 2.5f) {
        uint16_t c = pingColor(p, 0);
        if (c) drawArc((int)(rEdge + 0.5f), c);
      }
    }

    // Targets
    for (int i = 0; i < count; i++) {
      if (dist[i] > 0.0f) plotTarget(dist[i], angle[i], i, ms);
    }

    drawECG();

    // ── Push canvas → TFT in a single SPI burst ──
    pushCanvas();
  }


private:
  Adafruit_ST7735 *_d;
  GFXcanvas16     _cv;           // offscreen frame buffer (flicker-free)
  int   _ecgBuf[160];
  int   _ecgPhase;
  int   _ecgWriteIdx;
  bool  _ecgActive;
  struct Ping { unsigned long born; bool active; };
  Ping  _pings[PING_SLOTS];
  unsigned long _lastPing;

  // 唯一允许的推屏路径：一次开窗 + 一次连续 writePixels（已被实测证明零花屏）
  void pushCanvas() {
    _d->startWrite();
    _d->setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
    _d->writePixels(_cv.getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
    _d->endWrite();
  }

  float toRad(int deg) {
    return (deg - 90.0) * PI / 180.0;
  }

  void polarToScreen(float dist_m, float angle_deg, int &sx, int &sy) {
    float r = (dist_m / MAX_RANGE_M) * RADAR_R;
    float rad = toRad((int)angle_deg);
    sx = RADAR_CX + (int)(r * cos(rad));
    sy = RADAR_CY + (int)(r * sin(rad));
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
    drawArc(RING_2M, COL_RING);
    drawArc(RING_4M, COL_RING);
    drawArc(RING_6M, COL_RING_KEY);

    for (int a = -90; a <= 90; a += 30) {
      float rad = toRad(a);
      int x1 = RADAR_CX + (int)(8 * cos(rad));
      int y1 = RADAR_CY - 1;
      int x2 = RADAR_CX + (int)(RADAR_R * cos(rad));
      int y2 = RADAR_CY + (int)(RADAR_R * sin(rad));
      if (y2 < RADAR_CY)
        _cv.drawLine(x1, y1, x2, y2, (a == 0) ? COL_HUD : COL_GRID_LINE);
    }

    drawConeEdge(-60);
    drawConeEdge(+60);

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

  // ECG trace redrawn from the ring buffer every frame (no erase hacks)
  void drawECG() {
    const int BASE_Y = 113;
    _cv.drawFastHLine(0, BASE_Y, SCREEN_W, COL_ECG_BASE);
    for (int x = 0; x < SCREEN_W; x++) {
      int amp = _ecgBuf[x];
      if (amp == 0) continue;
      uint16_t c = (amp >= 4) ? COL_ECG_PEAK : COL_ECG_LINE;
      if (amp > 0) _cv.drawFastVLine(x, BASE_Y - amp, amp, c);
      else         _cv.drawFastVLine(x, BASE_Y, -amp, c);
    }
    // Connect trace points into a smooth waveform
    for (int x = 0; x < SCREEN_W - 1; x++) {
      int a0 = _ecgBuf[x], a1 = _ecgBuf[x + 1];
      if (a0 == a1) continue;
      uint16_t c = (a0 >= 4 || a1 >= 4) ? COL_ECG_PEAK :
                   (a0 != 0 || a1 != 0) ? COL_ECG_LINE : COL_ECG_BASE;
      _cv.drawLine(x, BASE_Y - a0, x + 1, BASE_Y - a1, c);
    }
  }

  // Semicircle arc drawn into the canvas (upper half only)
  void drawArc(int radius, uint16_t color) {
    int px = -1, py = -1;
    for (int deg = -90; deg <= 90; deg += 2) {
      float rad = toRad(deg);
      int x = RADAR_CX + (int)(radius * cos(rad));
      int y = RADAR_CY + (int)(radius * sin(rad));
      if (y > RADAR_CY) y = RADAR_CY;
      if (px >= 0) _cv.drawLine(px, py, x, y, color);
      px = x; py = y;
    }
  }

  void drawConeEdge(int deg) {
    float rad = toRad(deg);
    int x2 = RADAR_CX + (int)(RADAR_R * cos(rad));
    int y2 = RADAR_CY + (int)(RADAR_R * sin(rad));
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
