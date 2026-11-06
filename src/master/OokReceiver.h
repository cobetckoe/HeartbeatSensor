/*
 * OokReceiver -- OOK/ASK 接收解码器（主机侧）
 *
 * 协议见 docs/PROTOCOL-433M.md，与 STC8 从机固件成对实现。
 *
 * ★ 载波频率无关（本工程已统一为 433 MHz 方案）
 *   本协议是纯粹的脉宽编码，解码只依赖 DATA 脚上的高低电平时序，
 *   与载波频率无关 —— 只要发射/接收模块同频段即可。
 *   项目已确定统一采用 433 MHz 模组；换频段不需要改任何一行解码代码，
 *   只需按波长剪天线（λ/4 ≈ 17.3cm，裸铜丝）。
 *   若将来改回 315M 模组，同样无需改代码，只是天线长约 23.8cm。
 *
 * ★ 为什么不用 rc-switch 的接收端
 *   rc-switch 在 ISR 里跑 receiveProtocol() 逐位比对，一帧 146ms 全程占着
 *   CPU，会把 20MHz SPI 整屏推送拖出可见卡顿。这里 ISR 只做 micros() + 一次
 *   数组写入（几百 ns），整帧 CPU 占用 < 0.1%，刷屏完全不受影响。
 *   （rc-switch 的编码格式与本协议一致，它的嗅探工具可以用来抓包调试。）
 *
 * ★ 关于极性
 *   本协议是脉宽编码，ISR 只记「相邻边沿之间隔了多久」，这串时长整体反相后
 *   不变 —— 所以同步头 / 前导码的闸门判定天然与极性无关。
 *   但载荷位是「段 a 短还是段 b 短」，段 a 到底是高电平段还是低电平段，
 *   取决于接收模块 DATA 脚的极性（超再生 R1A / 超外差各家不同）。
 *   因此 decode() 用 CRC-8 对两种极性各解一遍来仲裁，不需要事先知道极性。
 *
 * ★ 三级抗噪门槛（433M 接收头底噪很大，必须严格）
 *   1) 必须找到 3.5~9.1ms 的超长低/高电平（同步头）
 *   2) 同步头前必须有 ≥20 个 2T 前导脉冲（噪声几乎凑不出来）
 *   3) 每位 (段a+段b) 的比值必须 ≈1:3（一长一短），最后过 CRC-8
 *
 * ★ 位判决为什么不写死绝对时长
 *   接收头带 AGC，同一帧内突发前段的幅度会变化，个别脉冲对会被局部压缩。
 *   协议的不变量其实是「两段之比约为 1:3」—— 这是比值关系，与绝对时长、
 *   幅度、AGC 漂移全都无关。因此三层判定依次收紧：
 *     ① 全帧 160 段先做短/长双簇聚类，得到 sMean / lMean 与中点 split
 *     ② 每位拿自己那两段相互比较（a<b 即 a 短），免疫整体漂移
 *     ③ 比值闸门 |lo/hi| 必须满足 3*lo <= 2*hi，拒掉「两段差不多长」的假位
 *   （早期用过的「两段之和必须落在 1000~1900us」绝对闸门已被证明会在
 *     局部压缩时误杀好帧，实测占全部解帧失败的 100%，已彻底删除）
 *
 * 用法：
 *   OokReceiver ook;
 *   ook.begin(OOK_RX_PIN);
 *   // loop 里：
 *   ook.poll();
 *   OokFrame f;
 *   while (ook.pop(f)) { ... }
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */

#ifndef OOK_RECEIVER_H
#define OOK_RECEIVER_H

#include <Arduino.h>
#include "driver/rmt.h"

// ══════════════════════════════════════════════════════
//  协议常量 —— 必须与 docs/PROTOCOL-433M.md 及从机固件一致
// ══════════════════════════════════════════════════════
// ── 协议常量（★ 仅作文档参考，接收端不引用这些宏）──
//   ★★ 权威定义在从机：src/slave/source/include/ook433.h
//      OOK_T_US / OOK_PREAMBLE_PULSES / OOK_SYNC_LOW_T
//      接收端靠【绝对微秒窗口】判定（OOK_PRE_LO/HI、OOK_SYNC_MIN/MAX 等），
//      不靠这里的宏。所以这几个值改了【不会】影响接收，
//      但必须跟着从机改，否则注释与实物不符，调参时会被误导。
//   ★ 前导已从 32 脉冲降到 20（省 16.8ms/帧），见从机 ook433.h 的说明。
#define OOK_T_US             350      // 基本时间单位
#define OOK_PREAMBLE_PULSES  20       // 前导码 2T 脉冲个数（= 40 个 2T 时长段）
#define OOK_SYNC_LOW_T       16       // 同步头低电平 = 16T
#define OOK_PAYLOAD_BITS     80       // 10 字节 × 8
#define OOK_PAYLOAD_LEN      10

// 一帧的总时长段数 = 40(前导) + 2(同步) + 160(数据) = 202
// ★ 实际可捕获 201 段：末位的低电平段直接接帧间隔，被硬件当空闲丢弃。
//   旧的 226 段对应 32 脉冲前导，已随前导缩短而更新。
//   ★ 这个 202 只是【说明性常量】：解码时真正的段数校验是
//     `need = i + 1 + OOK_PAYLOAD_BITS * 2 - 1`（= 159 载荷段），
//     由它算出来，不读 OOK_FRAME_SEGMENTS。故该宏已删除，数字留在此注释。

// ── 内部缓冲 ──
#define OOK_EDGE_CAP         400      // 时长缓冲（202 + 余量，可容两帧粘连）
// ★ 8 → 16：单帧从机连发 2~3 帧，4 个防区同时突发 = 12 帧 > 8。
//   环形队列满时丢的是【最旧】那一帧，也就是最先解出的那台从机的
//   唯一有效副本 —— 表现为「某个防区偶发整轮丢失」，与射频无关。
//   16 槽 = 4 区 × 3 帧 + 4 余量，彻底消除这条互相挤占的路径。
#define OOK_QUEUE_LEN        16       // 已解码帧队列

// decode() 失败原因的种类数（用于失败直方图，见 _failHist）
#define OOK_FAIL_KINDS       12

// 单比特纠错用的「已知从机 ID」环形表深度。
// 4 个防区 + 余量 = 8，足够覆盖交错到达的多防区帧。
#define OOK_ID_MEM           8

// ══════════════════════════════════════════════════════
//  ★★ 采集层：RMT 硬件捕获（解决「推屏就丢帧」的唯一根治手段）
// ══════════════════════════════════════════════════════
//  旧实现用 attachInterrupt + micros() 记录边沿时间戳。问题在于：
//  SPI 推屏期间（最长 13ms）中断被推迟，ISR 里读到的 micros() 是【迟到】的，
//  记下来的段长因此全错 —— 丢一个边沿那位就判错，整帧 CRC 就废。
//  实测三组对照：
//     busy() 全禁推屏 → crc=0，但每 3.4s 冻屏 200ms（maxGapMs=200）→ 用户觉得卡
//     完全放开推屏   → 满 50FPS，但 crc=1~3、ok=0~2                  → 收帧被毁
//     像素预算限流   → fps 仍 250（脏块本就小于预算），crc=2~6       → 无效
//  结论：「推小块也安全」是错的，只要收帧期间动 SPI 就丢边沿。
//
//  RMT 由【硬件】计时并缓存边沿，CPU 忙不忙都能记下来，主循环只是晚点来取。
//  这样推屏与收帧彻底解耦 → 可以满帧率刷新，同时 crc=0。
//
//  ★ 通道编号：C3 的 RMT 共 4 通道，但收发是分开的硬件——
//    ch0/ch1 = 只能【发送】，ch2/ch3 = 只能【接收】。
//    用 ch0 做接收会直接 rmt_config 失败（实测 ESP_ERR_INVALID_ARG）。
//    故接收必须用 ch2 或 ch3。
//  ★ 容量校验：一帧 202 段 / 146ms，单次推屏最长 13ms → 期间最多攒 18 个边沿；
//    RX 通道最多只能占 2 块内存（ch2 + ch3）= 96 段硬件缓存，
//    环形缓冲 4096B = 1024 段，余量充足。
#define OOK_RMT_CHANNEL      2        // RX 必须用 ch2/ch3（ch0/ch1 是 TX-only）
#define OOK_RMT_CLK_DIV      80       // 80MHz/80 → 1 tick = 1us
#define OOK_RMT_MEM_BLOCKS   2        // RX 最多 2 块 = 96 段硬件缓存
#define OOK_RMT_RX_BUF       4096     // 环形缓冲字节数（1024 个段）

// ── 判定窗口（微秒）──
#define OOK_FRAME_GAP_US     12000    // 超过此间隔视为帧间隔 → 重置缓冲

// ★★ 硬件空闲阈值（RMT idle_threshold 专用）
//
//   一帧的最后一个边沿到来之后，RMT 要等「连续 idle 这么久没有跳变」
//   才认为这次传输结束，然后才调用 _endOfTransfer() 去解码。
//   也就是说：从机发完 → 主机白等 idle_threshold → 才开始解码。
//
//   ★★ 曾尝试降到 9000 以减少 3ms 解码延时，已【否决并回退】：
//     帧内最长的段是同步头 16T ≈ 5600?s（实测 5528?s），
//     但弱信号下超再生接收头会把这一段拉长到 7000~8000?s。
//     9000 与最坏情况只差 1000?s，一旦超过就整帧被硬件截断 ——
//     而且这种截断【不进 crcErrors】，只表现为 ok 计数不涨，
//     现场症状是「离远一点就时好时坏」，比延时问题难查十倍。
//     收益只有 3ms / 146ms = 2%，完全不划算。
//
//   ★ 延时的大头在【心跳周期 2000ms】和【让路窗】，不在这 3ms。
//     真正有效的优化是缩短前导码（-16.8ms）和心跳（-1000ms）。
//
//   12000 与 OOK_SYNC_MAX(9100) 之间留了 2.9ms 安全间隙，
//   又远小于最小帧间隙 20000?s（否则两帧会被合并成一次传输）。
//   ★ 值恒等于 OOK_FRAME_GAP_US，用别名而不是再写一个 12000：
//     两个独立数字一旦有人只改一个，就会出现「分帧阈值与空闲阈值不一致」
//     的隐蔽 bug。
#define OOK_IDLE_US          OOK_FRAME_GAP_US
#define OOK_SYNC_MIN         3500     // 同步低电平下限  (10T)
// ★★ 同步低电平上限必须【严格小于】OOK_IDLE_US，否则同步头本身
//    就会被硬件当成「空闲」而提前截断一帧 —— 这一帧在解码前就没了，
//    且不会进 crcErrors，只表现为 ok 计数不涨，极难定位。
//    9100 < 12000 ? 有 2.9ms 安全间隙。
//    ★ 曾评估过把 idle 降到 9000（省 3ms 解码延时），已【否决】：
//      弱信号下超再生接收头会把 16T 同步头拉长，一旦超过 idle 就整帧截断，
//      而 3ms / 146ms 帧长 = 2% 的收益，换来「离远一点就收不到」的风险，
//      完全不划算。延时的大头在心跳周期（2000ms）和前导（28ms），
//      不在这 3ms。
#define OOK_SYNC_MAX         9100     // 同步低电平上限  (26T)
#define OOK_BIT_MIN          180      // 单段有效下限（约 0.5T）
#define OOK_BIT_MAX          1600     // 单段有效上限（约 4.6T）
#define OOK_BIT_SPLIT        700      // 1T / 3T 分界（正好是 2T）
// ── 【已删除】OOK_SUM_MIN = 1000 / OOK_SUM_MAX = 1900 ──
//   原本用来卡「一位 = 段a + 段b」的合法总长（1T+3T = 1400us）。
//   实际解码改成【双簇自适应门限】后（见 decode()：把所有段长按中间点
//   分成两簇，门限取两簇中点），逐位的合法性由「分簇是否干净」承担，
//   不再有固定总长窗口 —— 这样近距饱和把 3T 压缩到 1200us 也能解。
//   两个宏因此零引用，删掉以免误以为还存在固定窗口。
#define OOK_PRE_LO           500      // 前导脉冲 2T 判定下限
#define OOK_PRE_HI           900      // 前导脉冲 2T 判定上限

// ★★ 前导码闸门 —— 与从机 OOK_PREAMBLE_PULSES 强耦合，改一边必改另一边
//
//   从机侧前导已从 32 脉冲降到 20 脉冲（见 src/slave/source/include/ook433.h），
//   目的是省掉 16.8ms/帧的空口时间。这里必须能容得下更短的前导：
//     20 脉冲 = 40 个段（每脉冲 = 2T 高 + 2T 低 = 2 段）
//     WINDOW 32 < 40  → 检查窗口仍能覆盖到最后 32 段  ?
//     MIN_MATCH 20   → 40 段里命中 40 个，远超 20         ?
//   所以这里【不需要】跟着改，8 段余量仍在。
//
//   ★ 危险区：若把从机前导再砍到 16 脉冲以下（= 32 段以下），
//     窗口检查 total < OOK_PRE_WINDOW 就会失败 → 帧被整帧丢弃，
//     而且【不产生任何错误日志】（只是 sync/ok 计数不涨），
//     表现为「从机明明在发，主机一帧都收不到」的静默失联。
//     那种情况下必须同步把 WINDOW 调到 ≤ 2×脉冲数，MIN_MATCH 调到 ~一半。
#define OOK_PRE_WINDOW       32       // 检查同步头前多少个时长段
#define OOK_PRE_MIN_MATCH    20       // 其中至少多少个必须落在 2T 窗口内
#define OOK_MAX_RESYNC       40       // 一轮里最多尝试多少个同步头候选
// ★ 毛刺抑制：短于 ~100us 的段视为干扰，由硬件丢弃（不记入 _dur）。
//   协议里最短的合法段是 1T = 350us，所以 150us 以下的跳变必然无信息量。
//   实测依据：从机 P5.5 直连 GPIO20 时，3.5 秒内出现 58639 个边沿，
//   而协议理论只有 ~4275 段 —— 多出来的 54000 个全是 3~6us 高频振荡，
//   把真实段序列完全冲散，导致 sync/ok 恒为 0。
//   滤掉后 clean[] 立刻出现 800~900us 规整段，证明从机编码本身是好的。
//   ★ 实现方式已改为 RMT 硬件滤波：cfg.rx_config.filter_ticks_thresh = 100
//     （1 tick = 1us，即滤掉 <100us 的毛刺）。软件侧不再有 OOK_GLITCH_US
//     比较循环 —— 保留一个没人读的宏只会让人以为还有一条软件过滤路径。

// ── 【已删除的三个宏】OOK_BURST_HOLD_US / OOK_BUSY_SEGS / OOK_FRAME_MAX_US ──
//   它们原本组成一套「让路窗」机制：解出一帧后盖 6ms 静默章（_quietUntil），
//   并在检测到「前导码成型」（连续 N 段落进 2T 窗口）时把让路窗推到帧尾
//   （_frameUntil），主循环据此判断能否推屏。
//
//   ★ 为什么可以整套删掉（这里曾有大量实测调参记录，删宏但保留结论）：
//     这两个时间戳的两个读取方 busy() 和 quietLeftMs() 都已不存在
//     （busy() 只是 inFrame() 的别名；quietLeftMs() 零调用），
//     写进去之后再没人读 → 纯死写。
//     而主循环的门控条件本身更强：
//       rxBusy = ookReceiver.inFrame() || ookReceiver.runSegs() > 0
//     runSegs() 一看到「缓冲里有任何未处理段」就挡，比「连续 6 段落进
//     2T 窗口」严格得多 —— 帧的第一段就挡住了，不存在「拉起延迟」。
//     所以「让路窗」在功能上是冗余的，每轮 poll 只是白跑 6 次比较。
//
//   ★ 保留下来的实测结论（以后若真要做「让路窗」请先读这段）：
//     · 6ms 静默章是错的：帧间隔 20~59ms，6ms 一过就放行，正好踩在下一帧
//       前导上 → 每轮突发只解出 1 帧，11 分钟 209/440 = 命中率 47.5%。
//     · 改成 30ms（想盖住整个帧间隙）更糟：让路总时长 240ms→810ms，
//       每 3.4 秒冻结 810ms，帧率 40FPS→2FPS，用户直接反馈「屏幕变卡」。
//     · 「攒够 N 段才拉起保护」的方案：N=16 时拉起延迟 11.2ms，
//       与 20ms 推屏周期的撞车概率 71%（实测命中率 47.5% 与此吻合）；
//       N=6 时降到 36%。但既然现在 runSegs() 从第一段就挡，这条不适用。
//     · 结论：帧间隙不该用「盖时间戳」的方式封死，那是把
//       「不能推屏」和「不能收帧」混为一谈。正确判据是 pushSafe() 的三条。

//   ★ 推屏安全判据：距上一个边沿 ≥ 10ms 才算「在两帧之间」。
//     依据（实测）：帧内最长的一段是同步头的 16T ≈ 5.6ms，
//     而帧间隙（上一帧末位 → 下一帧前导）实测 20~59ms。
//     10ms 稳稳高于帧内最长段、又远小于最小帧间隙，
//     于是「≥10ms 无边沿」等价于「当前一定在两帧之间，推屏不会致盲接收机」。
//     ★ 不要再调大到 35ms：突发内两帧之间的间隔约 30ms，
//       调大后等于只在突发与突发之间才推屏，画面照样顿（实测 maxGapMs≈200）。
#define OOK_GAP_SAFE_US      10000    // 距上个边沿 ≥10ms → 在帧间隙（帧内最长 5.6ms，帧间空隙 20~59ms）

// ── 重复帧窗口：解出一帧后的这么多 ms 内，本突发的后续重复帧可被牺牲 ──
//   从机连发 2~3 帧、内容相同、间隔约 20~59ms；整串约 500ms。
//   取 900ms 覆盖整串，让这期间推屏不必等满 OOK_GAP_SAFE_US。
//
//   ★★ 关键约束：这个窗口【只跳过时间门限】，绝不跳过「帧内不许推」。
//     早期实现是无条件 return true，等于「解出任意一帧后 900ms 内，
//     哪怕正在收的是另一台从机的首帧，也照推不误，把它打散」。
//     单防区时看不出问题（后面收到的本来就是自己的重复帧，丢了无所谓）；
//     多防区时后果严重：A 区一帧解码成功 → 接下来 900ms 全放行 →
//     正好撞上 B 区的突发 → B 区第 1 帧（唯一有效副本）被 SPI 致盲打掉
//     → B 区整轮丢失 → 心跳稀疏 → 被判离线 / 报警来回复位。
//     现场表现就是「A 一动，B 就掉线」这类看似毫无逻辑的防区耦合。
//     所以 _inFrame / _head 的帧内否决必须先于本窗口生效。
#define OOK_REPEAT_WIN_MS    900

// ── FLAGS 位 ──
//   ★ 只有 PRESENCE / PAIRREQ / BOOT 三个位在当前固件里被真正读取。
//     下面注释里提到的 TAMPER(0x02) / LOWBAT(0x04) 是协议保留位 ——
//     从机目前从不置位，主机也不判断，所以【没有对应宏】。
//     若以后从机真加了防拆/低电上报，在这里补回 0x02 / 0x04 即可，
//     不要复用其他位的数值。
#define OOK_FLAG_PRESENCE    0x01     // 检测到人（核心开关量）
#define OOK_FLAG_PAIRREQ     0x08     // 主动请求配对

// ── 0x80：从机刚重启（开机后前几帧置位，正常运行永不置位）──
//   ★ 为什么需要这个独立位（一次真实的误判事故）：
//     seq 只有 8 位，256 帧回绕一次；从机实测 ~35~40 帧/分，
//     回绕周期只要 6~7 分钟。于是「seq 变小」既可能是重启，也可能是回绕，
//     靠猜是无法区分的：
//       判定窗口 5s  → 丢帧 85% 时相邻两帧间隔常 >5s，必然漏检，rst 恒为 0
//       判定窗口 60s → 离 6 分钟回绕周期太近，会把正常回绕误报成重启
//     两者实测都被证伪过，所以改用协议位做确定性判据。
//   ★ 不要把它当作「状态位」去和状态掩码比较，
//     它只描述「这一帧来自一次刚开机的从机」，不描述报警内容。
//   ★ 曾经还定义过一个 OOK_FLAG_STATE_MASK = 0x0F（描述报警状态的位集合），
//     但全工程没有任何地方用它做掩码运算 —— flags 是直接按位测的。
//     已删除，避免留下一个「看起来在用」的假接口。
#define OOK_FLAG_BOOT        0x80

// ══════════════════════════════════════════════════════
//  一帧解码结果
// ══════════════════════════════════════════════════════
struct OokFrame {
  uint8_t  id[7];      // STC8 出厂唯一 ID
  uint8_t  flags;      // OOK_FLAG_*
  uint8_t  seq;        // 帧序号（用于去重）
  uint32_t ms;         // 主机收到的时间戳
  uint16_t unit_us;    // 本次实测的 T（?s），用于判断信号质量
};

// CRC-8/ATM (poly 0x07, init 0x00) —— 与从机 crc8() 必须一致
static inline uint8_t ookCrc8(const uint8_t *d, size_t n) {
  uint8_t c = 0x00;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++)
      c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}

static inline bool ookIdEqual(const uint8_t *a, const uint8_t *b) {
  for (int i = 0; i < 7; i++) if (a[i] != b[i]) return false;
  return true;
}

// ID → "A1:B2:C3:D4:E5:F6:07"（网页 / 日志显示用），buf 至少 21 字节
static inline void ookIdToStr(const uint8_t *id, char *buf, size_t n) {
  if (n < 21) { if (n) buf[0] = '\0'; return; }
  snprintf(buf, n, "%02X:%02X:%02X:%02X:%02X:%02X:%02X",
           id[0], id[1], id[2], id[3], id[4], id[5], id[6]);
}

// ══════════════════════════════════════════════════════
class OokReceiver {
public:
  OokReceiver()
    : _pin(-1), _head(0), _consumed(true),
      _mux(portMUX_INITIALIZER_UNLOCKED),
      _qHead(0), _qTail(0), _lastUnitUs(0),
      _okFrames(0), _crcErrors(0), _noiseBursts(0), _overflow(0), _invFrames(0),
      _lastOkMs(0),
      _rmtInstalled(false), _rb(nullptr) {
    for (int i = 0; i < OOK_EDGE_CAP; i++) _dur[i] = 0;
    for (int i = 0; i < OOK_FAIL_KINDS; i++) _failHist[i] = 0;
    memset(_idMem, 0, sizeof(_idMem));
    _idMemN = 0;
    _idMemNext = 0;
    _bitFixed = 0;
    memset(_fixBitHist, 0, sizeof(_fixBitHist));
  }

  // ── 启动：pin = 433M 接收模块的 DATA 输出脚 ──
  bool begin(int pin) {
    if (_pin >= 0) end();
    _pin = pin;
    pinMode(_pin, INPUT);           // 接收模块自带驱动，不需要内部上拉
    _head = 0;
    _consumed = true;

    // ── RMT 接收通道：硬件记录每段电平持续时长（单位 1us）──
    //   ★ filter_ticks_thresh = 100 → 滤掉 <100us 的毛刺（等价旧 OOK_GLITCH_US）
    //   ★ idle_threshold = OOK_IDLE_US (9000) → 9ms 无跳变即认为一次传输结束
    //     ★ 这里【不再是】OOK_FRAME_GAP_US(12000)：改成 9ms 是为了减少
    //       解码延时（从机发完要等 idle 超时才解码），详见 OOK_IDLE_US 注释。
    //     这个值必须 <= OOK_FRAME_GAP_US，才能把帧间隔切成「一次传输」，
    //     使每帧独立解出（与旧 ISR 的 d > OOK_FRAME_GAP_US 判据一致）。
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_RX((gpio_num_t)pin,
                                             (rmt_channel_t)OOK_RMT_CHANNEL);
    cfg.clk_div        = OOK_RMT_CLK_DIV;
    cfg.mem_block_num  = OOK_RMT_MEM_BLOCKS;
    cfg.rx_config.filter_en          = true;
    cfg.rx_config.filter_ticks_thresh = 100;
    cfg.rx_config.idle_threshold      = OOK_IDLE_US;

    esp_err_t err = rmt_config(&cfg);
    if (err != ESP_OK) {
      Serial.printf("[OOK] rmt_config failed err=0x%x ch=%d blocks=%d\n",
                    (unsigned)err, (int)cfg.channel, (int)cfg.mem_block_num);
      return false;
    }
    err = rmt_driver_install(cfg.channel, OOK_RMT_RX_BUF, 0);
    if (err != ESP_OK) {
      Serial.printf("[OOK] rmt_driver_install failed err=0x%x\n", (unsigned)err);
      return false;
    }
    rmt_get_ringbuf_handle(cfg.channel, &_rb);
    if (_rb == nullptr) {
      Serial.printf("[OOK] ringbuf null\n");
      return false;
    }
    rmt_rx_start(cfg.channel, true);
    _rmtInstalled = true;

    Serial.printf("[OOK] RX on GPIO%d via RMT ch%d blocks=%d\n",
                  _pin, (int)OOK_RMT_CHANNEL, (int)OOK_RMT_MEM_BLOCKS);
    return true;
  }

  void end() {
    if (_pin < 0) return;
    if (_rmtInstalled) {
      rmt_rx_stop((rmt_channel_t)OOK_RMT_CHANNEL);
      rmt_driver_uninstall((rmt_channel_t)OOK_RMT_CHANNEL);
      _rmtInstalled = false;
      _rb = nullptr;
    }
    _pin = -1;
  }

  bool isAttached() const { return _pin >= 0; }

  // ── 主循环里调用（非阻塞；没有新数据时几乎零开销）──
  //   ★ RMT 版本：从环形缓冲取「硬件记录的段序列」，交给 decode()。
  //     decode() 一行未改 —— 它只依赖「一串时长」，
  //     与这些时长是 ISR 记的还是 RMT 记的无关。
  //   ★★ 切帧关键：硬件每个 item32 装两段（dur0 + dur1），
  //     一次传输的最后一项把 dur1 填 0 作结束标记。
  //     必须在【遇到结束标记的那一刻】就把这一帧解掉并清缓冲，
  //     绝不能先攒完整个环形缓冲再回头找帧尾 —— 因为一次 poll() 里
  //     很可能同时排到「上一帧的尾巴」+「下一帧的开头」，
  //     那样 _dur 会变成 201+N 段，need 永远对不上，整帧报废。
  void poll() {
    if (_pin < 0 || _rb == nullptr) return;

    // ── 1) 排空环形缓冲；见结束标记即切帧解码 ──
    //   ★ 必须 while 排空：xRingbufferReceive 每次只返回【一个】item
    //     （驱动用 NOSPLIT 环形缓冲，一次 send = 一个 4 字节 item），
    //     一帧 201 个 item、约 1380 item/s。每次只取一个 → 缓冲 1024 项
    //     几毫秒就满，ofl 疯涨、数据错位、crc 全错（实测 crc=2223/5s）。
    size_t n = 0;
    rmt_item32_t *items = nullptr;
    while ((items = (rmt_item32_t *)xRingbufferReceive(_rb, &n, 0)) != nullptr) {
      size_t cnt = n / sizeof(rmt_item32_t);
      for (size_t i = 0; i < cnt; i++) {
        rmt_item32_t it = items[i];
        if (it.duration0 != 0) { if (_head == 0) _inFrame = true; _pushDur(it.duration0); }
        if (it.duration1 != 0) _pushDur(it.duration1);
        else                   _endOfTransfer();   // 一次传输结束 → 立刻切帧
      }
      vRingbufferReturnItem(_rb, (void *)items);
    }

    // ── 【已删除】让路窗 arm（preRun / _frameUntil / OOK_BUSY_SEGS）──
    //   旧代码在这里判断「前导码已成型」→ 把 _frameUntil 推到帧尾，
    //   本意是给 busy()/quietLeftMs() 一个「正在收真帧」的时间戳。
    //   但这两个读取方都已不存在（busy() 只是 inFrame() 的别名，
    //   quietLeftMs() 零调用），_frameUntil 写进去之后再没人读 → 纯死写。
    //   ★ 为什么删掉不会失去任何保护：主循环的门控条件本身更强 ——
    //       rxBusy = ookReceiver.inFrame() || ookReceiver.runSegs() > 0
    //     runSegs() 的判据是「缓冲里还有任何未处理段」，比
    //     「连续 6 段落进 2T 窗口」严格得多，帧一开始就被挡住。
    //     所以这个窗口在功能上是冗余的，只是每轮 poll 白跑 6 次比较。
    //   ★ 历史调参结论（曾经很有价值，留档即可，不必留代码）：
    //     OOK_BUSY_SEGS 取 16 时保护窗要到帧开始后 11.2ms 才拉起，
    //     与 20ms 推屏周期撞车概率 71%，实测帧命中率只有 47.5%；
    //     取 6 时降到 36%。既然现在靠 runSegs() 从第一段就开始挡，
    //     这个「拉起延迟」问题从根本上不存在了。
  }

  // ── 一次 RMT 传输（= 一帧）收完：就地找同步头 + 解码 + 清缓冲 ──
  //   ★ 清缓冲无条件执行：RMT 的 idle_threshold 已经把帧切得很干净，
  //     这一次传输的数据只属于这一帧，解不出来就是这帧坏了。
  //     （旧 ISR 版靠 12ms 间隔天然重置缓冲，RMT 下没有那个副作用，
  //       必须显式清零，否则残留段会跟下一帧拼在一起 → 永久错位。）
  void _endOfTransfer() {
    int len = _head;
    _head = 0;
    _consumed = true;
    _inFrame = false;              // ★ 传输结束 → 帧间空隙，主循环可以推屏了

    if (len < 8) return;

    // 从后往前找同步头：最靠后的那个才是刚收完的那帧
    int tried = 0;
    for (int i = len - 1; i >= 1 && tried < OOK_MAX_RESYNC; i--) {
      uint16_t d = _dur[i];
      if (d < OOK_SYNC_MIN || d > OOK_SYNC_MAX) continue;
      tried++;

      // 同步头前面那一段必须是 ~1T（同步头 = 1T 高 + 16T 低）
      uint16_t dPrev = _dur[i - 1];
      if (dPrev < OOK_BIT_MIN || dPrev >= OOK_BIT_SPLIT) continue;

      // 再往前必须有足够多的 2T 前导脉冲
      int total = 0, match = 0;
      for (int k = 2; k <= OOK_PRE_WINDOW + 1; k++) {
        int idx = i - k;
        if (idx < 0) break;
        total++;
        if (_dur[idx] >= OOK_PRE_LO && _dur[idx] <= OOK_PRE_HI) match++;
      }
      if (total < OOK_PRE_WINDOW || match < OOK_PRE_MIN_MATCH) continue;

      // 数据段够不够
      // ★ 关键：末位的低电平段没有「闭合边沿」——它直接接下一帧的
      //   20~59ms 帧间隔（> OOK_FRAME_GAP_US），被硬件当作空闲丢弃。
      //   所以实际只攒得到 159 个载荷段，而不是 160。
      //   （一帧共 202 段：40 前导 + 2 同步 + 160 载荷，但只产生 201 个可捕获段）
      int need = i + 1 + OOK_PAYLOAD_BITS * 2 - 1;
      if (len < need) continue;

      OokFrame f;
      if (decode(&_dur[i + 1], len - (i + 1), f)) {
        _lastOkMs = millis();        // ★ 供主循环判断「这是不是同一突发的重复帧」
        push(f);
        // ★ 这里原来还有一句「盖静默章」：_quietUntil = micros() + 6ms。
        //   已删除 —— 它唯一的读取方 quietLeftMs() 零调用，纯写入无意义。
        //   现在保护「连发中的第 2/3 帧」靠的是 pushSafe() 里
        //   OOK_REPEAT_WIN_MS(900ms) 重复帧窗口 + 帧内绝对否决。
        return;
      }
    }

    _noiseBursts++;                  // 这一帧废了
  }

  // ── 取出一帧（FIFO，非阻塞）──
  bool pop(OokFrame &out) {
    if (_qTail == _qHead) return false;
    out = _queue[_qTail];
    _qTail = (_qTail + 1) % OOK_QUEUE_LEN;
    return true;
  }

  // ── 是否处于一次 RMT 传输内部（真帧正在收）──
  //   主循环据此决定能否进行长阻塞操作（如整屏 SPI 推送）。
  //   ★ 判据是「RMT 正在传输」，而不是「攒了任意边沿」—— 后者会被空闲
  //     噪声永久拉高（实测从机断电时噪声边沿 90s 五万+，屏幕冻在 1FPS）。
  //   ★ 历史上这里还有一个 busy() 别名（同样返回 _inFrame），已随死代码
  //     清理删掉 —— 两个名字返回同一个字段，只会让读代码的人以为它们
  //     语义不同。主循环统一用 inFrame()。
  bool inFrame() const { return _inFrame; }

  // ── 能否安全推屏（主循环每轮问一次，无副作用）──
  //   ★★ 判据来自实测，不是猜的。核心物理事实：
  //     SPI 连续传输会让 433MHz 接收机「瞎」住一段时间。若这段瞎区落在帧体
  //     中间，RMT 录到的就是一段 ~10ms 的平坦电平，解码时被当作一个超长段，
  //     整帧结构随即作废（[XFR] 实测 len=229 却只有 4 段 / len=223 SHORT）。
  //     所以判据必须是「当前绝不在一帧之内」，而不只是「缓冲有没有数据」。
  //
  //   两道放行条件：
  //     (1) 「已经解出一帧之后的重复帧」：从机每次上报连发 2~3 帧、内容完全相同。
  //         既然已经拿到一帧，后面这些重复帧即使被推屏打散也不影响功能。
  //         这是把「146ms 必冻」压成「每个突发只冻第一帧」的关键。
  //         ★ 窗口只有 OOK_REPEAT_WIN_MS(900ms)，且必须在下一个突发前失效，
  //           否则会长期压制接收（曾因 600ms 无条件放行导致命中率跌到 47%）。
  //     (2) 「帧与帧之间的真空期」：不在传输中、缓冲已清空、
  //         且距上一个边沿 ≥ OOK_GAP_SAFE_US。
  bool pushSafe() const {
    // ── (0) 帧内绝对否决，必须排在最前 ──
    //   ★ 这是多防区解耦的关键：只要 RMT 正在录一帧（或缓冲里还有未处理的段），
    //     就一律不许推屏，不管上一帧是不是刚解成功。
    //     否则「已解出一帧 → 本突发剩余重复帧随便牺牲」这条捷径
    //     会连【别的从机】的首帧一起牺牲，造成防区间互相踩踏。
    //     这一条不增加任何代价：真帧内部本来就没有可推屏的空隙。
    if (_inFrame || _head != 0) return false;

    // (1) 已解出一帧 → 本突发剩下的重复帧可以牺牲（从机每突发发 2~3 帧相同内容）
    //     注意此处已保证不在帧内，所以牺牲的确实是「完整的重复帧」。
    if (_lastOkMs != 0 && (millis() - _lastOkMs) < OOK_REPEAT_WIN_MS) return true;

    // (2) 真正空闲：必须「距上个边沿 ≥ OOK_GAP_SAFE_US」才允许推。
    //   ★ 为什么不能只看 _head==0 / !_inFrame：
    //     帧内被推屏打坏后，传输会在 9ms 空闲后结束 → _head 清零、
    //     _inFrame=false → 门控立刻放行 → 又推 → 又打坏。实测形成
    //     自持的死循环：noise 从 14 涨到 56，ok 只有 2~4 / 20s。
    //   ★ 10ms 这个值的依据：
    //     帧内最长的一段是同步头的 16T ≈ 5.6ms；
    //     帧间隙（上一帧末位到下一帧前导）实测 20~59ms。
    //     10ms 稳稳高于帧内最长段、又远小于最小帧间隙，于是判据等价于
    //     「当前一定在两帧之间」，而不是「当前在一帧里的某个长段上」。
    //     ★ 不要再调大到 35ms：那会跨过突发内 30ms 的帧间隔，
    //       等于只在突发与突发之间才推屏，画面照样顿。
    if (_lastEdgeUs == 0) return true;
    return (uint32_t)(micros() - _lastEdgeUs) >= OOK_GAP_SAFE_US;
  }

  // ── 【已删除】onPushDone() —— 不要重新引入 ──
  //   旧版在每次推屏后调用它，把 RMT 环形缓冲清空、_head 归零。
  //   那是 ISR 时代的遗留：当时靠「推屏会造成假边沿」来解释 CRC 错，
  //   于是推完就把缓冲丢掉。
  //   ★ RMT 版本下这是净损失：RMT 在硬件里把边沿录完了，
  //     清空只会丢掉【真边沿】，导致下一帧从中间开始 → 整帧错位。
  //   ★ 真正的病根是「连续 SPI 太长 → 接收机致盲 → 录到一段 ~10ms 平段」，
  //     解法是把单次推屏压在 3ms 量级（见 master.ino 的 3000px 预算），
  //     而不是事后擦屁股。
  //   ★ 另外它还会把 _head 清零 → pushSafe() 恒为真 →
  //     主循环陷入「推屏 → 停收 → 再推屏」死循环（实测 edges 掉到 66/5s）。

  // ── 【已删除】quietLeftMs() / _quietUntil / OOK_BURST_HOLD_US ──
  //   这是一套「解出一帧后给接收器盖 6ms 静默章，主循环据此让路」的机制，
  //   全工程没有任何读取方（grep quietLeftMs 只有定义本身）→ 纯死代码。
  //   ★ 删除它不改变任何行为，因为真正起作用的保护本来就在别处：
  //     · pushSafe() 的 (0) 帧内绝对否决（_inFrame || _head != 0）
  //     · pushSafe() 的 (2) 距上个边沿 ≥ OOK_GAP_SAFE_US(10ms)
  //     · pushSafe() 的 (1) OOK_REPEAT_WIN_MS(900ms) 重复帧窗口
  //   历史上曾把 6ms 改成 30ms 想盖住帧间隙，结果让路总时长从 240ms
  //   暴涨到 810ms、帧率从 40FPS 掉到 2FPS。那段教训保留在上面注释里，
  //   结论是：帧间隙不该用「盖静默章」的方式封死，而该用边沿距离判据。
  //   留一个只写不读的时间戳，只会让后来人以为还有一层保护存在。

  // ── 【已删除】lastOkMs() ──
  //   只是 _lastOkMs 的 getter，零调用。字段本身【必须保留】：
  //   pushSafe() 的 (1) 重复帧窗口就是靠它判断的（见上）。

  // 当前这次「断流之间」攒下的段数（主循环只读，用于显示让路判断）
  int runSegs() const { return _head; }

  // ── 解帧结果计数（诊断用）──
  uint32_t okFrames()    const { return _okFrames; }
  uint32_t crcErrors()   const { return _crcErrors; }
  uint32_t noiseBursts() const { return _noiseBursts; }
  uint32_t overflows()   const { return _overflow; }
  // ── 【已删除】invertedFrames() / rxInverted() ──
  //   两者都只是 _invFrames 的 getter，零调用。
  //   ★ _invFrames 字段本身【必须保留】：decode() 在「载荷按反相极性才过
  //     CRC」时自增它；如果以后要判断本机接收模块 DATA 脚极性，这是唯一证据。
  //     但既然没有任何地方读它，就不要留两个名字很像的读取接口，
  //     免得后来人以为固件会根据极性自动切换解码路径（它并不会 ——
  //     解码是双极性同时试、由 CRC 仲裁，见 decode() 注释）。

  // ── 最近一帧实测的基本时间单位 T（?s）──
  //   ★ 这是判断「链路误码到底是哪一类」的唯一直接证据，别删：
  //     协议标称 T = 350?s（1T 段 350?s、3T 段 1050?s）。
  //     · T ≈ 330~370  → 收发两端时钟都准，误码来自干扰/致盲/AGC
  //     · T 明显偏小   → 接收模块把短段压缩（近距饱和），
  //                       3T 段被压到 <OOK_BIT_MIN 就会整帧 _fail(7)
  //     · T 明显偏大   → 从机 HIRC 偏离，或 AGC 拖尾把长段拉长，
  //                       3T 段超 OOK_BIT_MAX(1600) 同样整帧废掉
  //   ★ 判读要点：如果 crc 高而 T 一直正常，问题在【干扰/推屏致盲】，
  //     继续调解码闸门毫无意义；只有 T 异常才该动 OOK_BIT_MIN/MAX。
  // ── 失败原因直方图（诊断用）──
  //   code 语义：1=载荷段不足 2/3=首轮段长越界 4=样本太少 5=两簇分不开
  //              6/7/8=判决轮段长越界 10=两段等长 11=CRC 失败
  uint16_t failCode(int code) const {
    return (code >= 0 && code < OOK_FAIL_KINDS) ? _failHist[code] : 0;
  }
  void clearFailHist() {
    for (int i = 0; i < OOK_FAIL_KINDS; i++) _failHist[i] = 0;
  }

  // ── 单比特纠错统计（诊断用）──
  uint32_t bitsFixed() const { return _bitFixed; }
  // 返回出错次数最多的比特位下标（0 = 载荷第 0 位/首字节最高位）；无记录返回 -1
  int worstFixBit() const {
    int best = -1;
    uint16_t bestN = 0;
    for (int k = 0; k < OOK_PAYLOAD_BITS; k++) {
      if (_fixBitHist[k] > bestN) { bestN = _fixBitHist[k]; best = k; }
    }
    return best;
  }
  uint16_t fixBitCount(int k) const {
    return (k >= 0 && k < OOK_PAYLOAD_BITS) ? _fixBitHist[k] : 0;
  }

  uint16_t lastUnitUs() const { return _lastUnitUs; }

private:
  // decode() 里的每道闸门都用这个单行返回，便于逐条对照协议做闸门审计
  //   ★ 顺带记一张失败原因直方图。
  //     没有这张表，「crc 一直在涨」只能靠猜是哪道闸门在拒帧；
  //     有了它，一次 60 秒就能确定是「位判错」(11=CRC)
  //     还是「段长越界」(2/3/7/8) 还是「两簇分不开」(5)。
  //     成本：失败时一次自增，可忽略。
  bool _fail(int code) {
    if (code >= 0 && code < OOK_FAIL_KINDS) _failHist[code]++;
    return false;
  }

  // ── 【已删除】bool preRun() const ──
  //   判据本来有两层：
  //     (a) 解码前的启发式——「最近 OOK_BUSY_SEGS 段全部落在 2T 前导窗口」，
  //         用来把「真帧前导」和「空闲噪声」分开（真帧前导是 40 段连续
  //         ~700us；超再生模块空闲噪声的段长是 2~3ms 量级）；
  //     (b) 供让路窗机制调用，决定何时把 _frameUntil 推到帧尾。
  //   ★ 现已无调用方，职责被两处更强的判据取代：
  //     · 收帧侧：decode() 自带完整的同步头搜索 + 前导窗口校验
  //       （OOK_PRE_WINDOW / OOK_PRE_MIN_MATCH 两重计数），
  //       噪声突发过不去；真帧走的是精确判定，比启发式更严。
  //     · 推屏侧：pushSafe() 的「10ms 无边沿」比「前导已成型」更早生效。
  //   保留一份半成品的前导检测，只会让人以为还有第三条判据在跑。
  //   ★ 顺带记下原来那个启发式为什么容易误判（以后若要复原，别再取 16）：
  //     噪声突发也能连续十几段落进窗口，取 16 段等于要求 11.2ms 才拉起，
  //     与 20ms 推屏周期撞车概率 71%；取 6 段仍有噪声误判风险。
  //     结论：这个方向本身不好，别再走。


  // ── 把一段时长追加进 _dur（等价旧 ISR 的「记录这一段」）──
  //   ★ RMT 已硬件分帧（idle_threshold 切成一次传输），等价旧 ISR 里的
  //     「d > OOK_FRAME_GAP_US → _head = 0」；毛刺也已由 filter_ticks_thresh
  //     在硬件里过滤，所以这里只负责存值。
  void _pushDur(uint32_t d) {
    if (d == 0) return;
    _lastEdgeUs = micros();        // ★ 推屏安全判据的时间基准
    if (_head < OOK_EDGE_CAP) {
      _dur[_head] = (d > 0xFFFFu) ? 0xFFFFu : (uint16_t)d;
      _head++;
      _consumed = false;
    } else {
      _overflow++;
    }
  }

  // ── 逐位解码 + CRC-8 ──
  //
  // ★ 双极性试解
  //   我们的时间数组只记「相邻边沿之间隔了多久」，整体反相不会改变这串时长，
  //   所以同步头/前导码的闸门对极性免疫。但载荷是脉宽编码：段 a 到底是
  //   「高电平段」还是「低电平段」取决于接收模块 DATA 脚的极性
  //   （超再生 R1A / 超外差各家不同，还会受模块供电影响），无法先验确定。
  //   因此把两种极性都解一遍，交给 CRC-8 仲裁——CRC 通过的那个就是对的。
  //   代价：CRC 有 1/256 的偶然通过率，但前面还有前导码闸门挡着，可接受。
  bool decode(const uint16_t *e, int avail, OokFrame &out) {
    uint8_t pNorm[OOK_PAYLOAD_LEN];   // 极性 A：a 短 ? bit 0（DATA 非反相）
    uint8_t pInv[OOK_PAYLOAD_LEN];    // 极性 B：a 短 ? bit 1（DATA 反相）
    memset(pNorm, 0, sizeof(pNorm));
    memset(pInv,  0, sizeof(pInv));

    // ★ 双簇自适应判决门限
    //   协议：每位 = 1T + 3T。实际链路（尤其超再生模块近距饱和）会把 3T 压缩、
    //   把 1T 拉长，固定门限 OOK_BIT_SPLIT(700us=2T) 就会把整帧的位成批判反。
    //   这里先从本帧自身估出「短簇均值」和「长簇均值」，取中点当门限。
    uint32_t sSum = 0, lSum = 0;
    int      sN = 0,   lN = 0;
    for (int j = 0; j < OOK_PAYLOAD_BITS; j++) {
      int qa = 2 * j;
      int qb = 2 * j + 1;
      if (qa >= avail) return _fail(1);
      uint16_t va = e[qa];
      if (va < OOK_BIT_MIN || va > OOK_BIT_MAX) return _fail(2);
      if (qb >= avail) break;            // 末位低电平段被帧间隔吞掉
      uint16_t vb = e[qb];
      if (vb < OOK_BIT_MIN || vb > OOK_BIT_MAX) return _fail(3);
      if (va < vb) { sSum += va; sN++; lSum += vb; lN++; }
      else         { sSum += vb; sN++; lSum += va; lN++; }
    }
    if (sN < 8 || lN < 8) return _fail(4);
    uint16_t sMean = (uint16_t)(sSum / (uint32_t)sN);
    uint16_t lMean = (uint16_t)(lSum / (uint32_t)lN);
    // 两簇必须分得开，否则这帧形状已坏，直接判废
    if (lMean < (uint32_t)sMean * 3u / 2u) return _fail(5);
    uint16_t split = (uint16_t)(((uint32_t)sMean + lMean) / 2u);

    uint32_t sum = 0;
    int      nsum = 0;
    for (int j = 0; j < OOK_PAYLOAD_BITS; j++) {
      int ia = 2 * j;
      int ib = 2 * j + 1;
      if (ia >= avail) return _fail(6);

      uint16_t a = e[ia];
      if (a < OOK_BIT_MIN || a > OOK_BIT_MAX) return _fail(7);
      // ★★ 相对判决：协议里每位恒为 1T+3T，两段必一长一短。
      //   所以「这一段是不是短段」根本不需要绝对门限，直接比较这对里两段的长短即可。
      //   超再生接收机的 AGC 在整帧内持续调整，帧头和帧尾的脉宽会不同，
      //   任何「全帧平均」门限都跟不上局部漂移；而每对自身相比则完全免疫。
      //   上面算出的 sMean/lMean/split 只用作「这帧像不像真信号」的合理性校验。
      bool aShort;

      if (ib < avail) {
        // 正常位：两段都在
        uint16_t b = e[ib];
        if (b < OOK_BIT_MIN || b > OOK_BIT_MAX) return _fail(8);
        uint16_t tot = (uint16_t)(a + b);
        // ★ 这里原来还有一道「两段比值必须 ≈ 1:3」的闸门（lo*3 > hi*2 则拒）。
        //   实测它把好帧大量误拒：超再生接收机在帧内某几个位的边沿会被干扰
        //   偏移 ~250us，实测 a=829 b=636（比值 0.77）。但这两个值的大小
        //   顺序依然正确，比特判决（短 = 1T）完全不受影响。
        //   真正的真假裁决者是 CRC-8，前面还有前导码+同步头两道闸门挡着噪声，
        //   再加一道模拟域比值闸门只会降低灵敏度。故删除。
        // ★ 每对自身相比：短的那段就是 1T，不依赖任何绝对阈值。
        //   不要在这里再加「与 split 交叉验证」——那会让判决重新依赖全帧平均，
        //   而漂移正是让 (b < split) 判错的原因，交叉验证会把好帧大批误拒。
        if (a == b) return _fail(10);                  // 两段一样长 → 不是合法位
        aShort = (a < b);
        sum += tot;
        nsum++;
      } else {
        // ★ 末位：低电平段被帧间隔吞掉了，只能凭高电平段判定。
        //   没有第二段可直接相比，退回双簇中点门限。
        aShort = (a < split);
        //   协议：bit0 = 1T 高 + 3T 低；bit1 = 3T 高 + 1T 低
        //   → 高电平段短就是 bit 0。不参与 T 估计。
      }

      uint8_t mask = (uint8_t)(0x80u >> (j & 7));
      // 协议：bit 0 = 1T 高 + 3T 低；bit 1 = 3T 高 + 1T 低。
      // 非反相时段 a = 高电平段 → a 短就是 bit 0；反相时正好相反。
      if (!aShort) pNorm[j >> 3] |= mask;
      else         pInv [j >> 3] |= mask;
    }

    const uint8_t *payload = nullptr;
    // ★★ 单比特纠错：接收机 AGC 会把个别位的边沿拉偏 ~250us，导致某一位
    //    长短判反 —— 段长本身仍落在合法窗口内，所以闸门放它过去，
    //    最后死在 CRC。实测这正是本链路的主要丢帧原因
    //    （ook + crc 恰等于从机理论帧数，说明前导/同步从不失败）。
    //    80 位里只要错 1 位，整帧就废；把这一位翻回来即可救回。
    uint8_t fixedBuf[OOK_PAYLOAD_LEN];
    if (ookCrc8(pNorm, OOK_PAYLOAD_LEN - 1) == pNorm[OOK_PAYLOAD_LEN - 1]) {
      payload = pNorm;
    } else if (ookCrc8(pInv, OOK_PAYLOAD_LEN - 1) == pInv[OOK_PAYLOAD_LEN - 1]) {
      payload = pInv;
      _invFrames++;                 // 记录：本机接收模块 DATA 是反相的
    } else {
      memcpy(fixedBuf, pNorm, OOK_PAYLOAD_LEN);
      if (_tryFixBits(fixedBuf)) {
        payload = fixedBuf;
      } else {
        memcpy(fixedBuf, pInv, OOK_PAYLOAD_LEN);
        if (_tryFixBits(fixedBuf)) { payload = fixedBuf; _invFrames++; }
      }
      if (payload == nullptr) {
        _crcErrors++;
        return _fail(11);
      }
    }

    memcpy(out.id, payload, 7);
    out.flags   = payload[7];
    out.seq     = payload[8];
    out.ms      = millis();
    out.unit_us = (uint16_t)(sum / (nsum > 0 ? nsum * 4 : 1));
    // ★ 记住这个身份，供后续帧做单比特纠错的身份校验
    _rememberId(out.id);
    return true;
  }

  // ── 记住一个已验证过的从机 ID（环形表）──
  void _rememberId(const uint8_t *id) {
    for (int i = 0; i < _idMemN; i++) {
      if (ookIdEqual(_idMem[i], id)) return;   // 已在表中，不重复占槽
    }
    memcpy(_idMem[_idMemNext], id, 7);
    _idMemNext = (uint8_t)((_idMemNext + 1) % OOK_ID_MEM);
    if (_idMemN < OOK_ID_MEM) _idMemN++;
  }

  bool _knownId(const uint8_t *id) const {
    for (int i = 0; i < _idMemN; i++) {
      if (ookIdEqual(_idMem[i], id)) return true;
    }
    return false;
  }

  // ── 单比特纠错（必须双重条件，绝不可只看 CRC）──
  //   条件 1：把某一位翻回来，CRC-8 恰好成立（随机概率约 80/256 ≈ 31%）
  //   条件 2：纠错后的 7 字节 ID 必须命中「已知从机 ID 表」
  //   两条同时成立，误接受概率 ≈ 0.31 × 2^-56，可视为零。
  //   ★ 表为空时（开机后还没有任何一帧成功）一律不纠错 —— 宁可不救，
  //     也绝不凭空造出一台不存在的从机。
  //   ★ 必须扫描全部 80 个候选并做歧义检测：多个候选同时成立时放弃，
  //     宁可不救，也不能随机挑一个「CRC 恰好通过但数据错误」的结果。
  //
  //   ★★ 为什么到此为止，不做双比特纠错（实测证伪，勿再加）：
  //     CRC-8 的 syndrome 只有 8 位 = 256 个可能值，而双比特候选有
  //     C(80,2)=3160 种（即使限定到 FLAGS/SEQ/CRC 这 24 位也有 C(24,2)=276 种
  //     > 256）。数学上必然大量碰撞：给定一个 syndrome，平均有 276/256 ≈ 1.08
  //     个候选，能唯一定位的只占约 1/3，其余全部歧义。
  //     实测实现过一版并跑满 3 分钟：two=0 —— 一次都没成功救回，
  //     因为剩余失败帧的错误位数都 ≥3（位错误率 ~1.4%，80 位中
  //     2 位以上错误的概率约 31%，而多位错误的组合早已超出 CRC-8 的
  //     可纠正能力）。故删除该分支，避免留一段永不生效的死代码。
  //     要再提升，只能走物理层（天线/供电/接收模块）或降低误码率，
  //     而不是在 8 位 CRC 上堆解码技巧。
  bool _tryFixBits(uint8_t *p) {
    if (_idMemN == 0) return false;
    int found = -1;
    for (int k = 0; k < OOK_PAYLOAD_BITS; k++) {
      p[k >> 3] ^= (uint8_t)(0x80u >> (k & 7));
      bool ok = (ookCrc8(p, OOK_PAYLOAD_LEN - 1) == p[OOK_PAYLOAD_LEN - 1])
                && _knownId(p);
      p[k >> 3] ^= (uint8_t)(0x80u >> (k & 7));      // 还原
      if (ok) {
        if (found >= 0) return false;                // 多个候选 → 歧义，放弃
        found = k;
      }
    }
    if (found < 0) return false;
    p[found >> 3] ^= (uint8_t)(0x80u >> (found & 7));
    _bitFixed++;
    if (_fixBitHist[found] < 0xFFFFu) _fixBitHist[found]++;
    return true;
  }

  void push(const OokFrame &f) {
    _queue[_qHead] = f;
    _qHead = (_qHead + 1) % OOK_QUEUE_LEN;
    if (_qHead == _qTail) _qTail = (_qTail + 1) % OOK_QUEUE_LEN;  // 满 → 丢最旧
    _okFrames++;
    _lastUnitUs = f.unit_us;         // ★ 记下本帧实测 T，供健康日志判读
  }

  // ── 成员 ──
  int               _pin;
  uint16_t          _dur[OOK_EDGE_CAP];
  volatile int      _head;
  volatile bool     _consumed;
  portMUX_TYPE      _mux;

  OokFrame _queue[OOK_QUEUE_LEN];
  volatile int _qHead, _qTail;
  uint16_t _lastUnitUs;            // ★ 最近一帧实测 T（?s），仅诊断用

  uint32_t _okFrames, _crcErrors, _noiseBursts, _overflow, _invFrames;
  uint16_t _failHist[OOK_FAIL_KINDS];   // ★ decode() 失败原因直方图

  // ── 单比特纠错状态 ──
  //   ★ 不能用「上一帧的 ID」当基准：多防区是交错收的，
  //     A 区帧解出后紧接着来 B 区帧，基准被换成 A，B 就永远救不回来。
  //     改成「最近见过的 ID 表」（环形 8 槽，覆盖 4 个防区绰绰有余），
  //     纠错结果只要命中表里任意一个 ID 就算合法。
  uint8_t  _idMem[OOK_ID_MEM][7];       // 最近见过的从机 ID
  uint8_t  _idMemN;                     // 已填条目数
  uint8_t  _idMemNext;                  // 环形写入位置
  uint32_t _bitFixed;                   // 被成功救回的帧数
  uint16_t _fixBitHist[OOK_PAYLOAD_BITS]; // 各比特位出错次数（定位系统性偏差）
  volatile uint32_t _lastEdgeUs = 0;   // 最近一个 RMT 边沿的时刻
  volatile bool     _inFrame = false;  // RMT 传输进行中（真帧内）→ inFrame()
  volatile uint32_t _lastOkMs;     // 最近一次成功解帧的 millis()（pushSafe 用）

  // ── RMT 相关 ──
  bool          _rmtInstalled;
  RingbufHandle_t _rb;

};

#endif // OOK_RECEIVER_H
