/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 315 MHz report scheduler.
 *
 * Scheduling model
 * ----------------
 * No timer interrupt is used. OOK_SendFrame() runs with interrupts masked
 * for ~163 ms, so a tick interrupt would silently lose time anyway. The
 * main loop instead advances one "slot" every APP_TICK_MS via
 * OOK_DelayMs(), and all intervals below are counted in slots.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "board.h"
#include "ook315.h"
#include "app.h"

/* slot counts derived from the millisecond constants in app.h */
#define SLOT_LONGPRESS       (APP_LONGPRESS_MS   / APP_TICK_MS)
#define SLOT_PAIR_WINDOW     (APP_PAIR_WINDOW_MS / APP_TICK_MS)
#define SLOT_PAIR_BURST      (APP_PAIR_BURST_MS  / APP_TICK_MS)
#define SLOT_HEARTBEAT       (APP_HEARTBEAT_MS   / APP_TICK_MS)

/* ---- presence debounce --------------------------------------------- */
static u8  s_det_raw;
static u8  s_det_stable;
static u8  s_det_cnt;
static u8  s_present;
static u16 s_det_blind;          /* ticks left in the power-up blind time */

/* ---- button / pairing ---------------------------------------------- */
static u8  s_btn_down;
static u16 s_btn_hold;
static u8  s_btn_latched;        /* long press already acted upon      */
static u8  s_pairing;
static u16 s_pair_left;
static u16 s_pair_burst_left;

/* ---- reporting ------------------------------------------------------ */
static u16 s_hb_left;
static u8  s_state_dirty;        /* state changed, needs the 3x burst  */


////////////////////////////////////////
// 采样配对按键 (P3.2, 低电平按下), 长按进入配对模式
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollButton(void)
{
    if (BTN_PRESSED())
    {
        if (s_btn_hold < 0xFFFFu)
        {
            s_btn_hold++;
        }
        s_btn_down = 1u;
    }
    else
    {
        /* release: forget a short press, and re-arm the latch */
        s_btn_hold   = 0u;
        s_btn_down   = 0u;
        s_btn_latched = 0u;
    }

    if ((s_btn_down != 0u) && (s_btn_latched == 0u) &&
        (s_btn_hold >= SLOT_LONGPRESS))
    {
        s_btn_latched = 1u;
        s_pairing     = 1u;
        s_pair_left   = SLOT_PAIR_WINDOW;
        s_pair_burst_left = 0u;         /* fire the first burst at once */
    }
}


////////////////////////////////////////
// 采样人体检测输入 (P3.3), 连续 N 个时隙一致才认可, 消除抖动
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollDetect(void)
{
    /* ignore the sensor while RCWL-0515 settles after power-up */
    if (s_det_blind != 0u)
    {
        s_det_blind--;
        s_det_cnt = 0u;
        return;
    }

    s_det_raw = (u8)(DETECT_ACTIVE() ? 1u : 0u);

    if (s_det_raw != s_det_stable)
    {
        s_det_cnt++;
        if (s_det_cnt >= APP_DETECT_CONFIRM)
        {
            s_det_cnt   = 0u;
            s_det_stable = s_det_raw;
            if (s_det_stable != s_present)
            {
                s_present     = s_det_stable;
                s_state_dirty = 1u;
            }
        }
    }
    else
    {
        s_det_cnt = 0u;
    }
}


////////////////////////////////////////
// 继电器动作: 检测到人体时吸合 (+ 可选蜂鸣器/指示灯)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_DriveRelay(void)
{
    if (s_present != 0u)
    {
        RELAY_ON();
    }
    else
    {
        RELAY_OFF();
    }
}


////////////////////////////////////////
// 组帧并重复发送
// 入口参数: flags (FLAGS 字节), repeat (重复次数)
// 函数返回: 无
////////////////////////////////////////
static void App_Report(u8 flags, u8 repeat)
{
    u8  i;
    u16 gap_ms;

    for (i = 0u; i < repeat; i++)
    {
        OOK_SendFrame(flags);

        /* ---- sample the sensor during the inter-frame gap ----
         *
         * OOK_SendFrame() blocks for ~163 ms and the gap adds 20..59 ms,
         * so a full 3-frame state burst keeps the CPU busy for ~540 ms.
         * While that runs App_PollDetect() is never called, i.e. the node
         * is blind to the sensor for over half a second.
         *
         * If a person walks in exactly during that window, the node only
         * discovers it at the beginning of the next slot, so the reported
         * latency jumps between "one slot" and "one slot + one burst".
         * That was measured as the alternating fast/slow alarm onset.
         *
         * Fix: split the gap into APP_TICK_MS slices and poll the sensor
         * (plus drive the relay) inside it.  A state change that happens
         * mid-burst is then picked up within ~10 ms and the remaining
         * frames of this very burst already carry the new flags.
         */
        gap_ms = (u16)(20u + (OOK_IdByte(0u) % 40u));

        while (gap_ms >= APP_TICK_MS)
        {
            App_PollDetect();
            App_DriveRelay();

            if (s_state_dirty != 0u)
            {
                s_state_dirty = 0u;
                flags = 0u;
                if (s_present != 0u)
                {
                    flags |= OOK_FLAG_PRESENCE;
                }
                /* make sure at least one more frame goes out with the
                 * fresh flags, even if this was the last repeat */
                if ((u8)(i + 1u) >= repeat)
                {
                    repeat = (u8)(i + 2u);
                }
            }

            OOK_DelayMs(APP_TICK_MS);
            gap_ms = (u16)(gap_ms - APP_TICK_MS);
        }
        if (gap_ms != 0u)
        {
            OOK_DelayMs(gap_ms);
        }
    }
}


////////////////////////////////////////
// 报告调度: 状态变化优先, 否则按心跳周期上报
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollReport(void)
{
    u8 flags;

    flags = 0u;
    if (s_present != 0u)
    {
        flags |= OOK_FLAG_PRESENCE;
    }

    if (s_state_dirty != 0u)
    {
        s_state_dirty = 0u;
        s_hb_left     = SLOT_HEARTBEAT;
        App_Report(flags, APP_STATE_REPEAT);
        return;
    }

    if (s_hb_left == 0u)
    {
        s_hb_left = SLOT_HEARTBEAT;
        App_Report(flags, APP_HB_REPEAT);
        return;
    }

    s_hb_left--;
}


////////////////////////////////////////
// 配对模式: 周期性广播 PAIRREQ, 窗口结束后自动恢复
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollPairing(void)
{
    u8 flags;

    if (s_pairing == 0u)
    {
        return;
    }

    if (s_pair_left == 0u)
    {
        s_pairing = 0u;                 /* window over, back to normal   */
        s_hb_left = SLOT_HEARTBEAT;
        return;
    }
    s_pair_left--;

    /* during pairing the relay stays released so the sensor can be
     * carried to the master without clicking */
    RELAY_OFF();

    if (s_pair_burst_left == 0u)
    {
        s_pair_burst_left = SLOT_PAIR_BURST;

        flags = OOK_FLAG_PAIRREQ;
        if (s_present != 0u)
        {
            flags |= OOK_FLAG_PRESENCE;
        }
        App_Report(flags, APP_STATE_REPEAT);
        return;
    }
    s_pair_burst_left--;
}


////////////////////////////////////////
// 应用初始化: 端口模式、P5.4 复位脚还原、外设初始化、上电自检
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
void App_Init(void)
{
#if APP_PORT_TEST
    u8 i;
#endif

    /* P3.2 button stays quasi-bidirectional (internal weak pull-up, the
     * button simply shorts the pin to GND). */
    P3M1 &= (u8)~(P3_KEY_MASK | P3_DETECT_MASK);
    P3M0 &= (u8)~(P3_KEY_MASK | P3_DETECT_MASK);
    P32 = 1;

    /* P3.3 detect input: HIGH-Z INPUT, not quasi-bidirectional.
     * A quasi-bidirectional pin keeps its internal ~33k pull-up, which
     * divides against the required external 10k pull-down and leaves a
     * floating pin balanced at the logic threshold -- it then flips at
     * random and reports a phantom "person detected". High-Z input
     * (M0=0, M1=1) turns that pull-up off so the external pull-down
     * alone decides the level, while the module's push-pull OUT still
     * drives it normally when the module is present. */
    P3M1 |= (u8)P3_DETECT_MASK;
    P33 = 0;

    /* P5.4 继电器 / P5.5 315M 数据: 强推挽输出, 才有足够驱动能力 */
    P5M1 &= (u8)~0x30u;
    P5M0 |= 0x30u;

    /* P5.4 is shared with nRST. Clear RSTCFG.P54RST so the pin is a
     * plain I/O again; otherwise it can never be driven (this is the
     * classic "P5.4 does not toggle" trap). Also keep the low voltage
     * reset disabled so LVDS stays at its 2.0 V default. */
    RSTCFG &= (u8)~RSTCFG_P54RST;

    RELAY_OFF();                        /* relay released at power-up     */

    /* P3.0 / P3.1 stay quasi bi-directional for ISP download */

#if APP_PORT_TEST
    /* ── P5.5 端口自检（用万用表在 5 秒内定位故障）─────────────────
     * 阶段 1: 强推挽输出低 1.5 s      -> 万用表应读 0.0 V
     * 阶段 2: 0.5 Hz 方波翻转 4 s     -> 万用表应明显在 0 / 3.3 V 摆动
     * 阶段 3: 进入正常 OOK 发射
     *
     * 若阶段 1 就读到 3.3 V，说明 App_Init() 根本没被执行到
     * （烧录/复位/时钟问题），与发射代码无关。
     * 若阶段 2 能摆动，说明 P5.5 驱动完全正常，故障只在发射链路。 */
    TX315_LOW();
    OOK_DelayMs(1500u);

    for (i = 0u; i < 4u; i++)
    {
        TX315_HIGH();
        OOK_DelayMs(500u);
        TX315_LOW();
        OOK_DelayMs(500u);
    }
#endif

    OOK_Init();

    s_det_raw     = 0u;
    s_det_stable  = 0u;
    s_det_cnt     = 0u;
    s_present     = 0u;
    s_det_blind   = APP_DETECT_BLIND;
    s_btn_down    = 0u;
    s_btn_hold    = 0u;
    s_btn_latched = 0u;
    s_pairing     = 0u;
    s_pair_left   = 0u;
    s_pair_burst_left = 0u;
    s_hb_left     = SLOT_HEARTBEAT;
    s_state_dirty = 0u;

    /* short settle before the loop starts; the longer RCWL-0515 settling
     * window is handled by s_det_blind in App_PollDetect() */
    OOK_DelayMs(50u);
}


////////////////////////////////////////
// 应用主循环: 每个时隙跑一次 (10 ms)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
void App_Run(void)
{
#if APP_TX_TEST
    /* ── 发射自检：连续发射，占空比 ~89% ──
     * 正常模式每 3000ms 才发 163ms，万用表读到 0V 无法区分好坏。
     * 这里背靠背发射，P5.5 直流电压会到 1.3~1.5V；
     * 若仍是 0V，说明发射链路真的断了（P5.5 虚焊 / RSTCFG 没清掉
     * P54RST 导致 P5 口异常 / 模块没供电）。 */
    App_Report(OOK_FLAG_PRESENCE, 1u);
    OOK_DelayMs(20u);
#else
    App_PollButton();
    App_PollDetect();

    if (s_pairing != 0u)
    {
        App_PollPairing();
    }
    else
    {
        App_DriveRelay();
        App_PollReport();
    }

    OOK_DelayMs(APP_TICK_MS);
#endif
}
