/*
 * TuyaLink.h — 涂鸦智能家居接入（MQTT 标准协议 / TuyaLink）
 * ============================================================================
 * ★ 为什么不用 TuyaOS / TuyaOpen / MCU SDK：
 *    - TuyaOS/TuyaOpen 要换掉整个工程入口（main / tuya_app_main），
 *      会把本工程的 RMT 433MHz 解码、四防区状态机、ST7735 脏矩形 UI 全部作废；
 *    - MCU SDK 要走串口，而 C3 的 UART0 是 USB 调试口、UART1 已被雷达占用，
 *      没有第三个串口了。
 *   → 所以这里直接实现【TuyaLink = 涂鸦 MQTT 标准协议】，
 *     作为一个独立模块挂在现有 Arduino sketch 上，一行都不动射频与 UI 逻辑。
 *
 * ★ 协议事实（均已对照官方文档 / 官方 Java demo 核实）：
 *    - 只支持 MQTT 3.1.1；只支持 QoS 0 与 1
 *    - TLS 8883，根证书 Go Daddy Root CA - G2（已内联在下方）
 *    - clientId = "tuyalink_" + deviceId
 *    - username = "${did}|signMethod=hmacSha256,timestamp=${ts},secureMode=1,accessType=1"
 *    - password = hmacSha256("deviceId=${did},timestamp=${ts},secureMode=1,accessType=1",
 *                            deviceSecret)  → 64 位小写十六进制
 *    - 上行 tylink/${did}/thing/property/report
 *      下行 tylink/${did}/thing/property/set
 *      ★ 本固件【不 subscribe 下行】：所有 DP 都是「只上报 ro」，
 *        没有任何可下发点（原 wifi_enable 已删除，理由见 TuyaConfig.h 第 5 节）。
 *    - keepalive 60s，云端 1.5×90=150s 收不到包判离线
 *    - 一份设备证书只能有一个 MQTT 连接（多连会互踢）
 *
 * ★ 设计要点（为什么是「独立任务」而不是塞进 loop()）：
 *    TLS 握手是【秒级阻塞】操作。若放在 loop() 里，主循环会被卡 1~3 秒：
 *    看门狗会 panic、屏幕冻结、433MHz 收帧直接丢一大段。
 *    实测主循环预算只有 24~25ms，5s WDT 余量并不宽裕。
 *    所以 WiFi + NTP + MQTT 全部跑在自己那个 FreeRTOS 任务里，
 *    主循环只做两件极轻的事：
 *      1) 把防区状态压成 4bit 掩码交给任务（几个字节的写入）
 *      2) 调用 tuya.loop()（当前为空：本设备没有可下发 DP）
 *
 * ★ 与射频的相互影响（实测结论，写下来免得后来人踩）：
 *    2.4GHz WiFi 与 433MHz 不构成同频干扰，但【射频模块供电】是共用的。
 *    WiFi 发射瞬间电流可达 300mA 级，433 接收模块若供电去耦不足，
 *    会在 WiFi 发包瞬间漏收帧。若现场发现「开 WiFi 后 crc 变多」，
 *    优先处理接收模块的电源去耦，不要去改解码阈值。
 *
 * ★ WiFi 配网（手机 AP 配网，与涂鸦/米家同思路）：
 *    长按 BOOT 键 2 秒打开 WiFi 之后，任务按下面顺序走：
 *      ① NVS 里没有 WiFi 凭据           → 开 AP 热点让手机配网
 *      ② 有凭据但【扫描不到】那个 SSID   → 开 AP 热点让手机配网
 *      ③ 扫描得到 → 直连；连续失败 3 次  → 开 AP 热点让手机重新配网
 *    AP 热点名 Kanchuan-HB-XXXX（MAC 后 4 位），密码见 TuyaConfig.h。
 *    手机连上后自动弹配置页（DNSServer 把全部域名指到 192.168.4.1，
 *    即 captive portal）；没弹出就手动开 http://192.168.4.1。
 *    配网只在【本任务】里跑，主循环完全不参与，屏幕与射频不受影响。
 * ============================================================================
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */
#ifndef TUYA_LINK_H
#define TUYA_LINK_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <time.h>
#include <sys/time.h>
#include <mbedtls/md.h>
#include "TuyaConfig.h"
#include "DebugLog.h"

// ── Tuya 官方指定根证书：Go Daddy Root Certificate Authority - G2 ──
//   来源 https://images.tuyacn.com/tuyalink/cert/Go%20Daddy%20Root%20Certificate%20Authority%20-%20G2.cer
static const char TUYA_ROOT_CA[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIDxTCCAq2gAwIBAgIBADANBgkqhkiG9w0BAQsFADCBgzELMAkGA1UEBhMCVVMxEDAOBgNVBAgT
B0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxGjAYBgNVBAoTEUdvRGFkZHkuY29tLCBJbmMu
MTEwLwYDVQQDEyhHbyBEYWRkeSBSb290IENlcnRpZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5
MDkwMTAwMDAwMFoXDTM3MTIzMTIzNTk1OVowgYMxCzAJBgNVBAYTAlVTMRAwDgYDVQQIEwdBcml6
b25hMRMwEQYDVQQHEwpTY290dHNkYWxlMRowGAYDVQQKExFHb0RhZGR5LmNvbSwgSW5jLjExMC8G
A1UEAxMoR28gRGFkZHkgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBAL9xYgjx+lk09xvJGKP3gElY6SKDE6bFIEMBO4Tx5oVJnyfq
9oQbTqC023CYxzIBsQU+B07u9PpPL1kwIuerGVZr4oAH/PMWdYA5UXvl+TW2dE6pjYIT5LY/qQOD
+qK+ihVqf94Lw7YZFAXK6sOoBJQ7RnwyDfMAZiLIjWltNowRGLfTshxgtDj6AozO091GB94KPutd
fMh8+7ArU6SSYmlRJQVhGkSBjCypQ5Yj36w6gZoOKcUcqeldHraenjAKOc7xiID7S13MMuyFYkMl
NAJWJwGRtDtwKj9useiciAF9n9T521NtYJ2/LOdYq7hfRvzOxBsDPAnrSTFcaUaz4EcCAwEAAaNC
MEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFDqahQcQZyi27/a9
BUFuIMGU2g/eMA0GCSqGSIb3DQEBCwUAA4IBAQCZ21151fmXWWcDYfF+OwYxdS2hII5PZYe096ac
vNjpL9DbWu7PdIxztDhC2gV7+AJ1uP2lsdeu9tfeE8tTEH6KRtGX+rcuKxGrkLAngPnon1rpN5+r
5N9ss4UXnT3ZJE95kTXWXwTrgIOrmgIttRD02JDHBHNA7XIloKmf7J6raBKZV8aPEjoJpL1E/QYV
N8Gb5DKj7Tjo2GTzLH4U/ALqn83/B2gX2yKQOC16jdFU8WnjXzPKej17CuPKf1855eJ1usV2GDPO
LPAvTK33sefOT6jEm0pUBsV/fdUID+Ic/n4XuKxe9tQWskMJDE32p2u0mYRlynqI4uJEvlz36hz1
-----END CERTIFICATE-----
)CERT";

// ── 对外暴露的 WiFi 状态（供 UI 用）──
enum TuyaUiState : uint8_t {
  TUYA_UI_OFF = 0,      // WiFi 关闭
  TUYA_UI_WIFI = 1,     // 正在连 WiFi / 对时
  TUYA_UI_MQTT = 2,     // WiFi 通，正在连云端
  TUYA_UI_ONLINE = 3,   // 已连上涂鸦云
  TUYA_UI_ERROR = 4,    // 反复失败（背退中）
  TUYA_UI_AP = 5,       // 配网热点已开启，等手机连接
};

class TuyaLink {
public:
  // ══════════════════════════════════════════════════════════════
  //  初始化：只加载记忆 + 起任务，【绝不阻塞】
  // ══════════════════════════════════════════════════════════════
  void begin() {
    // ★ 注意：Preferences 同一个对象不能嵌套 begin，所以几件事分开做
    //
    // ★★ 配网后自动重启的落地（现场反馈「配好 WiFi 不会自动重启」）：
    //   配网页保存成功时除了写 ssid/pass，还会写一个一次性标记 prov=1，
    //   然后主动 ESP.restart()。下一次上电读到 prov=1 就：
    //      ① 直接 _wifiOn = true（不等用户再长按 BOOT）
    //      ② 清掉标记（一次性，之后断电重启仍回到「WiFi 关闭」）
    //      ③ 置 _forceDirect（凭据是刚写的，别再扫 3 秒，浪费用户时间）
    //   为什么用标记而不是把 "on" 直接写成 true：
    //     "on" 是用户开关 WiFi 的持久状态，配网重启不该改变它。分开存
    //     才能做到「配网这一下自动联网」+「以后仍然离线优先」。
    _bootAfterProv = false;
    {
      _prefs.begin("tuya", true);
      bool prov = _prefs.getBool("prov", false);
      _prefs.end();
      if (prov) {
        _prefs.begin("tuya", false);
        _prefs.putBool("prov", false);
        _prefs.end();
        _bootAfterProv = true;
      }
    }
#if !TUYA_BOOT_WIFI_OFF
    _prefs.begin("tuya", true);
    _wifiOn = _prefs.getBool("on", false);
    _prefs.end();
#else
    // ★ 开机一律 WiFi 关闭：不占射频、不拖启动、不会「自己连出去」。
    //   需要联网时手动长按 BOOT 2s。
    _wifiOn = false;
#endif
    if (_bootAfterProv) {
      // 配网刚写完凭据 → 这次开机直接联网
      _wifiOn      = true;
      _forceDirect = true;
    }
    // WiFi 凭据：NVS 优先，空则用 TuyaConfig.h 里的出厂默认
    //   ★ 三元组也在这一步一并加载（NVS > 编译期默认）
    _loadCreds();

    _mqtt.setClient(_tls);
    _mqtt.setServer(TUYA_MQTT_HOST, TUYA_MQTT_PORT);
    _mqtt.setKeepAlive(TUYA_KEEPALIVE_S);
    _mqtt.setBufferSize(512);                // 7 个 DP 的完整上报约 350B，留 46% 余量
    _mqtt.setSocketTimeout(TUYA_SOCKET_TIMEOUT_S);
    // ★ 不注册 setCallback / 不 subscribe 下行：本设备没有可下发 DP。
    //   这样即使云端误发 property/set，PubSubClient 也会直接丢弃，
    //   不会在射频任务里多走一段解析。

    // ★ topic / clientId 依赖 DeviceID，所以必须在 _loadCreds() 之后才能拼。
    //   原来这里是直接拼 TUYA_DEVICE_ID（编译期常量），现在改成拼运行期值 _devId，
    //   于是「配网页改 DeviceID → 不用重烧固件」才真正成立。
    snprintf(_topicUp, sizeof(_topicUp), "tylink/%s/thing/property/report", _devId);
    snprintf(_clientId, sizeof(_clientId), "tuyalink_%s", _devId);

    // AP 热点名：前缀 + MAC 后 4 位（同名设备多了也不会撞）
    //   ★ 前缀就在 TuyaConfig.h 的 TUYA_AP_SSID_PREFIX 里改。
    //   ⚠️ 不要照抄成 "SmartLife-XXXX" / "tuya-XXXX"：
    //      那是涂鸦 App「AP 配网」流程专用名的样子，可我们走的是自己的
    //      强制门户网页，不是涂鸦的 6666/6667 私有配网协议。
    //      起个像涂鸦的名字只会诱使用户去 App 里试"添加设备"，然后失败。
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(_apSsid, sizeof(_apSsid), "%s%02X%02X", TUYA_AP_SSID_PREFIX, mac[4], mac[5]);

    // ── 涂鸦三元组默认值（TuyaConfig.h）：日志里只打长度，不打印密钥本体 ──
    Serial.printf("[TUYA] triplet: did=%s dseclen=%u%s\n",
                  _devId[0] ? _devId : "<none>",
                  (unsigned)strlen(_devSecret),
                  _credsConfigured() ? "" : "  <== NOT CONFIGURED");

    _state = ST_OFF;
    _uiState = TUYA_UI_OFF;
    _lastReportMs = 0;

    // ★ 保存任务句柄：仅用于 [HEALTH] 里读栈高水位，判断 TLS 握手是否把栈压穿。
    // ★ 优先级保持 1（与 loopTask 相同），【不能再低】：
    //   ESP32-C3 是单核，loopTask 优先级就是 1 且内部没有 vTaskDelay，
    //   设成 0 会让 tuya 任务一次都调度不到（WiFi/MQTT 永远连不上）。
    //   详细推导见 TuyaConfig.h 里 TUYA_TASK_PRIO 的注释。
    xTaskCreatePinnedToCore(_taskEntry, "tuya", TUYA_TASK_STACK, this,
                            TUYA_TASK_PRIO, &_taskHandle, tskNO_AFFINITY);

    Serial.printf("[TUYA] module ready, wifi=%s%s (long-press BOOT 2s to toggle)\n",
                  _wifiOn ? "ON" : "OFF",
                  _bootAfterProv ? " (auto-on after provisioning)" : "");
    Serial.printf("[TUYA] stored wifi: %s\n", _ssid[0] ? _ssid : "<none, will start AP portal>");
    if (!_credsConfigured()) {
      Serial.println("[TUYA] !! credentials not filled in TuyaConfig.h - "
                     "WiFi will connect but cloud will reject");
    }
  }

  // ══════════════════════════════════════════════════════════════
  //  主循环钩子：只做几个字节的搬运，永不阻塞
  // ══════════════════════════════════════════════════════════════
  void loop() {
    // ── 目前没有可下发 DP，这里留空 ──
    //   ★ 历史：这里原来负责把 APP 下发的 wifi_enable 落到主循环上下文
    //     （任务不能直接改 _wifiOn，开关 WiFi 会牵动 UI 与按键状态）。
    //     该 DP 已删除 —— 见 TuyaConfig.h 第 5 节：WiFi 关着时设备离线、
    //     APP 的指令根本发不过来，能用时也只能把自己踢下线，属于无用点。
    //   ★ 保留这个空函数是因为 master.ino 里有一处 tuya.loop() 调用。
    //     以后若要加新的可下发 DP，务必把「落地动作」写在这里、
    //     而不是在射频任务里直接改状态 —— 那是这条注释唯一的价值。
  }

  // ══════════════════════════════════════════════════════════════
  //  防区状态快照（主循环调用，只是几个字节的写入）
  // ══════════════════════════════════════════════════════════════
  //   ★ online 参数【保留但不再上报】：
  //     平台上没有 zone_*_online 这四个 DP，_onMask 已于本次一并删除。
  //     参数留着是为了不改 master.ino 的调用点 —— 而且 zoneOnline 在
  //     主循环里还要喂 radarDisplay.setZoneHealth() 做屏幕三态显示，
  //     是真实存在的数据，不是死参数。
  //     ⚠️ 以后若在平台上新建了 online 点，请先在这里恢复掩码与上报，
  //        不要把参数直接接进 _report() 之外的地方。
  void updateZones(const bool *presence, const bool *online, int count) {
    uint8_t pm = 0;
    for (int i = 0; i < 4; i++) {
      if (presence && presence[i]) pm |= (uint8_t)(1u << i);
    }
    (void)online;                          // 当前不参与上报，见上方说明
    _presMask = pm;
    _zoneCount = (uint8_t)(count < 0 ? 0 : (count > 4 ? 4 : count));
  }

  // ══════════════════════════════════════════════════════════════
  //  雷达实时数据快照（主循环调用，纯整数写入，无浮点无分配）
  // ══════════════════════════════════════════════════════════════
  //   ★ 为什么单独开一个入口而不是塞进 updateZones()：
  //     雷达是 50Hz 刷新，防区状态是事件驱动。两者的节流策略完全不同
  //     （雷达要 3s 限流 + 死区，防区要立即上报），混在一起会互相拖累。
  //
  //   distM / angleDeg 传浮点（主循环本来就有浮点值），这里立刻换成
  //   整数存储：距离按 1cm、角度按 1°。上报时直接用整数拼 JSON，
  //   避免在 512 字节的 snprintf 里做浮点格式化（dtostrf 易算错长度）。
  //
  //   ★ 距离为什么用【厘米】而不是 0.1m：
  //     平台 DP 的「倍数」必须填 0（= 0 位小数），填 1 会让 APP 把值
  //     除以 10 再显示（实测整屏数字小 10 倍）。既然不允许小数位，
  //     那就直接发厘米 —— 180 就是 180cm，语义最直白。
  //
  //   ★ targets = 0 时强制 dist/angle 归零：没有目标就没有距离可言，
  //     留着上一次的距离会让 APP 显示「无人但距离 1.8m」，很怪。
  void updateRadar(float distM, float angleDeg, int targets) {
    int t = targets;
    if (t < 0) t = 0;
    if (t > 3) t = 3;

    int dcm = 0, ang = 0;
    if (t > 0) {
      // 距离：米 → 厘米。夹在 DP 声明的 0~800 范围内（0~8.00m）
      float cm = distM * 100.0f;
      if (!(cm >= 0.0f)) cm = 0.0f;          // 顺带挡 NaN
      if (cm > 800.0f) cm = 800.0f;
      dcm = (int)(cm + 0.5f);

      // 角度：夹在 -90 ~ 90
      if (!(angleDeg >= -90.0f)) angleDeg = -90.0f;
      if (angleDeg > 90.0f)     angleDeg = 90.0f;
      ang = (int)(angleDeg >= 0.0f ? (angleDeg + 0.5f) : (angleDeg - 0.5f));
    }

    _radarDist   = (int16_t)dcm;
    _radarAngle  = (int8_t)ang;
    _radarTargets = (uint8_t)t;
  }

  // ══════════════════════════════════════════════════════════════
  //  WiFi 开关（按键三击 / APP 下发 / 内部复位都走这里）
  // ══════════════════════════════════════════════════════════════
  bool wifiEnabled() const { return _wifiOn; }

  void toggleWifi() { setWifiEnabled(!_wifiOn); }

  void setWifiEnabled(bool on) {
    if (_wifiOn == on) return;
    _wifiOn = on;
    _forceReport = true;

    if (on) {
      Serial.println("[TUYA] WiFi ON");
    } else {
      // ★ 必须走任务去停：在 loop() 里直接 WiFi.disconnect() 会阻塞几百 ms。
      Serial.println("[TUYA] WiFi OFF");
    }

    _prefs.begin("tuya", false);
    _prefs.putBool("on", _wifiOn);
    _prefs.end();

    // 唤醒任务，让它立刻按新状态动作（不用等背退计时）
    _wake = true;
    _uiState = on ? TUYA_UI_WIFI : TUYA_UI_OFF;
  }

  TuyaUiState uiState() const { return (TuyaUiState)_uiState; }
  bool online() const { return _uiState == TUYA_UI_ONLINE; }
  bool apMode() const { return _uiState == TUYA_UI_AP; }
  const char *apSsid() const { return _apSsid; }

  // 当前的 WiFi 名（空 = 还没配过网）
  const char *storedSsid() const { return _ssid; }

  // 当前生效的 DeviceID（NVS 或编译期默认），仅诊断/日志用
  const char *deviceId() const { return _devId; }

  // 清掉已经配好的 WiFi，下次开 WiFi 直接进 AP 配网
  //   （长按 5s 复位会调用它）
  //   ★ 三元组也一并清掉：复位后回到 TuyaConfig.h 里的编译期默认值，
  //     否则「配网页填错了一台设备的三元组」会变成一个无法摆脱的陷阱。
  //   ★ prov 标记也要清：如果复位发生在「刚配完网、还没重启」的那 0.9 秒内，
  //     不清的话下次上电会带着空凭据自动开 WiFi，白跑一轮。
  void forgetWifi() {
    _prefs.begin("tuya", false);
    _prefs.remove("ssid");
    _prefs.remove("pass");
    _prefs.remove("did");
    _prefs.remove("dsec");
    _prefs.remove("prov");
    _prefs.end();
    _ssid[0] = 0;
    _pass[0] = 0;
    _bootAfterProv = false;
    strncpy(_devId, TUYA_DEVICE_ID, sizeof(_devId) - 1);
    strncpy(_devSecret, TUYA_DEVICE_SECRET, sizeof(_devSecret) - 1);
    _devId[sizeof(_devId) - 1] = 0;
    _devSecret[sizeof(_devSecret) - 1] = 0;
    Serial.println("[TUYA] wifi credentials + triplet cleared");
  }

  // 诊断用
  uint32_t mqttConnects() const { return _connects; }
  int lastRc() const { return _lastRc; }
  const char *stateName() const {
    switch (_state) {
      case ST_OFF:      return "OFF";
      case ST_SCAN:     return "SCAN";
      case ST_WIFI:     return "WIFI";
      case ST_TIME:     return "TIME";
      case ST_MQTT:     return "MQTT";
      case ST_ONLINE:   return "ONLINE";
      case ST_AP:       return "AP";
      case ST_BACKOFF:  return "BACKOFF";
      default:          return "?";
    }
  }

private:
  enum State : uint8_t {
    ST_OFF,      // WiFi 射频关闭
    ST_SCAN,     // 扫描确认目标 WiFi 是否在范围内
    ST_WIFI,     // 正在关联
    ST_TIME,     // 等 NTP 对时
    ST_MQTT,     // 连涂鸦云
    ST_ONLINE,   // 已在线
    ST_AP,       // 配网热点运行中
    ST_BACKOFF,  // 失败背退
  };

  Preferences       _prefs;
  WiFiClientSecure  _tls;
  PubSubClient      _mqtt;
  WebServer         _http{80};
  DNSServer         _dns;

  char    _topicUp[80];
  char    _clientId[48];
  char    _apSsid[40] = {0};

  char    _ssid[33] = {0};      // 当前 WiFi 名（NVS）
  char    _pass[65] = {0};      // 当前 WiFi 密码（NVS）

  // ★ 运行期三元组（NVS 优先，空则回落 TuyaConfig.h）
  //   为什么不做成全局常量：换设备/换产品时，现场只要连热点改一下网页就行，
  //   不必带着笔记本去重新编译烧录。容量按官方最大值留足：
  //   DeviceID 目前实际 22 字符，留 48；DeviceSecret 实际 16 字符，留 64。
  char    _devId[48]     = {0};
  char    _devSecret[64] = {0};

  volatile bool     _wifiOn       = false;
  volatile uint8_t  _uiState      = TUYA_UI_OFF;
  volatile State    _state        = ST_OFF;
  volatile bool     _wake         = false;
  volatile bool     _forceReport  = false;

  volatile uint8_t  _presMask     = 0;
  volatile uint8_t  _zoneCount    = 0;

  // 雷达实时数据（由 updateRadar() 写入，_report() 读取）
  //   dist 单位 1cm（0~800）、angle 单位 1°（-90~90）、targets 0~3
  volatile int16_t  _radarDist    = 0;
  volatile int8_t   _radarAngle   = 0;
  volatile uint8_t  _radarTargets = 0;

  volatile uint32_t _connects     = 0;
  volatile int      _lastRc       = 0;

  uint8_t   _reportedPres  = 0xFF;   // 0xFF = 还没上报过
  bool      _reportedInit  = false;

  // 已上报过的雷达值（用于死区比较）+ 上次上报时刻（用于 3s 节流）
  int16_t       _reportedDist    = -32768;  // 哨兵：保证首次一定判定为「变化」
  int8_t        _reportedAngle   = -128;
  uint8_t       _reportedTargets = 0xFF;
  unsigned long _lastRadarMs     = 0;

  unsigned long _lastReportMs  = 0;
  unsigned long _backoffMs     = 0;
  unsigned long _stateStartMs  = 0;
  unsigned long _apStartMs     = 0;
  uint8_t       _connFails     = 0;   // 连续直连失败次数 → 达 TUYA_CONN_RETRY 转 AP
  bool          _apRoutesReady = false;  // WebServer 路由只注册一次（否则每次进 AP 泄漏）

  static void _taskEntry(void *arg) {
    ((TuyaLink *)arg)->_run();
    vTaskDelete(nullptr);
  }

  // 任务句柄（setup 里由 xTaskCreatePinnedToCore 填入）
  //   只给 [HEALTH] 读 uxTaskGetStackHighWaterMark 用，不参与任何逻辑。
  //   ★ 单独开 public 区：调用方是 master.ino 的 loop()。
public:
  TaskHandle_t taskHandle() const { return _taskHandle; }
private:

  bool _credsConfigured() const {
    // ★ 判据已改成「运行期值」：NVS 里配过就用 NVS，否则用 TuyaConfig.h 默认。
    //   这样从配网页改完三元组无需重新编译，下一次连云端就是新设备。
    return strlen(_devId) > 8 && strlen(_devSecret) > 8;
  }

  // ══════════════════════════════════════════════════════════════
  //  WiFi 凭据读写（NVS；空则回落 TuyaConfig.h 出厂默认）
  // ══════════════════════════════════════════════════════════════
  void _loadCreds() {
    _prefs.begin("tuya", true);
    String s = _prefs.getString("ssid", "");
    String p = _prefs.getString("pass", "");
    // ★ 三元组也从 NVS 读：配网页写过就以 NVS 为准，否则回落编译期默认值
    String did = _prefs.getString("did", "");
    String dsc = _prefs.getString("dsec", "");
    _prefs.end();

    if (s.length() > 0) {
      strncpy(_ssid, s.c_str(), sizeof(_ssid) - 1);
      strncpy(_pass, p.c_str(), sizeof(_pass) - 1);
    } else if (strlen(TUYA_WIFI_SSID) > 0) {
      // 出厂默认值（TuyaConfig.h 填了才用）
      strncpy(_ssid, TUYA_WIFI_SSID, sizeof(_ssid) - 1);
      strncpy(_pass, TUYA_WIFI_PASS, sizeof(_pass) - 1);
    }

    // 三元组：NVS > 编译期默认
    strncpy(_devId, did.length() ? did.c_str() : TUYA_DEVICE_ID, sizeof(_devId) - 1);
    strncpy(_devSecret, dsc.length() ? dsc.c_str() : TUYA_DEVICE_SECRET, sizeof(_devSecret) - 1);
    _devId[sizeof(_devId) - 1] = 0;
    _devSecret[sizeof(_devSecret) - 1] = 0;
  }

  // 保存三元组到 NVS（配网页调用）；传空字符串 = 删除覆盖值，回落到编译期默认
  //   ★ 返回 false 表示长度超限被拒 —— 超长会被 strncpy 悄悄截断，
  //     截断后的 DeviceID 与云端登记的对不上，表现为 rc=4 却毫无线索。
  bool _saveTriplet(const char *did, const char *dsec) {
    if ((did && strlen(did) > sizeof(_devId) - 1) ||
        (dsec && strlen(dsec) > sizeof(_devSecret) - 1)) {
      Serial.printf("[TUYA] triplet rejected: too long (did=%u/%u dsec=%u/%u)\n",
                    did ? (unsigned)strlen(did) : 0u, (unsigned)(sizeof(_devId) - 1),
                    dsec ? (unsigned)strlen(dsec) : 0u, (unsigned)(sizeof(_devSecret) - 1));
      return false;
    }
    _prefs.begin("tuya", false);
    if (did && did[0]) _prefs.putString("did", did); else _prefs.remove("did");
    if (dsec && dsec[0]) _prefs.putString("dsec", dsec); else _prefs.remove("dsec");
    _prefs.end();
    strncpy(_devId, (did && did[0]) ? did : TUYA_DEVICE_ID, sizeof(_devId) - 1);
    strncpy(_devSecret, (dsec && dsec[0]) ? dsec : TUYA_DEVICE_SECRET, sizeof(_devSecret) - 1);
    _devId[sizeof(_devId) - 1] = 0;
    _devSecret[sizeof(_devSecret) - 1] = 0;
    Serial.printf("[TUYA] triplet saved: did=%s dseclen=%u\n",
                  _devId, (unsigned)strlen(_devSecret));
    return true;
  }

  void _saveCreds(const char *ssid, const char *pass) {
    _prefs.begin("tuya", false);
    _prefs.putString("ssid", ssid);
    _prefs.putString("pass", pass);
    _prefs.end();
    strncpy(_ssid, ssid, sizeof(_ssid) - 1);
    strncpy(_pass, pass, sizeof(_pass) - 1);
  }

  // ──────────────────────────────────────────────────────────────
  //  扫描：确认 _ssid 到底在不在范围内
  //    ★ 为什么用【异步扫描】而不是 WiFi.begin() 硬试：
  //      连一个不在范围的热点要等 20s 超时才失败；而扫描 3~4 秒就有
  //      结论。现场差别很大：「长按 2s → 3 秒后弹热点让你配网」远好过
  //      「干等 20 秒才弹」。
  //    ★ 为什么是 async 而不是阻塞 scanNetworks()：
  //      阻塞扫描期间本任务无法响应「关 WiFi」请求，最长会卡 4 秒。
  //      改成异步后每 100ms 轮询一次，随时能被打断。
  //
  //   ★★ 现场 bug（已修）：「手动重启后还没搜到就跳进二维码配网」
  //      根因不在扫描太短，而在【扫描根本没跑起来】：
  //      刚上电、或刚从 WIFI_OFF/WIFI_AP_STA 切到 WIFI_STA 时，
  //      esp_wifi 内部状态还没 READY，esp_wifi_scan_start() 直接返回
  //      ESP_ERR_WIFI_STATE；Arduino 的 scanNetworks() 于是【立刻】
  //      返回 WIFI_SCAN_FAILED（0 毫秒，不是 6 秒）。
  //      老代码把「调用被拒」当成「附近没有这个 WiFi」→ 瞬间开热点，
  //      用户看到的就是「刚点开 WiFi 就弹二维码，一个搜索过程都没有」。
  //      现在：调用被拒 / 扫到 0 个 AP / 扫描内部超时，全部判为「抖动」
  //      并重试；只有「确实扫到若干 AP 但没有目标」才累积到开热点。
  // ──────────────────────────────────────────────────────────────
  //  _scanStart() 返回 false = 试了几次射频都拒绝扫描（需要外层稍后重试）
  bool _scanStart() {
    _scanStartMs = millis();
    _scanTries++;
    Serial.printf("[TUYA] scan for '%s' (attempt %u) ...\n", _ssid,
                  (unsigned)_scanTries);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    // ★ 内层小重试：刚从 WIFI_OFF / WIFI_AP_STA 切过来时，esp_wifi 需要
    //   几十到几百毫秒才进入 READY 状态，这期间 esp_wifi_scan_start() 直接被
    //   拒（Arduino 层表现为【立刻】返回 WIFI_SCAN_FAILED，0 毫秒，不是 6 秒）。
    //   不在这里挡住，外层就会把「被拒」误当「附近没有这个 WiFi」→ 秒开热点。
    for (int i = 0; i < 4; i++) {
      delay(i == 0 ? TUYA_SCAN_SETTLE_MS : 300);
      WiFi.scanDelete();
      int16_t r = WiFi.scanNetworks(true, false, false, TUYA_SCAN_CHAN_MS);
      if (r != WIFI_SCAN_FAILED) return true;      // RUNNING 或已出结果都算成功
      Serial.printf("[TUYA]   radio not ready (try %d/4), wait...\n", i + 1);
    }
    Serial.println("[TUYA] scan request rejected 4x -> give up this attempt");
    return false;
  }

  //  返回值：
  //    -1 = 还在扫
  //     0 = 扫描本身失败 / 一个 AP 都没扫到（射频没起来 → 重试）
  //     1 = 找到了目标
  //     2 = 扫到了别的 AP，但没有目标（唯一可信的「不在范围」）
  int8_t _scanPoll() {
    // 自保：库内部超时判定之外再加一道，防止 async 扫描永远不回调
    if (millis() - _scanStartMs > TUYA_SCAN_HARD_MS) {
      WiFi.scanDelete();
      Serial.println("[TUYA] scan hard timeout -> retry");
      return 0;
    }
    int16_t r = WiFi.scanComplete();
    if (r == WIFI_SCAN_RUNNING) return -1;
    if (r == WIFI_SCAN_FAILED || r < 0) {
      WiFi.scanDelete();
      Serial.println("[TUYA] scan failed -> retry");
      return 0;
    }
    if (r == 0) {
      // 家里不可能一个 AP 都扫不到；出现 0 个只能是射频没起来
      WiFi.scanDelete();
      Serial.println("[TUYA] scan got 0 APs -> retry");
      return 0;
    }
    bool found = false;
    for (int i = 0; i < r; i++) {
      if (WiFi.SSID(i) == _ssid) { found = true; _lastRssi = WiFi.RSSI(i); break; }
    }
    Serial.printf("[TUYA] scan: %d APs, target %s\n", (int)r,
                  found ? "FOUND" : "ABSENT");
    if (found) {
      WiFi.scanDelete();          // 几百字节结果没必要一直占着
      return 1;
    }
    // ★ 没找到时不 scanDelete：这份结果正好给接下来的 AP 配网页当候选列表。
    //   否则在 AP 模式下再去扫一次 —— 那会让热点广播中断 1~3 秒，
    //   手机端 WiFi 列表/页面直接掉线，配网体验很差。
    return 2;
  }

  unsigned long _scanStartMs = 0;
  int           _lastRssi     = 0;
  bool          _forceDirect  = false;   // 跳过扫描直接连（配网重启后 / 背退恢复）
  uint8_t       _scanTries    = 0;       // 本次找网已发起几次扫描
  uint8_t       _scanAbsent   = 0;       // 连续「扫到 AP 但没有目标」的次数
  unsigned long _scanNextMs   = 0;       // 下一次扫描的时间点（0 = 不等待）

  // 配网保存后的「干净重启」（见 TuyaConfig.h 的 TUYA_PROV_RESTART_MS）
  bool          _restartPending = false;
  unsigned long _restartAtMs    = 0;
  bool          _bootAfterProv  = false;  // 本次启动是配网后的自动重启

  // ══════════════════════════════════════════════════════════════
  //  AP 配网门户（captive portal）
  //    · DNSServer 把所有域名都解析到 192.168.4.1
  //      → 手机连上热点后系统会认为「需要登录网络」，自动弹出配置页
  //    · WebServer 提供 3 个路由：
  //        /        配网页（下拉框列出扫到的 WiFi + 密码框）
  //        /save    提交凭据 → 写 NVS → 关热点 → 转直连
  //        /rescan  重新扫描（页面上的「刷新」按钮）
  //    ★ 全部跑在 tuya 任务里：主循环、屏幕、433MHz 收帧完全不受影响
  // ══════════════════════════════════════════════════════════════
  void _apStart() {
    Serial.printf("[TUYA] start AP portal: %s (%s)\n", _apSsid,
                  TUYA_AP_OPEN ? "open, no password" : "WPA2");
    WiFi.persistent(false);
    // 先断开 STA 侧：从 ST_WIFI 失败进来时，STA 可能还在后台重连/扫描，
    //   会和热点抢射频时间，导致手机连上了却打不开页面。
    WiFi.disconnect(false, false);
    // ★ 用 AP_STA 而不是纯 AP：纯 AP 模式下 scanNetworks() 返回不了结果，
    //   配网页上的 WiFi 列表就是空的，「刷新列表」也会失效。
    WiFi.mode(WIFI_AP_STA);
#if TUYA_AP_OPEN
    // 开放热点：手机点一下就直连，不需要输密码（见 TuyaConfig.h 的说明）
    WiFi.softAP(_apSsid);
#else
    WiFi.softAP(_apSsid, TUYA_AP_PASS);
#endif
    delay(100);                                   // 等 DHCP 服务起来
    Serial.printf("[TUYA] AP ip=%s\n", WiFi.softAPIP().toString().c_str());

    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());         // 任何域名 → 本机（触发弹窗）

    // ★ 路由只注册一次：WebServer::stop()/begin() 都不清 handler 链表，
    //   每次进 AP 都 on() 一遍会按 4 条/次的速度泄漏（还越挂越多）。
    if (!_apRoutesReady) {
      _apRoutesReady = true;
      _http.on("/",        [this]() { _apHandleRoot(); });
      _http.on("/save",    [this]() { _apHandleSave(); });
      _http.on("/rescan",  [this]() { _apHandleRescan(); });
      _http.onNotFound(    [this]() { _apHandleRoot(); });  // captive portal：全指回首页
    }
    _http.begin();

    // ★ 如果进来之前刚好扫过一次（"目标热点不在范围内"那条路径），
    //   就直接用那份结果，别再扫 —— 扫描会让热点广播中断，手机掉线。
    if (WiFi.scanComplete() < 0) {
      WiFi.scanDelete();
      WiFi.scanNetworks(true, false, false, 300);
    } else {
      Serial.printf("[TUYA] reuse previous scan (%d APs) for portal\n",
                    (int)WiFi.scanComplete());
    }

    _apStartMs = millis();
    _state     = ST_AP;
    _uiState   = TUYA_UI_AP;
  }

  void _apStop() {
    _http.stop();
    _dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[TUYA] AP portal stopped");
  }

  // 扫描结果 → HTML 下拉框选项（带 RSSI 条数，便于用户挑信号好的）
  void _apSsidOptions(String &out) {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) { out += F("<option value=''>(扫描中，请稍候刷新)</option>"); return; }
    if (n <= 0)                 { out += F("<option value=''>(没扫到，可手输)</option>"); return; }
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
      s.replace("'", "&#39;"); s.replace("\"", "&quot;");
      int r = WiFi.RSSI(i);
      int bars = (r > -55) ? 4 : (r > -67) ? 3 : (r > -78) ? 2 : 1;
      out += "<option value='" + s + "'>";
      for (int b = 0; b < bars; b++) out += F("|");
      out += " " + s + "</option>";
    }
  }

  void _apHandleRoot() {
    String o;
    // ★ 1600 → 2600：新增了三元组折叠区（3 个 input + 说明文字，约 900 字符）
    o.reserve(2600);
    o += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Kanchuan HB</title><style>"
           "body{font-family:-apple-system,sans-serif;margin:0;padding:24px;"
           "background:#111;color:#eee}h2{margin:0 0 4px;font-size:20px}"
           "p{color:#888;font-size:13px;margin:0 0 20px}"
           "label{display:block;font-size:13px;color:#aaa;margin:14px 0 6px}"
           "select,input{width:100%;box-sizing:border-box;padding:11px;"
           "border-radius:8px;border:1px solid #333;background:#1c1c1c;"
           "color:#eee;font-size:15px}"
           "button{width:100%;margin-top:22px;padding:13px;border:0;"
           "border-radius:8px;background:#0af;color:#001;font-size:16px;"
           "font-weight:600}a{color:#0af;font-size:13px;text-decoration:none}"
           "</style></head><body><h2>Kanchuan HB</h2>"
           "<p>心跳雷达 · WiFi 配网</p><form method='GET' action='/save'>"
           "<label>选择 WiFi（2.4GHz）</label><select name='ssid'>");
    _apSsidOptions(o);
    o += F("</select><label>或手动输入 WiFi 名</label>"
           "<input name='ssid2' placeholder='留空则用上面选中的'>"
           "<label>WiFi 密码</label>"
           "<input name='pass' type='password' placeholder='WiFi 密码'>"
           "<button type='submit'>保存并连接</button></form>");

    // ── 涂鸦三元组（可选，折叠起来不干扰普通用户）──
    //   ★ 留空保存 = 保持原值；想彻底回退到固件内置默认值请长按 BOOT 5s 复位。
    //   ★ DeviceID 不回显真实值到输入框（避免手机上误改），只显示类别与尾部几位。
    o += F("<details style='margin-top:26px;border-top:1px solid #333;padding-top:14px'>"
           "<summary style='color:#0af;font-size:14px;cursor:pointer'>"
           "涂鸦云三元组（选填）</summary>"
           "<p style='margin-top:10px'>仅当需要换设备/换产品时才填。留空保存则保持原有值。</p>"
           "<form method='GET' action='/save'>"
           "<label>DeviceID</label>"
           "<input name='did' placeholder='当前：");
    o += _devId[0] ? _devId : "(未配置)";
    o += F("'>"
           "<label>DeviceSecret</label>"
           "<input name='dsec' placeholder='留空则不修改'>"
           "<button type='submit'>保存三元组</button></form></details>");

    o += F("<p style='margin-top:18px'><a href='/rescan'>刷新列表</a></p>"
           "</body></html>");
    _http.send(200, "text/html; charset=utf-8", o);
  }

  void _apHandleRescan() {
    WiFi.scanDelete();
    // ★ 这里必须用阻塞扫描吗？不 —— 但必须保证【请求真的被接受】。
    //   AP_STA 模式下射频是活的，一般都能拿到；万一返回失败就静默忽略，
    //   页面上会显示「没扫到，可手输」，用户点「刷新列表」再来一次即可。
    if (WiFi.scanNetworks(true, false, false, TUYA_SCAN_CHAN_MS) == WIFI_SCAN_FAILED) {
      Serial.println("[TUYA] portal rescan rejected");
    }
    _http.sendHeader("Location", "/");
    _http.send(302, "text/plain", "");
  }

  // ──────────────────────────────────────────────────────────────
  //  配网保存成功 → 写一次性自动联网标记 + 排一次干净重启
  //    ★ 为什么不是「关热点 → 原地切 STA」：
  //      AP 刚停、射频刚切模式、WebServer 刚 stop，紧接着 WiFi.begin()
  //      很容易停在「已关联但拿不到 IP」的半死状态。现场表现就是
  //      「提示已保存，但设备再也没上线」，而用户完全无法自救。
  //      重启是唯一的干净状态，这也是涂鸦/米家/ESPHome 的通行做法。
  //    ★ 标记叫 prov（不是直接写 on=true）：
  //      "on" 是用户开关 WiFi 的持久状态，配网不该改动它；
  //      分开存才能做到「配网这一下自动联网 + 以后仍然离线优先」。
  // ──────────────────────────────────────────────────────────────
  void _scheduleProvRestart() {
    _prefs.begin("tuya", false);
    _prefs.putBool("prov", true);
    _prefs.end();
    _connFails     = 0;
    _backoffMs     = 0;
    _restartPending = true;
    _restartAtMs    = millis() + TUYA_PROV_RESTART_MS;
    Serial.printf("[TUYA] provision ok -> restart in %lums\n",
                  (unsigned long)TUYA_PROV_RESTART_MS);
  }

  void _apHandleSave() {
    // ── ① 三元组表单（第二个 form，可能不带 ssid）──
    String did  = _http.arg("did");
    String dsec = _http.arg("dsec");
    did.trim();
    dsec.trim();

    String s1 = _http.arg("ssid2");
    s1.trim();
    String s2 = _http.arg("ssid");
    s2.trim();
    String ssid = s1.length() ? s1 : s2;
    String pass = _http.arg("pass");

    // 三元组表单：没有 ssid 但填了 did/dsec → 只改三元组，不动 WiFi
    if (ssid.length() == 0 && (did.length() || dsec.length())) {
      // ★ 只填一个会得到「半新半旧」的组合：那种组合几乎必定被云端拒，
      //   而且日志只报 rc=4，查不出原因。所以要求两个都填。
      if (did.length() == 0 || dsec.length() == 0) {
        _http.send(400, "text/html; charset=utf-8",
                   F("<meta charset='utf-8'><body style='font-family:sans-serif;"
                     "background:#111;color:#eee;padding:24px'>"
                     "<h3>两个都要填</h3><p>DeviceID 与 DeviceSecret 必须同时修改。"
                     "</p><a style='color:#0af' href='/'>返回</a></body>"));
        return;
      }
      if (!_saveTriplet(did.c_str(), dsec.c_str())) {
        _http.send(400, "text/html; charset=utf-8",
                   F("<meta charset='utf-8'><body style='font-family:sans-serif;"
                     "background:#111;color:#eee;padding:24px'>"
                     "<h3>长度超限</h3><p>DeviceID 最多 47 字符，DeviceSecret 最多 63 字符。"
                     "</p><a style='color:#0af' href='/'>返回</a></body>"));
        return;
      }
      // DeviceID 变了 → clientId / topic 必须重算，否则连上去是旧设备身份
      snprintf(_topicUp, sizeof(_topicUp), "tylink/%s/thing/property/report", _devId);
      snprintf(_clientId, sizeof(_clientId), "tuyalink_%s", _devId);
      _mqtt.disconnect();

      _http.send(200, "text/html; charset=utf-8",
                 "<meta charset='utf-8'><body style='font-family:sans-serif;"
                 "background:#111;color:#eee;padding:24px'>"
                 "<h3>三元组已保存</h3><p>DeviceID 已更新为 <b>" + String(_devId) +
                 "</b>。<br>设备正在重启，随后会自动联网并用新身份连云端。</p>"
                 "<p style='color:#888'>热点即将消失，可以关闭此页面。</p>"
                 "</body>");
      // ★ 重启而不是原地重启状态机：见 _scheduleProvRestart() 的说明
      _scheduleProvRestart();
      return;
    }

    if (ssid.length() == 0) {
      _http.send(400, "text/html; charset=utf-8",
                 F("<meta charset='utf-8'>没填 WiFi 名。<a href='/'>返回</a>"));
      return;
    }

    // 同一表单里同时改三元组也允许（两栏都填的情况）
    bool tripChanged = false;
    if (did.length() && dsec.length()) {
      if (!_saveTriplet(did.c_str(), dsec.c_str())) {
        _http.send(400, "text/html; charset=utf-8",
                   F("<meta charset='utf-8'><body style='font-family:sans-serif;"
                     "background:#111;color:#eee;padding:24px'>"
                     "<h3>三元组长度超限</h3><p>WiFi 未保存。返回重改。</p>"
                     "<a style='color:#0af' href='/'>返回</a></body>"));
        return;
      }
      snprintf(_topicUp, sizeof(_topicUp), "tylink/%s/thing/property/report", _devId);
      snprintf(_clientId, sizeof(_clientId), "tuyalink_%s", _devId);
      _mqtt.disconnect();
      tripChanged = true;
    }

    Serial.printf("[TUYA] portal saved ssid='%s' passlen=%u\n",
                  ssid.c_str(), (unsigned)pass.length());
    _saveCreds(ssid.c_str(), pass.c_str());

    _http.send(200, "text/html; charset=utf-8",
               "<meta charset='utf-8'><body style='font-family:sans-serif;"
               "background:#111;color:#eee;padding:24px'>"
               "<h3>已保存</h3><p>设备正在重启并连接 <b>" + ssid +
               "</b>，热点即将关闭。<br>约 10~20 秒后屏幕左上角图标会变成 WIFI，"
               "收到云端应答后变绿色 ON。</p>" +
               (tripChanged ? String("<p>涂鸦三元组也已更新为 <b>") + _devId + "</b>。</p>"
                            : String("")) +
               "<p style='color:#888'>可以关闭此页面了。</p>"
               "</body>");

    // ★ 先回包再重启：直接重启会把 TCP 打断，手机端看到「连接被重置」，
    //   用户会以为没保存成功，反复点保存。TUYA_PROV_RESTART_MS 就是为此留的。
    _scheduleProvRestart();
  }

  // ──────────────────────────────────────────────────────────────
  //  HMAC-SHA256 → 64 位小写十六进制
  //    ★ 官方文档说「左补零到 64 位」：32 字节转 hex 天然就是 64 位，
  //      所以这里不需要补零；反过来说，如果你哪天改成截断输出就必须补。
  // ──────────────────────────────────────────────────────────────
  static void _hmacSha256Hex(const char *plain, const char *key, char out[65]) {
    unsigned char mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == nullptr || mbedtls_md_setup(&ctx, info, 1) != 0) {
      mbedtls_md_free(&ctx);
      memset(out, '0', 64);
      out[64] = 0;
      return;
    }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)plain, strlen(plain));
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
      out[i * 2]     = H[(mac[i] >> 4) & 0x0F];
      out[i * 2 + 1] = H[mac[i] & 0x0F];
    }
    out[64] = 0;
  }

  // 当前时间戳（秒；TUYA_TS_UNIT_MS=1 时乘 1000）
  static uint64_t _ts() {
    time_t t = time(nullptr);
    if (t < 1700000000) return 0;      // 未对时，签名必失败，直接用 0 表示无效
#if TUYA_TS_UNIT_MS
    return (uint64_t)t * 1000ULL;
#else
    return (uint64_t)t;
#endif
  }

  // ──────────────────────────────────────────────────────────────
  //  ★★ 下行解析已整体删除（原 _handleDownlink / _mqttCallback / _self）
  //     本设备 7 个 DP 全部是「只上报 ro」，没有任何可下发点。
  //     被删掉的 wifi_enable 无法自洽：WiFi 关着时设备离线（指令发不到），
  //     WiFi 开着时唯一能做的就是把它关掉、然后自己踢自己下线。
  //     → 平台侧不要建 rw 点，固件侧不 subscribe thing/property/set。
  //     以后真要做远程控制时，请注意：
  //       ① 必须留一条【不依赖被控对象本身】的信道（例如把控制点放在
  //          常开的 WiFi 上、被控的是继电器而不是 WiFi 本身）；
  //       ② 落地动作要回到主循环上下文执行，不要在射频任务里改状态。
  // ──────────────────────────────────────────────────────────────

  // ──────────────────────────────────────────────────────────────
  //  上报：全部 DP 一次打出去
  // ──────────────────────────────────────────────────────────────
  void _report(bool all) {
    if (!_mqtt.connected()) return;
    uint64_t t = _ts();
    if (t == 0) return;                    // 没对时就不上报（云端会丢弃时间非法数据）

    uint8_t pm = _presMask, zc = _zoneCount;
    int16_t rd = _radarDist;
    int8_t  ra = _radarAngle;
    uint8_t rt = _radarTargets;

    // ★ 缓冲 768 → 512：DP 从 11 个减到 7 个（删掉 4 个 zone_*_online）。
    //   7 个 DP 的完整上报实测约 350 字节，512 留了 46% 余量。
    //   截断的后果不是崩溃而是「末尾 DP 丢失」——很难查，所以仍给足余量。
    //   ⚠️ 不要再改回 640/768：PubSubClient 的 buffer 是常驻堆，
    //      本工程同时挂着 120KB 画布，省下的 256B 是实打实的。
    char js[512];
    int n = snprintf(js, sizeof(js),
        "{\"msgId\":\"%lu\",\"time\":%llu,\"data\":{",
        (unsigned long)_msgId++, (unsigned long long)t);

    // ── 布尔型 DP（4 个防区 presence；zone_*_online 已在平台侧不存在，故删除）──
    //   ★★ 不要加回 online 四个点：平台上没建，云端会静默丢弃，
    //      publish 仍返回 true，日志里看不出任何异常，属于最难查的一类问题。
    struct Item { const char *code; bool val; };
    Item items[4] = {
      { TUYA_DP_PRES_A, zc > 0 && (pm & 0x01) },
      { TUYA_DP_PRES_B, zc > 1 && (pm & 0x02) },
      { TUYA_DP_PRES_C, zc > 2 && (pm & 0x04) },
      { TUYA_DP_PRES_D, zc > 3 && (pm & 0x08) },
    };

    for (int i = 0; i < 4; i++) {
      if (n >= (int)sizeof(js) - 90) break;   // 保险：绝不越界
      n += snprintf(js + n, sizeof(js) - n,
                    "%s\"%s\":{\"value\":%s,\"time\":%llu}",
                    (n > 0 && js[n - 1] != '{') ? "," : "",
                    items[i].code,
                    items[i].val ? "true" : "false", (unsigned long long)t);
    }

    // ── 数值型 DP（雷达）──
    //   ★ 逗号必须靠「上一个字符是不是 '{'」来决定，不能再靠 i 的下标：
    //     布尔段可能因为越界 break 而提前结束，用下标拼逗号会拼出
    //     "{,," 这种非法 JSON，云端直接丢包。这里改成看 js[n-1] 更稳。
    //   ★ 三个 DP 都是整数，直接 %d —— 全程不碰浮点，长度可算。
#if !TUYA_RADAR_SINGLE_DP
    {
      if (n < (int)sizeof(js) - 90) {
        n += snprintf(js + n, sizeof(js) - n,
                      "%s\"%s\":{\"value\":%d,\"time\":%llu}",
                      (n > 0 && js[n - 1] != '{') ? "," : "",
                      TUYA_DP_DIST, (int)rd, (unsigned long long)t);
      }
      if (n < (int)sizeof(js) - 90) {
        n += snprintf(js + n, sizeof(js) - n,
                      "%s\"%s\":{\"value\":%d,\"time\":%llu}",
                      (n > 0 && js[n - 1] != '{') ? "," : "",
                      TUYA_DP_ANGLE, (int)ra, (unsigned long long)t);
      }
    }
#endif
    {
      if (n < (int)sizeof(js) - 90) {
        n += snprintf(js + n, sizeof(js) - n,
                      "%s\"%s\":{\"value\":%d,\"time\":%llu}",
                      (n > 0 && js[n - 1] != '{') ? "," : "",
                      TUYA_DP_TGT, (int)rt, (unsigned long long)t);
      }
    }

    if (n < (int)sizeof(js) - 4) {
      snprintf(js + n, sizeof(js) - n, "}}");
    } else {
      js[sizeof(js) - 3] = '}';
      js[sizeof(js) - 2] = '}';
      js[sizeof(js) - 1] = 0;
    }

    if (_mqtt.publish(_topicUp, js)) {
      _reportedPres = pm;
      _reportedInit = true;
      _reportedDist    = rd;
      _reportedAngle   = ra;
      _reportedTargets = rt;
      _lastRadarMs     = millis();
      _lastReportMs = millis();
      if (all) {
        dbgLog("[TUYA] full report ok (%dB)\n", n + 2);
      }
    } else {
      Serial.printf("[TUYA] publish FAILED (len=%d, mqtt_state=%d)\n", n, _mqtt.state());
    }
  }
  TaskHandle_t _taskHandle = nullptr;   // tuya 任务句柄（见 taskHandle()）
  uint32_t _msgId = 1;

  // ──────────────────────────────────────────────────────────────
  //  真正干活的任务
  // ──────────────────────────────────────────────────────────────
  void _run() {
    for (;;) {
      unsigned long now = millis();

      // ── 配网保存后的「干净重启」 ──
      //   必须放在总闸【之前】：重启是最高优先级动作，不能被 WiFi 状态机拦下。
      //   延时的意义只是让刚发出的 200 回包真正送到手机上（见 TUYA_PROV_RESTART_MS）。
      if (_restartPending && (long)(now - _restartAtMs) >= 0) {
        Serial.println("[TUYA] provision saved -> restarting into station mode");
        Serial.flush();
        delay(60);                 // USB-CDC 收尾，让最后那行日志先出去
        ESP.restart();
      }

      // ── 总闸：WiFi 关着 → 彻底停掉射频，然后低频空转 ──
      if (!_wifiOn) {
        if (_state != ST_OFF) {
          if (_state == ST_AP) _apStop();
          _mqtt.disconnect();
          WiFi.disconnect(true);
          WiFi.mode(WIFI_OFF);
          _state = ST_OFF;
          _uiState = TUYA_UI_OFF;
          _connFails = 0;
          _backoffMs = 0;
          Serial.println("[TUYA] radio off");
        }
        _wake = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      switch (_state) {
        // ── 关闭 → 决定「扫一下」还是「直接连」 ──
        case ST_OFF: {
          if (_forceDirect && _ssid[0]) {
            // 刚配完网 / 背退恢复：凭据是新鲜的，别再扫描浪费 3 秒
            _forceDirect = false;
            _beginStation(now);
            break;
          }
          _forceDirect = false;
          if (_ssid[0] == 0) {
            // 从没配过网 → 直接开热点
            Serial.println("[TUYA] no wifi credentials -> AP portal");
            _apStart();
            break;
          }
          // 新一轮找网：清掉上次的计数
          _scanTries  = 0;
          _scanAbsent = 0;
          _scanNextMs = 0;
          Serial.printf("[TUYA] wifi creds='%s' -> scan\n", _ssid);
          if (_scanStart()) {
            _state = ST_SCAN;
            _stateStartMs = now;
            _uiState = TUYA_UI_WIFI;
          } else {
            // 射频还没就绪：稍等再试，别急着开热点
            _state = ST_SCAN;
            _stateStartMs = now;
            _uiState = TUYA_UI_WIFI;
            _scanNextMs = now + TUYA_SCAN_RETRY_MS;
          }
          break;
        }

        // ── 扫描：在范围内就直连；确认不在才开热点 ──
        case ST_SCAN: {
          // 重试间隔：等到了再发起下一次
          if (_scanNextMs) {
            if ((long)(now - _scanNextMs) < 0) break;
            _scanNextMs = 0;
            // 总次数兜底：任何路径都不许无限重试，否则用户会看到「一直连不上
            // 也不开热点」的死循环。到顶就开热点让人重新配。
            if (_scanTries >= TUYA_SCAN_MAX_TRIES) {
              Serial.printf("[TUYA] scan gave up after %u tries -> AP portal\n",
                            (unsigned)_scanTries);
              _apStart();
              break;
            }
            if (!_scanStart()) { _scanNextMs = now + TUYA_SCAN_RETRY_MS; break; }
            _stateStartMs = now;
            break;
          }

          int8_t r = _scanPoll();
          if (r < 0) break;                 // 还在扫，下轮再看

          if (r == 1) {                     // 找到了
            _scanAbsent = 0;
            _beginStation(now);
            break;
          }

          if (r == 0) {
            // 扫描本身失败 / 0 个 AP → 射频没就绪，重试
            if (_scanTries < TUYA_SCAN_RETRY) {
              Serial.printf("[TUYA] scan retry %u/%u\n",
                            (unsigned)_scanTries, (unsigned)TUYA_SCAN_RETRY);
              _scanNextMs = now + TUYA_SCAN_RETRY_MS;
              break;
            }
            // 重试次数用尽，才承认「扫不出来」，开热点
            Serial.printf("[TUYA] scan unusable after %u tries -> AP portal\n",
                          (unsigned)_scanTries);
            _apStart();
            break;
          }

          // r == 2：扫到了别的 AP，但没有目标。可能是路由器刚上电还没广播，
          //   也可能确实不在范围。复核 TUYA_SCAN_ABSENT_TRIES 次再下结论，
          //   避免一次偶然漏扫就把用户丢进配网流程。
          _scanAbsent++;
          if (_scanAbsent < TUYA_SCAN_ABSENT_TRIES) {
            Serial.printf("[TUYA] target absent (%u/%u) -> rescan\n",
                          (unsigned)_scanAbsent, (unsigned)TUYA_SCAN_ABSENT_TRIES);
            WiFi.scanDelete();              // 重扫前必须清，否则下轮立刻返回旧结果
            _scanNextMs = now + TUYA_SCAN_RETRY_MS;
            break;
          }
          Serial.println("[TUYA] target wifi not in range -> AP portal");
          _apStart();
          break;
        }

        // ── 等 WiFi 关联 ──
        case ST_WIFI: {
          if (WiFi.status() == WL_CONNECTED) {
            _connFails = 0;
            Serial.printf("[TUYA] WiFi ok  ip=%s rssi=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            // 对时：签名里的 timestamp 必须真实，否则云端直接拒
            configTime(0, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
            _state = ST_TIME;
            _stateStartMs = now;
            _uiState = TUYA_UI_WIFI;
          } else if (now - _stateStartMs > TUYA_WIFI_TIMEOUT_MS) {
            Serial.printf("[TUYA] WiFi timeout (status=%d)\n", (int)WiFi.status());
            // ★ 密码错 / 热点关了 / 换了路由器 → 失败够多次就别再死试，
            //   开热点让用户重新配（这就是「常用方案」里的兜底）
            if (++_connFails >= TUYA_CONN_RETRY) {
              _connFails = 0;
              Serial.printf("[TUYA] %d consecutive failures -> AP portal\n",
                            (int)TUYA_CONN_RETRY);
              _apStart();
            } else {
              _enterBackoff();
            }
          }
          break;
        }

        // ── 等 NTP 对时 ──
        case ST_TIME: {
          if (WiFi.status() != WL_CONNECTED) { _state = ST_WIFI; _stateStartMs = now; break; }
          if (_ts() != 0) {
            Serial.printf("[TUYA] time synced (%llu)\n", (unsigned long long)_ts());
            _state = ST_MQTT;
            _stateStartMs = now;
          } else if (now - _stateStartMs > TUYA_TIME_TIMEOUT_MS) {
            Serial.println("[TUYA] NTP timeout");
            _enterBackoff();
          }
          break;
        }

        // ── 连 MQTT ──
        case ST_MQTT: {
          if (WiFi.status() != WL_CONNECTED) { _state = ST_WIFI; _stateStartMs = now; break; }

          uint64_t ts = _ts();
          char user[192], plain[192], pass[65];
          // ★ 用运行期 _devId / _devSecret，不用编译期宏 ——
          //   这样配网页改完三元组，长按 2s 重开 WiFi 就能连新设备。
          snprintf(user, sizeof(user),
                   "%s|signMethod=hmacSha256,timestamp=%llu,secureMode=1,accessType=1",
                   _devId, (unsigned long long)ts);
          snprintf(plain, sizeof(plain),
                   "deviceId=%s,timestamp=%llu,secureMode=1,accessType=1",
                   _devId, (unsigned long long)ts);
          _hmacSha256Hex(plain, _devSecret, pass);

          _tls.setCACert(TUYA_ROOT_CA);

          Serial.printf("[TUYA] mqtt connect %s:%d ...\n", TUYA_MQTT_HOST, TUYA_MQTT_PORT);
          bool ok = _mqtt.connect(_clientId, user, pass);
          _lastRc = _mqtt.state();
          if (ok) {
            _connects++;
            _state = ST_ONLINE;
            _uiState = TUYA_UI_ONLINE;
            _backoffMs = 0;
            _reportedInit = false;         // 强制首轮全量上报
            Serial.printf("[TUYA] ONLINE (%s)\n", _topicUp);
            _report(true);
          } else {
            // rc 含义：-4 超时/网络 / 4 用户名密码错 / 5 未授权
            //   若长期是 4 → 先怀疑 TuyaConfig.h 里 TUYA_TS_UNIT_MS 设反了
            Serial.printf("[TUYA] mqtt connect FAILED rc=%d state=%d\n",
                          _lastRc, (int)_mqtt.state());
            _enterBackoff();
          }
          break;
        }

        // ── 在线：维持 + 上报 ──
        case ST_ONLINE: {
          if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[TUYA] WiFi lost");
            _mqtt.disconnect();
            _state = ST_WIFI;
            _stateStartMs = now;
            _uiState = TUYA_UI_WIFI;
            break;
          }
          if (!_mqtt.connected()) {
            Serial.println("[TUYA] mqtt dropped");
            _state = ST_MQTT;
            _stateStartMs = now;
            _uiState = TUYA_UI_MQTT;
            break;
          }

          _mqtt.loop();

          // 变化上报 / 定期全量上报
          //   ★ 只比较 presence 掩码：online 掩码随 _onMask 一起被移除
          //     （平台上没有 zone_*_online 这四个 DP，上报了也会被静默丢弃）。
          //     防区掉线时 presence 本身会变 false（occupied 依赖 online），
          //     所以「掉线」仍会触发一次上报，只是 APP 上表现为「无人」。
          bool changed = !_reportedInit ||
                         _presMask != _reportedPres ||
                         _forceReport;

          // ── 雷达变化判定（带死区 + 2s 节流）──
          //   ★ 为什么必须死区：雷达 50Hz 刷新，目标静止时距离也会在
          //     ±几 cm 内来回跳。不去抖的话每 2s 都在上报「距离 180→179→181」，
          //     APP 上看到数字乱跳，MQTT 也白跑。
          //   ★ 为什么必须节流：万一现场有人快速走动，距离每帧都在变，
          //     没有 TUYA_RADAR_MIN_MS 就会 50 次/秒上报 → 云端限流断连。
          //   ★ 个数变化优先级最高：0↔1 是「有人/无人」级别的语义变化，
          //     必须立刻上报，不受死区影响（但仍受 3s 节流保护）。
          bool radarDiff = (_radarTargets != _reportedTargets);
          if (!radarDiff && _radarTargets > 0) {
            int dd = (int)_radarDist - (int)_reportedDist;
            if (dd < 0) dd = -dd;
            int da = (int)_radarAngle - (int)_reportedAngle;
            if (da < 0) da = -da;
            radarDiff = (dd >= TUYA_RADAR_DIST_EPS) || (da >= TUYA_RADAR_ANGLE_EPS);
          }
          if (radarDiff && (uint32_t)(now - _lastRadarMs) >= TUYA_RADAR_MIN_MS) {
            changed = true;
          }

          bool periodic = (now - _lastReportMs) >= TUYA_FULL_REPORT_MS;
          if (changed || periodic) {
            _forceReport = false;
            _report(periodic && !changed);
          }
          break;
        }

        // ── AP 配网中：只跑 DNS + HTTP，什么都不连 ──
        case ST_AP: {
          _dns.processNextRequest();
          _http.handleClient();
          // ★ 超时只在「已有凭据、只是连不上」时才生效：
          //   5 分钟没人配 → 关热点真的再试一次直连。
          //   必须置 _forceDirect，否则 ST_OFF 会先扫描，扫不到目标又立刻
          //   重开热点 —— 热点闪一下就回来，等于永远不停，谈不上"重试直连"。
          // ★ 从没配过网（_ssid 为空）→ 不开超时，热点常开。
          //   这类设备没有别的出路，关掉再开只会让用户看到热点忽隐忽现，
          //   误以为设备坏了。涂鸦 / Sonoff 也都是「不配好就一直开着」。
          if (_ssid[0] && (now - _apStartMs > TUYA_AP_TIMEOUT_MS)) {
            Serial.println("[TUYA] AP portal timeout -> retry station");
            _apStop();
            _forceDirect = true;
            _state = ST_OFF;
            _uiState = TUYA_UI_WIFI;
          }
          break;
        }

        // ── 背退 ──
        case ST_BACKOFF: {
          if (!_wake && (now - _stateStartMs) >= _backoffMs) {
            _wake = false;
            _state = ST_OFF;            // 从扫描/连 WiFi 重新来一遍
            _stateStartMs = now;
            _uiState = TUYA_UI_ERROR;
          }
          break;
        }
      }

      _wake = false;
      // ★ AP 模式要 10ms 一轮：WebServer/DNSServer 靠轮询，太慢手机端会转圈
      vTaskDelay(pdMS_TO_TICKS(_state == ST_AP ? 10 : 50));
    }
  }

  void _enterBackoff() {
    _wake = false;
    _state = ST_BACKOFF;
    _stateStartMs = millis();
    _uiState = TUYA_UI_ERROR;
    _backoffMs = _backoffMs ? (_backoffMs * 2) : 5000;
    if (_backoffMs > 60000UL) _backoffMs = 60000UL;
    Serial.printf("[TUYA] backoff %lums\n", _backoffMs);
  }

  // 进入 STA 连接（扫描通过 / 刚配完网两条路径共用）
  void _beginStation(unsigned long now) {
    Serial.printf("[TUYA] connecting WiFi '%s'\n", _ssid);
    WiFi.persistent(false);            // 不写 flash，省寿命也避免每次开机重连旧配置
    WiFi.setSleep(false);              // 关省电：省电模式会带来几百 ms 抖动
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("kanchuan-hb");
    WiFi.begin(_ssid, _pass);
    _state        = ST_WIFI;
    _stateStartMs = now;
    _uiState      = TUYA_UI_WIFI;
  }
};

#endif // TUYA_LINK_H
