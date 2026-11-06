/*
 * HeartbeatSensor -- STC8G1K08A 从机固件
 *
 * 433 MHz OOK 发射 + 共用的微秒计时基准。
 *
 * 报文格式必须与 src/master/OokReceiver.h 和 docs/PROTOCOL-433M.md 保持一致,
 * 只改一边会让接收静默失效。
 *
 * 计时: Timer0 12T 模式一次性 16 位递减计数, SYSCLK=24MHz 时 1 计数 = 0.5us,
 * 单次最长 32767us, 更长的等待靠循环拼。1T 内核让每次装定的开销只有 2us 左右,
 * 不到 350us 基准时间单元的 1%, 远在协议 +/-45% 的解码容差之内。
 *
 * ★ 不要"简化"成软件 NOP 空循环: Large 模式 (MemoryModel=2) 下局部计数器住在
 *   xdata, 每轮迭代要几条 MOVX, 位周期会涨到 ~566us 而不是 350us (慢 1.6 倍,
 *   2T 前导变成 ~1120us, 直接掉出接收机 500..900us 的窗口)。
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "config.h"
#include "board.h"
#include "ook433.h"

/* Timer0 counts per microsecond in 12T mode (2 at 24 MHz) */
#define T0_COUNTS_PER_US    ((u16)(SYSCLK / 12000000UL))

/* longest single Timer0 shot we allow ourselves to request */
#define T0_MAX_US           30000u

/* TF0 轮询的迭代预算。
 *
 * 定值规则: 守卫在正常运行时绝不能触发, 所以它的总耗时要明显【大于】
 * 本文件里最长的一次延时。65535 轮按 0.3us(快)~1.0us(悲观) 算是 20~66ms,
 * 而最长的两次请求是 16T 同步段 5600us 和 OOK_DelayMs 的 5000us 分片,
 * 最坏情况仍有 3.5~4 倍余量, 不可能被截断。
 *
 * OOK_DelayMs 按 5ms 而不是 30ms 分片, 纯粹为了保住这个比例: 30ms 分片会和
 * 守卫本身同一量级, 循环稍快就会把它悄悄缩短。
 */
#define T0_GUARD_MAX        65535u

/* ms delay chunk size; see the guard sizing rule above */
#define T0_MS_CHUNK_US      5000u

/* fallback ID used when the factory ID cannot be read, so that the node
 * still shows up (with a colliding ID) instead of staying invisible */
static u8 code s_id_fallback[7] = { 0x4Bu, 0x43u, 0x33u, 0x31u, 0x35u, 0xAAu, 0x01u };

static u8 s_id[7];
static u8 s_frame[OOK_PAYLOAD_LEN];
static u8 s_seq;


/* ------------------------------------------------------------------ */
/* shared timing base                                                  */
/* ------------------------------------------------------------------ */

////////////////////////////////////////
// 微秒级延时: 借用 Timer0 计数, 不占用 CPU 空转
// 入口参数: us (延时微秒数, 0 表示直接返回)
// 函数返回: 无
////////////////////////////////////////
void OOK_DelayUs(u16 us)
{
    u32 ticks;
    u16 reload;
    u16 data guard;

    if (us == 0u)
    {
        return;
    }
    if (us > T0_MAX_US)
    {
        us = T0_MAX_US;
    }

    ticks  = (u32)us * (u32)T0_COUNTS_PER_US;
    if (ticks > 65535UL)
    {
        ticks = 65535UL;
    }
    reload = (u16)(65536UL - ticks);

    TF0 = 0;
    TH0 = (u8)(reload >> 8);
    TL0 = (u8)(reload & 0x00FFu);
    TR0 = 1;

    /* bounded poll: see T0_GUARD_MAX. Never spin forever with DATA high. */
    guard = 0u;
    while ((TF0 == 0) && (guard < T0_GUARD_MAX))
    {
        guard++;
    }

    TR0 = 0;
    TF0 = 0;
}


////////////////////////////////////////
// 毫秒级延时: 拆成 5ms 小段, 保证单次不会接近超时守卫的尺度
// 入口参数: ms (延时毫秒数)
// 函数返回: 无
////////////////////////////////////////
void OOK_DelayMs(u16 ms)
{
    while (ms >= 5u)
    {
        OOK_DelayUs(T0_MS_CHUNK_US);
        ms -= 5u;
    }
    if (ms != 0u)
    {
        OOK_DelayUs((u16)(ms * 1000u));
    }
}


/* ------------------------------------------------------------------ */
/* unique ID                                                           */
/* ------------------------------------------------------------------ */

////////////////////////////////////////
// 读取出厂唯一 ID (手册 7.3.3 节: 位于 ROM 末字节区)
// 入口参数: index (0 ~ 6)
// 函数返回: ID 字节
////////////////////////////////////////
static u8 OOK_ReadIdRom(u8 index)
{
    u8 code *p;

    p = (u8 code *)OOK_ID_ADDR;
    return p[index];
}


////////////////////////////////////////
// 判断 ID 是否有效 (全 FF 或全 00 视为无效)
// 入口参数: 无
// 函数返回: 1 有效, 0 无效
////////////////////////////////////////
static u8 OOK_IdIsValid(void)
{
    u8 i;
    u8 all_ff;
    u8 all_00;

    all_ff = 1u;
    all_00 = 1u;
    for (i = 0u; i < 7u; i++)
    {
        if (s_id[i] != 0xFFu)
        {
            all_ff = 0u;
        }
        if (s_id[i] != 0x00u)
        {
            all_00 = 0u;
        }
    }
    return (u8)(!(all_ff || all_00));
}


u8 OOK_IdByte(u8 index)
{
    if (index >= 7u)
    {
        return 0x00u;
    }
    return s_id[index];
}


/* ------------------------------------------------------------------ */
/* waveform generator                                                  */
/* ------------------------------------------------------------------ */

////////////////////////////////////////
// 发送一个数据位: 0 = 1T 高 + 3T 低, 1 = 3T 高 + 1T 低
// 入口参数: one (0 或 1)
// 函数返回: 无
////////////////////////////////////////
static void OOK_SendBit(u8 one)
{
    TX433_HIGH();
    if (one != 0u)
    {
        OOK_DelayUs((u16)(OOK_T_US * 3u));
        TX433_LOW();
        OOK_DelayUs(OOK_T_US);
    }
    else
    {
        OOK_DelayUs(OOK_T_US);
        TX433_LOW();
        OOK_DelayUs((u16)(OOK_T_US * 3u));
    }
}


////////////////////////////////////////
// 发送前导码: OOK_PREAMBLE_PULSES 个 2T 方波 (占空比 50%, 抑制接收头 AGC 漂移)
// 入口参数: 无
// 函数返回: 无
//
// ★ 脉冲数从 32 降到 20，省 16.8ms/帧（详见 ook433.h 的注释）。
//   注意：主机侧 OokReceiver.h 的 OOK_PRE_WINDOW(32 段) 仍能容纳
//   20 脉冲（= 40 段），但若再往下砍到 16 脉冲以下就会【静默失联】。
////////////////////////////////////////
static void OOK_SendPreamble(void)
{
    u8 i;

    for (i = 0u; i < OOK_PREAMBLE_PULSES; i++)
    {
        TX433_HIGH();
        OOK_DelayUs((u16)(OOK_T_US * 2u));
        TX433_LOW();
        OOK_DelayUs((u16)(OOK_T_US * 2u));
    }
}


////////////////////////////////////////
// 发送同步头: 1T 高 + 16T 低 (全帧最长的低电平, 供接收端定位)
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void OOK_SendSync(void)
{
    TX433_HIGH();
    OOK_DelayUs(OOK_T_US);
    TX433_LOW();
    OOK_DelayUs((u16)(OOK_T_US * OOK_SYNC_LOW_T));
}


////////////////////////////////////////
// 发送载荷: 10 字节, 每字节高位在前, 共 80 位
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
static void OOK_SendPayload(void)
{
    u8 i;
    u8 b;
    u8 v;

    for (i = 0u; i < OOK_PAYLOAD_LEN; i++)
    {
        v = s_frame[i];
        for (b = 0u; b < 8u; b++)
        {
            OOK_SendBit((u8)((v & 0x80u) ? 1u : 0u));
            v = (u8)(v << 1);
        }
    }
}


/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

////////////////////////////////////////
// CRC-8/ATM: 多项式 0x07, 初值 0x00, 不反转, 输出不异或
// 入口参数: pbuf (待校验数据), len (字节数)
// 函数返回: CRC 值
////////////////////////////////////////
u8 OOK_Crc8(const u8 *pbuf, u8 len)
{
    u8 c;
    u8 i;
    u8 b;

    c = 0x00u;
    for (i = 0u; i < len; i++)
    {
        c ^= pbuf[i];
        for (b = 0u; b < 8u; b++)
        {
            if (c & 0x80u)
            {
                c = (u8)((c << 1) ^ 0x07u);
            }
            else
            {
                c = (u8)(c << 1);
            }
        }
    }
    return c;
}


////////////////////////////////////////
// 组帧并发送: 唯一 ID + 标志 + 序号, 末位附 CRC
// 入口参数: flags (FLAGS 字节)
// 函数返回: 无
////////////////////////////////////////
void OOK_SendFrame(u8 flags)
{
    u8 saved_ea;
    u8 i;

    for (i = 0u; i < 7u; i++)
    {
        s_frame[i] = s_id[i];
    }
    s_frame[OOK_OFF_FLAGS] = flags;
    s_frame[OOK_OFF_SEQ]   = s_seq;
    s_frame[OOK_OFF_CRC]   = OOK_Crc8(s_frame, (u8)(OOK_PAYLOAD_LEN - 1u));

    saved_ea = (u8)EA;
    EA = 0;                             /* keep the bit stream jitter free */

    OOK_SendPreamble();
    OOK_SendSync();
    OOK_SendPayload();

    TX433_LOW();                     /* leave the transmitter idle */
    EA = saved_ea;

    s_seq++;
}


////////////////////////////////////////
// 发射初始化: 配置 Timer0 为 12T 模式, 并装载唯一 ID
// 入口参数: 无
// 函数返回: 无
////////////////////////////////////////
void OOK_Init(void)
{
    u8 i;

    /* Timer0 is no longer used for bit timing (see OOK_DelayUs), but it is
     * still parked in a known idle state so nothing ever runs unattended. */
    AUXR &= (u8)~0x80u;                     /* T0x12 = 0 -> 12T mode      */
    TMOD  = (u8)((TMOD & 0xF0u) | 0x01u);   /* Timer0 mode 1, GATE0=0, C/T0=0 */
    TR0 = 0;
    TF0 = 0;
    TH0 = 0;
    TL0 = 0;

    TX433_LOW();                         /* transmitter idle, no carrier */

    for (i = 0u; i < 7u; i++)
    {
        s_id[i] = OOK_ReadIdRom(i);
    }
    if (!OOK_IdIsValid())
    {
        for (i = 0u; i < 7u; i++)
        {
            s_id[i] = s_id_fallback[i];
        }
    }

    s_seq = 0u;
}
