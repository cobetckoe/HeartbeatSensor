/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 * Board pin map.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "config.h"

/* ------------------------------------------------------------------------
 * STC8G1K08A (SOP8) pin-out
 *
 *   pin 1  P5.4  RELAY      active low -> relay driver transistor
 *   pin 2  Vcc
 *   pin 3  P5.5  TX433      OOK data into the 433 MHz T2 transmitter
 *   pin 4  Gnd
 *   pin 5  P3.0  RXD        reserved for ISP download
 *   pin 6  P3.1  TXD        reserved for ISP download
 *   pin 7  P3.2  BTN_PAIR   active low push button to GND
 *   pin 8  P3.3  DETECT     RCWL-0515 OUT (polarity selectable)
 *
 * P5.4 is multiplexed with nRST. It only works as an output while
 * RSTCFG.P54RST is 0 -- see App_Init().
 * ---------------------------------------------------------------------- */

/* ---- relay ---------------------------------------------------------- */
#define PIN_RELAY         P54
#define RELAY_ON()        (PIN_RELAY = 0)
#define RELAY_OFF()       (PIN_RELAY = 1)

/* ---- pairing button ------------------------------------------------- */
#define PIN_BTN           P32
#define BTN_PRESSED()     (PIN_BTN == 0)

/* ---- presence input: RCWL-0515 microwave radar (pin 8) --------------
 *
 * POLARITY IS SELECTABLE.  The idle / detect levels of this module have
 *   not been confirmed on real hardware, so both variants are supported.
 *   Measure first (see the procedure below), then pick:
 *
 *     DETECT_ACTIVE_HIGH = 1   OUT idles LOW, goes HIGH on detection
 *     DETECT_ACTIVE_HIGH = 0   OUT idles HIGH, is pulled LOW on detection
 *                              (typical of an open-collector output)
 *
 * EXTERNAL RESISTOR (still required, even though the pin is now quasi-
 *   bidirectional):
 *
 *   The pin's own weak pull-up is far too weak and far too HIGH to act
 *   as a reference: it pulls an unconnected line up to VCC, which with
 *   DETECT_ACTIVE_HIGH = 1 reads as "presence" -- i.e. an unplugged
 *   sensor would hold the relay on forever.  A real resistor is what
 *   defines the idle level:
 *
 *     DETECT_ACTIVE_HIGH = 1  ->  10k PULL-DOWN from P3.3 to GND
 *     DETECT_ACTIVE_HIGH = 0  ->  10k PULL-UP   from P3.3 to 3.3 V
 *
 *   With the 10k pull-down fitted the idle line sits at ~0.8 V (the
 *   internal ~30k weak pull-up barely lifts it), a clean logic LOW, and
 *   an unplugged sensor reads "nobody" -- the fail-safe direction.
 *
 *   Prefer the pull-up variant whenever the module output may be
 *   open-collector: with no pull-up such an output can never read HIGH,
 *   which looks exactly like "the sensor never triggers".
 *
 * PIN MODE (see App_Init): QUASI-BIDIRECTIONAL (M1=0, M0=0), latch = 1.
 *
 *   Why NOT high-Z (M1=1, M0=0): a high-Z pin has no internal pull
 *   device at all, so with the sensor unplugged -- or during the first
 *   milliseconds while the module's OUT is still settling -- the level
 *   is set by leakage alone and is unpredictable.  The quasi-
 *   bidirectional weak pull-up (~30k) supplies a defined level for free.
 *
 *   Why this does NOT flap: the weak pull-up drives an unconnected pin
 *   firmly to VCC, so it reads a solid HIGH, not a threshold-level
 *   chatter.  The chatter everyone worries about is a high-Z phenomenon.
 *
 *   The trade-off points the OTHER way: with the sensor unplugged the
 *   pin reads HIGH, i.e. "presence", so the relay would latch on.  The
 *   external 10k pull-down below cancels that -- 3.3 V * 10k / (10k +
 *   30k) ~= 0.8 V, a clean LOW, so an unplugged sensor reads "nobody".
 *
 *   Why the internal pull-up array (PxPU) must stay OFF: P3PU adds a
 *   strong ~4.1k pull-up, and against the 10k pull-down that gives
 *   3.3 V * 10k / 14.1k ~= 2.3 V -- ABOVE the logic threshold, so an
 *   idle line would read HIGH and every node would false-alarm forever.
 *
 * LEVEL SHIFTING: the module is confirmed to run from 3.3 V on this
 *   build, so its high level is already within this 3.3 V part's range
 *   and no divider is needed.  Only if a future revision powers the
 *   module from 5 V would a series resistor (4.7k..10k) or a divider
 *   become necessary, since a 5 V push-pull high would exceed the
 *   rating of this part. */
#define PIN_DETECT        P33

#define DETECT_ACTIVE_HIGH   1

#if DETECT_ACTIVE_HIGH
#define DETECT_ACTIVE()   (PIN_DETECT != 0)
#else
#define DETECT_ACTIVE()   (PIN_DETECT == 0)
#endif

/* P3 pin masks (bits of P3M0 / P3M1) */
#define P3_KEY_MASK       0x04u      /* P3.2 pairing button        */
#define P3_DETECT_MASK    0x08u      /* P3.3 RCWL-0515 presence in */

/* ---- 433 MHz OOK transmitter data ---------------------------------- */
#define PIN_TX433         P55
#define TX433_HIGH()      (PIN_TX433 = 1)
#define TX433_LOW()       (PIN_TX433 = 0)

/* ---- RSTCFG (0xFF) -------------------------------------------------- */
/* B6 ENLVR, B4 P54RST, B1:B0 LVDS
 *   P54RST = 0 -> RST pin works as plain I/O (P5.4)
 *   P54RST = 1 -> RST pin works as active-low reset input
 * The STC-ISP option "P5.4 as reset pin" latches this bit at power-on,
 * so software must clear it on every boot to use P5.4 as an output. */
#define RSTCFG_P54RST     0x10u

/* ---- port mode values (PxM0/PxM1) ----------------------------------- */
#define PORTM_QUASI       0x00u   /* M0=0 M1=0 weak pull-up, bi-directional */
#define PORTM_PUSHPULL    0x10u   /* M0=1 M1=0 strong output (per pin mask) */
#define P5_PUSHPULL_MASK  0x30u   /* P5.4 (relay) + P5.5 (TX433) as outputs */

#endif /* __BOARD_H__ */
