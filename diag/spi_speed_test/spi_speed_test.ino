/*
 * ST7735 hardware-SPI diagnostic  (standalone, no radar, no UART, no NVS)
 * =====================================================================
 * Purpose : find the HIGHEST hardware-SPI clock that renders the panel
 *           without any garble on THIS board + THIS wiring, and measure
 *           the real full-frame push time (= smoothness).
 *
 * How it works:
 *   - Hardware SPI on the project pins (SCLK=6, MOSI=7), full 160x128
 *     frame pushed from a GFXcanvas16 buffer, exactly like the working
 *     software-SPI firmware does.
 *   - It walks through a list of SPI frequencies, 6 s each, then loops.
 *   - The on-screen label always shows the CURRENT frequency in big text.
 *   - Serial prints one line per second: freq, push time, fps.
 *
 * What to look for:
 *   - Any flicker / colour noise / shifted rows / drifting image = GARBLE.
 *   - A clean, stable picture = that frequency is safe.
 *   - Bottom line tells you the frame rate you would actually get.
 *
 * Serial: send 'n' to jump to the next frequency immediately.
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "driver/gpio.h"   /* gpio_set_drive_capability — 提高引脚驱动能力 */

/* ---- pins, same as the project ---- */
#define TFT_CS    1
#define TFT_DC    2
#define TFT_RST   3
#define TFT_MOSI  7
#define TFT_SCLK  6
#define TFT_BLK   10

/* ---- hardware-SPI display, full-frame canvas ---- */
static Adafruit_ST7735 display(&SPI, TFT_CS, TFT_DC, TFT_RST);
static GFXcanvas16     cv(160, 128);

static const uint32_t FREQS[] = {
    1000000UL,  2000000UL,  4000000UL,  6000000UL,  8000000UL,
    12000000UL, 16000000UL, 20000000UL, 27000000UL, 40000000UL};
static const uint8_t NFREQ = sizeof(FREQS) / sizeof(FREQS[0]);

static uint8_t       fidx        = 0;
static uint32_t      frame       = 0;
static uint32_t      framesStep  = 0;
static uint32_t      pushUs      = 0;   /* last measured push time  */
static uint32_t      pushUsMax   = 0;
static uint32_t      fpsStep     = 0;   /* fps of the previous step */
static unsigned long stepStart   = 0;
static unsigned long lastReport  = 0;
static bool          driveMax    = true;  /* GPIO 驱动能力: true=40mA, false=5mA */

/* 把 SCLK/MOSI 的 IOMUX 驱动能力调到最大: 边沿更陡, 抗长线反射。
   这是"改接线"之外, 固件层面唯一能改善信号完整性的一招。 */
static void applyDrive(void) {
  gpio_drive_cap_t cap = driveMax ? GPIO_DRIVE_CAP_3 : GPIO_DRIVE_CAP_0;
  gpio_set_drive_capability((gpio_num_t)TFT_SCLK, cap);
  gpio_set_drive_capability((gpio_num_t)TFT_MOSI, cap);
}

static void drawPattern(uint32_t freq, uint32_t fnum) {
  const uint16_t bars[8] = {0xF800, 0x07E0, 0x001F, 0x07FF,
                            0xF81F, 0xFFE0, 0xFFFF, 0x0000};

  cv.fillScreen(0x0000);

  /* 8 vertical colour bars -> wrong byte order is instantly obvious */
  for (uint8_t i = 0; i < 8; i++)
    cv.fillRect(i * 20, 0, 20, 64, bars[i]);

  /* single-pixel checkerboard -> bit/byte order, aliasing */
  for (int y = 64; y < 96; y++)
    for (int x = 0; x < 32; x++)
      if (((x + y) & 1) == 0) cv.drawPixel(x, y, 0xFFFF);

  /* thin horizontal comb -> missing/duplicated rows, tearing */
  for (int y = 96; y < 128; y += 2) cv.drawFastHLine(0, y, 160, 0x4208);

  /* thin vertical comb on the right -> column offset problems */
  for (int x = 144; x < 160; x += 2) cv.drawFastVLine(x, 64, 64, 0x4208);

  /* corner markers: TL red / TR green / BL blue / BR white
     -> reveals shifts, mirroring, wrong rotation */
  cv.fillRect(0,   0,   6, 6, 0xF800);
  cv.fillRect(154, 0,   6, 6, 0x07E0);
  cv.fillRect(0,   122, 6, 6, 0x001F);
  cv.fillRect(154, 122, 6, 6, 0xFFFF);

  /* moving bar -> shows stutter / tearing */
  int mx = (int)((fnum * 3) % 152);
  cv.fillRect(mx, 66, 4, 26, 0xFFE0);

  /* ---- big frequency label ---- */
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu MHz", (unsigned long)(freq / 1000000UL));
  cv.fillRect(0, 38, 160, 26, 0x0000);
  cv.setTextColor(0xFFFF, 0x0000);
  cv.setTextSize(2);
  int16_t bx, by;
  uint16_t bw, bh;
  cv.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  cv.setCursor((160 - (int)bw) / 2, 44);
  cv.print(buf);

  /* ---- small readout ---- */
  snprintf(buf, sizeof(buf), "push %lu.%02lu ms  %lu fps",
           (unsigned long)(pushUs / 1000),
           (unsigned long)((pushUs % 1000) / 10),
           (unsigned long)fpsStep);
  cv.setTextSize(1);
  cv.getTextBounds(buf, 0, 0, &bx, &by, &bw, &bh);
  cv.setCursor((160 - (int)bw) / 2, 76);
  cv.print(buf);

  snprintf(buf, sizeof(buf), "#%lu  DRV:%s", (unsigned long)frame,
           driveMax ? "40mA" : "5mA");
  cv.setCursor(4, 108);
  cv.print(buf);
}

static void pushFrame(void) {
  uint32_t t0 = micros();
  display.startWrite();
  display.setAddrWindow(0, 0, 160, 128);
  display.writePixels(cv.getBuffer(), (uint32_t)160 * 128);
  display.endWrite();
  pushUs = micros() - t0;
  if (pushUs > pushUsMax) pushUsMax = pushUs;
}

static void enterStep(uint8_t idx) {
  /* close the previous step's statistics */
  if (stepStart != 0 && framesStep != 0) {
    unsigned long dt = millis() - stepStart;
    fpsStep = (dt > 0) ? (uint32_t)((uint64_t)framesStep * 1000 / dt) : 0;
    Serial.printf("[RESULT] %lu MHz : %lu frames, push %lu.%02lu ms max, %lu fps\n",
                  (unsigned long)(FREQS[fidx] / 1000000UL),
                  (unsigned long)framesStep,
                  (unsigned long)(pushUsMax / 1000),
                  (unsigned long)((pushUsMax % 1000) / 10),
                  (unsigned long)fpsStep);
  }

  fidx         = idx;
  framesStep   = 0;
  pushUsMax    = 0;
  stepStart    = millis();
  lastReport   = 0;

  display.setSPISpeed(FREQS[fidx]);
  Serial.printf("\n>>> NOW TESTING %lu MHz  (watch the screen, 6 s)\n",
                (unsigned long)(FREQS[fidx] / 1000000UL));
}

void setup(void) {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=====================================================");
  Serial.println(" ST7735 hardware-SPI diagnostic");
  Serial.println(" SCLK=6 MOSI=7 CS=1 DC=2 RST=3 BLK=10");
  Serial.println(" Sweeps 1..40 MHz, 6 s each, then loops.");
  Serial.println(" Send 'n' over serial to skip to the next step.");
  Serial.println(" Send 'd' over serial to toggle drive strength (40mA/5mA).");
  Serial.println(" Tell the assistant which frequencies stay clean.");
  Serial.println("=====================================================");

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

  /* Explicit pin setup on the hardware SPI bus BEFORE the panel driver
     runs. This must come first so the driver's internal no-arg
     SPI.begin() cannot fall back to the variant default pins. */
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);
  applyDrive();

  display.initR(INITR_BLACKTAB);
  display.setRotation(1);

  if (cv.getBuffer() == nullptr) {
    Serial.println("[FATAL] canvas allocation failed");
    display.fillScreen(0xF800);
    while (true) delay(1000);
  }

  enterStep(0);
}

void loop(void) {
  /* serial: 'n' -> next frequency */
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'n' || c == 'N') {
      enterStep((uint8_t)((fidx + 1) % NFREQ));
    } else if (c == 'd' || c == 'D') {
      driveMax = !driveMax;
      applyDrive();
      Serial.printf("  >> drive strength = %s\n",
                    driveMax ? "MAX (40 mA)" : "MIN (5 mA)");
    }
  }

  frame++;
  framesStep++;

  drawPattern(FREQS[fidx], frame);
  pushFrame();

  unsigned long now = millis();

  /* 1 Hz serial report */
  if (now - lastReport >= 1000) {
    lastReport = now;
    unsigned long dt = now - stepStart;
    uint32_t fps = (dt > 0) ? (uint32_t)((uint64_t)framesStep * 1000 / dt) : 0;
    Serial.printf("  %2lu MHz  push %5lu us  %3lu fps\n",
                  (unsigned long)(FREQS[fidx] / 1000000UL),
                  (unsigned long)pushUs, (unsigned long)fps);
  }

  /* 6 s per frequency, then advance */
  if (now - stepStart >= 6000UL) {
    enterStep((uint8_t)((fidx + 1) % NFREQ));
  }
}
