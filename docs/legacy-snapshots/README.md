# 历史快照存档

已废弃但保留参考的历史版本，随 Git 保存，**不参与编译**（构建只以 `src/master/`
为源码根，本目录根本不在编译路径上）。

> 文件名带 `.txt` 后缀，是为了让编辑器和 PlatformIO 都不把它们当成有效源码。
> **不要改名，也不要拿去编译。**

## 文件清单

| 文件 | 版本代号 | 说明 |
|---|---|---|
| `C3_Radar_Tracker_v9_before_softspi.ino.txt` | **Military Heartbeat v6** | C3 全屏雷达 + 底部 ECG 波形 + NATO 磷光绿配色。名字里的 `before_softspi` 是当时临时命名，它其实是**硬件 SPI** 实现，与软 SPI 无关 |
| `C3_RadarDisplay_softspi.h.txt` | **Military Tactical v7** | v6 的渲染器升级版：声呐式 ping 圆环替掉旋转扫描线，`GFXcanvas16` 全帧双缓冲。同样是**硬件 SPI** 实现 |
| `v9_main.cpp.txt` | **Semicircle Sweep v8** | S3/C3 统一版主程序，含 S3 引脚映射；往复扫描束、按行增量刷新 |
| `v9_RadarDisplay.h.txt` | **v9 初版** | 半圆雷达扫描渲染器，静态层只画一次，每帧只重绘扫描束 / 目标 / ECG 亮点 |
| `v9_RadarDisplay_alt.h.txt` | **v9 另一分支** | 同上思路的并行试验版本，未采用 |
| `logo_converter_v1.py.txt` | LOGO 脚本 v1 | PNG → 点阵转换初版 |
| `make_logo_v2.py.txt` | LOGO 生成器 v2 | |
| `make_logo_v3.py.txt` | LOGO 生成器 v3 | |
| `make_logo_v4.py.txt` | LOGO 生成器 v4 | 现行 `utils/make_logo.py` 的前身 |

### 为什么不用 v9 那套写法

`v9_main.cpp.txt` 与两个 `v9_RadarDisplay*.h.txt` 都是**多次 `setAddrWindow` +
32 像素小块推送**。现行渲染器已改成「整帧合成到离屏画布 → 每个脏矩形只做一次
`setAddrWindow` → 连续 `writePixels`」，块数从上百次降到个位数，这才是现在能跑到
~50 FPS 的原因。**不要复用 v9 的分块推送写法。**

### 版本演进（为什么会有这么多份）

| 代号 | 关键变化 |
|---|---|
| v6 | 全屏雷达（R=96）+ 底部 ECG 波形 |
| v7 | 声呐 ping 圆环取代旋转扫描线 |
| v8 | 统一 S3/C3，往复扫描束，按行增量刷新 |
| v9 | 半圆雷达 + 静态层/动态层分离，脏矩形推送 |
| **现行** | 去掉 ECG 波形与旋转扫描线，改 4 防区状态条；HW SPI 20 MHz；门控分级放行 |

> **ECG 波形和旋转扫描线在现行版本里已彻底删除**，相关的 10 个颜色常量也一并清掉了
> （数值留档在 `RadarDisplay.h` 的「已删除」注释块里）。日后若要恢复，算法可以从
> 这些快照里找，但推送方式不要照抄。

## 已删除的历史内容

以下已从工程根部删除，内容保存在本目录或 Git 提交 `3089a2f`：

| 已删除 | 原因 |
|---|---|
| `src/` | v9 废弃固件，与维护中的 sketch 冲突 |
| `include/` | 仅被 v9 的 `src/main.cpp` 引用 |
| 根 `platformio.ini` | 指向废弃的 `src/main.cpp`，`default_envs` 会误导构建 |
| `arduino/ESP32_Radar_Tracker/` | ESP32-S3 旧版 |
| `circuitpython/` | 历史实验脚本，与实际 ST7735 SPI 硬件不符 |
| `C3_Radar_Tracker_softspi.ino.txt` | 与 `…_v9_before_softspi.ino.txt` **字节完全相同**（9590 B，同 MD5），纯重复 |

当前维护的固件：`src/master/`（主机 ESP32-C3）+ `src/slave/`（从机 STC8G）。
