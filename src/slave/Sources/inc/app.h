/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 315 MHz report scheduler.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef __APP_H__
#define __APP_H__

#include "config.h"

/* scheduler tick: one application slot every 10 ms */
#define APP_TICK_MS          10u

/* hold the button this long to enter pairing mode */
#define APP_LONGPRESS_MS     2000u

/* how long the node keeps broadcasting PAIRREQ before giving up.
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

/* the detect input is ignored for this many ticks right after power-up,
 * so the sensor has time to settle before its OUT pin is trusted.
 * 300 x 10 ms = 3.0 s
 *
 * WHY 3 s and not 1.5 s: RCWL-0515 needs roughly 2 s after its supply
 * comes up before OUT is meaningful; during warm-up the pin often sits
 * at an indeterminate level.  1.5 s left a window where the first
 * samples were taken from an unsettled sensor. */
#define APP_DETECT_BLIND     300u

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
