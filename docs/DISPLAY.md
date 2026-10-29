# 显示实现要点

屏幕是 ST7735 1.8" 160×128，横屏（`setRotation(1)`）。本文只讲**渲染**，
雷达算法见根 README，日志判读见 [SERIAL-LOG.md](SERIAL-LOG.md)。

---

## 1. SPI 初始化

必须在 `display.initR()` **之前**显式调用引脚版 `SPI.begin()`：

```cpp
SPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);   // SCLK=GPIO6, MOSI=GPIO7, 不读
display.initR(INITR_BLACKTAB);
display.setRotation(1);
display.setSPISpeed(TFT_SPI_HZ);
```

**任何地方都不要再调无参 `SPI.begin()`**。C3 变体文件里的默认 SCK 是旧的
GPIO4，一旦被调起会把 SCLK 从 GPIO6 挪走 → 花屏。

MISO 传 `-1` 会触发 Arduino HAL 的 `spiAttachMISO(-1)` 并打一条
`log_e("SPI Does not have default pins on ESP32C3!")`。本屏是只写设备，
这条提示无害，但成品开机日志里出现 `[E]` 会让现场误以为故障。
压制方式是编译期参数（`platformio.ini` 的 `-DCORE_DEBUG_LEVEL=0`），
**不是** `esp_log_level_set("spi", ...)` —— 因为 `ARDUHAL_LOG_FORMAT` 不经过
IDF 日志系统。

---

## 2. 推帧规则

整帧先合成到 `GFXcanvas16` 离屏缓冲，再一次性推上屏，**不在屏上直接画**。

| 规则 | 为什么 |
|---|---|
| 脏矩形起始列必须为偶数（`if (r0 & 1) r0--;`） | RISC-V 非对齐访存会 panic（`LoadStoreAlignment`） |
| 每块脏矩形只做一次 `setAddrWindow`，连续 `writePixels` 推完 | 每次 `setAddrWindow` 都有固定开销，碎块推送会成倍拖慢 |
| 单帧脏像素预算默认 3000（`setPushBudget(3000)`） | 连续 SPI 会让 433 MHz 超再生接收机致盲，见第 3 节 |
| 超预算的段留到下一帧继续推 | 保证单次推屏时长上界，从而保证安静窗存在 |
| 背景静态部分开机预烘焙成 `_bg`，每帧一次 `memcpy` | 实测 912 µs/帧 → ~60 µs/帧 |
| 脏矩形按「真实变化像素数」计预算，**不是矩形面积** | 面积口径会把细长条（波纹横跨整行）算成巨额，预算瞬间耗尽 |

`GFXcanvas16` 需要 **40 KB 连续堆**。所以稳态运行中**绝不能有堆分配**，
否则碎片化之后下次 `begin()` 就拿不到连续块。`dbgLog()` 用 224 字节栈缓冲，
也是这个原因。

---

## 3. 分级强制放行（这是「流畅性」的核心机制）

**物理事实：连续 SPI 会致盲 433 MHz 超再生接收机。** 推屏时接收机收不到任何
数据，帧直接被吃掉。所以推屏必须落在「帧间隙」里，而帧间隙只有 20~59 ms。

门控条件（`master.ino` 主循环）：

```cpp
rxBusy = ookReceiver.inFrame() || ookReceiver.runSegs() > 0;
pushOk = ookReceiver.pushSafe();
canPush = !rxBusy && pushOk;
```

`pushSafe()` 三条判据（**顺序不能改**）：

1. `_inFrame || _head != 0` → 绝对否决（帧内一律不许推）
2. 距上次成功解帧 < `OOK_REPEAT_WIN_MS`（900 ms）→ 放行（同一突发的重复帧）
3. 距上一个边沿 ≥ `OOK_GAP_SAFE_US`（10 ms）→ 放行（确实在两帧之间）

**没有强制放行会怎样**：WiFi 打开后 2.4 GHz 干扰让 433 接收模块持续自激，
安静窗永不成立 → 屏幕直接冻结。所以有三级阶梯：

| 等待时长 | 动作 | 预算 |
|---|---|---|
| 正常 | 等门控自然成立 | 基线（初始 3000） |
| > `DEFER_SOFT_MS`（120 ms） | 软强制 | 1200 px |
| > `DEFER_HARD_MS`（400 ms），**帧内也强制** | 硬强制 | 600 px |

预算基线自适应，形成负反馈：正常推 +20/帧（上限 3000），硬强制 −300（下限 1500）。
这样既不需要现场调参，也不会因为长期硬强制而把预算压死。

强制放行的次数每分钟打印在 `[GATE] fs= fh=` 行，判读见
[SERIAL-LOG.md](SERIAL-LOG.md#gate)。

---

## 4. SPI 时钟

固定 20 MHz（`TFT_SPI_HZ = 20000000UL`）。

| SPI 时钟 | 整屏推送 | 说明 |
|---|---|---|
| 40 MHz | 11.3 ms | **不要用**：谐波直接干扰 433 MHz 接收 |
| **20 MHz** | **19.4 ms** | 当前值，速度与抗干扰的平衡点 |
| 16 MHz | 24.2 ms | 更保险，但对帧率无实质影响 |
| 8 MHz | 48.5 ms | 太慢，一张全屏刷新就吃掉一个帧间隙 |

---

## 5. 配色分级

纯黑底 + 亮绿 + 纯白。「余光扫一眼」的设备，**结构元素必须比内容暗一档**，
否则强光下满屏同亮，反而看不出目标在哪。

| 亮度 | 用途 |
|---|---|
| 纯白 `0xFFFF` | 中心十字 / HUD 文字 / 分隔线（最强对比） |
| 满绿 `0x07E0` | 6 M 边界 / 方位刻度数字 |
| 亮绿 `0x05E0` | 2 M / 4 M 刻度圈 |
| 中绿 `0x03E0` | 方位网格线 |
| Cyan / Amber / Red | 三个目标的光点（按航迹 ID 分配，颜色稳定不跳） |

> 曾经有 ECG 波形和旋转扫描线两个视觉元素，v4 改版时被「声呐波纹式 ping
> 圆环」取代，相关的 10 个颜色常量已删除。数值记在 `RadarDisplay.h` 的注释里，
> 以后要恢复可直接照抄。

---

## 6. 版面几何

```
RADAR_CX 80     RADAR_CY 96     圆心（半圆，圆心在底线中点）
RADAR_R  96                     半径
MAX_RANGE_M 6.0                 量程
RING_2M 32   RING_4M 64   RING_6M 96
TOP_Y 104    H 22   SLOT_W 40   底部防区条
```

**角度约定（已验证，不要改）**：`trigTbl()` 建表时 `t[i] = cos((deg-90)°)`、
`t[91+i] = sin((deg-90)°)`，`deg = -90 + 2*i`。于是：

- `deg = -90` → 屏幕左水平方向
- `deg = 0` → 屏幕正上方

`drawGrid()` 的循环是 `for (a = -90; a <= 90; a += 30)`，但带 `if (y2 < RADAR_CY)`
守卫，所以屏幕上**只画出 -90 / -60 / -30 / 0** 四条方位线。

> **Adafruit GFX 经典 5×7 字模只有 ASCII（0x20–0x7F），没有 `°` 字形。**
> 度数符号是用 `drawCircle(x, y, 1)` 画一个小圆点表示的，不要去改字模。
