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
//   取 10s：容忍连丢 3 次心跳，又不会让掉电状态拖太久。
#define ZONE_OFFLINE_MS 10000UL

// updateFromRadioEx() 返回值
#define OOK_RX_NONE    0   // 没匹配上任何防区，也没配对上
#define OOK_RX_KNOWN   1   // 更新了一个已登记防区
#define OOK_RX_PAIRED  2   // ★ 新从机首次写入（配对成功）

struct ZoneInfo {
  uint8_t mac[7];
  char name[12];
  bool enabled;
  bool vibration;
  bool occupied;
  bool present;
  uint8_t seq;
  uint32_t lastSeenMs;
};

class ZoneManager {
public:
  ZoneManager()
    : _count(0), _activeIdx(0), _pairing(false), _silent(false), _pairTarget(-1) {
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
      // ★ occupied / present 是【运行期状态】，不入 EEPM：
      //   上电一律从 false 开始，由从机心跳重新建立。
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
      _zones[i].seq = 0;
      _zones[i].lastSeenMs = 0;
    }
    _count = 0;
  }

  int count() const { return _count; }
  int activeIndex() const { return _activeIdx; }
  bool isPairing() const { return _pairing; }
  bool isSilent() const { return _silent; }

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
      _zones[idx].lastSeenMs = millis();
      _zones[idx].seq = frame.seq;
      if (idx >= _count) _count = idx + 1;
      _activeIdx = idx;
      save();
      return OOK_RX_PAIRED;
    }

    for (int i = 0; i < _count; i++) {
      if (matchMac(_zones[i].mac, frame.id)) {
        _zones[i].present = (frame.flags & OOK_FLAG_PRESENCE) != 0;
        _zones[i].occupied = _zones[i].present;
        _zones[i].lastSeenMs = millis();
        _zones[i].seq = frame.seq;
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
        _zones[idx].lastSeenMs = millis();
        _zones[idx].seq = frame.seq;
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

  // ── 离线看门狗：主循环每轮调用一次 ──
  //   超过 ZONE_OFFLINE_MS 没收到该防区任何一帧 → 判定从机已离线，
  //   清掉 present/occupied，报警闪烁随之自行熄灭。
  //   返回本次新判定为离线的防区数（供日志用，0 = 无变化）。
  //
  //   ★ 用 (uint32_t)(now - lastSeenMs) 做差：millis() 回绕也正确。
  //   ★ 开机时 lastSeenMs = 0，且 occupied/present 一律从 false 开始
  //     （它们是运行期状态，不写入 EEPM，见 save()）。
  //     所以绝不会出现「上电就狂闪」的假报警。
  int tick() {
    uint32_t now = millis();
    int dropped = 0;
    for (int i = 0; i < _count; i++) {
      if (!_zones[i].enabled) continue;
      if (!_zones[i].present && !_zones[i].occupied) continue;
      if ((uint32_t)(now - _zones[i].lastSeenMs) >= ZONE_OFFLINE_MS) {
        _zones[i].present = false;
        _zones[i].occupied = false;
        dropped++;
      }
    }
    return dropped;
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
  ZoneInfo _zones[ZONE_MAX];
  int _count;
  int _activeIdx;
  bool _pairing;
  bool _silent;
  int _pairTarget;      // -1 = 自动选槽；>=0 = 配对写入指定防区
  Preferences _prefs;
};

#endif
