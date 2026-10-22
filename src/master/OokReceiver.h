/*
 * OokReceiver -- OOK/ASK 接收解码器（主机侧）
 *
 * 协议见 docs/PROTOCOL-315M.md，与 STC8 从机固件成对实现。
 *
 * ★ 载波频率无关
 *   本协议是纯粹的脉宽编码，解码只依赖 DATA 脚上的高低电平时序，
 *   与载波是 315MHz 还是 433.92MHz 完全无关 —— 只要发射/接收模块
 *   同频段即可。换频段不需要改任何一行代码，只需按波长剪天线
 *   （λ/4：315M≈23.8cm，433M≈17.3cm）。
 *
 * ★ 为什么不用 rc-switch 的接收端
 *   rc-switch 在 ISR 里跑 receiveProtocol() 逐位比对，一帧 163ms 全程占着
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
 * ★ 三级抗噪门槛（315M 接收头底噪很大，必须严格）
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
 */

#ifndef OOK_RECEIVER_H
#define OOK_RECEIVER_H

#include <Arduino.h>
#include "driver/rmt.h"

// ══════════════════════════════════════════════════════
//  协议常量 —— 必须与 docs/PROTOCOL-315M.md 及从机固件一致
// ══════════════════════════════════════════════════════
#define OOK_T_US             350      // 基本时间单位
#define OOK_PREAMBLE_PULSES  32       // 前导码 2T 脉冲个数（= 64 个 2T 时长段）
#define OOK_SYNC_LOW_T       16       // 同步头低电平 = 16T
#define OOK_PAYLOAD_BITS     80       // 10 字节 × 8
#define OOK_PAYLOAD_LEN      10

// 一帧的总时长段数 = 64(前导) + 2(同步) + 160(数据) = 226
#define OOK_FRAME_SEGMENTS   226

// ── 内部缓冲 ──
#define OOK_EDGE_CAP         400      // 时长缓冲（226 + 余量，可容两帧粘连）
#define OOK_QUEUE_LEN        8        // 已解码帧队列

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
//  ★ 容量校验：一帧 226 段 / 163ms，单次推屏最长 13ms → 期间最多攒 18 个边沿；
//    RX 通道最多只能占 2 块内存（ch2 + ch3）= 96 段硬件缓存，
//    环形缓冲 4096B = 1024 段，余量充足。
#define OOK_RMT_CHANNEL      2        // RX 必须用 ch2/ch3（ch0/ch1 是 TX-only）
#define OOK_RMT_CLK_DIV      80       // 80MHz/80 → 1 tick = 1us
#define OOK_RMT_MEM_BLOCKS   2        // RX 最多 2 块 = 96 段硬件缓存
#define OOK_RMT_RX_BUF       4096     // 环形缓冲字节数（1024 个段）

// ── 判定窗口（微秒）──
#define OOK_FRAME_GAP_US     12000    // 超过此间隔视为帧间隔 → 重置缓冲
#define OOK_SYNC_MIN         3500     // 同步低电平下限  (10T)
#define OOK_SYNC_MAX         9100     // 同步低电平上限  (26T)
#define OOK_BIT_MIN          180      // 单段有效下限（约 0.5T）
#define OOK_BIT_MAX          1600     // 单段有效上限（约 4.6T）
#define OOK_BIT_SPLIT        700      // 1T / 3T 分界（正好是 2T）
#define OOK_SUM_MIN          1000     // 一位 (段a+段b) 下限
#define OOK_SUM_MAX          1900     // 一位 (段a+段b) 上限
#define OOK_PRE_LO           500      // 前导脉冲 2T 判定下限
#define OOK_PRE_HI           900      // 前导脉冲 2T 判定上限
#define OOK_PRE_WINDOW       32       // 检查同步头前多少个时长段
#define OOK_PRE_MIN_MATCH    20       // 其中至少多少个必须落在 2T 窗口内
#define OOK_MAX_RESYNC       40       // 一轮里最多尝试多少个同步头候选
// ★ 毛刺抑制：短于这个时长的段视为干扰，直接丢弃（不记入 _dur）。
//   协议里最短的合法段是 1T = 350us，所以 150us 以下的跳变必然无信息量。
//   实测依据：从机 P5.5 直连 GPIO20 时，3.5 秒内出现 58639 个边沿，
//   而协议理论只有 ~4275 段 —— 多出来的 54000 个全是 3~6us 高频振荡，
//   把真实段序列完全冲散，导致 sync/ok 恒为 0。
//   滤掉后 clean[] 立刻出现 800~900us 规整段，证明从机编码本身是好的。
#define OOK_GLITCH_US        150

// ── 帧突发静默窗（微秒）──
//   从机每次上报是「连发 2~3 帧」，帧间空档只有 20~59ms。
//   解完一帧后给接收器盖一个静默章，主循环据此让路，
//   免得把连发中的第 2/3 帧前导码吃掉。
//
//   ★★ 这里曾经是 6ms —— 那是个严重的逻辑错误，已用实测数据证伪：
//     帧间隔是 20~59ms，6ms 一过 busy() 立刻变 false，
//     主循环（每 20ms 一轮）马上推屏，正好踩在下一帧的前导码上。
//     实测后果：每轮突发只解出 1 帧，另外 2 帧被 SPI 推送确定性吃掉。
//     测量数据：心跳周期 3366ms（=3000 设计周期 + 连发 2 帧 366ms），
//     但 11 分钟内只收到 209 帧（应为 ~440）→ 命中率 47.5%。
//     而这一帧命中与否直接决定端到端延迟：
//       命中第 1 帧 → 190ms；命中第 2 帧 → 757ms；
//       整轮全丢 → 只能等下一次心跳，3400ms（用户感知的「2~3 秒」）。
//
//   ★ 新的取值 6ms（恢复原值）：
//     ★★ 关键认知：这个窗【不需要】盖住整个帧间隙。
//     真正保证不丢帧的不是这个 6ms，而是上面的 OOK_FRAME_MAX_US 让路窗——
//     它一直盖到帧尾之后，帧间隙里才放开。放开后主循环推一次屏（2~3ms），
//     帧间隙还剩 17~56ms，足够在下一帧前导到来前推完。
//     ★ 曾误改为 30ms（想盖住 20~59ms 的帧间隙），后果是：
//     让路总时长从 240ms 暴涨到 3x270ms=810ms，每 3.4 秒冻结 810ms，
//     实测屏幕帧率从 ~40FPS 掉到 ~2FPS —— 用户直接反馈「屏幕变卡了」。
//     帧间隙本来就不该用 busy() 封死，那是把「不能推屏」和「不能收帧」混为一谈。
#define OOK_BURST_HOLD_US    6000     // 6ms：只保护解帧后的紧邻瞬间

// ── 「正在收真帧」判据 ──
//   ★ 不要用「有没有边沿」当让路条件：超再生接收模块空闲时也在狂吐
//     噪声边沿（实测 90s 累计 5 万+），让路条件永远成立 → 屏幕被冻在
//     1 FPS（实测 push=3 / skip=88）。
//   ★★ 也不要只看「攒了多少段」！实测【从机断电】时，噪声突发照样能把
//     段数顶到 159（[OOK-STAT] peak=159 且 ok=0），屏幕被连续让路几百 ms，
//     表现为「从机在不在都一样，屏幕偶尔卡一下」。
//   唯一可靠的判据是「前导码成型」：连续 OOK_BUSY_SEGS 段的宽度
//   全部落在 2T 窗口内（真帧前导是 64 段连续的 ~700us）。
//
//   ★★ 这个值直接决定端到端延迟，必须尽量小（实测结论）：
//     保护窗是在攒够 OOK_BUSY_SEGS 段之后才拉起的，
//     即帧开始后 OOK_BUSY_SEGS x 2T ≈ OOK_BUSY_SEGS x 700us 才生效。
//     而主循环每 UPDATE_INTERVAL(20ms) 推一次屏，推屏持续 1.5~3ms，
//     所以「帧头这 OOK_BUSY_SEGS x 700us 内正好撞上一次推屏」的概率
//     ≈ (推屏 3ms + 拉起延迟) / 20ms。
//       取 16 时：拉起延迟 11.2ms → 风险 (3+11.2)/20 = 71%
//       取  6 时：拉起延迟  4.2ms → 风险 (3+ 4.2)/20 = 36%
//     实测 16 时命中率 47.5%，与此模型吻合。
//   ★ 为什么不能更小（比如 2~3）：超再生模块空闲噪声段长是 2~3ms 量级，
//     连续 2~3 段落进 500~900us 窗口的概率不可忽略；一旦误判就白让路
//     240ms。取 6 段（需连续 6 段都落在 2T 窗口）已足够把噪声排除
//     （从机断电实测 noiseBursts 长时间为 0），同时把风险砍掉一半。
//   ★ 取值与注释必须一致：这里曾是「注释写 8 / 实际 16」，
//     两者差 2 倍，调参时极易被注释误导。
#define OOK_BUSY_SEGS        6

// ── 让路窗长度（微秒）──
//   一旦判定前导码成型，就把让路窗推到此刻之后的 OOK_FRAME_MAX_US。
//   一帧 226 段 ≈ 163ms（前导 44.8ms + 同步 6ms + 载荷 112ms）。
//
//   ★★ 这个值必须【精密】：它同时决定「能不能收全帧」和「屏幕卡不卡」。
//     太短 → 帧还没收完就放行推屏 → 帧被踩断；
//     太长 → 帧早收完了还不让推屏 → 白等，帧间隙里推不了屏 → 卡顿。
//
//     帧长 163ms，帧间隙 20~59ms，推一次屏 2~3ms。
//     取 170ms = 帧长 + 7ms 余量：
//       - 帧结束（163ms）后仍有 7ms 让路，容得下 ISR 抖动与 AGC 拖尾；
//       - 170ms 一到立刻放行，帧间隙还剩 20-(170-163)=13ms，
//         足够推一次屏（2~3ms），推完后距下一帧前导还有 10ms 以上 ——
//         所以【推屏永远落在帧间隙里，绝不会踩到下一帧前导】。
//    ★ 原值 240ms 是错的：它比帧长长 77ms，这 77ms 纯属白等，
//      帧间隙被吃掉一半，导致推屏与下一帧前导抢时间。
#define OOK_FRAME_MAX_US     170000

//   ★ 推屏安全判据：距上一个边沿 ≥ 10ms 才算「在两帧之间」。
//     依据（实测）：帧内最长的一段是同步头的 16T ≈ 5.6ms，
//     而帧间隙（上一帧末位 → 下一帧前导）实测 20~59ms。
//     10ms 稳稳高于帧内最长段、又远小于最小帧间隙，
//     于是「≥10ms 无边沿」等价于「当前一定在两帧之间，推屏不会致盲接收机」。
//     ★ 不要再调大到 35ms：突发内两帧之间的间隔约 30ms，
//       调大后等于只在突发与突发之间才推屏，画面照样顿（实测 maxGapMs≈200）。
#define OOK_GAP_SAFE_US      10000    // 距上个边沿 ≥10ms → 在帧间隙（帧内最长 5.6ms，帧间空隙 20~59ms）

// ── 重复帧窗口：解出一帧后的这么多 ms 内，后续帧视为可丢弃的重复帧 ──
//   从机连发 2~3 帧、内容相同、间隔 ~30ms；整串 ~500ms。
//   这里取 900ms 覆盖整串，让这期间推屏不受限（画面流畅）。
#define OOK_REPEAT_WIN_MS    900

// ── FLAGS 位 ──
#define OOK_FLAG_PRESENCE    0x01     // 检测到人（核心开关量）
#define OOK_FLAG_TAMPER      0x02     // 防拆 / 异常
#define OOK_FLAG_LOWBAT      0x04     // 电池低
#define OOK_FLAG_PAIRREQ     0x08     // 主动请求配对

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
      _qHead(0), _qTail(0),
      _okFrames(0), _crcErrors(0), _noiseBursts(0), _overflow(0), _invFrames(0),
      _quietUntil(0), _lastOkMs(0),
      _frameUntil(0), _preRunPrev(false),
      _rmtInstalled(false), _rb(nullptr) {
    for (int i = 0; i < OOK_EDGE_CAP; i++) _dur[i] = 0;
  }

  // ── 启动：pin = 315M 接收模块的 DATA 输出脚 ──
  bool begin(int pin) {
    if (_pin >= 0) end();
    _pin = pin;
    pinMode(_pin, INPUT);           // 接收模块自带驱动，不需要内部上拉
    _head = 0;
    _consumed = true;

    // ── RMT 接收通道：硬件记录每段电平持续时长（单位 1us）──
    //   ★ filter_ticks_thresh = 100 → 滤掉 <100us 的毛刺（等价旧 OOK_GLITCH_US）
    //   ★ idle_threshold = 12000 → 12ms 无跳变即认为一次传输结束
    //     这个值必须 <= OOK_FRAME_GAP_US，才能把帧间隔切成「一次传输」，
    //     使每帧独立解出（与旧 ISR 的 d > OOK_FRAME_GAP_US 判据一致）。
    rmt_config_t cfg = RMT_DEFAULT_CONFIG_RX((gpio_num_t)pin,
                                             (rmt_channel_t)OOK_RMT_CHANNEL);
    cfg.clk_div        = OOK_RMT_CLK_DIV;
    cfg.mem_block_num  = OOK_RMT_MEM_BLOCKS;
    cfg.rx_config.filter_en          = true;
    cfg.rx_config.filter_ticks_thresh = 100;
    cfg.rx_config.idle_threshold      = OOK_FRAME_GAP_US;

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
  //     decode()/preRun()/busy() 一行未改 —— 它们只依赖「一串时长」，
  //     与这些时长是 ISR 记的还是 RMT 记的无关。
  //   ★★ 切帧关键：硬件每个 item32 装两段（dur0 + dur1），
  //     一次传输的最后一项把 dur1 填 0 作结束标记。
  //     必须在【遇到结束标记的那一刻】就把这一帧解掉并清缓冲，
  //     绝不能先攒完整个环形缓冲再回头找帧尾 —— 因为一次 poll() 里
  //     很可能同时排到「上一帧的尾巴」+「下一帧的开头」，
  //     那样 _dur 会变成 225+N 段，need 永远对不上，整帧报废。
  void poll() {
    if (_pin < 0 || _rb == nullptr) return;

    // ── 1) 排空环形缓冲；见结束标记即切帧解码 ──
    //   ★ 必须 while 排空：xRingbufferReceive 每次只返回【一个】item
    //     （驱动用 NOSPLIT 环形缓冲，一次 send = 一个 4 字节 item），
    //     一帧 225 个 item、约 1380 item/s。每次只取一个 → 缓冲 1024 项
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

    // ── 2) 让路窗：真前导码正在成型 → 把让路窗推到帧尾 ──
    //   ★ 边沿触发（只 arm 一次）：不然持续满足判据的噪声会让让路窗
    //     被无限续期，又变成长时间冻屏。窗长本身有 OOK_FRAME_MAX_US 上限。
    //   ★ 推屏期间不再需要这个窗（硬件自己记边沿），它现在只服务于 busy()。
    if (_head >= OOK_BUSY_SEGS) {
      bool pre = preRun();
      if (pre && !_preRunPrev) _frameUntil = micros() + OOK_FRAME_MAX_US;
      _preRunPrev = pre;
    } else {
      _preRunPrev = false;
    }
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
      //   （一帧共 226 段：64 前导 + 2 同步 + 160 载荷，但只产生 225 个可捕获段）
      int need = i + 1 + OOK_PAYLOAD_BITS * 2 - 1;
      if (len < need) continue;

      OokFrame f;
      if (decode(&_dur[i + 1], len - (i + 1), f)) {
        _lastOkMs = millis();        // ★ 供主循环判断「这是不是同一突发的重复帧」
        push(f);
        // ★ 盖静默章：从机连发 2~3 帧，用于让主循环优先把整串收完再动屏幕。
        _quietUntil = micros() + OOK_BURST_HOLD_US;
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

  // ── 接收器是否「正在收真帧」──
  //   主循环据此决定能否进行长阻塞操作（如整屏 SPI 推送）。
  //   ★ 判据之一是「前导码已成型」（攒够 OOK_BUSY_SEGS 段），
  //     而不是「攒了任意边沿」—— 后者会被空闲噪声永久拉高。
  bool busy() const { return _inFrame; }

  // ── 是否处于一次 RMT 传输内部（真帧正在收）──
  bool inFrame() const { return _inFrame; }

  // ── 能否安全推屏（主循环每轮问一次，无副作用）──
  //   ★★ 判据来自实测，不是猜的。核心物理事实：
  //     SPI 连续传输会让 315MHz 接收机「瞎」住一段时间。若这段瞎区落在帧体
  //     中间，RMT 录到的就是一段 ~10ms 的平坦电平，解码时被当作一个超长段，
  //     整帧结构随即作废（[XFR] 实测 len=229 却只有 4 段 / len=223 SHORT）。
  //     所以判据必须是「当前绝不在一帧之内」，而不只是「缓冲有没有数据」。
  //
  //   两道放行条件：
  //     (1) 「已经解出一帧之后的重复帧」：从机每次上报连发 2~3 帧、内容完全相同。
  //         既然已经拿到一帧，后面这些重复帧即使被推屏打散也不影响功能。
  //         这是把「163ms 必冻」压成「每个突发只冻第一帧」的关键。
  //         ★ 窗口只有 OOK_REPEAT_WIN_MS(900ms)，且必须在下一个突发前失效，
  //           否则会长期压制接收（曾因 600ms 无条件放行导致命中率跌到 47%）。
  //     (2) 「帧与帧之间的真空期」：不在传输中、缓冲已清空、
  //         且距上一个边沿 ≥ OOK_GAP_SAFE_US。
  bool pushSafe() const {
    // (1) 已解出一帧 → 本突发剩下的重复帧可以随便牺牲（从机每突发发 2~3 帧相同内容）
    if (_lastOkMs != 0 && (millis() - _lastOkMs) < OOK_REPEAT_WIN_MS) return true;

    // (2) 真正空闲：必须「距上个边沿 ≥ OOK_GAP_SAFE_US」才允许推。
    //   ★ 为什么不能只看 _head==0 / !_inFrame：
    //     帧内被推屏打坏后，传输会在 12ms 空闲后结束 → _head 清零、
    //     _inFrame=false → 门控立刻放行 → 又推 → 又打坏。实测形成
    //     自持的死循环：noise 从 14 涨到 56，ok 只有 2~4 / 20s。
    //   ★ 10ms 这个值的依据：
    //     帧内最长的一段是同步头的 16T ≈ 5.6ms；
    //     帧间隙（上一帧末位到下一帧前导）实测 20~59ms。
    //     10ms 稳稳高于帧内最长段、又远小于最小帧间隙，于是判据等价于
    //     「当前一定在两帧之间」，而不是「当前在一帧里的某个长段上」。
    //     ★ 不要再调大到 35ms：那会跨过突发内 30ms 的帧间隔，
    //       等于只在突发与突发之间才推屏，画面照样顿。
    if (_inFrame || _head != 0) return false;
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

  int quietLeftMs() const {
    int32_t l = (int32_t)(_quietUntil - micros());
    return (l > 0) ? (int)(l / 1000) : 0;
  }

  // ── 最近一次成功解帧的时刻（millis）──
  //   主循环用它判断：当前正在收的这一帧，是不是「刚刚那一帧的重复发送」。
  //   从机每次上报都是连发 2~3 帧，内容完全相同；既然已经取到一帧，
  //   后面这些重复帧即使被整屏 SPI 推送打散也不影响功能。
  uint32_t lastOkMs() const { return _lastOkMs; }

  // 当前这次「断流之间」攒下的段数（主循环只读，用于显示让路判断）
  int runSegs() const { return _head; }

  // ── 解帧结果计数（诊断用）──
  uint32_t okFrames()    const { return _okFrames; }
  uint32_t crcErrors()   const { return _crcErrors; }
  uint32_t noiseBursts() const { return _noiseBursts; }
  uint32_t overflows()   const { return _overflow; }
  uint32_t invertedFrames() const { return _invFrames; }
  // 接收模块 DATA 极性：0=非反相，1=反相（由已成功解码的帧推断）
  bool rxInverted() const { return _invFrames > _okFrames - _invFrames; }

private:
  // decode() 里的每道闸门都用这个单行返回，便于逐条对照协议做闸门审计
  static bool _fail(int) { return false; }

  // ── 前导码成型判据 ──
  //   最近 OOK_BUSY_SEGS 段【全部】落在 2T 窗口 [OOK_PRE_LO, OOK_PRE_HI]。
  //   ★ 这是把「真帧前导」和「空闲噪声」分开的可靠手段：
  //     真帧前导 = 64 段连续的 ~700us；
  //     超再生模块空闲噪声的段长是 2~3ms 量级（从机断电实测 peak=159 即此类），
  //     连续 16 段都落进 500~900us 的概率极低。
  //   只读 _dur 的前 _head 项（非临界区）。纯启发式判据，即使读到并发写入
  //   的那一项也无害：最坏只是让路窗多开一次，而窗长有硬上限。
  bool preRun() const {
    int n = _head;
    if (n < OOK_BUSY_SEGS) return false;
    for (int i = n - OOK_BUSY_SEGS; i < n; i++) {
      uint16_t d = _dur[i];
      if (d < OOK_PRE_LO || d > OOK_PRE_HI) return false;
    }
    return true;
  }

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
    if (ookCrc8(pNorm, OOK_PAYLOAD_LEN - 1) == pNorm[OOK_PAYLOAD_LEN - 1]) {
      payload = pNorm;
    } else if (ookCrc8(pInv, OOK_PAYLOAD_LEN - 1) == pInv[OOK_PAYLOAD_LEN - 1]) {
      payload = pInv;
      _invFrames++;                 // 记录：本机接收模块 DATA 是反相的
    } else {
      _crcErrors++;
      return _fail(11);
    }

    memcpy(out.id, payload, 7);
    out.flags   = payload[7];
    out.seq     = payload[8];
    out.ms      = millis();
    out.unit_us = (uint16_t)(sum / (nsum > 0 ? nsum * 4 : 1));
    return true;
  }

  void push(const OokFrame &f) {
    _queue[_qHead] = f;
    _qHead = (_qHead + 1) % OOK_QUEUE_LEN;
    if (_qHead == _qTail) _qTail = (_qTail + 1) % OOK_QUEUE_LEN;  // 满 → 丢最旧
    _okFrames++;
  }

  // ── 成员 ──
  int               _pin;
  uint16_t          _dur[OOK_EDGE_CAP];
  volatile int      _head;
  volatile bool     _consumed;
  portMUX_TYPE      _mux;

  OokFrame _queue[OOK_QUEUE_LEN];
  volatile int _qHead, _qTail;

  uint32_t _okFrames, _crcErrors, _noiseBursts, _overflow, _invFrames;
  volatile uint32_t _quietUntil;   // 突发静默窗到期时刻（micros）
  volatile uint32_t _lastEdgeUs = 0;   // 最近一个 RMT 边沿的时刻
  volatile bool     _inFrame = false;  // RMT 传输进行中（真帧内）→ busy()
  volatile uint32_t _lastOkMs;     // 最近一次成功解帧的 millis()
  volatile uint32_t _frameUntil;   // 「真帧前导已成型」让路窗到期时刻（micros）
  bool              _preRunPrev;   // 上一次 preRun() 的结果（边沿触发用）

  // ── RMT 相关 ──
  bool          _rmtInstalled;
  RingbufHandle_t _rb;

};

#endif // OOK_RECEIVER_H
