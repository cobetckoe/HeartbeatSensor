/*
 * HeartbeatSensor -- STC8G1K08A slave firmware
 *
 * Application layer: pairing button, presence detection, relay drive and
 * the 433 MHz report scheduler.
 *
 * Scheduling model
 * ----------------
 * No timer interrupt is used. OOK_SendFrame() runs with interrupts masked
 * for ~163 ms, so a tick interrupt would silently lose time anyway. The
 * main loop instead advances one "slot" every APP_TICK_MS via
 * OOK_DelayMs(), and all intervals below are counted in slots.
 *
 * Power-up state
 * --------------
 * The node boots into STANDBY: the sensor is NOT sampled, the relay stays
 * released and only a slow "nobody here" heartbeat is sent, so the
 * master keeps the zone online.
 *
 * Button semantics -- only ONE gesture is ambiguous (a single click in
 * standby) and that ambiguity is confined to the single state where it
 * cannot be avoided, so every transition OUT of a running mode is
 * immediate (see App_PollButton):
 *
 *   standby : single click -> arm, confirmed after the 400 ms window
 *             double click -> pairing
 *   armed   : single click -> disarm IMMEDIATELY on release,
 *             double click NOT recognised
 *   pairing : single click -> leave pairing early, back to standby
 *             20 s window expiry  -> back to standby as well
 *
 * Every mode can only be started FROM standby, which is exactly why
 * leaving a mode never has to wait for a possible second click.
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
#define SLOT_DBLCLICK        (APP_DBLCLICK_MS    / APP_TICK_MS)
#define SLOT_ARM_BLIND       (APP_ARM_BLIND_MS   / APP_TICK_MS)

////////////////////////////////////////
// 多机共存：按设备 ID 计算心跳相位偏移（单位: 时隙 = 10ms）
//
// ★★ 为什么必须有这个偏移 —— 这是多防区丢帧最隐蔽的根因：
//
//   本协议是纯单向 OOK，从机【听不到】信道是否被占用，没有载波侦听、
//   没有重传、没有随机退避。两台从机只要同时开始发前导码，
//   两路 433M 载波在接收头里叠加，【两帧会一起毁掉】，不是互相干扰一点点，
//   而是完全解不出来。
//
//   而每台从机的心跳都由「上电那一刻」起算 APP_HEARTBEAT_MS。现场安装时是逐台
//   上电的，相位天然错开，看着一切正常。但只要遇到一次【总断电后同时
//   上电】（配电箱合闸、UPS 切换、雷击跳闸），4 台从机的心跳相位就
//   永久锁死在同一毫秒网格上，此后每一轮心跳都必然碰撞：
//       · 4 台全部同相 → 每一轮 4 串突发叠成一团 → 几乎全部解不出
//       · 主机上表现为所有防区同时掉线、报警全部失效，且【重启一次就好】
//         （再逐台上电又错开了）—— 典型的「现场好了、回来又好了」幽灵故障
//
//   修法：把 ID 哈希到 0..SLOT_HEARTBEAT-1 个时隙（即 0..APP_HEARTBEAT_MS-10ms）上，
//   让 4 台从机在一个心跳周期里均匀铺开，最多只有 1/4 概率撞在一起，
//   且因为偏移是【由 ID 决定的确定值】，重启/断电后相位关系保持不变，
//   不会出现「这次好了下次又撞」。
//
//   ★ 为什么用 ID 而不是随机数：STC8 没有可靠的随机源，上电读到的
//     时钟/AD 噪声在批量生产时可能高度相关（同批板子同时上电）。
//     ID 是出厂唯一的，哈希后天然分散，且可复现、便于现场复盘。
//
//   ★ 为什么偏移量取满整个心跳周期：4 防区时理想间距是
//     APP_HEARTBEAT_MS/4（心跳 2000ms 时 = 500ms），取满周期只是让它
//     均匀覆盖；实测碰撞率从「必然 100%」降到「约 1/4」，
//     代价仅仅是首次心跳最晚推迟一个心跳周期（2000ms，
//     远小于主机 ZONE_OFFLINE_MS=45s，不影响在线判定）。
////////////////////////////////////////
static u16 App_PhaseOffsetSlots(void)
{
    u16 h;

    /* 简单的移位异或散列：ID 的 7 个字节里挑 3 个参与，避免占用过多
     * 代码空间（STC8G1K08A 只有 8K flash，已用掉大半）。 */
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
static u16 s_det_blind;          /* ticks left in the power-up blind time */

/* ---- 布防状态 ------------------------------------------------------
 * ★ 0 = 待机（上电默认）：不采样、不报警、只发慢心跳
 *   1 = 布防：正常检测 + 报警 + 2 秒心跳
 *   这是本次改动的核心状态位，App_PollDetect / App_DriveRelay /
 *   App_PollReport 三处都受它控制。 */
static u8  s_armed;

/* ---- button / pairing ---------------------------------------------- */
static u8  s_btn_down;
static u16 s_btn_hold;
static u8  s_clicks;             /* 双击窗口内已累计的点击数          */
static u16 s_click_wait;         /* 双击窗口剩余时隙, 0 = 没有待定单击 */
/* ★ 刚退出检测 / 刚退出配对后的短暂静音窗口（时隙）。
 *   检测态只认单击，所以「在检测态快速连按两下」= 两次单击：
 *   第一下关掉检测回到待机，第二下若被当成待机单击，400 ms 窗口
 *   一过又把检测打开 —— 用户看到的是「连按两下反而开了」。
 *   这里吞掉紧跟其后的那一下，让「连按两下」的结果就是关闭。
 *   ★ 只在按键松开时倒数，按住不动不消耗。 */
static u16 s_click_mute;
static u8  s_pairing;
static u16 s_pair_left;
static u16 s_pair_burst_left;

/* ---- reporting ------------------------------------------------------ */
static u16 s_hb_left;
static u8  s_state_dirty;        /* state changed, needs the 3x burst  */

/* 上电相位错开剩余时隙（见 App_PhaseOffsetSlots）。
 * ★ 只用来推迟【无人基线帧 / 心跳】这类「不发也不影响安全」的报文，
 *   真实报警（s_present=1）一律立即发出，绝不因相位而延迟。 */
static u16 s_phase_left;

/* ★ 开机标志剩余帧数。每发一帧减 1，减到 0 之后永不再置 OOK_FLAG_BOOT。
 *   用途：让主机【确定性地】识别「从机刚重启」，而不是靠猜 seq 回绕。
 *   为什么不是只标记第 1 帧：
 *     射频实测丢帧率可达 85%，只发 1 帧大概率丢失，主机就永远看不到重启证据。
 *     发 BOOT_BURST 帧（每次心跳带 2 帧，即覆盖前 4 次心跳≈8s）
 *     才能保证在丢帧环境下也能被收到至少一次。 */
static u8  s_boot_frames;

/* 开机后携带 BOOT 标志的帧数（含每个心跳连发的重复帧）
 *   8 帧 ≈ 4 次心跳 ≈ 8s（心跳 2000ms × 4）。取这个值的原因：
 *     · 足够长：低至 15% 的收帧率下，8 帧仍几乎必然被收到 1 帧以上
 *     · 足够短：远小于 seq 回绕周期（~6 分钟），不会长期污染 flags
 *     · 不影响报警：BOOT 位不参与任何功能判断，只用于诊断
 *   注意烧录/出货时不要改大，否则主机侧的 rst 统计会持续被拉高。 */
#define APP_BOOT_FRAMES  8u


////////////////////////////////////////
// 进入配对模式 (只能在【待机态】双击 P3.2 触发)
// 入口参数: 无
// 函数返回: 无
//
// ★ 配对只能从待机进入：
//   检测态只认单击，第一下松手就已经回到待机，第二下会被
//   s_click_mute 静音吞掉，因此不存在「检测中直接跳进配对」的路径。
//
// ★ 退出路径见 App_ExitPairing()：20 s 超时，或操作员再单击一下提前退出。
////////////////////////////////////////
static void App_EnterPairing(void)
{
    s_pairing         = 1u;
    s_pair_left       = SLOT_PAIR_WINDOW;
    s_pair_burst_left = 0u;         /* 立即发第一串 PAIRREQ */

    /* ★ 进配对必须【彻底退出布防】：配对期间不检测、不报警，退出
     *   配对后也停在待机态 —— 所有模式只能从待机启动，于是配对结束
     *   必须回到同一个干净状态。 */
    s_armed       = 0u;
    s_det_blind   = 0u;
    s_det_raw     = 0u;
    s_det_stable  = 0u;
    s_det_cnt     = 0u;
    s_present     = 0u;
    s_state_dirty = 1u;

    /* 清掉按键残留，保证 20 s 窗口内不会带着一次待定单击 */
    s_clicks     = 0u;
    s_click_wait = 0u;
    s_click_mute = 0u;

    RELAY_OFF();
}


////////////////////////////////////////
// 当前应使用的心跳周期 (单位: 时隙)
// 入口参数: 无
// 函数返回: SLOT_HEARTBEAT(2s) 或 SLOT_STANDBY_HB(10s)
//
// ★ 为什么开机帧期间必须用【快】节奏，哪怕处于待机：
//
//   主机侧看到 OOK_FLAG_BOOT 且 PRESENCE=0 的帧时，会刻意【不采信】
//   这个「无人」（见 ZoneManager.h 的盲区假帧保护）——因为旧固件会在
//   上电盲区里发假帧。这个保护是按【收帧次数】退出的，从机一共发
//   APP_BOOT_FRAMES 串带 BOOT 的帧。
//
//   若待机也用 10 秒节奏，这 8 串要发 80 秒，期间主机一直保留【上一
//   次断电前的有人状态】—— 现场看到的是「从机重启后屏上还一直亮着
//   有人，一分钟后才自己消」。用 2 秒节奏只需 16 秒就退出保护窗口，
//   与改动前的观感一致，待机省电的那部分也不受影响（占比极小）。
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
// ★ 触发条件只有两种, 都在这里收敛成同一个动作：
//   1) 20 s 窗口到点（自动超时，见 App_PollPairing）
//   2) 配对期间单击一下（人工提前退出，见 App_PollButton）
//
// ★★ 为什么没有「配对成功后自动退出」：
//   433M 对从机是【纯单向】链路（只有 TX、没有 RX），主机配对成功之后
//   没有办法把结果回告从机。「配对成功即退出」在从机侧做不到，
//   替代方案就是上面两条：超时兜底 + 操作员看到主机提示配对完成后
//   单击一下退出。文档必须写清这一点，否则现场会以为从机能感知配对结果。
//
// ★ 退出后一定停在【待机】：继电器释放、不采样、只发 10 s 慢心跳。
//   绝不自动进入检测 —— 所有模式只能从待机人为启动。
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

    /* 清掉按键残留：配对期间可能按到一半就退出了 */
    s_clicks     = 0u;
    s_click_wait = 0u;
    s_click_mute = SLOT_DBLCLICK;   /* 刚松开的那一下不能再被当成待机单击 */

    /* ★ 立刻补一帧「待机 / 无人」，让主机把该防区刷新干净 */
    s_state_dirty = 1u;
    s_hb_left     = App_HbPeriod();

    /* ★ 这里把相位闸门【清零】，而不是重新套用 App_PhaseOffsetSlots()。
     *   原因：相位闸门在 App_PollReport() 里排在 s_state_dirty 之前，
     *   若退出时重新装一个非零偏移，上面那一帧「待机/无人」刷新会被
     *   推迟最多一个完整心跳周期（2 s）—— 明明已经退出配对，屏上却
     *   还挂着旧状态，正好违背「退出要快」这个目标。
     *   多机同相的担心在这里也不成立：相位错开只需要保证【常态心跳】
     *   不同相，而配对是逐台人工按键退出的，各台退出时刻本来就错开；
     *   相位闸门本质上是上电时的一次性延时，不该被配对流程反复重置。 */
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
        /* ★ 开启检测时重新走一次短盲区(500ms)，并在盲区结束时重新采基线。
         *   为什么需要：按下按钮的那只手本身就贴在天线前面，若立刻采样，
         *   按下瞬间就会被判成「有人」并吸合继电器，用户看到的是
         *   「一按按钮继电器就咔哒一下」，会以为开关接反了。 */
        s_armed     = 1u;
        s_det_blind = SLOT_ARM_BLIND;
        s_det_cnt   = 0u;
        s_hb_left   = App_HbPeriod();   /* 布防后恢复 2 秒心跳 */
    }
    else
    {
        /* ★ 回到待机必须【立刻】放开继电器、把状态清成无人，并马上补
         *   一帧(s_state_dirty)，不能让主机继续显示上一次的 PRESENCE=1
         *   直到下一个心跳才消失。 */
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
// 按键状态机 (P3.2, 低电平按下)
//
// ★★ 三种状态、两套判据 —— 这是避免「单击/双击混淆」的核心：
//
//   状态      单击                          双击
//   --------  ------------------------------  ---------------
//   待机      开检测（等 400ms 窗口确认）      进配对（20 s）
//   检测      关检测（松手【立即】生效）        不识别
//   配对      退出配对，回待机                 不识别
//
// ★★ 所有模式只能从【待机】启动：
//   于是「关检测」和「退出配对」这两条路在物理上不可能还有第二下，
//   根本不需要等窗口 —— 退出永远是秒回的，这就是把判据按状态拆开的
//   全部目的。
//
// ★★ 为什么要引入「上电默认待机」（更早的改动，根因）
//
//   原来上电盲区一过就自动开始检测。但 RCWL-0515 是装在从机壳体内部的，
//   上电后它会把【从机自己】当成目标，OUT 直接被拉高 ——
//   于是每台从机一上电就报「有人」，主机上所有防区同时亮红、
//   继电器全部吸合。这个误报无法靠「先上电再装壳」规避，
//   因为上电那一刻它就已经误报了。
//
//   改法：上电进入【待机】——不采样、不驱动继电器、只按 10 秒一次发
//   「无人」心跳（保证主机不把该防区判成离线）。必须人工单击 P3.2 才
//   真正开启检测。这样上电/装柜/调试全程从机是安静的，装好、人离开、
//   再按一下按钮才算真正布防。
//
// ★★ 为什么判据要按状态分开（而不是三态都「松手即刻切换」）
//
//   上一版让单击在三个状态下都立即切换，顺带把「开检测」也做成零延迟。
//   但开检测本来不需要这么快（后面还有 APP_ARM_BLIND_MS=500ms 撤离盲区），
//   零延迟的代价却是：待机态「单击开检测」与「双击进配对的第一下」
//   完全无法区分，只能靠窗口兜底，语义始终含混。
//   现在反过来：把模糊留在待机（那里等 400ms 完全可接受，因为开检测
//   本来就要等盲区），把「立即生效」留给所有【退出】动作。
//
// ★ 为什么不再用长按进配对：
//   长按在配电箱里极易误触（按钮被线束压住 = 一直按住 = 进配对），
//   而配对是破坏性的（20 秒不报警）。双击则是明确、几乎不可能误触的动作。
//
// ★ 超过 APP_CLICK_MAX_MS 的按压一律【丢弃】，不折算成单击：
//   否则「按钮被卡住」会变成「反复开关检测」，比什么都不做更危险。
//
// ★ 已知限制：OOK_SendFrame() 期间 EA=0 且整段忙等 ~146ms，这一小段
//   时间里按键完全扫不到。若某次按压【整段】落在一次发射中途，这次
//   点击会被吞掉。待机时发射只占 340ms/10s（3.4%），布防时占
//   340ms/2s（17%）。工程上可接受（再按一次即可），因为用定时器中断
//   采样在本芯片上是不可行的 —— 发射期间中断本来就是关的。
//   若现场反映「偶尔要按两次」，把 APP_STANDBY_HB_MS 调大、或让用户
//   把按压时间保持在 100ms 以上（越短的按压越容易整段落进发射窗口）。
//
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollButton(void)
{
    if (BTN_PRESSED())
    {
        if (s_btn_down == 0u)
        {
            s_btn_down = 1u;
            s_btn_hold = 0u;
        }
        else if (s_btn_hold < 0xFFFFu)
        {
            s_btn_hold++;
        }
    }
    else if (s_btn_down != 0u)
    {
        s_btn_down = 0u;            /* 松手: 本次按压结束 */

        /* 太短 = 抖动; 太长 = 误触/按钮卡住。两种情况都不算一次点击。 */
        if ((s_btn_hold >= SLOT_CLICK_MIN) && (s_btn_hold <= SLOT_CLICK_MAX))
        {
            if (s_pairing != 0u)
            {
                /* ---- 配对态: 单击 = 提前退出回待机 ----
                 * 从机只有 TX 没有 RX，感知不到主机「配对成功」；替代做法
                 * 就是让操作员看到主机提示后单击退出。20 s 超时仍兜底。 */
                App_ExitPairing();
            }
            else if (s_armed != 0u)
            {
                /* ---- 检测态: 只认单击，松手【立即】回待机 ----
                 * ★ 这里【绝不开双击窗口】：关闭是安全侧动作，没有任何理由
                 *   为了「可能还有第二下」而白等 0.4 s。
                 *   之所以敢不开窗口，是因为检测态根本不识别双击 ——
                 *   所有模式只能从待机启动，「关掉」之后不存在「还想进配对」
                 *   的合法路径，第二下按下去也只是待机单击，会被
                 *   s_click_mute 吞掉。 */
                s_clicks     = 0u;
                s_click_wait = 0u;
                s_click_mute = SLOT_DBLCLICK;
                App_SetArmed(0u);
            }
            else if (s_click_mute != 0u)
            {
                /* 刚退出检测/配对，紧跟其后的这一下属于「连按两下」的
                 * 第二下，静音吞掉，避免又把检测打开。 */
            }
            else
            {
                /* ---- 待机态: 唯一需要区分单击/双击的地方 ----
                 * 第一下只记数并开窗口；窗口到点确认没有第二下，才真正
                 * 开检测（见函数末尾）。 */
                if (s_clicks < 2u)
                {
                    s_clicks++;
                }

                if (s_clicks >= 2u)
                {
                    /* 双击: 立即进配对，不必等窗口结束 */
                    s_clicks     = 0u;
                    s_click_wait = 0u;
                    App_EnterPairing();
                }
                else
                {
                    s_click_wait = SLOT_DBLCLICK;
                }
            }
        }
        else if (s_btn_hold > SLOT_CLICK_MAX)
        {
            s_clicks     = 0u;      /* 长按: 清掉待定单击, 什么都不做 */
            s_click_wait = 0u;
        }

        s_btn_hold = 0u;
    }

    /* ★ 双击窗口只在【松手状态】下倒数：
     *   按住不动不该消耗窗口，否则「慢慢按两下」会被判成两次单击。 */
    if ((s_btn_down == 0u) && (s_click_wait != 0u))
    {
        s_click_wait--;

        if (s_click_wait == 0u)
        {
            /* ★ 窗口到点 = 确认这确实是【单击】-> 开检测。
             *   只有待机态会开这个窗口（检测态/配对态都不开），
             *   所以这里再确认一次状态仍然合法即可。
             *   ★ 绝不能在这里 toggle：开检测是唯一一次动作。 */
            if ((s_clicks != 0u) && (s_armed == 0u) && (s_pairing == 0u))
            {
                App_SetArmed(1u);
            }
            s_clicks = 0u;
        }
    }

    /* ★ 静音窗口同样只在【松手状态】下倒数 */
    if ((s_btn_down == 0u) && (s_click_mute != 0u))
    {
        s_click_mute--;
    }
}


////////////////////////////////////////
// 采样人体检测输入 (P3.3), 连续 N 个时隙一致才认可, 消除抖动
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_PollDetect(void)
{
    /* ★★ 待机态【完全不采样】。
     *
     *   RCWL-0515 装在从机壳内，上电后会把从机机身当成目标，OUT 直接
     *   拉高。只要还在采样，待机就等于「一直有人」，所以待机时连盲区
     *   都不推进 —— 这样单按开启检测时一定从一个完整的盲区重新开始。 */
    if (s_armed == 0u)
    {
        return;
    }

    /* ignore the sensor while RCWL-0515 settles after being armed */
    if (s_det_blind != 0u)
    {
        s_det_blind--;
        s_det_cnt = 0u;
        if (s_det_blind == 0u)
        {
            /* ★★ 盲区结束的那一刻：直接拿当前电平当【基线】，不要等去抖。
             *
             *   如果这里只是 return，那么 s_present 还是 App_Init() 里的 0，
             *   App_PollReport() 会立刻发出一帧 flags=00（无人）——
             *   主机上「一直有人」的报警就被这一帧打掉了，50ms 后才被
             *   下一帧恢复。现场看到的就是「报警隔几秒自己消一下又重报」，
             *   而且极易被误判成「采样延时大」或「去抖太慢」。
             *
             *   上电第一次采样没有「前一状态」可比，本来就不需要去抖：
             *   去抖的作用是滤掉运行中的电平抖动，不是决定初始值。
             *   直接采信，让盲区后的第一帧就带真实状态。 */
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
// 继电器动作: 检测到人体时吸合 (+ 可选蜂鸣器/指示灯)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void App_DriveRelay(void)
{
    /* ★ 待机态绝不吸合：即使 s_present 是陈旧的 1 也强制释放，
     *   保证「按掉检测」之后继电器一定回到释放状态。 */
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
        OOK_SendFrame(flags);

        /* ---- sample the sensor during the inter-frame gap ----
         *
         * OOK_SendFrame() blocks for ~146 ms and the gap adds 20..59 ms,
         * so a full 3-frame state burst keeps the CPU busy for ~500 ms.
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
            /* ★ 帧间隙里也必须扫按键：一次状态突发是 3 帧 ≈ 510 ms，
             *   期间若不扫按键，一次快按会被整个吞掉；更糟的是
             *   双击窗口会被跳过，两次点击可能被判成两次单击。 */
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
                /* ★ 必须补回 PAIRREQ：进配对时 App_EnterPairing() 置了
                 *   s_state_dirty，所以配对首串的第一帧之后就会走进本分支
                 *   重建 flags。若这里只按 PRESENCE 重建，第 2/3 帧会被
                 *   【丢掉 PAIRREQ 位】，主机就只有第 1 帧可用于配对。
                 *   本分支只在 App_Report 的帧间空隙里执行，非配对时
                 *   s_pairing 恒为 0，因此这一行不影响任何普通上报。 */
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

    /* ★★ 开启检测后的短盲区期间一帧都不发。
     *   盲区里 P3.3 还不可信，能组出来的 flags 全是猜的；而 s_present
     *   此时被清成 0，于是会发出 flags=00（无人）的假帧，
     *   把主机上「一直有人」的报警打掉，半秒后再重新报上。
     *   现场看到的就是「报警每隔几秒自己消一次又重报」，而且很容易
     *   被误判成「采样延时大」—— 真正的原因是从机重启 + 这段假帧。
     *   不说话是安全的：主机的离线看门狗是 ZONE_OFFLINE_MS = 45 秒
     *   （ZoneManager.h 里 45000UL），远长于本盲区（500ms），
     *   不会把节点判成离线。
     *   ★ 调参时【必须以主机侧为准】，注释只是说明量级关系，不是判据。
     *   ★ 注意：待机态(s_armed==0)不会走到这里 —— 待机要发慢心跳，
     *     而且 s_det_blind 在待机时恒为 0（见 App_SetArmed）。 */
    if (s_det_blind != 0u)
    {
        return;
    }

    flags = 0u;
    if (s_present != 0u)
    {
        flags |= OOK_FLAG_PRESENCE;
    }

    /* ★★ 上电相位错开必须排在 s_state_dirty / 心跳【之前】。
     *
     *   为什么不能放在 s_state_dirty 之后（曾经就是这么写的，等于没生效）：
     *   盲区结束那一刻 App_PollDetect() 一定会置 s_state_dirty（采基线），
     *   于是每台从机都在「上电/布防后的盲区结束」把这一帧发出去 —— 4 台同相上电时
     *   首帧 100% 碰撞，而且这个分支还会重置 s_hb_left，
     *   把心跳相位【一起】重置到同一网格，之后每一轮继续碰撞。
     *   原位错开形同虚设，必须先让相位闸门拦下这一帧。
     *
     *   ★ 唯一例外：本帧带 PRESENCE（真实有人）时立刻放行并取消剩余等待。
     *     报警决不能因为「相位没轮到」而推迟，宁可碰撞也不能漏报；
     *     碰撞只是丢一帧，后面状态突发的重复帧还会补上。
     */
    if (s_phase_left != 0u)
    {
        if ((flags & OOK_FLAG_PRESENCE) != 0u)
        {
            s_phase_left = 0u;           /* 真报警：取消剩余的相位等待 */
        }
        else
        {
            s_phase_left--;
            return;
        }
    }

    /* ★ 注意这里的位置：相位闸门之后、s_state_dirty 之前。
     *   待机态没有任何「状态变化」可言（永远 PRESENCE=0），所以
     *   s_state_dirty 只会在布防态真正检测到人/人离开时被置位。 */

    /* ★ 开机后前几帧带 BOOT 标志：让主机确定性地识别「从机刚重启」。
     *   必须先于 s_state_dirty 分支计算，否则状态帧会漏带该位。
     *   ★ 计数只在【真正发出】时才减，所以相位期间不会白白消耗 BOOT 名额。 */
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
        /* ★ 心跳周期随布防状态变化：
         *   布防态 2 秒(APP_HEARTBEAT_MS)，待机态 10 秒(APP_STANDBY_HB_MS)。
         *   待机必须仍然发帧，否则主机 ZONE_OFFLINE_MS(45s) 一到就把该
         *   防区判成离线，现场会以为设备坏了。10 秒远小于 45 秒。 */
        s_hb_left = App_HbPeriod();
        App_Report(flags, APP_HB_REPEAT);
        return;
    }

    s_hb_left--;
}


////////////////////////////////////////
// 配对模式: 周期性广播 PAIRREQ, 20 s 窗口到点后【超时退出并回待机】
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
        /* ★ 超时退出。统一走 App_ExitPairing()，保证「20 s 超时退出」与
         *   「配对期间单击提前退出」两条路径的收尾完全一致（都回待机）。 */
        App_ExitPairing();
        return;
    }
    s_pair_left--;

    /* 配对期间继电器保持释放，方便把探头拿到主机旁，不会一路咔哒响 */
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

    /* P3.3 detect input: QUASI-BIDIRECTIONAL (M1=0, M0=0), latch = 1.
     *
     * ★ 为什么不用高阻输入（M1=1, M0=0）：
     *   高阻脚完全没有内部上拉器件，一旦传感器没接、或模块的 OUT 在
     *   上电瞬间还没建立，引脚就悬空，电平由漏电流决定 —— 不可控。
     *   准双向口的内部弱上拉（~30k）免费提供了一个确定电平。
     *
     * ★ 为什么准双向口不会「悬空乱跳」：
     *   弱上拉会把悬空脚牢牢拉到 VCC，读到的是一整个干净的高电平，
     *   不是在逻辑阈值上飘。真正的「阈值附近抖动」恰恰是【高阻模式】
     *   下什么都没接才会出现的现象。
     *
     * ★ 但准双向口带来的是【另一个方向】的问题：
     *   传感器拔掉时引脚读高 -> DETECT_ACTIVE_HIGH=1 下就是「有人」
     *   -> 继电器永久吸合（误报）。
     *   所以外部 10k 下拉仍必须装：3.3V * 10k / (10k + 30k) ≈ 0.8V，
     *   是一个很干净的 LOW，此时拔掉传感器 = 不报警（失效安全）。
     *
     * ★ 锁存器必须留 1，绝不能留 0：
     *   准双向口锁存器 = 0 时强下拉管导通，可灌 20mA，会把模块的 OUT
     *   直接摁死到 0V。
     *
     * ★ 内部上拉阵列（P3PU）必须关掉：
     *   它提供的是强上拉 ~4.1k，与外部 10k 下拉分压得
     *   3.3V * 10k / 14.1k ≈ 2.3V —— 高于逻辑阈值，空载就会读成高电平，
     *   整条链路变成永久误报。 */
    P3M1 &= (u8)~P3_DETECT_MASK;      /* M1=0, M0=0 -> 准双向口            */
    P3M0 &= (u8)~P3_DETECT_MASK;
    P33 = 1;                          /* 锁存器=1: 弱上拉，绝不强下拉      */

    /* P3PU 位于扩展 XFR 空间(0xFE13)，只有 P_SW2.EAXFR=1 时才可访问。
     * ★ 必须显式开/关，不能依赖全局状态：EAXFR=1 期间 MOVX 的目标是
     *   XSFR 而不是 XRAM，而本工程有 50 字节 xdata 变量，若把标志留在
     *   1 上，那些变量会被写到 XSFR 地址去。 */
    EAXSFR();
    P3PU &= (u8)~P3_DETECT_MASK;      /* 关掉内部强上拉阵列(~4.1k)         */
    EAXRAM();

    /* P5.4 继电器 / P5.5 433M 数据: 强推挽输出, 才有足够驱动能力 */
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
    /* ★ 上电默认【待机】：s_armed = 0 时 App_PollDetect() 直接返回，
     *   所以这里不需要上电盲区 —— 盲区改为在「开启检测」那一刻才开始数
     *   （见 App_SetArmed），这样上电过程一帧误报都不会有。 */
    s_armed       = 0u;
    s_det_blind   = 0u;
    s_btn_down    = 0u;
    s_btn_hold    = 0u;
    s_clicks      = 0u;
    s_click_wait  = 0u;
    s_click_mute  = 0u;
    s_pairing     = 0u;
    s_pair_left   = 0u;
    s_pair_burst_left = 0u;
    /* ★ 首次心跳用【快】节奏，不用待机的 10 秒：
     *   s_boot_frames 还在等待发送，而主机是按「收到带 BOOT 的无人帧」
     *   才退出盲区假帧保护的。若首帧就等到 10 秒，主机会多保留 10 秒
     *   上次断电前的有人状态。App_HbPeriod() 会在 BOOT 帧发完后自动
     *   切回待机节奏（见该函数注释）。 */
    s_hb_left     = SLOT_HEARTBEAT;
    /* ★ 首次上报前的相位错开延时（见 App_PhaseOffsetSlots()）。
     *   为什么不直接把 s_det_blind 加上偏移：盲区的作用是「不信传感器」，
     *   把它拉长会推迟真实报警最多一个心跳周期（APP_HEARTBEAT_MS）。
     *   这里改为「传感器照常采样、只是晚点开口」，检测能力不受影响。 */
    s_phase_left  = App_PhaseOffsetSlots();
    s_state_dirty = 0u;
    s_boot_frames = APP_BOOT_FRAMES;   /* ★ 前几帧带 BOOT 标志，供主机识别重启 */

    /* short settle before the loop starts (the sensor itself is not read
     * until the node is armed, so no RCWL-0515 warm-up window is needed) */
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
    /* ── 检测脚自检：P3.3 原样镜像到继电器，不过任何滤波 ──
     * 有高电平 -> 继电器吸合(指示灯亮)；低电平 -> 释放。
     * 若模块 OUT 量到高、这里继电器却始终不动，故障就到端口为止，
     * 与应用层/上报逻辑无关。 */
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
    /* ── 发射自检：连续发射，占空比 ~89% ──
     * 正常模式每 APP_HEARTBEAT_MS(2000ms) 才发 146ms，占空比约 7%，
     * 万用表读到 0V 无法区分好坏。
     * 这里背靠背发射，P5.5 直流电压会到 1.3~1.5V；
     * 若仍是 0V，说明发射链路真的断了（P5.5 虚焊 / RSTCFG 没清掉
     * P54RST 导致 P5 口异常 / 模块没供电）。 */
    App_Report(OOK_FLAG_PRESENCE, 1u);
    OOK_DelayMs(20u);
#else
    /* 待机 / 布防的分支都判断在被调函数内部（看 s_armed），
     * 主循环结构保持原样，不想在这里多加一个 if 把逻辑拆散。 */
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
