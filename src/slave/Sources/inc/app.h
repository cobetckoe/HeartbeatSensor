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

/* report the unchanged state this often (heartbeat) */
#define APP_HEARTBEAT_MS     3000u

/* repeats per burst on a state change / on a heartbeat */
#define APP_STATE_REPEAT     3u
#define APP_HB_REPEAT        2u

/* the detect input must read the same level this many ticks in a row
 * (5 x 10 ms = 50 ms) before the state is accepted */
#define APP_DETECT_CONFIRM   5u

/* the detect input is ignored for this many ticks right after power-up,
 * so the sensor has time to settle before its OUT pin is trusted.
 * 150 x 10 ms = 1.5 s */
#define APP_DETECT_BLIND     150u

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
 * APP_TX_TEST = 1  ->  ignore the 3 s heartbeat schedule and transmit
 *                      back to back (frame 163 ms + 20 ms gap).
 *                      WHY: in normal operation the duty cycle is only
 *                      ~5 % (163 ms every 3000 ms), so a multimeter on
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

void App_Init(void);
void App_Run(void);

#endif /* __APP_H__ */
