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
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "board.h"
#include "ook433.h"
#include "app.h"

/* slot counts derived from the millisecond constants in app.h */
#define SLOT_LONGPRESS       (APP_LONGPRESS_MS   / APP_TICK_MS)
#define SLOT_PAIR_WINDOW     (APP_PAIR_WINDOW_MS / APP_TICK_MS)
#define SLOT_PAIR_BURST      (APP_PAIR_BURST_MS  / APP_TICK_MS)
#define SLOT_HEARTBEAT       (APP_HEARTBEAT_MS   / APP_TICK_MS)

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

    /* ★★ 上电盲区期间一帧都不发。
     *   盲区里 P3.3 还不可信，能组出来的 flags 全是猜的；而 s_present
     *   在 App_Init 里被清成 0，于是会发出 flags=00（无人）的假帧，
     *   把主机上「一直有人」的报警打掉，3 秒后再重新报上。
     *   现场看到的就是「报警每隔几秒自己消一次又重报」，而且很容易
     *   被误判成「采样延时大」—— 真正的原因是从机重启 + 这段假帧。
     *   不说话是安全的：主机的离线看门狗是 ZONE_OFFLINE_MS = 45 秒
     *   （ZoneManager.h 里 45000UL），远长于 3 秒盲区，
     *   不会把节点判成离线。
     *   ★ 这里原先写成「= 10 秒」，后来又跟着主机改成 30 秒，
     *     现在主机抬到了 45 秒 —— 调参时【必须以主机侧为准】，
     *     注释只是说明量级关系，不是判据。 */
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
     *   于是每台从机都在「上电后 APP_DETECT_BLIND」把这一帧发出去 —— 4 台同相上电时
     *   首帧 100% 碰撞，而且这个分支还会 s_hb_left = SLOT_HEARTBEAT，
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
        /* ★ 恢复常态时必须【重新】套用相位偏移，不能直接把 s_hb_left
         *   设成 SLOT_HEARTBEAT：
         *   配对时是人为把设备挪到主机旁边、逐台按的，若退出配对都以
         *   「同一个心跳网格」起算心跳，几台设备又会重新同相 ——
         *   等于刚用 ID 错开好的相位被配对流程抹掉。 */
        s_hb_left    = SLOT_HEARTBEAT;
        s_phase_left = App_PhaseOffsetSlots();
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
    s_det_blind   = APP_DETECT_BLIND;
    s_btn_down    = 0u;
    s_btn_hold    = 0u;
    s_btn_latched = 0u;
    s_pairing     = 0u;
    s_pair_left   = 0u;
    s_pair_burst_left = 0u;
    s_hb_left     = SLOT_HEARTBEAT;
    /* ★ 首次上报前的相位错开延时（见 App_PhaseOffsetSlots()）。
     *   为什么不直接把 s_det_blind 加上偏移：盲区的作用是「不信传感器」，
     *   把它拉长会推迟真实报警最多一个心跳周期（APP_HEARTBEAT_MS）。
     *   这里改为「传感器照常采样、只是晚点开口」，检测能力不受影响。 */
    s_phase_left  = App_PhaseOffsetSlots();
    s_state_dirty = 0u;
    s_boot_frames = APP_BOOT_FRAMES;   /* ★ 前几帧带 BOOT 标志，供主机识别重启 */

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
