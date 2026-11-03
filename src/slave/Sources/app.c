/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 433 MHz report scheduler.
 *
 * 一个时隙 = APP_TICK_MS(10 ms), 由主循环用 OOK_DelayMs() 推进。
 * 不使用定时器中断: OOK_SendFrame() 期间 EA=0, 中断只会丢时间。
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "board.h"
#include "ook433.h"
#include "app.h"

/* slot counts derived from the millisecond constants in app.h */
#define SLOT_PAIR_WINDOW     (APP_PAIR_WINDOW_MS / APP_TICK_MS)
#define SLOT_PAIR_BURST      (APP_PAIR_BURST_MS  / APP_TICK_MS)
#define SLOT_HEARTBEAT       (APP_HEARTBEAT_MS   / APP_TICK_MS)
#define SLOT_STANDBY_HB      (APP_STANDBY_HB_MS  / APP_TICK_MS)
#define SLOT_CLICK_MIN       (APP_CLICK_MIN_MS   / APP_TICK_MS)
#define SLOT_CLICK_MAX       (APP_CLICK_MAX_MS   / APP_TICK_MS)
#define SLOT_CLICK_MUTE      (APP_CLICK_MUTE_MS  / APP_TICK_MS)
#define SLOT_PAIR_HOLD       (APP_PAIR_HOLD_MS      / APP_TICK_MS)
#define SLOT_PAIR_HOLD_MAX   (APP_PAIR_HOLD_MAX_MS  / APP_TICK_MS)
#define SLOT_ARM_BLIND       (APP_ARM_BLIND_MS   / APP_TICK_MS)

////////////////////////////////////////
// 按设备 ID 计算心跳相位偏移 (单位: 时隙)
// 入口参数: 无
// 函数返回: 0 .. SLOT_HEARTBEAT-1
//
// 纯单向 OOK 没有载波侦听, 多台同相上电后心跳会永远撞在一起, 两帧一起
// 毁掉。用出厂唯一 ID 散列成确定值, 断电重启后相位关系保持不变。
////////////////////////////////////////
static u16 App_PhaseOffsetSlots(void)
{
    u16 h;

    /* 移位异或散列: ID 里挑 3 个字节参与, 少占代码空间 */
    h  = (u16)((u16)OOK_IdByte(0u) << 4);
    h ^= (u16)((u16)OOK_IdByte(3u) << 8);
    h ^= (u16)OOK_IdByte(5u);
    h ^= (u16)(h >> 7);

    return (u16)(h % SLOT_HEARTBEAT);
}

/* ---- presence debounce --------------------------------------------- */
static u8  s_det_raw;
static u8  s_det_stable;
static u8  s_det_cnt;
static u8  s_present;
static u16 s_det_blind;          /* 布防后盲区剩余时隙 */

/* ---- 布防状态 ------------------------------------------------------
 * 0 = 待机(上电默认): 不采样、不报警、只发慢心跳
 * 1 = 布防: 正常检测 + 报警 + 2 秒心跳
 * App_PollDetect / App_DriveRelay / App_PollReport 三处都受它控制。 */
static u8  s_armed;

/* ---- button / pairing ---------------------------------------------- */
static u8  s_btn_down;
static u16 s_btn_hold;           /* 本次按压已持续的时隙数               */
static u8  s_btn_tx;             /* 本次按压跨越过发射块, 时长被低估     */
static u8  s_btn_lock;           /* 去抖: 登记一次点击后静音 30 ms       */
static volatile u8 s_btn_edge;   /* INT0 锁存的按下边沿                 */
static u16 s_click_mute;         /* 刚退出检测/配对后的静音窗口(时隙)    */
static u8  s_pairing;
static u16 s_pair_left;
static u16 s_pair_burst_left;

/* ---- reporting ------------------------------------------------------ */
static u16 s_hb_left;
static u8  s_state_dirty;        /* state changed, needs the 3x burst  */

/* 上电相位错开剩余时隙; 真实报警(PRESENCE)不受它影响 */
static u16 s_phase_left;

/* 开机标志剩余帧数; 减到 0 后不再置 OOK_FLAG_BOOT */
static u8  s_boot_frames;

/* 开机后携带 BOOT 标志的帧数: 8 帧 = 4 次心跳 = 8 s */
#define APP_BOOT_FRAMES  8u


////////////////////////////////////////
// 进入配对模式 (只能在待机态长按 P3.2 触发)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_EnterPairing(void)
{
    s_pairing         = 1u;
    s_pair_left       = SLOT_PAIR_WINDOW;
    s_pair_burst_left = 0u;         /* 立即发第一串 PAIRREQ */

    /* 进配对必须彻底退出布防, 退出后停在待机 */
    s_armed       = 0u;
    s_det_blind   = 0u;
    s_det_raw     = 0u;
    s_det_stable  = 0u;
    s_det_cnt     = 0u;
    s_present     = 0u;
    s_state_dirty = 1u;

    /* 清掉静音窗口: 长按松手后不该再吞掉配对窗口里的下一次点击 */
    s_click_mute = 0u;

    RELAY_OFF();
}


////////////////////////////////////////
// 当前应使用的心跳周期 (单位: 时隙)
// 入口参数: 无
// 函数返回: SLOT_HEARTBEAT(2 s) 或 SLOT_STANDBY_HB(10 s)
//
// 带 BOOT 标志的帧未发完时用快节奏, 主机才能尽快退出盲区假帧保护。
////////////////////////////////////////
static u16 App_HbPeriod(void)
{
    if ((s_armed != 0u) || (s_boot_frames != 0u))
    {
        return (u16)SLOT_HEARTBEAT;
    }
    return (u16)SLOT_STANDBY_HB;
}


////////////////////////////////////////
// 退出配对模式, 回到待机
// 入口参数: 无
// 函数返回: 无
//
// 20 s 超时与配对中单击两条路径都收敛到这里。从机没有 RX, 收不到主机
// 的配对结果, 所以不存在「配对成功即退出」。
////////////////////////////////////////
static void App_ExitPairing(void)
{
    s_pairing         = 0u;
    s_pair_left       = 0u;
    s_pair_burst_left = 0u;

    s_armed       = 0u;
    s_det_blind   = 0u;
    s_det_raw     = 0u;
    s_det_stable  = 0u;
    s_det_cnt     = 0u;
    s_present     = 0u;

    /* 刚松开的那一下不能再当成待机单击 */
    s_click_mute = SLOT_CLICK_MUTE;

    /* 立刻补一帧「待机 / 无人」, 让主机刷新该防区 */
    s_state_dirty = 1u;
    s_hb_left     = App_HbPeriod();

    /* 相位闸门清零: 相位门排在 s_state_dirty 之前, 重新装偏移会把
     * 上面那一帧推迟最多一个心跳周期。 */
    s_phase_left = 0u;

    RELAY_OFF();
}


////////////////////////////////////////
// 切换 布防 / 待机
// 入口参数: on = 1 开启检测, 0 回到待机
// 函数返回: 无
////////////////////////////////////////
static void App_SetArmed(u8 on)
{
    if (on != 0u)
    {
        /* 开启检测时走一次短盲区(500 ms), 盲区结束时重采基线:
         * 按按钮的手此刻还贴在天线前, 立即采样会被判成有人。 */
        s_armed     = 1u;
        s_det_blind = SLOT_ARM_BLIND;
        s_det_cnt   = 0u;
        s_hb_left   = App_HbPeriod();   /* 布防后恢复 2 秒心跳 */
    }
    else
    {
        /* 回待机必须立刻放开继电器、清成无人, 并马上补一帧 */
        s_armed      = 0u;
        s_det_blind  = 0u;
        s_det_raw    = 0u;
        s_det_stable = 0u;
        s_det_cnt    = 0u;
        s_present    = 0u;
        s_hb_left    = App_HbPeriod();
        RELAY_OFF();
    }

    s_state_dirty = 1u;
}


////////////////////////////////////////
// INT0: 只锁存「按下过」
//
// OOK_SendFrame() 发射时 EA=0 忙等约 146 ms, 这期间扫不到电平, 但下降沿
// 仍被硬件记进 IE0, 恢复中断后立即进本函数。整段落在发射块里的快速按压
// 因此不会被吞掉。
////////////////////////////////////////
void INT0_Isr(void) interrupt 0
{
    s_btn_edge = 1u;
}

////////////////////////////////////////
// 一次按压结束后的语义分派
// 入口参数: hold = 按压持续时隙数, 0 = 时长未知(只靠边沿恢复)
// 函数返回: 无
//
//   待机  单击(30..1200 ms) -> 开检测
//         长按(1500..5000 ms) -> 进配对
//   布防  单击或长按 -> 立即关检测(不设上限)
//   配对  单击或长按 -> 立即退配对
//
// 所有动作都在松手那一拍完成, 没有等待窗口。
////////////////////////////////////////
static void App_OnRelease(u16 hold)
{
    if (s_btn_lock != 0u)
    {
        return;                         /* 抖动 */
    }

    if (hold == 0u)
    {
        /* 按下与松开都落在发射块里, 时长未知。
         * 待机的一次突发是连续 340 ms, 一次 80 ms 的快速按压完全可能
         * 整段落在里面; 这种按压必须放行, 否则开启检测偶发失灵。
         * 机械按键短接 GND + 内部弱上拉, 孤立下降沿噪声的概率极低,
         * 且 s_btn_lock 会挡住紧随其后的抖动。
         *
         * ★ 现在这条路径【只在待机 / 配对态】可达:
         *   布防态的边沿事件在 App_PollButton 里就被挡掉了,
         *   免得干扰把刚开好的检测误关掉 (那是漏报)。
         *   所以下面 armed 分支的 hold 一定来自一次真实的按下采样。 */
    }
    else
    {
        /* 低于下限当抖动丢弃; 发射块会把时长算少, 那种情况不判下限 */
        if ((hold < SLOT_CLICK_MIN) && (s_btn_tx == 0u))
        {
            return;
        }
    }

    if (s_pairing != 0u)
    {
        s_btn_lock = SLOT_CLICK_MIN;
        App_ExitPairing();
        return;
    }

    if (s_armed != 0u)
    {
        /* 退出方向不设时长上限: 按住不放也必须能关掉检测。
         *
         * ★ 走到这里一定意味着轮询观测到过【真实的按下】
         *   (App_PollButton 的 s_btn_down 分支), 因为纯边沿事件
         *   在布防态已被拒绝 —— 见 App_PollButton 里的说明。
         *   这条路径是「关掉检测」的唯一入口, 必须可靠。 */
        s_btn_lock = SLOT_CLICK_MIN;
        App_SetArmed(0u);
        s_click_mute = SLOT_CLICK_MUTE;
        return;
    }

    /* ---- 待机态 ---- */
    if (s_click_mute != 0u)
    {
        /* 刚退出检测/配对, 紧跟的一下属于连按两下, 吞掉 */
        return;
    }

    if (hold <= SLOT_CLICK_MAX)
    {
        s_btn_lock = SLOT_CLICK_MIN;
        App_SetArmed(1u);
    }
    else if ((hold >= SLOT_PAIR_HOLD) && (hold <= SLOT_PAIR_HOLD_MAX))
    {
        s_btn_lock = SLOT_CLICK_MIN;
        App_EnterPairing();
    }
    /* 超长按压: 按钮被压住, 什么都不做 */
}


////////////////////////////////////////
// 按键扫描 (P3.2, 低电平按下)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollButton(void)
{
    u8 pressed;
    u8 edge;

    if (s_btn_lock != 0u) { s_btn_lock--; }

    pressed    = (u8)(BTN_PRESSED() ? 1u : 0u);
    edge       = s_btn_edge;        /* 先读后清, 顺序不能反 */
    s_btn_edge = 0u;

    if (pressed != 0u)
    {
        if (s_btn_down == 0u) { s_btn_down=1u; s_btn_hold=0u; s_btn_tx=0u; }
        else if (s_btn_hold < 0xFFFFu) { s_btn_hold++; }
    }
    else if (s_btn_down != 0u)
    {
        s_btn_down = 0u;
        App_OnRelease(s_btn_hold);
        s_btn_hold = 0u;
    }
    else if (edge != 0u)
    {
        /* 按下与松开都落在发射块里: 只由边沿补救, 时长按未知处理。
         *
         * ★ 布防态【不许】靠边沿关闭检测:
         *   能走到这里, 说明轮询从头到尾没采样到过按下
         *   (s_btn_down 一直是 0), 也就是【只有下降沿、没看到低电平】。
         *   这种边沿绝大多数不是人手按的, 而是干扰:
         *     · 433 发射突发 / 继电器吸合造成的地弹与 EMI
         *     · 长线束上耦合的毛刺
         *   一旦误触发就会把刚开好的检测关掉 —— 现场表现正是
         *   「开了之后很快自己又关了」, 那是漏报, 代价最大。
         *   所以布防态关闭检测必须有一次【真实的按下采样】。
         *   代价: 按下和松开正好都落在 146 ms 发射块里时,
         *   这一次按压不生效, 再按一下即可 (约 5~7 % 的概率)。
         *
         *   待机 / 配对态不受影响: 那两个方向误触发的代价正好相反。
         *   待机误开只是多发几帧, 配对误退只是少配几秒;
         *   而上一轮修的「开启检测偶发失灵」正是靠这条边沿路径
         *   救回来的, 不能砍。
         */
        if (s_armed == 0u)
        {
            App_OnRelease(0u);
        }
    }

    if (s_click_mute != 0u) { s_click_mute--; }
}


////////////////////////////////////////
// 采样人体检测输入 (P3.3), 连续 N 个时隙一致才认可
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollDetect(void)
{
    /* 待机态完全不采样: RCWL-0515 装在从机壳内, 上电后会把机身当成
     * 目标拉到高电平, 只要采样就等于「一直有人」。 */
    if (s_armed == 0u)
    {
        return;
    }

    if (s_det_blind != 0u)
    {
        s_det_blind--;
        s_det_cnt = 0u;
        if (s_det_blind == 0u)
        {
            /* 盲区结束直接拿当前电平当基线: 首次采样没有前一状态可比,
             * 不需要去抖。 */
            s_det_raw    = (u8)(DETECT_ACTIVE() ? 1u : 0u);
            s_det_stable = s_det_raw;
            s_present    = s_det_raw;
            s_state_dirty = 1u;
        }
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
// 继电器动作: 检测到人体时吸合
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_DriveRelay(void)
{
    /* 待机态绝不吸合, 保证「按掉检测」后继电器一定释放 */
    if (s_armed == 0u)
    {
        RELAY_OFF();
        return;
    }

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
        if (s_btn_down != 0u)
        {
            s_btn_tx = 1u;      /* 本帧期间扫不到按键, 时长会被算少 */
        }
        OOK_SendFrame(flags);

        /* 帧间隙也按时隙切片轮询: 一次 3 帧突发约 510 ms, 期间不采样
         * 就等于半秒看不见人。 */
        gap_ms = (u16)(20u + (OOK_IdByte(0u) % 40u));

        while (gap_ms >= APP_TICK_MS)
        {
            /* 帧间隙同样要扫按键和检测 */
            App_PollButton();
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
                /* 重建时必须补回 PAIRREQ, 否则配对突发的第 2/3 帧
                 * 会丢掉该位。 */
                if (s_pairing != 0u)
                {
                    flags |= OOK_FLAG_PAIRREQ;
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

    /* 短盲区内一帧都不发: 此时 P3.3 不可信, 会发出 flags=00 的假帧
     * 把主机上的报警打掉。主机看门狗 45 s, 远长于本盲区 500 ms。 */
    if (s_det_blind != 0u)
    {
        return;
    }

    flags = 0u;
    if (s_present != 0u)
    {
        flags |= OOK_FLAG_PRESENCE;
    }

    /* 相位闸门排在状态帧与心跳之前, 否则首次上报会重新对齐心跳相位。
     * 唯一例外: 带 PRESENCE 的真实报警立刻放行并取消剩余等待。 */
    if (s_phase_left != 0u)
    {
        if ((flags & OOK_FLAG_PRESENCE) != 0u)
        {
            s_phase_left = 0u;           /* 真报警: 取消剩余的相位等待 */
        }
        else
        {
            s_phase_left--;
            return;
        }
    }

    /* 开机后前几帧带 BOOT 标志, 让主机确定性识别「从机刚重启」。
     * 只在真正发出时才减, 相位期间不消耗名额。 */
    if (s_boot_frames != 0u)
    {
        s_boot_frames--;
        flags |= OOK_FLAG_BOOT;
    }

    if (s_state_dirty != 0u)
    {
        s_state_dirty = 0u;
        s_hb_left     = App_HbPeriod();
        App_Report(flags, APP_STATE_REPEAT);
        return;
    }

    if (s_hb_left == 0u)
    {
        /* 待机也必须继续发帧, 否则主机 45 s 后判离线 */
        s_hb_left = App_HbPeriod();
        App_Report(flags, APP_HB_REPEAT);
        return;
    }

    s_hb_left--;
}


////////////////////////////////////////
// 配对模式: 周期性广播 PAIRREQ, 20 s 窗口到点后超时退出
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
        /* 超时退出, 与单击提前退出走同一条收尾路径 */
        App_ExitPairing();
        return;
    }
    s_pair_left--;

    /* 配对期间继电器保持释放, 方便把探头拿到主机旁, 不会一路咔哒响 */
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
// 应用初始化: 端口模式、P5.4 复位脚还原、外设初始化
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
void App_Init(void)
{
#if APP_PORT_TEST
    u8 i;
#endif

    /* P3.2 按键: 准双向口, 内部弱上拉, 按键直接短到 GND */
    P3M1 &= (u8)~(P3_KEY_MASK | P3_DETECT_MASK);
    P3M0 &= (u8)~(P3_KEY_MASK | P3_DETECT_MASK);
    P32 = 1;

    /* P3.2 = INT0, 下降沿触发 */
    IT0 = 1;
    EX0 = 1;

    /* P3.3 检测输入: 准双向口(M1=0, M0=0), 锁存器保持 1。
     * 锁存器=0 时强下拉管导通(可灌 20 mA), 会把模块 OUT 摁到 0 V。
     * 内部上拉阵列 P3PU 必须关掉: 4.1k 与外部 10k 下拉分压后约 2.3 V,
     * 空载就会读成高电平, 整条链路变成永久误报。 */
    P3M1 &= (u8)~P3_DETECT_MASK;
    P3M0 &= (u8)~P3_DETECT_MASK;
    P33 = 1;

    /* P3PU 在扩展 XFR 空间(0xFE13), 只有 P_SW2.EAXFR=1 时可访问;
     * 用完必须立刻 EAXRAM() 复位, 否则 xdata 变量会写到 XSFR 地址。 */
    EAXSFR();
    P3PU &= (u8)~P3_DETECT_MASK;
    EAXRAM();

    /* P5.4 继电器 / P5.5 433M 数据: 强推挽输出, 才有足够驱动能力 */
    P5M1 &= (u8)~0x30u;
    P5M0 |= 0x30u;

    /* P5.4 与 nRST 共用: 清掉 RSTCFG.P54RST 才能当普通 I/O 驱动 */
    RSTCFG &= (u8)~RSTCFG_P54RST;

    RELAY_OFF();                        /* relay released at power-up     */

    /* P3.0 / P3.1 stay quasi bi-directional for ISP download */

#if APP_PORT_TEST
    /* ── P5.5 端口自检(万用表 5 秒内定位故障) ──
     * 阶段 1: 强推挽输出低 1.5 s  -> 应读 0.0 V
     * 阶段 2: 0.5 Hz 方波翻转 4 s -> 应明显摆动
     * 阶段 3: 进入正常 OOK 发射 */
    TX433_LOW();
    OOK_DelayMs(1500u);

    for (i = 0u; i < 4u; i++)
    {
        TX433_HIGH();
        OOK_DelayMs(500u);
        TX433_LOW();
        OOK_DelayMs(500u);
    }
#endif

    OOK_Init();

    s_det_raw     = 0u;
    s_det_stable  = 0u;
    s_det_cnt     = 0u;
    s_present     = 0u;

    /* 上电默认待机: App_PollDetect() 直接返回, 不需要上电盲区 */
    s_armed       = 0u;
    s_det_blind   = 0u;

    s_btn_down    = 0u;
    s_btn_hold    = 0u;
    s_btn_tx      = 0u;
    s_btn_lock    = 0u;
    s_btn_edge    = 0u;
    s_click_mute  = 0u;
    s_pairing     = 0u;
    s_pair_left   = 0u;
    s_pair_burst_left = 0u;

    /* 首次心跳用快节奏: 主机要收到带 BOOT 的帧才退出盲区保护 */
    s_hb_left     = SLOT_HEARTBEAT;

    /* 首次上报前的相位错开延时 */
    s_phase_left  = App_PhaseOffsetSlots();

    s_state_dirty = 0u;
    s_boot_frames = APP_BOOT_FRAMES;   /* 前几帧带 BOOT 标志 */

    /* 循环开始前的短暂稳定; 传感器在布防前不读, 无需暖机等待 */
    EA = 1;                             /* INT0 生效 */

    OOK_DelayMs(50u);
}


////////////////////////////////////////
// 应用主循环: 每个时隙跑一次 (10 ms)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
void App_Run(void)
{
#if APP_DETECT_TEST
    /* ── 检测脚自检: P3.3 原样镜像到继电器, 不过任何滤波 ── */
    if (DETECT_ACTIVE())
    {
        RELAY_ON();
    }
    else
    {
        RELAY_OFF();
    }
    OOK_DelayMs(APP_TICK_MS);
#elif APP_TX_TEST
    /* ── 发射自检: 背靠背发射, 占空比约 89% ──
     * 正常占空比只有 7%, 万用表在 P5.5 上永远读到 0 V;
     * 测试模式下会读到 1.3~1.5 V DC。 */
    App_Report(OOK_FLAG_PRESENCE, 1u);
    OOK_DelayMs(20u);
#else
    /* 待机/布防的分支在被调函数内部判断 (看 s_armed) */
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
