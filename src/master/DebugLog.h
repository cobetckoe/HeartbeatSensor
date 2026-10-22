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
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>
#include <stdarg.h>
#include <esp_system.h>
#include <esp_rom_sys.h>

// 固定 224 字节栈缓冲，足够容纳现有最长日志行（~135B）。
static void dbgLog(const char *fmt, ...) {
  char buf[224];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  Serial.write((const uint8_t *)buf, (size_t)n);
}

// ── 复位原因 ──
//   ★ 现场说「死机」时，这一条直接给出答案，不必猜：
//     POWERON / SW        = 正常上电或软件重启（程序没崩，是别的原因）
//     PANIC               = 程序异常（空指针/越界/栈溢出），串口会有 Guru Meditation
//     INT_WDT / TASK_WDT  = 看门狗：主循环被长时间阻塞
//     BROWNOUT            = 电源被拉垮 —— 震动马达 + 雷达 + 315M 模块同时用电时最常见
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

#endif  // DEBUG_LOG_H
