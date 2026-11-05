/*
 * HeartbeatSensor -- N4 master firmware (ESP32-C3-MINI-1-N4)
 * Zone state machine for the 4 defence zones.
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ZONE_MANAGER_H
#define ZONE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>
#include "OokReceiver.h"

#define ZONE_MAX 4

// ── 防区离线看门狗 ──
//   从机每 APP_HEARTBEAT_MS = 3s 心跳一次。若某防区超过此时长没收到
//   任何帧，说明从机掉电 / 断联 / 超出射频范围 → 必须把 present/occupied
//   清掉。
//   ★ 没有这个看门狗时：从机直接关机后，主机侧 occupied 永远是 true，
//     报警闪烁会一直持续到主机重启（现场已复现）。
//     原因：lastSeenMs 只写不读，没有任何地方判定「多久没见了」。
//
//   ★★ 取 10s 是【错误的】，已实测证伪，别再改回去：
//      10s 只等于 3.3 个心跳。而 433MHz 单向链路的实测收帧率只有
//      ~42%（[HEALTH] ook=17 / 约 40 帧/分），连丢 3 帧的概率
//      0.58? ≈ 19% —— 也就是【每 15~20 秒就会假判离线一次】。
//      假离线的后果正是现场症状：
//        occupied=false → 2s 后解锁震动锁存 → 下一帧到达又变有人
//        → 重新报警 + 再震一次
//      表现就是「报警复位然后一会再报警」「每隔一段震动一次」。
//      很容易被误读成「从机在重启」，实际从机一次都没重启（rst=0）。
//
//   ★ 取 30s（= 10 个心跳）：连丢 10 帧的概率 0.58?? ≈ 0.04%，可忽略；
//     而从机真的掉电时，最多多等 30s 才熄灭报警，现场完全可以接受。
//     真机断联本来就该是「慢一点熄灭」而不是「频繁误报」。
//
//   ★★ 现场仍报「偶然无人却判离线」→ 30s 还不够，抬到 45s（= 15 个心跳）。
//      为什么 30s 仍会误判：上面 0.04% 的算法假设「各帧丢失独立」，
//      但 433MHz 单向链路的丢帧是【成簇】的 —— 推屏致盲、AGC 失锁、
//      同频干扰都会一次吃掉连续 2~3 帧，独立性假设不成立。
//      实测 f=10~20 帧/分（即收帧率 25%~50%）时，成簇丢失会让
//      「连续 30s 无帧」的概率从理论 0.04% 涨到百分级，
//      按 15~20s 一次心跳窗口算，就是「隔一会儿闪一次离线」。
//      45s 的好处：要连丢 15 个心跳才误判，成簇丢失也几乎打不穿；
//      代价：从机真掉电时晚 15s 才熄灯，现场无感。
#define ZONE_OFFLINE_MS 45000UL

// ── 离线后的「保留期」：判离线 ≠ 立刻清空有人状态 ──
//   ★ 这是修「偶然无人却判离线」的第二半，比抬阈值更关键。
//     旧逻辑一旦 online 变 false 就【立刻】把 present/occupied 清 0，
//     于是链路偶发丢帧 → 屏上「有人」瞬间变「无人」+ 报警解锁，
//     然后下一帧到达又变回有人 → 又报警又震动。
//     现场看到的就是「报警复位然后一会再报警」。
//   ★ 现在：判离线后先保留最后已知的 present/occupied 这么久，
//     真掉电才在这之后清空。等于给链路抖动一个缓冲垫。
//     45s + 15s = 60s 才真正清空，而从机掉电时 60s 熄灭现场可接受。
#define ZONE_PRES_GRACE_MS 15000UL

// ── 报警复位去抖（每防区独立）──
// ── 报警复位判据（每防区独立）：连续 N 帧明确「无人」──
//   ★★ 判据从「墙钟计时」改成「帧计数」，这是修「从机报警消得太慢」的核心。
//      旧实现：occupied 一旦变 false 就开始计时，满 ZONE_CLEAR_HOLD_MS
//      （曾 6s，现场误复位后又抬到 12s）才解锁存。问题有两层：
//        ① 12s 是硬等待 —— 从机 50ms 去抖确认无人、第一个「无人」帧
//           ~150ms 就到达主机了，可屏幕还要再等 12s 才熄灭，
//           现场感受就是「P3.3 都拉低好几秒了主机还在报警」。
//        ② 12s 这个值本身是「用时间换抗丢帧」的妥协，可它抗的是【丢帧】，
//           而真正会让报警误复位的是【假无人帧】（盲区边缘/采样抖动发出的
//           flags=00），丢帧只会让计时变慢、并不会让假帧变多。
//           用时间窗去挡假帧，方向本来就错了。
//   ★ 帧计数为什么又准又快：
//        · 从机状态变化时连发 APP_STATE_REPEAT=3 帧，帧间隔 20~59ms，
//          正常离开 → 3 个「无人」帧在 ~450ms 内到齐 → 立刻解锁存。
//        · 单个假无人帧只能让计数 +1，下一帧心跳带着「有人」就清零，
//          报警不会被打断 —— 这正是旧 2s 窗口做不到的。
//        · 要求【连续】帧，等价于「从机自己连续多次都说没人」，
//          假帧要凑满连续 N 次，概率远低于一次成簇丢帧。
//   ★★ 为什么取 2 而不是 3：
//       从机一次状态变化只连发 3 帧，而实测 crc 丢帧约 3~5%，
//       要求 3 帧全中 → 一旦丢 1 帧就得等下一次心跳（2000ms），
//       现场又变成「要等 2 秒」。取 2 允许在同一个突发内丢 1 帧，
//       正常离开仍在 ~450ms 内解锁，这是「快」和「稳」的平衡点。
//       安全性不变：单个假无人帧只能 +1，凑不出 2 个连续，
//       下一帧带 PRESENCE 立刻清零。
//   ★ 掉电兜底：从机真死掉后不再有帧，计数会冻住 → 靠下面 tick() 里
//      「离线缓冲垫用尽」那一支把锁存直接解掉，保证最终一定会解。
//   ★ 计数只统计【已登记防区收到的有效帧】，BOOT 盲区假帧被挡在上层，
//     不会污染计数。
#ifndef ZONE_CLEAR_FRAMES
#define ZONE_CLEAR_FRAMES 2u
#endif

// ★ 已废弃：时间判据不再参与决策，保留宏名只为兼容旧引用。
//   0 表示「不启用时间判据」。不要再把它调回 6000/12000 —— 那正是
//   「报警消得慢」的病根，抗丢帧的职责现在由 ZONE_CLEAR_FRAMES 承担。
#ifndef ZONE_CLEAR_HOLD_MS
#define ZONE_CLEAR_HOLD_MS 0UL
#endif

// updateFromRadioEx() 返回值
#define OOK_RX_NONE    0   // 没匹配上任何防区，也没配对上
#define OOK_RX_KNOWN   1   // 更新了一个已登记防区
#define OOK_RX_PAIRED  2   // ★ 新从机首次写入（配对成功）

struct ZoneInfo {
  uint8_t mac[7];
  char name[12];
  bool enabled;
  bool vibration;
  bool occupied;        // 单帧语义：这一帧报告「有人」
  bool present;         // 与 occupied 同义（保留以兼容旧代码）
  bool online;          // ★ 运行期：最近还收得到该从机的帧（与有无人无关）
  bool alarmLatched;    // ★ 运行期：本防区独立报警锁存
  bool justOffline;     // ★ 本轮刚掉线（online 1→0 的下降沿），仅用于日志计数
  uint8_t seq;
  uint8_t lastFlags;    // 最近一帧原始 FLAGS（诊断用）
  uint16_t frameCount;  // ★ 累计收到本防区的有效帧数（链路质量）
  uint16_t frameWindow; // ★ 最近一个 60s 窗口内的帧数
  uint16_t frameWindowBase; // 上一次 rollFrameWindow 时的 frameCount
  uint32_t lastSeenMs;
  uint32_t clearSinceMs; // 本防区「开始无人」的时刻（0 = 未计时）— 仅日志用
  uint8_t clearFrames;   // ★ 连续收到「明确无人」帧的计数，达到 ZONE_CLEAR_FRAMES 即解锁存
  uint32_t offlineSinceMs; // ★ 本防区「刚开始判离线」的时刻（0 = 在线）
};

class ZoneManager {
public:
  ZoneManager()
    : _count(0), _activeIdx(0), _pairing(false), _silent(false), _pairTarget(-1),
      _seqDrops(0), _alarmEvents(0), _lastRebootMs(0) {
    clearAll();
  }

  void begin() {
    _prefs.begin("zones", false);
    memset(_zones, 0, sizeof(_zones));
    _count = _prefs.getUInt("count", 0);
    for (int i = 0; i < _count && i < ZONE_MAX; i++) {
      char key[16];
      snprintf(key, sizeof(key), "m%d", i);
      _prefs.getBytes(key, _zones[i].mac, 7);
      snprintf(key, sizeof(key), "n%d", i);
      _prefs.getString(key, _zones[i].name, sizeof(_zones[i].name));
      snprintf(key, sizeof(key), "e%d", i);
      _zones[i].enabled = _prefs.getBool(key, true);
      snprintf(key, sizeof(key), "v%d", i);
      _zones[i].vibration = _prefs.getBool(key, true);
      // ★ occupied / present / online / alarmLatched 都是【运行期状态】，不入 EEPM：
      //   上电一律从 false 开始，由从机心跳重新建立。
      _zones[i].occupied = false;
      _zones[i].present = false;
      _zones[i].online = false;
      _zones[i].alarmLatched = false;
      _zones[i].justOffline = false;
      _zones[i].lastFlags = 0;
      _zones[i].lastSeenMs = 0;
      _zones[i].clearSinceMs = 0;
      _zones[i].clearFrames = 0;
      _zones[i].offlineSinceMs = 0;
      _zones[i].frameCount = 0;
      _zones[i].frameWindow = 0;
      _zones[i].frameWindowBase = 0;
      if (_zones[i].name[0] == '\0') snprintf(_zones[i].name, sizeof(_zones[i].name), "Z%d", i + 1);
    }
    _prefs.end();
  }

  void clearAll() {
    for (int i = 0; i < ZONE_MAX; i++) {
      memset(_zones[i].mac, 0, 7);
      snprintf(_zones[i].name, sizeof(_zones[i].name), "Z%d", i + 1);
      _zones[i].enabled = true;
      _zones[i].vibration = true;
      _zones[i].occupied = false;
      _zones[i].present = false;
      _zones[i].online = false;
      _zones[i].alarmLatched = false;
      _zones[i].justOffline = false;
      _zones[i].seq = 0;
      _zones[i].lastFlags = 0;
      _zones[i].frameCount = 0;
      _zones[i].frameWindow = 0;
      _zones[i].frameWindowBase = 0;
      _zones[i].lastSeenMs = 0;
      _zones[i].clearSinceMs = 0;
      _zones[i].clearFrames = 0;
      _zones[i].offlineSinceMs = 0;
    }
    _count = 0;
    _alarmEvents = 0;
  }

  int count() const { return _count; }
  int activeIndex() const { return _activeIdx; }
  bool isPairing() const { return _pairing; }
  bool isSilent() const { return _silent; }

  // ── 从机重启计数（诊断用）──
  //   ★ 为什么要这个：seq 每次开机都从 0 重新数（OOK_Init 里 s_seq = 0）。
  //     正常情况下 seq 递增，256 帧才回绕一次（心跳 3s/次 → 十几分钟）。
  //     所以「短时间内 seq 倒退」只可能是一个原因：从机重启了。
  //   ★ 为什么必须在主机侧做：从机的 P3.0/P3.1 被 ISP 下载占用，
  //     没有空闲串口把日志送出来；而重启在主机侧是一眼就能看出来的。
  //   ★ 重启会造成什么后果（现场已见）：
  //     每次重启重新进入 APP_DETECT_BLIND 上电盲区，这段时间内从机
  //     上报 flags=00 → 主机报警熄灭；盲区过完再恢复 01。
  //     表现就是「报警每隔几秒自己消一次又重报」，很容易被误判成
  //     「采样延时大」。看到这个计数在涨，就不要再去调采样/去抖参数了。
  uint16_t seqDrops() const { return _seqDrops; }

  // 距最近一次检测到「从机重启」的毫秒数。
  // ★ 用途：判断从机是不是在【反复】重启（而不是只重启过一次）。
  //   如果这个值一直很小（几十秒内），说明从机在重启循环里出不来 ——
  //   典型原因是 433 发射瞬间把 Vcc 拉塌（欠压复位），
  //   或者 STC-ISP 下载时勾了「允许看门狗复位」而固件从不喂狗。
  uint32_t rebootAgeMs() const {
    if (_seqDrops == 0) return 0xFFFFFFFFu;
    return (uint32_t)(millis() - _lastRebootMs);
  }

  void setPairing(bool v) { _pairing = v; }
  void setSilent(bool v) { _silent = v; }
  void setActiveIndex(int idx) { if (idx >= 0 && idx < ZONE_MAX) _activeIdx = idx; }

  void setZoneName(int idx, const char *name) {
    if (idx < 0 || idx >= ZONE_MAX) return;
    strncpy(_zones[idx].name, name ? name : "Z", sizeof(_zones[idx].name) - 1);
    _zones[idx].name[sizeof(_zones[idx].name) - 1] = '\0';
  }

  void setZoneVibration(int idx, bool on) {
    if (idx < 0 || idx >= ZONE_MAX) return;
    _zones[idx].vibration = on;
  }

  bool matchMac(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 7; i++) if (a[i] != b[i]) return false;
    return true;
  }

  // 槽位是否未写入 MAC：STC8 唯一 ID 不可能是全零，故全零 = 空槽。
  bool _isSlotEmpty(int i) const {
    if (i < 0 || i >= ZONE_MAX) return false;
    for (int j = 0; j < 7; j++) {
      if (_zones[i].mac[j] != 0) return false;
    }
    return true;
  }

  // 开始配对：清空所有已存防区，保证一定有可写入的槽位。
  // ★ 若不清空，旧记录会把 4 个槽位占满 → 配对永远「超时」却不知为何。
  void beginPairing() {
    clearStored();
    _pairing = true;
  }

  // 配对写入目标：
  //   idx >= 0  → 精确写入选定防区（UI 提示 "Active zone N start search" 时）
  //   idx <  0  → 自动：优先第一个空槽；4 槽全满则覆盖当前选定防区
  void setPairTarget(int idx) { _pairTarget = (idx >= 0 && idx < ZONE_MAX) ? idx : -1; }
  int  pairTarget() const { return _pairTarget; }

  bool updateFromRadio(const OokFrame &frame) {
    return updateFromRadioEx(frame) != OOK_RX_NONE;
  }

  // 返回值区分三态，供配对流程精确判断（不能只看 count 变化：
  // 覆盖已有槽位时 count 不变，但确实是一次成功配对）。
  //
  // ★★ 配对必须由【从机主动请求】发起，不能「主机进配对模式就抓第一帧」。
  //   否则：从机没按配对键，只是照常心跳 → 主机一进配对模式就把它抓走；
  //   现场有多台从机时，谁先发谁就被配走，完全失控。
  //   从机长按 P3.2 后会连发 OOK_FLAG_PAIRREQ，这里必须校验该位。
  int updateFromRadioEx(const OokFrame &frame) {
    const bool pairReq = (frame.flags & OOK_FLAG_PAIRREQ) != 0;

    // ── 配对写入：目标防区已显式选定，且本帧确实是配对请求 ──
    if (_pairing && _pairTarget >= 0 && pairReq) {
      int idx = _pairTarget;
      // 该设备若已登记在别的槽位，先把它从旧槽位移走，
      // 否则一台设备会同时占两个防区（旧槽位一直亮「有人」）。
      for (int i = 0; i < _count; i++) {
        if (i != idx && matchMac(_zones[i].mac, frame.id)) {
          memset(_zones[i].mac, 0, 7);
          _zones[i].enabled = false;
          _zones[i].present = false;
          _zones[i].occupied = false;
        }
      }
      // ★ 一个防区只能对应一个从机：覆盖写入前先清空该槽，
      //   否则残留的旧 MAC 会让 matchMac() 继续命中旧设备。
      memset(_zones[idx].mac, 0, 7);
      memcpy(_zones[idx].mac, frame.id, 7);
      snprintf(_zones[idx].name, sizeof(_zones[idx].name), "Z%d", idx + 1);
      _zones[idx].enabled = true;
      _zones[idx].vibration = true;
      _zones[idx].present = (frame.flags & OOK_FLAG_PRESENCE) != 0;
      _zones[idx].occupied = _zones[idx].present;
      _zones[idx].alarmLatched = false;
      _zones[idx].clearSinceMs = 0;
      _zones[idx].clearFrames = 0;
      _zones[idx].offlineSinceMs = 0;
      _touchZone(idx, frame);
      if (idx >= _count) _count = idx + 1;
      _activeIdx = idx;
      save();
      return OOK_RX_PAIRED;
    }

    for (int i = 0; i < _count; i++) {
      if (matchMac(_zones[i].mac, frame.id)) {
      // ── 从机重启探测 ──
      //   ★★ 以 OOK_FLAG_BOOT 为准（确定性判据），不要用 seq 倒退去猜。
      //     原因：seq 8 位、256 帧回绕，而从机 ~35~40 帧/分 → 回绕只要 6~7 分钟，
      //     「seq 变小」无法区分重启和回绕。曾经用 5s 窗口 → 丢帧严重时必然漏检
      //     （rst 恒为 0，掩盖了真实重启）；用 60s 窗口 → 又太靠近回绕周期。
      //     二者都实测不可靠。
      //
      //   ★ 看见 BOOT 位时还做一件事：如果这一帧同时声称「无人」，
      //     它很可能来自上电盲区（s_present 在 App_Init 里被清成 0）。
      //     绝不可以用这种帧去清掉报警——否则主机上「一直有人」的报警
      //     会被从机的每一次重启打掉，现场完全无法分辨是「重启」还是「采样慢」。
      //     注：新版从机已在盲区内完全不发帧，所以这个保护主要是兼容旧固件。
      static bool s_seenBoot[ZONE_MAX] = {false, false, false, false};
      const bool bootFlag = (frame.flags & OOK_FLAG_BOOT) != 0;

      if (bootFlag && !s_seenBoot[i]) {
        s_seenBoot[i] = true;
        if (_seqDrops < 0xFFFFu) _seqDrops++;
        _lastRebootMs = millis();
      } else if (!bootFlag) {
        // BOOT 位消失 = 这批开机帧发完了，下次再看到就是又重启了一次
        s_seenBoot[i] = false;
      }

      // 盲区假帧保护：BOOT 帧声称「无人」时不采信（保留上一状态）
      if (bootFlag && (frame.flags & OOK_FLAG_PRESENCE) == 0) {
        _touchZone(i, frame);              // 仍算「活着」，但不动 present
        return OOK_RX_KNOWN;               // ★ 也不动 clearFrames（假帧不算证据）
      }
        _zones[i].present = (frame.flags & OOK_FLAG_PRESENCE) != 0;
        _zones[i].occupied = _zones[i].present;
        // ★★★ 报警复位的【唯一证据来源】：每收到一帧已登记防区的有效帧，
        //   按本帧的 PRESENCE 位累计/清零 clearFrames。
        //   有人 → 清零（报警继续锁存）
        //   无人 → +1，攒满 ZONE_CLEAR_FRAMES 后由 tick() 解除锁存
        //   放在这里而不是 tick() 里的原因：只有这里知道「又来了一帧新证据」，
        //   tick() 是无帧驱动的轮询，它无法区分「没有新帧」和「有新帧但都是无人」。
        if (_zones[i].present) {
          _zones[i].clearFrames = 0;
        } else if (_zones[i].clearFrames < ZONE_CLEAR_FRAMES) {
          _zones[i].clearFrames++;         // 饱和计数，避免 uint8 溢出回绕
        }
        _touchZone(i, frame);
        return OOK_RX_KNOWN;
      }
    }

    // ── 无显式目标的「自动配对」：优先写第一个空槽，全满则覆盖当前选定防区 ──
    //   ★ 同样必须校验 PAIRREQ：否则从机只是正常心跳也会被配走。
    if (_pairing && pairReq) {
      int idx = -1;
      for (int i = 0; i < ZONE_MAX; i++) {
        if (_isSlotEmpty(i)) { idx = i; break; }
      }
      if (idx < 0) idx = _activeIdx;
      if (idx >= 0 && idx < ZONE_MAX) {
        for (int i = 0; i < _count; i++) {   // 从旧槽位移走（一机一区）
          if (i != idx && matchMac(_zones[i].mac, frame.id)) {
            memset(_zones[i].mac, 0, 7);
            _zones[i].enabled = false;
            _zones[i].present = false;
            _zones[i].occupied = false;
          }
        }
        memset(_zones[idx].mac, 0, 7);       // 一区一机：先清空再写
        memcpy(_zones[idx].mac, frame.id, 7);
        snprintf(_zones[idx].name, sizeof(_zones[idx].name), "Z%d", idx + 1);
        _zones[idx].enabled = true;
        _zones[idx].vibration = true;
        _zones[idx].present = (frame.flags & OOK_FLAG_PRESENCE) != 0;
        _zones[idx].occupied = _zones[idx].present;
        _zones[idx].alarmLatched = false;
        _zones[idx].clearSinceMs = 0;
        _zones[idx].clearFrames = 0;
        _zones[idx].offlineSinceMs = 0;
        _touchZone(idx, frame);
        if (idx >= _count) _count = idx + 1;
        _activeIdx = idx;
        save();
        return OOK_RX_PAIRED;
      }
    }
    return OOK_RX_NONE;
  }

  bool anyPresent() const {
    for (int i = 0; i < _count; i++) {
      if (_zones[i].enabled && _zones[i].occupied) return true;
    }
    return false;
  }

  // ── 离线看门狗 + 每防区独立报警锁存：主循环每轮调用一次 ──
  //   ★★ 这是「各防区独立解耦」的核心：每个槽位只依据【自己】的
  //      lastSeenMs / occupied 做判断，任何一个防区（包括停用的旧探头）
  //      状态变化都绝不影响其它防区。
  //
  //   三条独立规则，全部按槽位 i 单独执行：
  //     (1) 在线判定：距本槽位上次收帧 >= ZONE_OFFLINE_MS → online=false。
  //         ★ 关键修正：旧实现只在 present||occupied 为真时才判离线，
  //           于是一个一直报「无人」的防区【永远不会】被判离线，
  //           online 状态形同不存在，界面无法区分「无人」和「掉线」。
  //     (2) 报警锁存：本槽位 online && occupied → 立即锁存；
  //         连续 ZONE_CLEAR_FRAMES 帧「无人」→ 只解本槽位的锁存。
  //         ★ 离线【不】直接解锁存：避免丢几帧就闪一次报警又重报。
  //     (3) 振动：只有「本槽位锁存从 0 → 1」这一上升沿才计一次事件，
  //         由主循环取走，防区之间互不抢占。
  //
  //   返回本次新判定为离线的防区数（仅用于日志，0 = 无变化）。
  int tick() {
    uint32_t now = millis();
    int dropped = 0;
    for (int i = 0; i < _count; i++) {
      if (!_zones[i].enabled) { _zones[i].online = false; _zones[i].justOffline = false; continue; }

      // ── (1) 在线判定（与有无人无关）──
      bool wasOnline = _zones[i].online;
      _zones[i].online = (uint32_t)(now - _zones[i].lastSeenMs) < ZONE_OFFLINE_MS;
      // ★ justOffline 只在「online 由 1 变 0 的那一轮」为真。
      //   为什么必须单独记：主循环原来在 zOffline>0 时把所有当前离线的
      //   防区全部打印一遍，于是只要有一台掉线，另一台【早就掉线】的也会
      //   被重复打印 —— 现场看到「每 30 秒又打印一次 #2 offline」，
      //   会误判成 #2 在反复掉线，实际它一直没恢复。
      //   有了这一位，日志里「#N offline」每次出现就对应一次真实的
      //   下降沿，可以拿来数掉线次数。
      _zones[i].justOffline = (wasOnline && !_zones[i].online);
      if (_zones[i].justOffline) dropped++;

      // ── (1b) 离线缓冲垫：判离线【不】立刻清有人状态 ──
      //   ★★ 这是修「偶然无人却判离线」的关键，比抬阈值更有效。
      //     旧代码在这里无条件 present=false / occupied=false，
      //     于是只要链路偶发成簇丢帧（推屏致盲、AGC 失锁、同频干扰
      //     都会一次吃掉 2~3 帧），屏上就会「有人」闪成「无人」，
      //     报警锁存被解除，下一帧又变回有人 → 再报警、再震一次。
      //     现场症状正是「偶然无人却判断离线」「报警复位一会又报警」。
      //   ★ 新逻辑：判离线后先原样保留最后已知的 present/occupied，
      //     整整 ZONE_PRES_GRACE_MS 之后才清空。链路抖动被完全吸收，
      //     真掉电也只在 45s+15s=60s 后才熄灭，现场无感。
      //   ★ offlineSinceMs 的维护：恢复在线时立刻归零（下次掉线重新计时）。
      if (_zones[i].online) {
        _zones[i].offlineSinceMs = 0;
      } else if (_zones[i].offlineSinceMs == 0) {
        _zones[i].offlineSinceMs = now;    // 下降沿：开始计时
      } else if ((uint32_t)(now - _zones[i].offlineSinceMs) >= ZONE_PRES_GRACE_MS) {
        // 缓冲垫用完了，才认人真的没了
        _zones[i].present  = false;
        _zones[i].occupied = false;
        // ★★ 掉电兜底：这里必须【同时】把报警锁存解掉。
        //   复位判据换成帧计数之后，从机真死掉就不会再有新帧进来，
        //   clearFrames 会永远冻在旧值上，锁存再也没人解 —— 屏幕会一直
        //   停在报警态（虽然它已被上面判成离线、画成灰框，但 tuya 上报
        //   和 _alarmEvents 会一直挂着）。旧的时间判据没有这个漏洞，
        //   因为时间照样流逝。所以「缓冲垫用尽 = 真掉电」这一刻要显式解锁。
        _zones[i].alarmLatched = false;
        _zones[i].clearFrames  = 0;
        _zones[i].clearSinceMs = 0;
      }

      // ── (2) 本防区独立的报警锁存 / 复位 ──
      //   ★★ 判据从「online && occupied」改成「只看 occupied」，这是缓冲垫
      //     能真正生效的必要条件。旧判据里带着 online，等于判离线的那一刻
      //     就把报警锁存也一并判成「无人」→ 缓冲垫保留的 occupied 白留了，
      //     锁存照样会解锁，抖动依然会闪一次报警。
      //   ★ 只看 occupied 为什么安全：occupied 只会在两种情况下变 false —
      //       ① 收到明确的「无人」帧（真信号，该解锁）
      //       ② 离线缓冲垫（45s + 15s）用尽后被清空（真掉电，该解锁）
      //     两者都是可信判据，不需要再叠一个 online 条件。
      //     停用防区已在循环开头 continue 掉，不会走进来。
      //
      //   ★★★ 复位判据 = 连续 ZONE_CLEAR_FRAMES 帧明确「无人」（帧计数），
      //        不再是墙钟计时。clearFrames 由 updateFromRadioEx() 在每收到
      //        一帧已登记防区的有效帧时维护（有人→清零，无人→+1）。
      //        这里只负责读计数做决定，所以 tick() 调多快都不会影响复位速度
      //        —— 旧实现里 tick() 的调用频率直接决定了解锁时刻，是个隐藏耦合。
      bool occ = _zones[i].occupied;
      if (occ) {
        _zones[i].clearSinceMs = 0;
        _zones[i].clearFrames = 0;
        if (!_zones[i].alarmLatched) {
          _zones[i].alarmLatched = true;
          if (_alarmEvents < 0xFFFFu) _alarmEvents++;
        }
      } else if (_zones[i].alarmLatched) {
        if (_zones[i].clearSinceMs == 0) _zones[i].clearSinceMs = now;  // 仅日志用
        // ★ 判离线期间不动计数：offline 时根本没有新帧进来，计数自然冻结。
        //   这里显式判一下只是为了「链路恢复后重新累计」，避免把掉线前
        //   攒的旧计数当成新证据。
        if (!_zones[i].online) {
          _zones[i].clearFrames = 0;      // 掉线期间证据作废，等重新收帧
        } else if (_zones[i].clearFrames >= ZONE_CLEAR_FRAMES) {
          _zones[i].alarmLatched = false;
          _zones[i].clearFrames  = 0;
          _zones[i].clearSinceMs = 0;
        }
      } else {
        _zones[i].clearSinceMs = 0;
        _zones[i].clearFrames  = 0;
      }
    }
    return dropped;
  }

  // ── 每防区运行期状态查询（界面 / 主循环用，全部按槽位独立）──
  bool zoneOnline(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    return _zones[idx].enabled && _zones[idx].online;
  }

  bool zoneAlarmLatched(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    return _zones[idx].enabled && _zones[idx].alarmLatched;
  }

  // 任一防区处于报警锁存（只用于驱动振动器，不影响任何防区自身状态）
  bool anyLatched() const {
    for (int i = 0; i < _count; i++) {
      if (_zones[i].enabled && _zones[i].alarmLatched) return true;
    }
    return false;
  }

  // 自开机以来累计的「新报警事件」次数（每个防区锁存 0→1 各计一次）
  uint16_t alarmEvents() const { return _alarmEvents; }

  // ── 每防区收帧计数（链路质量，可测量）──
  //   ★ 为什么必须按防区单独统计：
  //     「多防区可靠性」不能靠感觉。原来只有全局 okFrames，
  //     A 区收得很好、B 区几乎收不到时，全局数字照样漂亮，
  //     现场只会看到「B 老是掉线」，却拿不出证据。
  //     现在每个槽位一个累计计数 + 一个 60 秒滑动窗口计数，
  //     在 [ZONE] 健康行里逐个打印，哪个防区的射频链路真的差一目了然。
  uint16_t zoneFrames(int idx) const {
    if (idx < 0 || idx >= _count) return 0;
    return _zones[idx].frameCount;
  }

  // 最近 60 秒内该防区收到的帧数（窗口由主循环每 60s 调 rollWindow 推进）
  uint16_t zoneFramesWindow(int idx) const {
    if (idx < 0 || idx >= _count) return 0;
    return _zones[idx].frameWindow;
  }

  // 主循环每 60 秒调用一次：把累计计数转入滑动窗口
  void rollFrameWindow() {
    for (int i = 0; i < _count; i++) {
      _zones[i].frameWindow = (uint16_t)(_zones[i].frameCount - _zones[i].frameWindowBase);
      _zones[i].frameWindowBase = _zones[i].frameCount;
    }
  }

  bool zoneOffline(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    if (!_zones[idx].enabled) return false;
    return !_zones[idx].online;
  }

  // ── 本防区是否在【本次 tick】刚刚掉线（online 1→0 的下降沿）──
  //   ★ 只用于日志。判离线请用 zoneOffline()（持续为真），
  //     判「刚掉」才用这个 —— 两者混用会造成同一台从机被反复打印。
  bool zoneJustOffline(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    if (!_zones[idx].enabled) return false;
    return _zones[idx].justOffline;
  }

  // 距该防区上一次收到帧的毫秒数（诊断用）
  uint32_t zoneAgeMs(int idx) const {
    if (idx < 0 || idx >= _count) return 0;
    return (uint32_t)(millis() - _zones[idx].lastSeenMs);
  }

  bool zoneOccupied(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    return _zones[idx].occupied;
  }

  bool zoneEnabled(int idx) const {
    if (idx < 0 || idx >= _count) return false;
    return _zones[idx].enabled;
  }

  const char *zoneName(int idx) const {
    if (idx < 0 || idx >= _count) return "--";
    return _zones[idx].name;
  }

  // ── 已保存防区的 7 字节 ID（诊断用）──
  //   ★ 为什么需要：从机换芯片 / 换整块板子后，STC8 的出厂唯一 ID 会变，
  //     而 EEPROM 里存的还是旧 ID → matchMac() 全部不命中 →
  //     防区永不更新 → 判离线 → 报警反复复位。
  //     开机把这里打印出来，和从机实际发的 ID 一对照，立刻就能确诊。
  const uint8_t *zoneMac(int idx) const {
    static const uint8_t kNone[7] = {0, 0, 0, 0, 0, 0, 0};
    if (idx < 0 || idx >= _count) return kNone;
    return _zones[idx].mac;
  }

  void save() {
    _prefs.begin("zones", false);
    _prefs.putUInt("count", _count);
    for (int i = 0; i < _count && i < ZONE_MAX; i++) {
      char key[16];
      snprintf(key, sizeof(key), "m%d", i);
      _prefs.putBytes(key, _zones[i].mac, 7);
      snprintf(key, sizeof(key), "n%d", i);
      _prefs.putString(key, _zones[i].name);
      snprintf(key, sizeof(key), "e%d", i);
      _prefs.putBool(key, _zones[i].enabled);
      snprintf(key, sizeof(key), "v%d", i);
      _prefs.putBool(key, _zones[i].vibration);
      // ★ 只持久化「配置」，不持久化 occupied / present。
    }
    _prefs.end();
  }

  void clearStored() {
    _prefs.begin("zones", false);
    _prefs.clear();
    _prefs.end();
    clearAll();
  }

private:
  // ── 收到一帧的公共记账：刷新在线时间、原始标志、序号 ──
  //   ★ 所有「收到已登记设备帧」的路径都必须走这里，
  //     否则某条分支忘了刷 lastSeenMs 就会让该防区无故判离线。
  void _touchZone(int i, const OokFrame &frame) {
    _zones[i].lastSeenMs = millis();
    _zones[i].seq       = frame.seq;
    _zones[i].lastFlags = frame.flags;
    _zones[i].online    = true;      // ★ 链路恢复立刻算在线，不等 tick
    // ★ 恢复在线的一瞬间必须清掉「刚掉线」标记，否则该槽位的 justOffline
    //   会一直挂着上一次的值，日志里会重复打印一次并不存在的掉线。
    _zones[i].justOffline = false;
    // ★ 逐防区收帧计数：直接自增，【故意让它自然回绕】(uint16)。
    //   窗口值用 (frameCount - frameWindowBase) 的无符号减法算，
    //   回绕时差值依然正确。若改成饱和在 0xFFFE，窗口就会永久变成 0，
    //   反而把「这个防区链路极好」误报成「一帧都收不到」。
    _zones[i].frameCount++;
  }

  ZoneInfo _zones[ZONE_MAX];
  int _count;
  int _activeIdx;
  bool _pairing;
  bool _silent;
  int _pairTarget;      // -1 = 自动选槽；>=0 = 配对写入指定防区
  uint16_t _seqDrops;   // 从机重启次数（seq 倒退）
  uint16_t _alarmEvents; // 累计新报警事件次数（每防区独立计数）
  uint32_t _lastRebootMs;  // 最近一次检测到从机重启的 millis()
  Preferences _prefs;
};

#endif
