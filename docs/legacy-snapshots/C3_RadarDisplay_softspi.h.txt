/*
 * Radar Display — Military Tactical v7
 *
 * ★ Heartbeat ping rings: a halo pulses outward from the radar center
 *   (sonar-style ripple — replaces the old rotating sweep line)
 * ★ Full-frame double-buffer (GFXcanvas16) → flicker-free redraw
 * ★ ECG at bottom only
 * ★ NATO phosphor green high-contrast palette
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
#define PING_SLOTS    4     // concurrent expanding rings
#define PING_LIFE_MS  2400  // one ring travels center → edge
#define PING_IDLE_MS  1200  // spawn cadence when quiet
#define PING_BEAT_MS  750   // spawn cadence while heartbeat active

// ═══════════════════════════════════════════════════
//  NATO Military High-Contrast Phosphor Green
// ═══════════════════════════════════════════════════
#define COL_BG          0x0000
#define COL_RING        0x0108
#define COL_RING_KEY    0x03E0
#define COL_GRID_LINE   0x0108
#define COL_CONE        0x0120
#define COL_CROSS       0x03E0
#define COL_SWEEP       0x07E0
#define COL_SWEEP_MID   0x04E0
#define COL_SWEEP_DIM   0x02C0
#define COL_SWEEP_FAINT 0x0180
#define COL_SWEEP_GHOST 0x00C0
#define COL_TARGET_1    0x07FF  // Cyan
#define COL_TARGET_2    0xFFE0  // Amber
#define COL_TARGET_3    0xF800  // Red
#define COL_HUD         0x07E0
#define COL_TEXT_DIM    0x0180
#define COL_ALERT       0xF800
#define COL_ECG_BASE    0x0120
#define COL_ECG_LINE    0x03E0
#define COL_ECG_PEAK    0x07E0

#define MAX_RANGE_M 6.0

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
      if (p > 0.93f) continue;                // whole halo fades before the edge
      float rEdge = 5.0f + p * ((float)RADAR_R + 4.0f - 5.0f);
      for (int k = 0; k < 4; k++) {           // wavefront + trailing tail
        float r = rEdge - k * 3.0f;
        if (r < 2.5f) continue;
        if (p > 0.80f && k > 1) continue;     // tail dissolves first
        uint16_t c = pingColor(p, k);
        if (c) drawArc((int)(r + 0.5f), c);
      }
    }

    // Targets
    for (int i = 0; i < count; i++) {
      if (dist[i] > 0.0f) plotTarget(dist[i], angle[i], i, ms);
    }

    drawECG();

    // ── Push canvas → TFT in a single SPI burst ──
    _d->startWrite();
    _d->setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
    _d->writePixels(_cv.getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
    _d->endWrite();
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
