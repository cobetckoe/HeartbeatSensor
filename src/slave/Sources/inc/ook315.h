/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * OOK transmitter implementing the KC315 private protocol.
 * The wire format MUST stay in sync with src/master/OokReceiver.h and
 * docs/PROTOCOL-315M.md -- changing one side without the other breaks
 * reception silently.
 *
 * NOTE ON CARRIER FREQUENCY
 *   This protocol is pure pulse-width encoding on the DATA pin, so the
 *   carrier (315 MHz / 433.92 MHz) is IRRELEVANT to decoding -- only the
 *   TX and RX module pair must share the same band. Swapping a 315M T2 for
 *   a 433M one needs no code change whatsoever; only re-tune the antenna.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef __OOK315_H__
#define __OOK315_H__

#include "config.h"

/* ---- protocol timing (see docs/PROTOCOL-315M.md section 2-4) ---- */
#define OOK_T_US               350u  /* base time unit, microseconds       */
#define OOK_PREAMBLE_PULSES     32u  /* 2T square pulses, high level start */
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

/* ---- 315M transmitter data pin is defined in board.h as PIN_TX315 (P5.5) ---- */

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

#endif /* __OOK315_H__ */
