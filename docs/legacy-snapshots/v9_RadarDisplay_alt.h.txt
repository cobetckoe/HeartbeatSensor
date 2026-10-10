/*
 * RadarDisplay.h - 半圆雷达扫描渲染器（低占用 / 无撕裂 / 增量刷新）v9
 *
 * 设计目标
 *   1) 流畅：只把"变化的像素"推给屏幕，撕裂不可见。
 *   2) 省资源：静态层只画一次；每帧只重绘扫描束、目标、ECG 亮点。
 *   3) 半圆：圆心在底边中部，半径铺满上半屏（R=96，160x128 全用上）。
 *
 * 渲染管线（每帧）
 *   清脏表 -> 标记（旧位置 + 新位置）-> 从静态层恢复脏行
 *   -> 画动态内容（扫描束 / 目标 / ECG 亮点）-> 按"行块"合并推送
 *
 * 两层缓冲
 *   静态层 _static : 量程环、角度辐条、±60 边界、十字、标签、ECG 波形
 *   显示层 _cv     : 静态层 + 当前动态内容（屏幕上应该显示的样子）
 *   恢复脏行时直接把 _static 对应行 memcpy 回 _cv，天然擦除上一帧的动态内容。
 *
 * 内存占用：160*128*2 x2 ≈ 80KB（ESP32-C3 有 320KB RAM，充裕）
 */

#ifndef RADAR_DISPLAY_H
#define RADAR_DISPLAY_H

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 存在 include/LogoImage.h（由 utils/logo_converter.py 生成）时启用图片 LOGO
#if defined(__has_include)
#if __has_include("LogoImage.h")
#define LOGO_IMAGE_AVAILABLE 1
#include "LogoImage.h"
#endif
#endif

// ============================ 屏幕与几何 ============================
#define SCREEN_W 160
#define SCREEN_H 128

#define RADAR_CX 80      // 圆心 X（屏幕水平中点）
#define RADAR_CY 96      // 圆心 Y（半圆底边）
#define RADAR_R  96      // 半径（铺满上半屏）

#define RING_2M ((int)(RADAR_R * 2.0f / 6.0f))   // 32
#define RING_4M ((int)(RADAR_R * 4.0f / 6.0f))   // 64
#define RING_6M RADAR_R                          // 96

#define MAX_RANGE_M 6.0f

#define DEG_2_RAD 0.0174532925199433f
#define RAD_2_DEG 57.29577951308232f

// ============================ 扫描束参数 ============================
// 扫描为"往复式"（左->右->左），没有跳变，视觉上最顺滑。
#define SWEEP_HALF_MS   1900.0f   // 单程（-90 度 -> +90 度）耗时
#define SWEEP_BEAM_DEG  5.0f      // 光束张角（含拖尾）
#define SWEEP_EDGE_DEG  89.9f     // 角度夹紧上限（避免 tan 发散）

// ============================ ECG 参数 ============================
#define ECG_BASE_Y   113
#define ECG_SWEEP_MS 1000         // 亮点扫过整屏的时间
#define ECG_PAUSE_MS 140          // 到右端后的停顿

// ==================== 开机 LOGO（用你自己的图片） ====================
// 使用方法：
//   1) 把你的 LOGO 图片放到 utils/logo.png（或 logo.jpg / logo.bmp）
//   2) 运行 python utils/logo_converter.py
//      会生成 include/LogoImage.h 并自动编译进固件
//   3) 重新烧录即可。图片会自动缩放到 160x128 屏幕。
// 没有图片时：显示下面这些常量定义的矢量备用画面。
#define LOGO_TEXT     "KANCHUAN RADAR"
#define LOGO_SUB      "LD2450 TRACKER"
#define LOGO_HOLD_MS  1500UL     // LOGO 显示时长（毫秒）
#define LOGO_R        26         // 备用画面图标半径
#define LOGO_CY       44         // 备用画面图标圆心高度
#define LOGO_TEXT_Y   82         // 备用画面标题 Y
#define LOGO_SUB_Y    96         // 备用画面副标题 Y

// ============================ 调色板（NATO 荧光绿） ============================
#define COL_BG        0x0000
#define COL_RING_DIM  0x0180      // 量程环（暗绿）
#define COL_RING_KEY  0x0320      // 6m 外环（亮一档）
#define COL_SPOKE     0x0100      // 30 度辐条
#define COL_AXIS      0x03E0      // 正前方轴线
#define COL_CONE      0x0200      // ±60 度探测边界
#define COL_CROSS     0x03E0      // 圆心十字
#define COL_TEXT_DIM  0x0120      // 标签

#define COL_BEAM_TAIL 0x0100      // 光束拖尾（最暗）
#define COL_BEAM_MID  0x0300
#define COL_BEAM_HEAD 0x05E0      // 光束头部（最亮绿）
#define COL_BEAM_EDGE 0xFFFF      // 头部亮线（白）

#define COL_TARGET_1  0x07FF      // 目标 1：青
#define COL_TARGET_2  0xFFE0      // 目标 2：琥珀
#define COL_TARGET_3  0xF800      // 目标 3：红

#define COL_HUD       0x03E0
#define COL_ECG_BASE  0x0120
#define COL_ECG_LINE  0x0320
#define COL_ECG_PEAK  0x05E0
#define COL_PULSE     0x07FF      // ECG 亮点：青

// 目标脏区半径（要覆盖脉动外环 + 线宽）
#define TGT_DIRTY_R 11

// 推送时每个行块最多合并的行数，以及允许的最大包围盒膨胀系数
#define PUSH_MAX_ROWS 8
#define PUSH_BBOX_SLACK 2.0f

class RadarDisplay {
public:
  RadarDisplay(Adafruit_ST7735 *d)
      : _d(d), _cv(SCREEN_W, SCREEN_H), _static(nullptr), _stage(nullptr),
        _ready(false), _haveOld(false), _headOld(0.0f), _tailOld(0.0f),
        _pulseOld(-99) {
    for (int i = 0; i < 3; i++) {
      _tgtX[i] = 0;
      _tgtY[i] = 0;
      _tgtValid[i] = false;
    }
  }

  bool ready() const { return _ready; }

  // ---------------------------------------------------------------
  //  初始化：分配缓冲 -> 画静态层 -> 整屏推一次
  // ---------------------------------------------------------------
  void begin() {
    if (_cv.getBuffer() == nullptr) {  // 画布分配失败，直接给个黑屏
      _d->fillScreen(COL_BG);
      _ready = false;
      return;
    }
    if (_static == nullptr) {
      _static = (uint16_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));
    }
    if (_stage == nullptr) {
      _stage = (uint16_t *)malloc((size_t)SCREEN_W * PUSH_MAX_ROWS * sizeof(uint16_t));
    }
    if (_static == nullptr || _stage == nullptr) {
      _d->fillScreen(COL_BG);
      _ready = false;
      return;
    }

    composeStatic();
    memcpy(_cv.getBuffer(), _static, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));

    _d->startWrite();
    _d->setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
    pushPixels(_cv.getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
    _d->endWrite();

    _haveOld = false;
    _pulseOld = -99;
    for (int i = 0; i < 3; i++) _tgtValid[i] = false;
    _ready = true;
  }

  // ---------------------------------------------------------------
  //  开机 LOGO：优先显示 include/LogoImage.h 里的图片点阵
  //  （由 utils/logo_converter.py 从你的 PNG/JPG 生成）；
  //  没有该文件时显示内置矢量备用画面。仅开机时调用一次。
  // ---------------------------------------------------------------
  void showBootLogo(unsigned long holdMs = LOGO_HOLD_MS) {
#if LOGO_IMAGE_AVAILABLE
    // ---- 图片路径：黑底 + 逐行居中贴图（每行按 32 像素分片推送）----
    _d->fillScreen(COL_BG);
    int dw = (LOGO_W > SCREEN_W) ? SCREEN_W : LOGO_W;
    int dh = (LOGO_H > SCREEN_H) ? SCREEN_H : LOGO_H;
    int ox = (SCREEN_W - dw) / 2;
    int oy = (SCREEN_H - dh) / 2;
    uint16_t tmp[32];
    _d->startWrite();
    for (int y = 0; y < dh; y++) {
      _d->setAddrWindow((uint16_t)ox, (uint16_t)(oy + y), (uint16_t)dw, 1);
      for (int x = 0; x < dw; x += 32) {
        int n = dw - x;
        if (n > 32) n = 32;
        for (int i = 0; i < n; i++) {
          tmp[i] = pgm_read_word(&LOGO_BITMAP[(size_t)y * LOGO_W + x + i]);
        }
        pushPixels(tmp, (uint32_t)n);
      }
    }
    _d->endWrite();
#else
    drawVectorLogo();
#endif
    delay(holdMs);
  }

  // ---------------------------------------------------------------
  //  每帧调用：ms=millis(), dist/angle/count=有效目标
  // ---------------------------------------------------------------
  void updateFrame(unsigned long ms, const float *dist, const float *angle, int count) {
    if (!_ready) return;

    clearDirty();

    // ---- 1) 扫描束：新位置 + 旧位置一起标记（旧位置用于擦除）----
    float head = sweepHeadDeg(ms);
    float tail = head - SWEEP_BEAM_DEG;
    if (head < tail) { float t = head; head = tail; tail = t; }   // 区间排序

    float lo = tail, hi = head;
    if (_haveOld) {
      lo = fminf(lo, fminf(_headOld, _tailOld));
      hi = fmaxf(hi, fmaxf(_headOld, _tailOld));
    }
    markWedge(fmaxf(lo, -SWEEP_EDGE_DEG), fminf(hi, SWEEP_EDGE_DEG));
    _headOld = sweepHeadDeg(ms);
    _tailOld = _headOld - SWEEP_BEAM_DEG;
    if (_tailOld > _headOld) { float t = _tailOld; _tailOld = _headOld; _headOld = t; }
    _haveOld = true;

    // ---- 2) 目标：计算新坐标，标记新旧两个脏区 ----
    int nx[3], ny[3];
    bool nv[3] = {false, false, false};
    for (int i = 0; i < count && i < 3; i++) {
      nv[i] = targetToScreen(dist[i], angle[i], nx[i], ny[i]);
    }
    for (int i = 0; i < 3; i++) {
      if (_tgtValid[i]) markRect(_tgtX[i] - TGT_DIRTY_R, _tgtY[i] - TGT_DIRTY_R,
                                 _tgtX[i] + TGT_DIRTY_R, _tgtY[i] + TGT_DIRTY_R);
      if (nv[i]) markRect(nx[i] - TGT_DIRTY_R, ny[i] - TGT_DIRTY_R,
                          nx[i] + TGT_DIRTY_R, ny[i] + TGT_DIRTY_R);
    }

    // ---- 3) ECG 亮点 ----
    int pulseX = ecgPulseX(ms);
    markRect(_pulseOld - 3, ECG_BASE_Y - 13, _pulseOld + 3, ECG_BASE_Y + 2);
    markRect(pulseX - 3, ECG_BASE_Y - 13, pulseX + 3, ECG_BASE_Y + 2);

    // ---- 4) 擦除：脏行从静态层恢复 ----
    restoreDirty();

    // ---- 5) 画动态内容 ----
    drawBeam(_tailOld, _headOld);
    for (int i = 0; i < 3; i++) {
      if (nv[i]) drawTarget(nx[i], ny[i], i, ms);
      _tgtValid[i] = nv[i];
      _tgtX[i] = nx[i];
      _tgtY[i] = ny[i];
    }
    drawPulse(pulseX);
    _pulseOld = pulseX;

    // ---- 6) 推送脏行（按行块合并，减少 SPI 事务数）----
    pushDirty();
  }

private:
  Adafruit_ST7735 *_d;
  GFXcanvas16 _cv;         // 显示层（静态 + 动态）
  uint16_t *_static;       // 静态层（用于擦除）
  uint16_t *_stage;        // 行块推送暂存
  bool _ready;

  bool _haveOld;
  float _headOld, _tailOld;
  int _pulseOld;
  int _tgtX[3], _tgtY[3];
  bool _tgtValid[3];

  int16_t _dmin[SCREEN_H];  // 每行脏区左边界
  int16_t _dmax[SCREEN_H];  // 每行脏区右边界

  // ===================== 脏区表 =====================
  void clearDirty() {
    for (int y = 0; y < SCREEN_H; y++) {
      _dmin[y] = 32767;
      _dmax[y] = -1;
    }
  }

  void markRow(int y, int x0, int x1) {
    if (y < 0 || y >= SCREEN_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (x1 < x0) return;
    if (x0 < _dmin[y]) _dmin[y] = (int16_t)x0;
    if (x1 > _dmax[y]) _dmax[y] = (int16_t)x1;
  }

  void markRect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++) markRow(y, x0, x1);
  }

  // 标记一个扇形（上仰角 phiLo..phiHi，0=正前方，+90=右）覆盖的行
  void markWedge(float phiLo, float phiHi) {
    if (phiHi <= phiLo) return;
    for (int y = 0; y < RADAR_CY; y++) {
      float d = (float)(RADAR_CY - y);
      float mx = acosf(fminf(1.0f, d / (float)RADAR_R)) * RAD_2_DEG;
      float a = fmaxf(phiLo, -mx);
      float b = fminf(phiHi, mx);
      if (b <= a) continue;
      int x0 = (int)lroundf(RADAR_CX + d * tanf(a * DEG_2_RAD));
      int x1 = (int)lroundf(RADAR_CX + d * tanf(b * DEG_2_RAD));
      if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
      markRow(y, x0 - 1, x1 + 1);
    }
  }

  void restoreDirty() {
    uint16_t *b = _cv.getBuffer();
    for (int y = 0; y < SCREEN_H; y++) {
      if (_dmin[y] > _dmax[y]) continue;
      int x = _dmin[y];
      int w = _dmax[y] - _dmin[y] + 1;
      memcpy(b + (size_t)y * SCREEN_W + x, _static + (size_t)y * SCREEN_W + x,
             (size_t)w * sizeof(uint16_t));
    }
  }

  // ================================================================
  //  备用矢量 LOGO（没有 LogoImage.h 时使用）
  // ================================================================
  void drawVectorLogo() {
    _d->fillScreen(COL_BG);

    const int cx = SCREEN_W / 2;
    const int cy = LOGO_CY;
    const int r2 = LOGO_R;
    const int r1 = r2 * 2 / 3;
    const int r0 = r2 / 3;
    _d->drawCircleHelper(cx, cy, r2, 0x03, COL_RING_KEY);
    _d->drawCircleHelper(cx, cy, r1, 0x03, COL_RING_DIM);
    _d->drawCircleHelper(cx, cy, r0, 0x03, COL_RING_DIM);
    _d->drawFastHLine(cx - r2 - 2, cy, r2 * 2 + 5, COL_AXIS);
    _d->drawLine(cx, cy, cx, cy - r2 - 3, COL_AXIS);
    _d->fillCircle(cx, cy, 2, COL_CROSS);

    _d->setTextSize(1);
    _d->setTextColor(COL_BEAM_HEAD);
    _d->setCursor((SCREEN_W - (int)strlen(LOGO_TEXT) * 6) / 2, LOGO_TEXT_Y);
    _d->print(F(LOGO_TEXT));
    _d->setTextColor(COL_TEXT_DIM);
    _d->setCursor((SCREEN_W - (int)strlen(LOGO_SUB) * 6) / 2, LOGO_SUB_Y);
    _d->print(F(LOGO_SUB));
  }

  // ================================================================
  //  像素推送：32 像素分片
  //
  //  实测结论（本机 ST7735 + ESP32-C3）：fillScreen 这类内部按
  //  "每次 32 像素"分片调用 writePixels 的路径显示完全正常；而
  //  单次超长突发（整屏 40KB 一口气推）无论字打包还是纯字节路径
  //  都会花屏，且与 SPI 频率无关。因此这里统一用 32 像素分片
  //  （与库 writeColor 的 SPI_MAX_PIXELS_AT_ONCE 相同粒度）。
  // ================================================================
  void pushPixels(uint16_t *buf, uint32_t n) {
    while (n) {
      uint32_t c = (n > 32) ? 32 : n;
      _d->writePixels(buf, c);
      buf += c;
      n -= c;
    }
  }

  // 推送：把连续脏行合并成"行块"，每块用一次 setAddrWindow + 一次推送
  void pushDirty() {
    uint16_t *b = _cv.getBuffer();
    int y = 0;
    while (y < SCREEN_H) {
      if (_dmin[y] > _dmax[y]) { y++; continue; }

      int y0 = y;
      int bx0 = _dmin[y], bx1 = _dmax[y];
      long useful = bx1 - bx0 + 1;
      y++;
      while (y < SCREEN_H && (y - y0) < PUSH_MAX_ROWS && _dmin[y] <= _dmax[y]) {
        int nx0 = min(bx0, (int)_dmin[y]);
        int nx1 = max(bx1, (int)_dmax[y]);
        long nUse = useful + (_dmax[y] - _dmin[y] + 1);
        long nBox = (long)(nx1 - nx0 + 1) * (y - y0 + 1);
        if ((float)nBox > (float)nUse * PUSH_BBOX_SLACK) break;  // 包围盒膨胀太多就断开
        bx0 = nx0; bx1 = nx1; useful = nUse;
        y++;
      }

      int h = y - y0;
      int w = bx1 - bx0 + 1;
      for (int r = 0; r < h; r++) {
        memcpy(_stage + (size_t)r * w, b + (size_t)(y0 + r) * SCREEN_W + bx0,
               (size_t)w * sizeof(uint16_t));
      }
      _d->startWrite();
      _d->setAddrWindow((uint16_t)bx0, (uint16_t)y0, (uint16_t)w, (uint16_t)h);
      pushPixels(_stage, (uint32_t)w * h);
      _d->endWrite();
    }
  }

  // ===================== 静态层 =====================
  void composeStatic() {
    _cv.fillScreen(COL_BG);

    // 量程环（点状，Alien 风格）
    dottedArc(RING_2M, COL_RING_DIM, 3.0f, 3.0f);
    dottedArc(RING_4M, COL_RING_DIM, 3.0f, 3.0f);
    dottedArc(RING_6M, COL_RING_KEY, 4.0f, 2.0f);

    // 角度辐条（每 30 度）
    for (int a = -90; a <= 90; a += 30) {
      if (a == 0) continue;
      spoke(a, 8, RADAR_R, COL_SPOKE);
    }
    spoke(0, 0, RADAR_R, COL_AXIS);

    // ±60 度探测边界
    spoke(-60, 0, RADAR_R, COL_CONE);
    spoke(60, 0, RADAR_R, COL_CONE);

    // 圆心十字 + 中心点
    _cv.drawLine(RADAR_CX - 5, RADAR_CY, RADAR_CX + 5, RADAR_CY, COL_CROSS);
    _cv.drawLine(RADAR_CX, RADAR_CY - 5, RADAR_CX, RADAR_CY + 5, COL_CROSS);
    _cv.fillCircle(RADAR_CX, RADAR_CY, 1, COL_CROSS);

    // 距离标签（沿中轴摆放）
    ringLabel(RING_2M, "2");
    ringLabel(RING_4M, "4");

    // 顶部静态 HUD
    _cv.setTextSize(1);
    _cv.setTextColor(COL_TEXT_DIM);
    _cv.setCursor(2, 2);
    _cv.print(F("LD2450"));
    _cv.setCursor(SCREEN_W - 22, 2);
    _cv.print(F("6m"));

    // 雷达 / ECG 分隔线
    _cv.drawFastHLine(0, RADAR_CY + 1, SCREEN_W, COL_HUD);

    // ECG 静态波形
    drawECGTrace();
  }

  void spoke(int deg, int r0, int r1, uint16_t c) {
    float rad = deg * DEG_2_RAD;
    float s = sinf(rad), co = cosf(rad);
    int x0 = RADAR_CX + (int)lroundf(r0 * s);
    int y0 = RADAR_CY - (int)lroundf(r0 * co);
    int x1 = RADAR_CX + (int)lroundf(r1 * s);
    int y1 = RADAR_CY - (int)lroundf(r1 * co);
    if (y0 > RADAR_CY) y0 = RADAR_CY;
    if (y1 > RADAR_CY) y1 = RADAR_CY;
    _cv.drawLine(x0, y0, x1, y1, c);
  }

  void dottedArc(int r, uint16_t c, float onDeg, float offDeg) {
    float period = onDeg + offDeg;
    for (float a = -90.0f; a < 90.0f; a += period) {
      arcSeg(a, fminf(a + onDeg, 90.0f), r, c);
    }
  }

  void arcSeg(float a0, float a1, int r, uint16_t c) {
    int n = (int)ceilf((a1 - a0) / 2.0f);
    if (n < 1) n = 1;
    int px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
      float a = a0 + (a1 - a0) * (float)i / (float)n;
      float rad = (a - 90.0f) * DEG_2_RAD;   // -90 度=左, 0=正前(上), +90=右
      int x = RADAR_CX + (int)lroundf((float)r * cosf(rad));
      int y = RADAR_CY + (int)lroundf((float)r * sinf(rad));
      if (y > RADAR_CY) y = RADAR_CY;
      if (i > 0) _cv.drawLine(px, py, x, y, c);
      px = x; py = y;
    }
  }

  void ringLabel(int r, const char *t) {
    int y = RADAR_CY - r - 9;
    if (y < 12 || y > RADAR_CY - 8) return;
    _cv.setTextSize(1);
    _cv.setTextColor(COL_TEXT_DIM);
    _cv.setCursor(RADAR_CX - 2, y);
    _cv.print(t);
  }

  // ===================== 动态层 =====================

  // 扫描头角度：往复三角波，-90 -> +90 -> -90，无跳变
  float sweepHeadDeg(unsigned long ms) {
    float period = SWEEP_HALF_MS * 2.0f;
    float t = fmodf((float)ms, period);
    float f = (t < SWEEP_HALF_MS) ? (t / SWEEP_HALF_MS)
                                  : (2.0f - t / SWEEP_HALF_MS);
    return -90.0f + 180.0f * f;
  }

  // 扫描束：三段渐变扇形 + 头部亮线
  void drawBeam(float tail, float head) {
    float a0 = fmaxf(tail, -SWEEP_EDGE_DEG);
    float a1 = fminf(head, SWEEP_EDGE_DEG);
    if (a1 <= a0) return;

    float span = a1 - a0;
    float e1 = a0 + span * 0.34f;
    float e2 = a0 + span * 0.67f;
    const uint16_t cols[3] = {COL_BEAM_TAIL, COL_BEAM_MID, COL_BEAM_HEAD};
    float edges[4] = {a0, e1, e2, a1};

    for (int y = 0; y < RADAR_CY; y++) {
      float d = (float)(RADAR_CY - y);
      float mx = acosf(fminf(1.0f, d / (float)RADAR_R)) * RAD_2_DEG;
      for (int b = 0; b < 3; b++) {
        float lo = fmaxf(edges[b], -mx);
        float hi = fminf(edges[b + 1], mx);
        if (hi <= lo) continue;
        int x0 = (int)lroundf(RADAR_CX + d * tanf(lo * DEG_2_RAD));
        int x1 = (int)lroundf(RADAR_CX + d * tanf(hi * DEG_2_RAD));
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        if (x1 < 0 || x0 > SCREEN_W - 1) continue;
        _cv.drawFastHLine(x0, y, x1 - x0 + 1, cols[b]);
      }
    }

    // 头部亮线（白光刃）
    // 不用 drawLine —— Bresenham 斜线在 160x128 上有台阶（锯齿）。
    // 改成"逐行取 1 像素"，与扇形填充同一套 x = CX + d*tan(phi) 关系，
    // 前缘笔直、和扇体严丝合缝。
    for (int y = 0; y < RADAR_CY; y++) {
      float d = (float)(RADAR_CY - y);
      float mx = acosf(fminf(1.0f, d / (float)RADAR_R)) * RAD_2_DEG;
      if (a1 > mx || a1 < -mx) continue;
      int x = (int)lroundf(RADAR_CX + d * tanf(a1 * DEG_2_RAD));
      if (x >= 0 && x < SCREEN_W) _cv.drawPixel(x, y, COL_BEAM_EDGE);
    }
  }

  bool targetToScreen(float dist, float ang, int &sx, int &sy) {
    if (dist <= 0.0f) return false;
    if (dist > MAX_RANGE_M) dist = MAX_RANGE_M;
    float r = dist / MAX_RANGE_M * (float)RADAR_R;
    float rad = ang * DEG_2_RAD;
    sx = RADAR_CX + (int)lroundf(r * sinf(rad));
    sy = RADAR_CY - (int)lroundf(r * cosf(rad));
    if (sy > RADAR_CY) sy = RADAR_CY;
    return (sx > 0 && sx < SCREEN_W - 1 && sy > 0);
  }

  void drawTarget(int x, int y, int idx, unsigned long ms) {
    uint16_t c = (idx == 0) ? COL_TARGET_1 : (idx == 1) ? COL_TARGET_2 : COL_TARGET_3;
    int phase = (int)((ms / 250 + (unsigned long)idx * 83) % 250UL);
    int r = 4 + phase / 50;              // 脉动外环 4..8
    _cv.drawCircle(x, y, r, c);
    _cv.fillCircle(x, y, 2, c);
    _cv.drawPixel(x, y, COL_BEAM_EDGE);
  }

  // ===================== ECG =====================
  // P-QRS-T 模板，90 个采样点（幅度单位：像素）
  static const int8_t *ecgWave() {
    static const int8_t w[90] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0,
      0, 1, 2, 3, 3, 2, 1, 0,
      0, 0, 0, 0,
      -1, -2, 0,
      4, 10, 10, 4, 0,
      -1, -3, -1,
      0, 0, 0, 0,
      0, 1, 2, 3, 3, 2, 1, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    return w;
  }

  // 每 80 像素一拍，整屏正好 2 拍
  inline int ecgAmp(int x) {
    return (int)ecgWave()[(x % 80) * 90 / 80];
  }

  void drawECGTrace() {
    _cv.drawFastHLine(0, ECG_BASE_Y, SCREEN_W, COL_ECG_BASE);
    int py = ECG_BASE_Y;
    for (int x = 0; x < SCREEN_W; x++) {
      int amp = ecgAmp(x);
      int y = ECG_BASE_Y - amp;
      uint16_t c = (amp >= 4) ? COL_ECG_PEAK : COL_ECG_LINE;
      if (x > 0) _cv.drawLine(x - 1, py, x, y, c);
      else _cv.drawPixel(x, y, c);
      py = y;
    }
  }

  int ecgPulseX(unsigned long ms) {
    unsigned long t = ms % (ECG_SWEEP_MS + ECG_PAUSE_MS);
    float f = (t < ECG_SWEEP_MS) ? (float)t / (float)ECG_SWEEP_MS : 1.0f;
    return -3 + (int)lroundf(f * (float)(SCREEN_W + 6));
  }

  // 亮点像记录笔一样把波形"描"亮
  void drawPulse(int cx) {
    for (int x = cx - 1; x <= cx + 1; x++) {
      if (x < 0 || x >= SCREEN_W) continue;
      int y = ECG_BASE_Y - ecgAmp(x);
      _cv.drawFastVLine(x, y, ECG_BASE_Y - y + 1, COL_PULSE);
    }
  }
};

#endif  // RADAR_DISPLAY_H
