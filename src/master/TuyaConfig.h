/*
 * TuyaConfig.h — 涂鸦接入【唯一需要你手动改的文件】
 * ============================================================================
 * ★ 只改这一个文件，其它代码都不要动。
 *
 * 第一步：在 https://iot.tuya.com 创建产品
 *   1) 产品 → 创建产品 → 选「传感/安防」类目（自定义品类也可以）
 *   2) Product Development → 选 TuyaLink（不是 TuyaOS / MCU SDK）
 *   3) Function Definition（功能定义）里按下面「DP 编码表」新建功能点，
 *      编码(code)必须【逐字一致】，多一个下划线都会导致云端不认这个数据点
 *   4) Device Development → Device Debugging →
 *      「Activation & Verification」拿到 ProductID / DeviceID / DeviceSecret
 *   5) 点「Get 2 Free Licenses」领免费授权，否则设备连上去也会被踢
 *
 * 第二步：填设备三元组（就在下面）
 *
 * 第三步：编译烧录。WiFi 不用在这里写 —— 开机后【长按 BOOT 键 2 秒】
 *         打开 WiFi，第一次会自动开热点 Kanchuan-HB-XXXX，
 *         手机连上去填 WiFi 名和密码即可（详见下面第 1 节）。
 * ============================================================================
 *
 * Copyright (C) 2026 Kanchuan Industrial
 * SPDX-License-Identifier: MIT
 */
#ifndef TUYA_CONFIG_H
#define TUYA_CONFIG_H

// ─────────────────────────────────────────────────────────────
//  1. WiFi —— 用手机配网（AP 热点），不用在这里写死
// ─────────────────────────────────────────────────────────────
//  ★ 常规方案（和涂鸦 / 米家 一样）：
//      长按 BOOT 键 2 秒 → 打开 WiFi
//        · NVS 里存过 WiFi 且【扫描得到】该热点  → 直接连
//        · 没存过 / 扫描不到 / 连续失败 3 次      → 自己开一个 AP 热点
//      热点名：Kanchuan-HB-XXXX（XXXX = MAC 后 4 位）
//      热点默认【开放无密码】，手机点一下就直连（见下面 TUYA_AP_OPEN）
//      手机连上热点后浏览器会自动弹出配置页（强制门户 captive portal），
//      没弹出就手动打开 http://192.168.4.1
//      选 WiFi → 填密码 → 保存 → 热点关闭，设备转为直连，凭据写进 NVS
//
//  下面的 TUYA_WIFI_SSID / TUYA_WIFI_PASS 只在「NVS 为空」时当作
//  出厂默认值使用。留空（""）表示必须走手机配网。
//  ⚠️ ESP32-C3 只支持 2.4GHz。5GHz 热点连不上，且失败原因不会明确报出来。
#define TUYA_WIFI_SSID      ""     // 出厂默认 WiFi 名（可留空，推荐留空）
#define TUYA_WIFI_PASS      ""     // 出厂默认 WiFi 密码（可留空）

//  AP 配网热点（手机连这个）
#define TUYA_AP_SSID_PREFIX "Kanchuan-HB-"   // 热点名 = 前缀 + MAC 后 4 位
//  ★ 配网热点默认【开放、无密码】：手机在 WiFi 列表里点一下就直连，
//    和涂鸦 / 米家 / 各家路由器的配网热点行为一致。
//    理由：这台设备本来就要让陌生手机连上来填密码，"给热点再设个密码"
//    只会多一道门槛（用户还得先知道密码），却挡不住真正的攻击者
//    （它对 2 米内的手机就是敞开的）。
//    真正的防护是【时间窗口】：AP 只在需要配网时开，
//    已有凭据的情况下 5 分钟没人动就自动关（见 TUYA_AP_TIMEOUT_MS）。
//    要改成一机一密（每台密码不同，可印在机身标签上）就设成 0 并改 TUYA_AP_PASS。
#define TUYA_AP_OPEN        1                // 1 = 开放热点（推荐）/ 0 = 用下面密码
#define TUYA_AP_PASS        "kanchuan123"    // 仅在 TUYA_AP_OPEN = 0 时生效，必须 ≥ 8 位
#define TUYA_AP_TIMEOUT_MS  300000UL         // 已有凭据但连不上时，AP 开 5 分钟 → 自动关闭重试
#define TUYA_CONN_RETRY     3                // 直连连续失败 N 次 → 转 AP 重新配网

//  ★ 开机是否一律从「WiFi 关闭」开始
//    1 = 是（默认）。避免开机即占射频、避免误连、省电，需要时手动长按 2s 开
//    0 = 记住上次状态，断电重启自动恢复 —— 长期部署当智能家居用时改成 0
//  ★ 例外（不用改这里）：在配网页上成功保存过 WiFi / 三元组的设备，
//    会写一个一次性标记，重启后【自动开启 WiFi】并直接联网，把状态交给用户。
//    标记用一次就清掉，所以之后断电重启仍然回到「WiFi 关闭」。
#define TUYA_BOOT_WIFI_OFF  1

//  ── 扫描（找自家路由）策略 ──
//  ★ 这一组是现场反馈「手动重启后还没搜到就跳进二维码配网」的根治所在。
//    根因：刚上电/刚切 WIFI_STA 时 esp_wifi 会以 ESP_ERR_WIFI_STATE 直接拒绝
//    扫描请求，Arduino 的 scanNetworks() 于是【立刻】返回 WIFI_SCAN_FAILED
//    （不是 6 秒后，是 0 毫秒后）。原代码不看返回值，把「调用被拒」当成
//    「附近没有这个 WiFi」，于是瞬间就开了热点 —— 用户根本没看到搜索过程。
//  ★ 现在的区分：
//      · 扫描本身失败 / 扫到 0 个 AP  → 判为「抖动」，重试，最多 TUYA_SCAN_RETRY 次
//      · 扫到好几个 AP 但没有目标     → 判为「他家路由器确实不在」，但再复核
//                                      TUYA_SCAN_ABSENT_TRIES 次才开热点
//    只有后一种才是真结论，其余都先重试。
#define TUYA_SCAN_CHAN_MS       300          // 每信道最长驻留（ms），也决定单次扫描预算
#define TUYA_SCAN_SETTLE_MS     150          // 切到 WIFI_STA 后先等这么久再扫
#define TUYA_SCAN_RETRY_MS      1200UL       // 两次扫描之间的间隔
#define TUYA_SCAN_RETRY         4            // 扫描「本身失败/被拒/0 个AP」最多试几次（含首次）
#define TUYA_SCAN_ABSENT_TRIES  2            // 「扫到 AP 但没目标」要复核几次才开热点
#define TUYA_SCAN_MAX_TRIES     6            // 一切重试的总上限（务必 ≥ RETRY + ABSENT）
#define TUYA_SCAN_HARD_MS       12000UL      // 单次扫描硬超时（库内部判定之外的自保）

//  ★ 配网保存成功后【主动重启】
//    为什么不「关热点 → 原地切 STA」：AP 刚停、射频刚切模式、WebServer 刚 stop，
//    紧接着 WiFi.begin() 很容易停在「已关联但拿不到 IP」的半死状态，现场表现
//    就是「点了保存、提示已保存，但设备再也没上线」。重启是干净状态，最可靠
//    （涂鸦 / 米家 / ESPHome 的配网也都是配完就重启）。
//    这个延时是为了让 200 回包真的发到手机上，否则用户只看到「连接被重置」，
//    会以为没保存成功，又去点一次。
#define TUYA_PROV_RESTART_MS    900UL

//  ★ 配网二维码（进 AP 配网模式时，屏幕上会整屏画一个入网二维码）
//    内容是标准 WiFi 入网码："WIFI:T:nopass;S:Kanchuan-HB-xxxx;;"
//    用【手机系统相机】扫 → 系统直接弹「加入网络」，一点就进热点，全程不用手输。
//
//  ★ 为什么【不】提供「网址码」（http://192.168.4.1）这一版：
//    微信扫一扫虽然能打开网址，但扫码那一刻手机还挂在自家路由器上，
//    192.168.4.1 根本不是它所在网段的网关，这个地址根本打不开 ——
//    用户得先手动去 WLAN 列表里连上热点再扫，反而比相机扫多一步。
//    所以只保留「相机扫 → 一键入网」这一条路，屏上也有文字提示用相机。

// ─────────────────────────────────────────────────────────────
//  2. 设备三元组（IoT 平台 Activation & Verification 页面）
// ─────────────────────────────────────────────────────────────
//  ★★★ 现在这两个值【已经可以从 AP 配网页修改】，不必重新编译 ★★★
//    长按 BOOT 2s 开 WiFi → 手机连热点 Kanchuan-HB-XXXX → 打开配置页 →
//    展开「涂鸦三元组」→ 填 DeviceID / DeviceSecret → 保存。
//    保存后写入 NVS（Preferences 命名空间 "tuya"），下次上电直接生效。
//    ★ 优先级：NVS 里有值就用 NVS，NVS 为空才用下面这两个编译期默认值。
//    ★ 长按 BOOT 5s 工厂复位会连同三元组一起清掉，回到下面的默认值。
//
//  ★ 下面填的是【当前这台设备】的真实三元组，出厂固件因此开箱即用。
//    如果要把带密钥的源码提交到公开仓库，把这两个字符串清成 "" 即可 ——
//    设备端仍可通过配网页写入，功能不受影响。
//
//  ★ 三者缺一不可，且必须是【同一台设备】的三元组：
//      ProductID   = 产品级（同一个产品下所有设备共用）
//                  ⚠️ 本固件的 MQTT 握手【不需要 ProductID】，它只是留档 +
//                     上报日志用；真正参与签名的是 DeviceID + DeviceSecret。
//      DeviceID    = 设备级
//      DeviceSecret= 设备级
//    ★ 最常见的错：把【产品的 Access Secret】当成 DeviceSecret 填进来 ——
//      那是给云端 REST API 用的，设备端用它算出来的 HMAC 一定被拒
//      （MQTT 返回 4 = bad user name or password），而且日志不会说是哪个错了。
#define TUYA_PRODUCT_ID     "YOUR_PRODUCT_ID"
#define TUYA_DEVICE_ID      "YOUR_DEVICE_ID"
#define TUYA_DEVICE_SECRET  "YOUR_DEVICE_SECRET"

// ─────────────────────────────────────────────────────────────
//  3. 数据中心 MQTT 端点
// ─────────────────────────────────────────────────────────────
//  你在 iot.tuya.com 注册时选的是哪个数据中心，就必须连哪个 —— 连错了
//  表现是 MQTT 一直 CONNECTION_REFUSED / 直接断，且日志里没有有用信息。
//    中国          m1.tuyacn.com
//    中欧          m1.tuyaeu.com
//    西欧          m1-weaz.tuyaeu.com
//    美西          m1.tuyaus.com
//    美东          m1-ueaz.tuyaus.com
//    印度          m1.tuyain.com
//    新加坡        m1-sg.iotbing.com
#define TUYA_MQTT_HOST      "m1.tuyacn.com"
#define TUYA_MQTT_PORT      8883          // 8883 = TLS，不要用 1883

// ─────────────────────────────────────────────────────────────
//  4. 时间戳单位 —— 认证签名里 timestamp 用秒还是毫秒
// ─────────────────────────────────────────────────────────────
//  ★ 这是唯一一个官方文档自相矛盾的地方：
//      · 文档正文的认证表格写「10-digit current timestamp」→ 秒
//      · 官方 Java demo（可跑通的参考实现）用的是 System.currentTimeMillis()
//        → 13 位毫秒
//  默认按【秒】走（与文档表格一致）。若串口一直报
//      [TUYA] mqtt connect failed rc=-4 / rc=4  （= 用户名或密码错）
//  而 WiFi 与时间都正常，就把下面这行改成 1 再烧一次。
//  两种都能连通的情况是存在的，所以做成编译期开关，不要手工改代码。
#define TUYA_TS_UNIT_MS     0

// ─────────────────────────────────────────────────────────────
//  5. DP 编码表 —— 必须与平台「功能定义」里的 code 逐字一致
// ─────────────────────────────────────────────────────────────
//  约定：上位机 / APP 看到的是这些 bool 量。
//  若你在平台上用了别的 code，只改右边的字符串即可。
//
//  功能点类型统一选「布尔型(bool)」，数据传输类型统一选【只上报 ro】。
//
//  ★★ 已删除 wifi_enable（原「读写」DP）——原因是它【在物理上无法自洽】：
//     · WiFi 关着的时候设备本来就离线，APP 的指令根本发不到设备；
//     · 能用的时候只剩「把 WiFi 关掉」这一个动作，等于自己把自己踢下线，
//       想再打开还得跑到设备旁边长按 BOOT —— 比不用 APP 更麻烦。
//     → 所以整台设备【没有任何可下发 DP】，平台侧不必建 rw 点，
//       固件也不再 subscribe .../property/set（见 TuyaLink::begin）。
//     真正需要联网时：长按 BOOT 2s 开/关 WiFi，长按 5s 清凭据重新配网。
//
//  ★★ 已删除 zone_a/b/c/d_online 四个 DP（原「防区链路在线」）——
//     原因：平台上根本没有建这四个点。
//     这是最坏的一种失败模式：云端对未定义的 DP **不报错、静默丢弃**，
//     串口里 publish 返回 true（MQTT 层成功了），APP 上却什么都不显示，
//     现场完全无法从日志判断「是没发出去还是平台没这个点」。
//     → 固件上报的 DP 集合必须与平台功能定义【逐字、逐个数】一致。
//
//  ★★★ 本设备上报的 DP 必须严格等于平台上的这 7 个点，一个不多一个不少：
//
//     ID    code                类型    取值       间距  倍数  单位
//     101   radar_distance      value   0 ~ 800     1    0    cm
//     102   radar_angle         value   -90 ~ 90    1    0    °
//     103   radar_targets       value   0 ~ 3       2    0    tgt
//     104   zone_a_presence     bool    true/false  —    —    —
//     105   zone_b_presence     bool    true/false  —    —    —
//     106   zone_c_presence     bool    true/false  —    —    —
//     107   zone_d_presence     bool    true/false  —    —    —
//
//   ★ 「防区离线」在 APP 上不再单独可见 —— 这是有意的取舍：
//     防区掉线时 presence 会保持 false（zoneOnline=false 时 occupied 也是 false），
//     APP 看到的是「该防区无人」，与真实「无人」不可区分。
//     要区分请看【主机屏幕】：屏幕上的防区条有三态（离线灰显 / 无人 / 有人），
//     本机现场排查以屏幕为准，云端只做「有人/无人」的粗粒度呈现。
//
//   ⚠️ radar_targets 的「间距」平台上填的是 2，而固件按 1 递增上报 0/1/2/3。
//      间距只约束【APP 侧手动下发】的取值范围，本 DP 是只读上报，
//      云平台不会校验上报值是否落在间距栅格上，所以 0 和 1 都能正常显示。
//      但若以后在 APP 上做「按间距取整」的图表，会出现 1 被抹成 0/2 的错觉。
#define TUYA_DP_PRES_A      "zone_a_presence"
#define TUYA_DP_PRES_B      "zone_b_presence"
#define TUYA_DP_PRES_C      "zone_c_presence"
#define TUYA_DP_PRES_D      "zone_d_presence"

// ── 雷达实时数据 DP（数值型 / value）──
//   ★ 之前完全没推送雷达信息，根因是主机侧只把雷达目标喂给了屏幕
//     （radarDisplay.updateFrame），TuyaLink 里只有 4 个 bool DP，
//     雷达数据根本没有出口。下面这三个就是出口。
//
//   ★ 平台上必须建成「数值型(value)」，不能建成 bool —— 类型不匹配时
//     云端会静默丢弃该 DP，APP 上表现为「一直没有数据」，很难查。
//     每个 DP 的「取值范围 / 间距 / 倍数 / 单位」必须与下表一致，
//     否则云端校验不过（rc 不会报错，只是数据不显示）：
//
//       code                取值范围   间距  倍数  单位   含义
//       radar_distance      0 ~ 800    1     0     cm     最近目标距离（厘米）
//       radar_angle         -90 ~ 90   1     0     °      最近目标角度
//       radar_targets       0 ~ 3      2     0     tgt    当前目标个数
//
//   ★★ 「倍数」必须填 0 —— 这是现场踩过的坑，别再改回 1：
//     平台这一栏名字叫「倍数」，但接口字段 scale 的真实语义是【小数位数】，
//     不是乘数：
//       倍数 0 → scale=0 → 值原样显示          （整数）
//       倍数 1 → scale=1 → 值被除以 10 后显示   （一位小数）
//       倍数 2 → scale=2 → 值被除以 100 后显示  （两位小数）
//     之前本文件/文档写的是「填 1」，结果设备发 180，APP 显示 18.0 ——
//     数据整整小了 10 倍。本固件三个点都是整数，所以平台上一律填 0。
//
//   ★ 为什么距离用【厘米】而不是米或浮点：
//     TuyaLink 的 JSON 是自己拼的（不引 JSON 库，省 flash），
//     拼浮点要调 dtostrf，容易在 512 字节缓冲里算错长度。
//     统一「整数上报」既省代码又绕开浮点格式化。
//     单位填 cm，设备侧发整数 180，APP 上就显示 "180cm"（= 1.80 m）。
//     量程 600cm（MAX_RANGE_M = 6.0m），DP 上限留到 800cm 有余量。
//
//   ★ 想少建几个 DP：把下面 TUYA_RADAR_SINGLE_DP 置 1，
//     就只上报 radar_targets（有无人/个数），不发距离和角度。
#define TUYA_RADAR_SINGLE_DP 0

#define TUYA_DP_DIST        "radar_distance"
#define TUYA_DP_ANGLE       "radar_angle"
#define TUYA_DP_TGT         "radar_targets"

// ── 雷达 DP 的上报节流 ──
//   ★ 雷达是 50Hz 刷新、目标距离一直在小幅抖动。如果每帧变化都上报，
//     MQTT 会被打爆（云端有频控，超了直接限流/断连）。
//   ★ 2000ms 的含义：最快 2 秒上报一次雷达值。
//     涂鸦单设备的下行/上行频控大致在 1 条/秒量级，0.5 条/秒留了一倍余量；
//     而 2s 的刷新粒度对「看人在哪走动」这个用途完全够用
//     （原来设 3000ms，APP 上会觉得数字半天不动，偏迟钝）。
//   ★ 变化阈值：距离差 < 30cm、角度差 < 5°、个数不变 → 认为没变化，
//     不上报。避免目标站着不动时的「距离 180→179→181」抖动刷屏。
//     30cm 与原 0.3m 完全等价，换单位不改行为。
#define TUYA_RADAR_MIN_MS    2000UL
#define TUYA_RADAR_DIST_EPS  30       // cm 单位 → 30 = 0.30m
#define TUYA_RADAR_ANGLE_EPS 5        // 度

// ─────────────────────────────────────────────────────────────
//  6. 运行时参数（一般不用改）
// ─────────────────────────────────────────────────────────────
#define TUYA_KEEPALIVE_S      60      // 云端 1.5×90=150s 收不到包就判离线
#define TUYA_FULL_REPORT_MS   60000UL // 定期全量上报（防「云端状态漂移」）
#define TUYA_WIFI_TIMEOUT_MS  20000UL // WiFi 关联超时
#define TUYA_TIME_TIMEOUT_MS  20000UL // NTP 对时超时
#define TUYA_TASK_STACK      16384    // TLS 握手要在【本任务栈】上跑 mbedtls，
                                      // 12K 有过栈溢出风险，16K 稳（RAM 富余）

// ── tuya 任务优先级 ──
//   ★ 必须保持 1，【不能调低】。这是一次排查的结论，写下来免得再试：
//     ESP32-C3 是【单核】。Arduino 的 loopTask 优先级就是 1，且它内部
//     没有任何 vTaskDelay —— 一旦 loop() 返回就立刻再进 loop()，永远不主动
//     阻塞。FreeRTOS 的规则是「就绪的高优先级任务不被低优先级任务抢占」，
//     所以把 tuya 任务设成 0（等于 idle 优先级）的后果是：只要 loop() 在跑，
//     tuya 任务就【一次都调度不到】—— WiFi 连不上、MQTT 永远握手，设备直接废掉。
//     即使之后在 loop() 末尾补 vTaskDelay(1)，tuya 也只能拿到 ~25% CPU，
//     一次 TLS 握手会被拖到十几秒，照样超时失败。
//     结论：单核上「低于 loopTask」这条路走不通，保持同优先级（1）是对的。
//   ★ 那么「联网后偶然卡顿」怎么解？
//     真正的元凶不是这里，是【推屏门控被 WiFi 干扰卡死】—— 见 master.ino 里
//     DEFER_SOFT_MS / DEFER_HARD_MS 那一段的注释。TLS 握手期间屏幕只是
//     帧率暂时减半（约 25FPS，肉眼几乎不可见），不是卡顿。
#define TUYA_TASK_PRIO       1

// ── MQTT socket 超时（秒）──
//   ★ 原值 10 秒：一旦网络半死（服务器不回包但 TCP 不断），
//     PubSubClient 会在 publish/loop 里阻塞满 10 秒。虽然它在 tuya 任务里，
//     不直接卡主循环，但会让任务长时间不睡觉、持续抢 CPU，也拖慢重连。
//     3 秒足够宽带/手机热点的正常往返（实测 RTT < 300ms）。
#define TUYA_SOCKET_TIMEOUT_S 3

#endif // TUYA_CONFIG_H
