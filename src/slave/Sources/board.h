/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 * Board pin map.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#include "config.h"

/* ------------------------------------------------------------------------
 * STC8G1K08A (SOP8) pin-out
 *
 *   pin 1  P5.4  RELAY      active low -> relay driver transistor
 *   pin 2  Vcc
 *   pin 3  P5.5  TX315      OOK data into the 315 MHz T2 transmitter
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
 * EXTERNAL RESISTOR (not optional):
 *   P3.3 is a HIGH-Z INPUT, so it has no internal pull device of its
 *   own.  Anything that can be unplugged, or whose output can be
 *   high-impedance, needs a defined idle level in hardware:
 *
 *     DETECT_ACTIVE_HIGH = 1  ->  10k PULL-DOWN from P3.3 to GND
 *     DETECT_ACTIVE_HIGH = 0  ->  10k PULL-UP   from P3.3 to 3.3 V
 *
 *   Prefer the pull-up variant whenever the module output may be
 *   open-collector: with no pull-up such an output can never read HIGH,
 *   which looks exactly like "the sensor never triggers".
 *
 * PIN MODE (see App_Init): high-Z input, NOT quasi-bidirectional.  A
 *   quasi-bidirectional pin keeps an internal ~33k pull-up that fights
 *   any external resistor and parks an unconnected line near the logic
 *   threshold, where it flaps at random.  High-Z input turns that
 *   internal pull-up off so the external resistor alone sets the level.
 *
 * LEVEL SHIFTING: if the module runs from 5 V and its OUT is push-pull,
 *   its high level is 5 V, above this 3.3 V part's rating.  Add a series
 *   resistor (4.7k..10k) or a divider in that case. */
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

/* ---- 315 MHz OOK transmitter data ---------------------------------- */
#define PIN_TX315         P55
#define TX315_HIGH()      (PIN_TX315 = 1)
#define TX315_LOW()       (PIN_TX315 = 0)

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
#define P5_PUSHPULL_MASK  0x30u   /* P5.4 (relay) + P5.5 (TX315) as outputs */

#endif /* __BOARD_H__ */
