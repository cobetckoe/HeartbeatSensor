/*
 * BatteryMonitor.h — 电池电压采样 + 电量百分比换算
 * ============================================================
 * ★ 为什么单独放头文件（别搬回 master.ino 里）：
 *   PlatformIO 把 .ino 转成 .cpp 时，会用 ctags 扫描【.ino 本体】
 *   （不跟进 #include），并在【第一个被扫到的函数定义之前】
 *   统一插入所有函数原型。只要 .ino 里出现早于某个 struct 定义的
 *   函数，原型就会被插到 struct 之前，报：
 *       'UartConfig' was not declared in this scope
 *   本文件的函数如果留在 master.ino 里（且位置在 struct UartConfig
 *   之前），就会精确触发这个坑。放进头文件、在顶部 #include，
 *   定义天然位于所有调用点之前，原型插入点也不再碰到它。
 *   —— 与 DebugLog.h 是同一个原因、同一个解法。
 *
 * ★ 硬件前提（GPIO0 = ADC1_CH0）：
 *        VBAT ──[1MΩ]──┬──[1MΩ]── GND
 *                      ├──[100nF]── GND   ← 必须！见下面源阻抗说明
 *                      └── GPIO0
 *   分压比 1:2，4.2V 满电 → 中点 2.1V，在 12dB 衰减量程（约 0~3.1V）内。
 *
 * ★ 为什么必须用 ADC1：
 *   C3 的 ADC2 只有 GPIO5 一路，而 ADC2 在 WiFi 工作时【完全不可用】
 *   （射频占用）。本设备联网是常态，所以电池检测只能走 ADC1
 *   （GPIO0=CH0 / GPIO1=CH1 / GPIO2=CH2 / GPIO3=CH3 / GPIO4=CH4）。
 *   GPIO1/3/4 已被 TFT_DC/TFT_BLK/RADAR_RX 占用 → 只剩 GPIO0，
 *   且它不是 strapping 脚。
 *
 * ★ 为什么必须 12dB（ADC_11db）衰减：
 *   Arduino-ESP32 的 ADC_11db 对应硬件 12dB，量程约 0~3.1V。
 *   中点最高 2.1V，留了 1V 余量；用 6dB 或更小会削顶。
 *   用 analogSetPinAttenuation() 只作用于这一个脚。
 *
 * ★ 为什么用 analogReadMilliVolts() 而不是 analogRead()*3.3/4095：
 *   C3 的 ADC 参考电压标称 1100mV，实测个体差异在 1000~1200mV（±10%）。
 *   用固定 3.3V 换算会把「Vref 偏低的芯片」整体读数放大，
 *   满电 4.2V 可能读成 4.6V —— 电量显示长期偏高，比没有还糟。
 *   analogReadMilliVolts() 走的是出厂校准（eFuse 里的 Vref/TP 值），
 *   直接返回毫伏，跨芯片一致性由 SDK 保证。这是唯一正确做法。
 *
 * ★ 关于 500kΩ 源阻抗的完整说明（这是本设计唯一的硬件妥协）：
 *   SAR ADC 的采样保持电容约几 pF，采样窗口只有几百 ns。
 *   源阻抗 R 与采样电容 C 的时间常数 τ = R×C 决定能否在窗口内充到目标值。
 *   R=500kΩ 时 τ 达数十 µs 量级，远超采样窗口 → 读数偏低。
 *   解法不是换小电阻（那会让分压耗电从 2.1µA 涨到 21µA，对电池设备不可接受），
 *   而是【在中点并一颗 100nF】：电容在两次采样之间被分压缓慢充到真实值，
 *   采样瞬间由电容提供电荷 → 相当于把源阻抗降到接近 0。
 *   代码侧再配合「丢弃第 1 个读数 + 8 次平均」，双保险。
 *
 * ★ 为什么采样周期 10s：分压常驻漏电 4.2V/2MΩ = 2.1µA，
 *   对 2000mAh 电池约等于每年 1.8% 的自放电 —— 可忽略，
 *   所以「采样频率」不影响耗电，只影响 CPU。10s 一次足够看清电量趋势。
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>

// ── 电池电压检测（GPIO0 = ADC1_CH0）──
#define BAT_ADC_PIN        0        // ADC1_CH0
#define BAT_DIV_NUM        2        // VBAT = 中点 × 2（1MΩ + 1MΩ 等值分压）
#define BAT_ADC_SAMPLES    8        // 每次采样取 8 个读数平均（丢弃第 1 个）
#define BAT_SAMPLE_MS      10000UL  // 采样周期 10s（分压常驻耗电 2.1µA，可忽略）

static uint16_t s_batMv     = 0;      // 电池电压（毫伏）
static uint8_t  s_batPct    = 0;      // 电量百分比 0~100
static bool     s_batValid  = false;  // 是否已经采过一次（false = 未知）
static unsigned long s_batNextMs = 0; // 下次采样时刻

// 锂离子/锂聚合物放电曲线 → 百分比
//   ★ 用电压查表而不是线性换算：锂电池 3.7V~4.2V 区间只占约 30% 容量，
//     3.3V~3.7V 占了 70%。线性换算是错的，会让「3.9V 显示 60%」实际只有 40%。
//   ★ 表按【从高到低】排列，命中第一行即返回，实现简单且无浮点。
//   ★ 3.30V 以下直接判 0%：C3 的 LDO 在 3.3V 输入时已经接近 dropout，
//     再低就是欠压复位，显示 0% 正好提示用户充电。
static uint8_t batPctFromMv(uint16_t mv) {
  struct Pt { uint16_t mv; uint8_t pct; };
  static const Pt kCurve[] = {
    { 4200, 100 }, { 4150, 95 }, { 4060, 90 }, { 3980, 80 },
    { 3920, 70 },  { 3870, 60 }, { 3820, 50 }, { 3780, 40 },
    { 3740, 30 },  { 3690, 20 }, { 3610, 10 }, { 3500, 5  },
    { 3300, 0  },
  };
  for (unsigned i = 0; i < sizeof(kCurve) / sizeof(kCurve[0]); i++) {
    if (mv >= kCurve[i].mv) return kCurve[i].pct;
  }
  return 0;
}

// 采一次电池电压（阻塞约 1ms，10s 才跑一次，对 25ms 主循环无影响）
static void batterySample() {
  // 丢弃第 1 个读数：通道刚切换时采样保持电容还残留上一次的电平
  //   （这里虽然只有一路 ADC，但 SDK 内部会复用电容，首次读数仍偏）
  (void)analogReadMilliVolts(BAT_ADC_PIN);

  uint32_t sum = 0;
  for (int i = 0; i < BAT_ADC_SAMPLES; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
  }
  uint16_t mid = (uint16_t)(sum / BAT_ADC_SAMPLES);
  uint16_t vbat = (uint16_t)(mid * BAT_DIV_NUM);

  // 合理性钳位：低于 1.5V 基本等于「没接电池 / 分压断线」，
  //   高于 4.5V 说明分压比接错了。两种都按 0% 显示，便于现场一眼识别，
  //   而不是把错误值当真往屏幕上画。
  //   ⚠️ 越界时也要置 s_batValid=true：屏幕要显示红色的 0%，
  //     而不是「未知」不画 —— 两者现场含义完全不同：
  //     「不画」= 没采到（软件没跑起来），「0%」= 电池真没电了。
  s_batValid = true;
  if (vbat < 1500 || vbat > 4500) {
    s_batMv  = vbat;
    s_batPct = 0;
    return;
  }

  s_batMv = vbat;
  s_batPct = batPctFromMv(vbat);
}

// 初始化 ADC 通道 + 立刻采一次（开机就有电量显示）
static void batteryInit() {
  pinMode(BAT_ADC_PIN, INPUT);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);   // = 硬件 12dB，量程约 0~3.1V
  batterySample();
  s_batNextMs = millis() + BAT_SAMPLE_MS;
}

// 到点采样（在主循环里调用）。用有符号差值比较，避免 millis() 回绕下溢。
static void batteryTick(unsigned long now) {
  if ((long)(now - s_batNextMs) >= 0) {
    s_batNextMs = now + BAT_SAMPLE_MS;
    batterySample();
  }
}

// 0xFF = 未知（还没采过），屏幕整块不画
static uint8_t batteryDisplayPct() {
  return s_batValid ? s_batPct : 0xFF;
}

#endif // BATTERY_MONITOR_H
