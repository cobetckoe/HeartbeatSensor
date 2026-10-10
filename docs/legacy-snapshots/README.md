# 历史快照存档 (Legacy Snapshots)

这里保存的是**已经废弃但仍有参考价值**的历史版本文件。

之所以单独复制到这里，是因为原文件原名带 `.bak` / `.v9.bak`，已被 `.gitignore` 排除。
如果直接从磁盘删除，这些内容就彻底消失了。存进这里后，它们会随 Git 一起保存，
而主目录又能保持只有「当前在维护的代码」。

> ?? 这里的文件**不参与编译**，`platformio.ini` 的 `build_src_filter` 已排除 `docs/`。
> 阅读时请按 `.txt` 对待，不要当活跃代码引用。

---

## 文件清单

| 文件 | 原始路径 | 说明 |
|---|---|---|
| `C3_Radar_Tracker_v9_before_softspi.ino.txt` | `arduino/ESP32_C3_Radar_Tracker/ESP32_C3_Radar_Tracker.ino.bak` | 改用硬件 SPI **之前**的固件（软件位操作 SPI）。当时帧率极低（~3 fps）。 |
| `C3_Radar_Tracker_softspi.ino.txt` | `...ESP32_C3_Radar_Tracker.ino.softspi.bak` | 软 SPI 调优中间态。 |
| `C3_RadarDisplay_softspi.h.txt` | `arduino/ESP32_C3_Radar_Tracker/RadarDisplay.h.softspi.bak` | 配套的渲染器旧版（10 KB）。 |
| `v9_main.cpp.txt` | `src/main.cpp.v9.bak` | **废弃的 v9 分块渲染固件**。它用多次 `setAddrWindow` + 32 像素小块推送，是当年**花屏的根因**。 |
| `v9_RadarDisplay.h.txt` | `include/RadarDisplay.h.bak` | v9 渲染器初版。 |
| `v9_RadarDisplay_alt.h.txt` | `include/RadarDisplay.h.v9.bak` | v9 渲染器另一分支（22 KB）。 |
| `logo_converter_v1.py.txt` | `utils/logo_converter.py.bak` | LOGO 转换脚本初版。 |
| `make_logo_v2.py.txt` | `utils/make_logo.py.v2.bak` | LOGO 生成器 v2（旧配色：深色金）。 |
| `make_logo_v3.py.txt` | `utils/make_logo.py.v3.bak` | LOGO 生成器 v3（改用几何抽象造型）。 |
| `make_logo_v4.py.txt` | `utils/make_logo.py.v4.bak` | LOGO 生成器 v4（加小篆字体支持）。 |

---

## ? 请勿复用 v9 的分块渲染写法

`v9_main.cpp.txt` 与两个 `v9_RadarDisplay*.h.txt` 里的渲染思路是**已验证的错误做法**：

```cpp
// ? 花屏根因：多次开窗 + 小块推送
_d->setAddrWindow(x, y, w, h);
_d->pushPixels(buf, 32);
```

正确做法只有一个出口 —— 全屏单窗口、单次连续推送，现在的
`RadarDisplay.h::pushCanvas()` 就是这么写的：

```cpp
// ? 唯一允许的推送路径
_d->startWrite();
_d->setAddrWindow(0, 0, SCREEN_W, SCREEN_H);
_d->writePixels(_cv.getBuffer(), (uint32_t)SCREEN_W * SCREEN_H);
_d->endWrite();
```

历史上每一类「花屏 / 斜纹 / 错位 / 撕裂」问题，最终定位到的都是偏离这条路径。

---

## 主目录已清理的内容

以下目录/文件已从工程根部删除（内容均已保存在本目录或 Git 历史提交 `3089a2f` 中）：

| 已删除 | 原因 |
|---|---|
| `src/` | v9 废弃固件，与维护中的 sketch 冲突 |
| `include/` | 仅被 v9 的 `src/main.cpp` 引用 |
| 根 `platformio.ini` | 指向废弃的 `src/main.cpp`，且 `default_envs = esp32s3`，会误导构建 |
| `arduino/ESP32_Radar_Tracker/` | ESP32-S3 旧版，已不再维护 |
| `circuitpython/` | 11 个历史实验脚本，文档还在讲 OLED/I2C，与实际 ST7735 SPI 硬件不符 |

当前**唯一在维护**的固件是 → `arduino/ESP32_C3_Radar_Tracker/`
