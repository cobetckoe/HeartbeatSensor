/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 433 MHz report scheduler.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef __APP_H__
#define __APP_H__

#include "config.h"

/* 调度时隙: 每 10 ms 推进一次 */
#define APP_TICK_MS          10u

/* ---- 按键语义 (P3.2, 低电平按下) ------------------------------------
 *
 *   待机  单击 -> 开检测          长按 1.5 s -> 进配对(20 s)
 *   检测  单击 -> 关检测          长按        -> 同样关检测
 *   配对  单击 -> 退配对回待机     长按        -> 同样退配对
 *
 * 长按时长与主机 BTN_LONG_PAIR_MS(1500 ms) 一致。
 *
 * 全部动作都在【松手那一拍】完成, 没有任何等待窗口: 按一下松手即生效,
 * 不需要"等 400 ms 看看有没有第二下"。长按因此与单击完全不冲突。
 *
 * 上电默认待机: 不采样、不吸合继电器、只发 10 s 慢心跳。
 * (RCWL-0515 装在壳内, 上电即检测会把自己当成目标而误报)
 */

/* 短于这个时间的按压当抖动丢弃 (30 ms = 3 个时隙) */
#define APP_CLICK_MIN_MS     30u

/* 待机态单击的上限 (1200 ms), 超过则整次丢弃, 不折算成单击:
 * 按钮被线束压住时折算成单击会变成「反复开关检测」。
 * 检测/配对态的退出动作没有上限 —— 按住不放也必须能关掉。 */
#define APP_CLICK_MAX_MS     1200u

/* 待机态长按进入配对的时长 (1500 ms), 与主机 BTN_LONG_PAIR_MS 一致,
 * 现场两只手同时按住即可。与单击上限 1200 ms 之间留出 300 ms 空档,
 * 手抖的按压不会误进配对。
 * 上限 5000 ms 与主机 BTN_LONG_RESET_MS 对齐: 更长的按压视为按钮被
 * 压住, 丢弃, 免得线束挤压按钮几分钟后松开就莫名进配对。 */
#define APP_PAIR_HOLD_MS     1500u
#define APP_PAIR_HOLD_MAX_MS 5000u

/* 退出检测/配对后的静音窗口 (400 ms): 吞掉紧跟其后的那一下,
 * 避免「连按两下」把刚关掉的检测又打开。 */
#define APP_CLICK_MUTE_MS    400u

/* 开启检测前的短盲区 (500 ms): 盲区结束才重采基线, 避免把
 * 「刚按完按钮还没离开的手」当成持续有人。 */
#define APP_ARM_BLIND_MS     500u

/* ---- 按键指示灯 (P3.2, 纯硬件, 固件不参与) --------------------------
 *
 * 接线: VCC -- LED -- R(330R..1k) -- P3.2 --[按键]-- GND
 *
 * P3.2 平时锁存器=1, 弱上拉把引脚拉到 VCC, LED 两端等电位不亮;
 * 按下按键把引脚拉到 GND, 电流经 LED 灌入地, 灯亮, 同时读到低电平。
 * 灯只表示「手按在按钮上」, 不表示布防状态。
 * 330R 约 4 mA; 低于 220R 超出准双向口驱动能力, 灯反而变暗。
 */

/* 待机心跳周期 (10 s)。必须远小于主机 ZONE_OFFLINE_MS(45 s),
 * 否则待机中的防区会显示离线。每轮 2 帧约 340 ms, 占空比 3.4%。
 * 若现场待机偶尔闪离线, 改小到 5000u。 */
#define APP_STANDBY_HB_MS    10000u

/* 配对超时窗口 (20 s): 期间每 APP_PAIR_BURST_MS 发一串 PAIRREQ,
 * 到点自动回待机; 配对中单击可提前退出。
 * 从机没有 RX, 无法感知主机配对成功, 因此没有「配对成功即退出」。
 * Must exceed the master's PAIR_TIMEOUT_MS (15000) */
#define APP_PAIR_WINDOW_MS   20000u

/* 配对期间每这么久发一串 PAIRREQ */
#define APP_PAIR_BURST_MS    500u

/* 心跳周期 (2 s, 布防态): 状态不变时按此周期重发。
 *
 * 状态变化时立刻连发 3 帧, 所以「有人/无人」首次上报延时约 215 ms。
 * 只有 3 帧全丢才要等下一个心跳, 最坏 2 s + 帧长 146 ms + 解帧 12 ms。
 * 再小到 1000 ms 不行: 4 防区相位间距只剩 250 ms, 小于一次突发
 * 370 ms, 相邻防区必然重叠, 按 ID 错开相位的机制就失效了。
 * (解帧取 12 ms = 主机 RMT 判帧尾的 OOK_IDLE_US 空闲间隔)
 */
#define APP_HEARTBEAT_MS     2000u

/* 一次突发重复的帧数: 状态变化 / 心跳 */
#define APP_STATE_REPEAT     3u
#define APP_HB_REPEAT        2u

/* 检测输入必须连续这么多个时隙读到同一电平才认可 (5 x 10 ms = 50 ms) */
#define APP_DETECT_CONFIRM   5u

/* ---- 端口自检 -------------------------------------------------------
 *
 * 1 = 上电时 P5.5 先拉低 1.5 s, 再以 0.5 Hz 翻转 4 s, 之后才启动发射。
 * 万用表即可区分: 不拉低 = App_Init() 没执行; 会摆动 = 端口正常,
 * 故障在发射链路。量产必须为 0 (5.5 s 开机延时会占满主机嗅探窗口)。
 */
#define APP_PORT_TEST        0u

/* ---- 发射自检 -------------------------------------------------------
 *
 * 1 = 忽略心跳调度, 背靠背发射(帧 146 ms + 间隙 20 ms), 占空比约 89%。
 * 正常占空比只有 7%, 万用表在 P5.5 上永远读到 0 V; 测试模式下会读到
 * 1.3~1.5 V DC, 这是用万用表验证发射通路的唯一办法。量产必须为 0。
 */
#define APP_TX_TEST          0u

/* ---- 检测脚自检 -----------------------------------------------------
 *
 * 1 = 跳过上报/配对状态机, 把 P3.3 原样镜像到继电器:
 * 高电平吸合、低电平释放。
 * 用于区分「模块自身读高」和「接上从机后被拉低」: 继电器跟随模块说明
 * 端口正常, 该查 flags/上报路径; 继电器不动说明引脚被摁住。
 * 量产必须为 0。
 */
#define APP_DETECT_TEST      0u

void App_Init(void);
void App_Run(void);

#endif /* __APP_H__ */
