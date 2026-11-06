/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * OOK transmitter implementing the KC433 private protocol.
 * The wire format MUST stay in sync with src/master/OokReceiver.h and
 * docs/PROTOCOL-433M.md -- changing one side without the other breaks
 * reception silently.
 *
 * NOTE ON CARRIER FREQUENCY (project standardised on 433 MHz)
 *   This protocol is pure pulse-width encoding on the DATA pin, so the
 *   carrier frequency is IRRELEVANT to decoding -- only the TX and RX
 *   module pair must share the same band. The project now uses 433 MHz
 *   modules throughout; no decoding code changes are needed, only the
 *   antenna is trimmed for the band (lambda/4 = 17.3 cm on 433 MHz).
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef __OOK433_H__
#define __OOK433_H__

#include "config.h"

/* ---- protocol timing (see docs/PROTOCOL-433M.md section 2-4) ---- */
#define OOK_T_US               350u  /* base time unit, microseconds       */

/* ★★ 前导脉冲数：32 → 20（省 16.8ms/帧，帧长 162.75ms → 145.95ms）
 *
 *   为什么可以砍：前导码的唯一作用是「让超再生接收机的 AGC/峰值检波
 *   建立工作点」，实测建立时间在 1~2ms 量级，32 脉冲（44.8ms）远超所需。
 *   32 这个数字来自早期抄的通用遥控器协议，对单向 OOK 不是必需的。
 *
 *   为什么砍到 20 而不是更少：主机侧 OokReceiver.h 的同步头搜索要求
 *   同步头之前至少有 OOK_PRE_WINDOW = 32 个连续段落在 2T 窗口内，
 *   且其中至少 OOK_PRE_MIN_MATCH = 20 段命中。
 *   20 脉冲 = 40 段（每脉冲 2T 高 + 2T 低 = 2 段），
 *   对「至少 32 段」有 8 段余量，对「至少 20 命中」有 20 段余量。
 *   ★ 若要再往下砍，必须同时把主机侧 OOK_PRE_WINDOW 调小，
 *     两边不同步会导致【静默失联】（帧全被前导码闸门拒掉，不报错）。
 */
#define OOK_PREAMBLE_PULSES     20u  /* 2T square pulses, high level start */

#define OOK_SYNC_LOW_T          16u  /* sync = 1T high + 16T low           */
#define OOK_PAYLOAD_LEN         10u  /* ID(7) + FLAGS(1) + SEQ(1) + CRC(1) */

/* ---- payload field offsets ---- */
#define OOK_OFF_ID              0u
#define OOK_OFF_FLAGS           7u
#define OOK_OFF_SEQ             8u
#define OOK_OFF_CRC             9u

/* ---- FLAGS bit definitions (must match master) ---- */
#define OOK_FLAG_PRESENCE    0x01u   /* 1 = person detected (core switch)  */
#define OOK_FLAG_TAMPER      0x02u   /* 1 = tamper / fault                 */
#define OOK_FLAG_LOWBAT      0x04u   /* 1 = battery low                    */
#define OOK_FLAG_PAIRREQ     0x08u   /* 1 = slave requests pairing         */

/* ---- BOOT flag: 从机刚重启（只在开机后前几帧置位）----
 *
 * 为什么需要这一位（这是现场一次真实误判的教训）：
 *   seq 是 8 位，256 帧回绕一次。从机心跳 2s、状态变化再连发 3 帧，
 *   实测 ~35~40 帧/分 → 一次回绕只要 6~7 分钟。
 *   于是「seq 变小」根本无法区分【从机重启】和【正常回绕】：
 *   主机为这件事做的猜测式检测（窗口 5s 或 60s）全部不可靠：
 *     窗口 5s  → 丢帧严重时相邻两帧间隔常 >5s，必然漏检（rst 恒为 0）
 *     窗口 60s → 与 6 分钟回绕周期太近，会把回绕误判成重启
 *
 * 改用一个专用位之后，判据变成确定性的：
 *   正常运行时这一位【永远不置位】；只要主机看到它，就一定是从机重启了。
 *   另外它还能告诉主机「这一帧的 flags=00 是盲区里猜出来的、不可信」，
 *   避免把盲区假帧当成真实的「无人」而打掉报警。
 *
 * 兼容性：旧固件的 flags 不会带 0x80；主机只当它是普通帧处理，不报错。
 *   新主机 + 旧从机 → 只是拿不到重启诊断，功能不受影响。
 */
#define OOK_FLAG_BOOT        0x80u

/* ---- 433M transmitter data pin is defined in board.h as PIN_TX433 (P5.5) ---- */

/* ROM address of the factory-programmed 7-byte unique ID.
 * STC8G1K08 (8K flash) stores it in the last 7 bytes, 0x1FF9..0x1FFF.
 * Keep the firmware well below 0x1FF9 so a download never overwrites it. */
#define OOK_ID_ADDR           0x1FF9u

/* ---- API ---- */
void OOK_Init(void);
u8   OOK_Crc8(const u8 *pbuf, u8 len);
void OOK_SendFrame(u8 flags);
u8   OOK_IdByte(u8 index);
void OOK_DelayUs(u16 us);
void OOK_DelayMs(u16 ms);

#endif /* __OOK433_H__ */
