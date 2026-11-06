/*
 * DebugLog.h — 稳态【零堆分配】日志 + 复位原因
 * ============================================================
 * ★ 为什么单独放头文件（别搬回 master.ino 里）：
 *   PlatformIO 把 .ino 转成 .cpp 时，会在【文件里第一个函数定义之前】
 *   统一插入所有函数原型。一旦 master.ino 里出现早于 struct 定义的
 *   函数定义，原型就会被插到 struct 之前，报：
 *       'UartConfig' was not declared in this scope
 *   把函数放进头文件、在顶部 #include，原型插入点就保持在 setup()
 *   附近，不会再撞上前面的类型声明。
 *
 * ★ 为什么不用 Serial.printf()：
 *   Arduino-ESP32 的 Print::printf() 内部只有 64 字节栈缓冲，
 *   格式化结果超过 64 字节就会 malloc(len+1) → write → free。
 *   我们的 1Hz 诊断行全都超长：
 *       [OOK-STAT] ~110B / [PERF] ~135B / [PFSEG] ~85B / [HEALTH] ~110B
 *   → 每秒固定数次堆分配+释放，连续跑几小时会把堆挖成碎片。
 *   而 GFXcanvas16 需要 40KB（160x128x2）【连续块】，
 *   一旦碎片化到要不到连续块，推帧就会失败甚至卡死。
 *   本文件用固定栈缓冲 + vsnprintf + Serial.write：稳态不再分配。
 * ============================================================
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>
#include <stdarg.h>
#include <esp_system.h>
#include <esp_rom_sys.h>

// 固定 224 字节栈缓冲，足够容纳现有最长日志行（~135B）。
//
// ★★★ 为什么 write() 之前必须先查 availableForWrite() —— 这是真机死机的根因 ★★★
//   2024 现场「烧录后偶然死机」，核心转储解出来的栈是：
//       task_wdt_isr → abort()
//       ← loopTask 卡在 HWCDC::write() 内部的 delay(1) 永不出来
//   展开 Arduino core 的 HWCDC::write()（HWCDC.cpp:441~470）：
//       uint32_t tries = tx_timeout_ms;        // ← 我们设的是 0
//       while (connected && to_send) {
//           ... xRingbufferSend(..., tx_timeout_ms/portTICK_PERIOD_MS)
//           if (last_toSend == to_send) { tries--; delay(1); }   // 没进展
//           if (tries == 0) { connected = false; }               // 逃生门
//       }
//   tx_timeout_ms = 0 时：环形缓冲区满 → space = 0 → 送 0 字节 →
//   to_send 不变 → 走「没进展」分支 → tries-- 发生【无符号下溢】变成
//   4294967295 → 「tries == 0」这条唯一的逃生门永远不成立 → connected
//   永远保持 true → 无限循环 delay(1)。
//   每轮 delay(1) 都让出 CPU，所以不触发 INT_WDT，但 loop() 永不返回，
//   main.cpp 里 loop 之后的 esp_task_wdt_reset() 也就永不被调用 →
//   5 秒后 TASK_WDT 触发 → panic → 重启。这就是「偶然死机」。
//   ?? 复现条件：USB 插着（isPlugged 为真，才会走阻塞分支），但主机端
//      没有人来取数据（串口监视器没开 / 被占用 / 主机挂起）。
//      所以「烧录后接着观察」最容易撞上，拔掉 USB 单独跑反而不复现。
//   ★ 双重防护：
//     1) 这里先查剩余空间，放不下就【整条丢弃】，绝不进 write()；
//     2) setup() 里 setTxTimeoutMs 必须给【非 0】值（见 master.ino）。
//   注意：查完到 write 之间仍可能与 tuya 任务抢空间，所以第 2 条不能省。
static void dbgLog(const char *fmt, ...) {
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  // 环形缓冲区装不下就丢弃 —— 日志永远不值得拖住主循环
  if (Serial.availableForWrite() < n) return;
  Serial.write((const uint8_t *)buf, (size_t)n);
}

// ── 复位原因 ──
//   ★ 现场说「死机」时，这一条直接给出答案，不必猜：
//     POWERON / SW        = 正常上电或软件重启（程序没崩，是别的原因）
//     PANIC               = 程序异常（空指针/越界/栈溢出），串口会有 Guru Meditation
//     INT_WDT / TASK_WDT  = 看门狗：主循环被长时间阻塞
//     BROWNOUT            = 电源被拉垮 —— 震动马达 + 雷达 + 433M 模块同时用电时最常见
//   若反复看到 BROWNOUT，问题在供电/硬件，改代码没用。
//
//   ?? 为什么还要打印 raw（ROM 原始码）：
//     esp_reset_reason() 在部分情况下返回 ESP_RST_UNKNOWN
//     —— 例如刚用 esptool 烧录后的那次启动（USB-JTAG 复位不给原因），
//     或者 C3 上 0x01 同时代表 上电/掉电/超级看门狗 三者之一。
//     原始码能进一步区分：
//       0x03 CORE_SW      = 软件复位
//       0x0B CPU0_MWDT0   = CPU0 主看门狗（= 任务看门狗，主循环被卡住）
//       0x0F SYS_BROWNOUT = 掉电复位
//       0x10 SYS_RTC_WDT  = RTC 看门狗
//       0x15 CORE_USB_UART= USB 串口复位（就是烧录后那次）
static inline const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_EXT:       return "EXT";
    default:                return "UNKNOWN";
  }
}

// ROM 层原始复位码（C3 上比 esp_reset_reason() 更细）。
static inline int resetRawCode() {
  return (int)esp_rom_get_reset_reason(0);
}

// ★ RAW 码 → 名字。
//   为什么需要：esp_reset_reason() 对部分 C3 复位源直接返回 ESP_RST_UNKNOWN
//   （枚举里根本没有 USB 项），这时只有 raw 码能告诉我们到底是谁重启的。
//   ★★ 最容易误判的一个：0x15 = CORE_USB_UART = 【烧录后那次重启】，
//      不是崩溃！看到 reset=UNKNOWN raw=0x15 就是「刚烧完」，不必排查。
//   真正的崩溃码：
//     0x0B CPU0_MWDT0    → 任务看门狗（主循环被卡住，最符合「死机」描述）
//     0x07/0x08/0x11 MWDT0/1 → 同上，不同核/定时器
//     0x0F SYS_BROWNOUT  → 供电被拉垮（震动马达+雷达+433M 同时用电）
//     0x10 SYS_RTC_WDT   → RTC 看门狗
//     0x12 SYS_SUPER_WDT → 超级看门狗
//     0x03/0x0C CORE_SW/CPU0_SW → 软件主动重启（非崩溃）
//   名字取自 soc/reset_reasons.h（esp32c3 SDK）。
static inline const char *resetRawStr() {
  switch (resetRawCode()) {
    case 0x01: return "CHIP_POWER_ON";
    case 0x03: return "CORE_SW";
    case 0x05: return "CORE_DEEP_SLEEP";
    case 0x07: return "CORE_MWDT0";
    case 0x08: return "CORE_MWDT1";
    case 0x09: return "CORE_RTC_WDT";
    case 0x0B: return "CPU0_MWDT0";
    case 0x0C: return "CPU0_SW";
    case 0x0D: return "CPU0_RTC_WDT";
    case 0x0F: return "SYS_BROWNOUT";
    case 0x10: return "SYS_RTC_WDT";
    case 0x11: return "CPU0_MWDT1";
    case 0x12: return "SYS_SUPER_WDT";
    case 0x13: return "SYS_CLK_GLITCH";
    case 0x14: return "CORE_EFUSE_CRC";
    case 0x15: return "CORE_USB_UART";   // 烧录后重启，不是崩溃
    case 0x16: return "CORE_USB_JTAG";
    case 0x17: return "CORE_PWR_GLITCH";
    default:   return "?";
  }
}

#endif  // DEBUG_LOG_H
