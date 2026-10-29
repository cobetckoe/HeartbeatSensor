/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 433 MHz report scheduler.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef __APP_H__
#define __APP_H__

#include "config.h"

/* scheduler tick: one application slot every 10 ms */
#define APP_TICK_MS          10u

/* ---- 按键语义 (P3.2) -----------------------------------------------
 *
 * ★★ 三种状态、两套判据 —— 这是避免「单击/双击混淆」的核心：
 *
 *   状态      单击                          双击
 *   --------  ------------------------------  ---------------
 *   待机      开检测（等 400ms 窗口确认）      进配对（20 s）
 *   检测      关检测（松手【立即】生效）        不识别
 *   配对      退出配对，回待机                 不识别
 *
 * ★★ 所有模式只能从【待机】启动：
 *   于是「关检测」「退出配对」这两条路物理上不可能还有第二下，
 *   根本不需要等窗口 —— 退出永远是秒回的。上一版在检测态也开 400ms
 *   窗口等第二下，结果关检测要白等 0.4 s，现场感受就是「按了没反应」。
 *
 * ★ 配对模式的退出：20 s 超时自动回待机；配对期间单击一下可提前退出。
 *   从机没有 RX，无法感知主机「配对成功」，所以不存在「配对成功即
 *   自动退出」这条路径（详见 App_ExitPairing）。
 *
 * ★★ P3.2 上电默认【待机】：不采样、不吸合继电器、只发 10 秒一次的
 *   「无人」心跳。必须单击一次才真正开始检测。
 *
 *   根因：RCWL-0515 装在从机壳内，上电后会把【从机自己】当成目标，
 *   OUT 直接拉高。原来「上电即检测」= 每台从机一上电就误报有人，
 *   主机所有防区同时亮红、继电器全部吸合，且无法通过安装顺序规避。
 */

/* 短于这个时间的按压当抖动丢弃 (30 ms = 3 个时隙) */
#define APP_CLICK_MIN_MS     30u

/* 长于这个时间的按压整次丢弃 (1200 ms)
 * ★ 绝不折算成单击：按钮被线束压住时若折算成单击，会变成
 *   「反复开关检测」，比不响应更危险。 */
#define APP_CLICK_MAX_MS     1200u

/* 双击判定窗口 (400 ms) —— ★ 只在【待机态】使用
 * 待机态第一次松手后等这么久仍没有第二次按下，就确认这是单击
 * -> 开检测；400ms 内又完成一次按下-松开 -> 双击 -> 立即进配对。
 *
 * ★★ 检测态【不开】这个窗口：那里只认单击，松手立即关检测。
 *   检测态不识别双击 —— 所有模式只能从待机启动，所以「关掉」之后
 *   不存在「还想进配对」的合法路径，第二下按下去也只是待机单击，
 *   会被 s_click_mute 静音吞掉。因此关检测是秒回的，不付 0.4 s 代价。
 *
 * ★ 待机态开检测多等这 400 ms 可以接受：开检测本来还要再等
 *   APP_ARM_BLIND_MS(500 ms) 让人撤离，多 0.4 s 无感。
 *
 * ★ 退出检测后还会静音同样长的一段时间（s_click_mute）：
 *   操作员快速连按两下时，第二下不会被当成「待机单击」把检测又打开。 */
#define APP_DBLCLICK_MS      400u

/* 开启检测前的短盲区 (500 ms = 50 个时隙)
 * 盲区结束那一刻会重新采基线（见 App_PollDetect），避免把
 * 「刚按完按钮还没离开的手」当成持续有人。
 * ★ 这个值不要改小：按下按钮后操作人员需要撤离时间，500ms 只是
 *   让雷达重新建立基线，不构成"启动慢"的主观感受。 */
#define APP_ARM_BLIND_MS     500u

/* ---- 按键指示灯 (P3.2, 纯硬件, 固件不参与) ------------------------
 *
 * 接线：VCC -- LED -- R(330R..1k) -- P3.2 --[按键]-- GND
 *
 * 为什么这样接就能亮：P3.2 是准双向口，平时锁存器 = 1、内部弱上拉
 * 导通，引脚被拉到 VCC，LED 两端等电位 -> 无电流 -> 灯灭。按下按键
 * 把引脚拉到 GND，电流经 LED + R 灌入地 -> 灯亮；同时单片机读到
 * 低电平，按键逻辑照常工作。
 *
 * ★ 固件【完全不驱动】这颗灯，所以：
 *   · P5.4 保持纯继电器输出，不再为"提示灯"让步
 *   · 灯只表示"手按在按钮上"，不表示布防状态
 *   · 不会因为闪烁把继电器带得咔哒响
 *
 * ★ 电阻取值：3.3V 供电、LED 正向压降按 2.0V 估
 *     330R -> 约 4mA（够亮，准双向口的上拉承受得住）
 *     1k   -> 约 1.3mA（偏暗但省电，室内仍可见）
 *   低于 220R 会超出准双向口的驱动能力，灯反而变暗，甚至把
 *   引脚电平抬不到能识别的高电平，按键会读不到。
 */

/* 待机心跳周期 (10 s = 1000 个时隙)
 * ★ 必须远小于主机 ZONE_OFFLINE_MS (45000UL, 见 ZoneManager.h)，
 *   否则待机中的防区会在屏上/APP 上显示【离线】。
 *   每轮 2 帧 ≈ 340 ms 发射，占空比 3.4%
 *   （布防态 2 秒心跳是 16.7%，待机确实省下大部分射频功耗）。
 *   ★ 若现场待机时防区偶尔闪一下离线（链路丢帧是成簇的，
 *     10 秒周期内可能连丢 4 轮），把这个值改小到 5000u。 */
#define APP_STANDBY_HB_MS    10000u

/* 配对模式的超时窗口：这段时间内每 APP_PAIR_BURST_MS 发一串 PAIRREQ，
 * 到点【自动超时退出并回待机】（见 App_ExitPairing）；配对期间单击一下
 * 也可提前退出回待机。
 *
 * ★ 这是从机侧唯一的自动退出路径：433M 对从机是纯单向（只有 TX），
 *   主机配对成功后没法回告从机，所以「配对成功即退出」在从机侧做不到。
 *
 * Must exceed the master's PAIR_TIMEOUT_MS (15000) */
#define APP_PAIR_WINDOW_MS   20000u

/* one PAIRREQ burst every this often while pairing */
#define APP_PAIR_BURST_MS    500u

/* report the unchanged state this often (heartbeat)
 *
 * ★★ 3000 → 2000 的理由（这是「从机→主机延时」的主要来源）
 *
 *   状态【变化】时从机立刻连发 3 帧（s_state_dirty），所以「有人/无人」
 *   的首次上报延时只有 ~215ms，已经很短，不是瓶颈。
 *   真正的延时出现在【3 帧全丢】的时候：此时只能等下一个心跳。
 *   心跳 3000ms → 最坏要白等 3.0s + 帧长 146ms + 解帧 12ms ≈ 3.16s。
 *   改成 2000ms → 最坏 2.16s，用户可感知的「卡顿感」明显下降。
 *   ★ 解帧时间取 12ms 而不是更小的值：RMT 判定一帧结束靠空闲间隔
 *     （OOK_IDLE_US，现为 12000us），从机发完最后一 bit 后主机必须
 *     白等这个空闲间隔才会推一次数据 —— 这是硬性的、不可省的等待。
 *
 *   ★ 为什么不是 1000ms：多机碰撞预算不允许（4 防区时）
 *     - 相位错开把 4 台均匀铺在心跳周期上 → 间距 = 2000/4 = 500ms
 *     - 一次心跳突发 = 2 帧 × (146ms 帧长 + 20~59ms 间隙) ≈ 370ms
 *     - 370ms < 500ms ? 仍有 130ms 余量，不会踩到相邻防区
 *     若改成 1000ms，间距只剩 250ms < 370ms → 相邻防区必然重叠，
 *     等于把「按 ID 错开相位」这个多机共存机制彻底废掉。
 *   ★ 状态突发是 3 帧 ≈ 555ms > 500ms，会盖到下一个防区的心跳 ——
 *     可以接受：状态突发只在变化时发生（偶发），且 3 帧内容完全相同，
 *     即使被踩掉 1~2 帧，主机也已从第 1 帧拿到全部信息。
 *
 *   代价：帧流量 +50%（3s 一轮 → 2s 一轮）。实测链路丢帧是【成簇】的，
 *   帧密度上升会略增碰撞概率，但换来的是延时确定性变好 ——
 *   对「报警不能慢」这个场景，这笔交易是划算的。
 */
#define APP_HEARTBEAT_MS     2000u

/* repeats per burst on a state change / on a heartbeat */
#define APP_STATE_REPEAT     3u
#define APP_HB_REPEAT        2u

/* the detect input must read the same level this many ticks in a row
 * (5 x 10 ms = 50 ms) before the state is accepted */
#define APP_DETECT_CONFIRM   5u

/* ---- port self-test ------------------------------------------------
 *
 * APP_PORT_TEST = 1  ->  at power-up, drive P5.5 LOW for 1.5 s, then
 *                       toggle it at 0.5 Hz for 4 s, and only then start
 *                       the OOK transmitter.
 *
 *   WHY: it splits a "no signal" fault into three cases that a plain
 *   multimeter can tell apart in five seconds, with no extra tooling:
 *
 *     P5.5 does NOT even go LOW   -> the chip is not executing App_Init()
 *                                    (flash / reset / clock problem)
 *     P5.5 swings 0 <-> 3.3 V     -> port config and drive are fine,
 *                                    any remaining fault is in the
 *                                    transmitter path only
 *     P5.5 stuck at 3.3 V all the -> the pin is not being driven by this
 *     time                           code base at all
 *
 *   SET TO 0 FOR PRODUCTION.
 *
 *   Currently 0: the port has been proven good on the bench, and the
 *   5.5 s boot delay would overlap the master's 3 s auto-sniff window,
 *   filling the capture with the 0.5 Hz test square wave instead of the
 *   OOK frame. Flip back to 1 only when a fresh board needs triage.
 */
#define APP_PORT_TEST        0u

/* ---- transmitter self-test ------------------------------------------
 *
 * APP_TX_TEST = 1  ->  ignore the heartbeat schedule and transmit
 *                      back to back (frame 146 ms + 20 ms gap).
 *                      WHY: in normal operation the duty cycle is only
 *                      ~7 % (146 ms every 2000 ms), so a multimeter on
 *                      P5.5 reads 0 V whether or not the transmitter
 *                      works.  In test mode the duty cycle is ~89 %, so
 *                      P5.5 reads about 1.3-1.5 V DC when transmitting
 *                      and a flat 0 V when it is not.  That is the only
 *                      way to prove the TX path with a multimeter.
 *
 * APP_TX_TEST = 0  ->  normal operation.  SHIP WITH 0.
 *
 * Currently 0 (production).  The TX path has been proven on the bench
 * (master decodes CRC-valid frames, ok>0, crc=0), so the back-to-back
 * test burst is no longer needed and would only burn power at ~89 %
 * duty instead of the normal 5 %.
 */
#define APP_TX_TEST          0u

/* ---- detect-input self-test ----------------------------------------
 *
 * APP_DETECT_TEST = 1  ->  skip the report / pairing state machine and
 *                          mirror P3.3 straight onto the relay:
 *                          P3.3 HIGH -> relay clicks (indicator on)
 *                          P3.3 LOW  -> relay releases
 *
 *   WHY: the complaint "the module reads HIGH on its own but sits at
 *   LOW once wired to the slave" cannot be settled from the master
 *   side -- the master only ever sees a PRESENCE flag, already filtered
 *   by the blind window and the 5-tick confirmation.  Mirroring the raw
 *   pin onto the relay turns it into an immediate yes/no answer with
 *   nothing but a listening ear, and it runs the pin through exactly
 *   the same DETECT_ACTIVE() macro the production path uses, so a port
 *   fault cannot hide behind the application logic.
 *
 *   Reading it:
 *     relay follows the module -> the port is fine, look at the flag /
 *                                 report path instead
 *     relay never moves        -> the pin itself is being held low;
 *                                 check the 10k pull-down, the module
 *                                 supply voltage, and the OUT type
 *
 * APP_DETECT_TEST = 0  ->  normal operation.  SHIP WITH 0.
 */
#define APP_DETECT_TEST      0u

void App_Init(void);
void App_Run(void);

#endif /* __APP_H__ */
