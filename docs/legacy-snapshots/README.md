# 历史快照存档

已废弃但保留参考的历史版本，随 Git 保存，不参与编译（`platformio.ini` 的 `build_src_filter` 已排除 `docs/`）。

## 文件清单

| 文件 | 说明 |
|---|---|
| `C3_Radar_Tracker_v9_before_softspi.ino.txt` | 改硬件 SPI 之前的固件（软件位操作 SPI） |
| `C3_Radar_Tracker_softspi.ino.txt` | 软 SPI 调优中间态 |
| `C3_RadarDisplay_softspi.h.txt` | 配套渲染器旧版 |
| `v9_main.cpp.txt` | v9 分块渲染固件 |
| `v9_RadarDisplay.h.txt` | v9 渲染器初版 |
| `v9_RadarDisplay_alt.h.txt` | v9 渲染器另一分支 |
| `logo_converter_v1.py.txt` | LOGO 转换脚本初版 |
| `make_logo_v2.py.txt` | LOGO 生成器 v2 |
| `make_logo_v3.py.txt` | LOGO 生成器 v3 |
| `make_logo_v4.py.txt` | LOGO 生成器 v4 |

`v9_main.cpp.txt` 与两个 `v9_RadarDisplay*.h.txt` 用多次 `setAddrWindow` + 32 像素小块推送，不要复用这套写法。

## 已删除的历史内容

以下已从工程根部删除，内容保存在本目录或 Git 提交 `3089a2f`：

| 已删除 | 原因 |
|---|---|
| `src/` | v9 废弃固件，与维护中的 sketch 冲突 |
| `include/` | 仅被 v9 的 `src/main.cpp` 引用 |
| 根 `platformio.ini` | 指向废弃的 `src/main.cpp`，`default_envs` 会误导构建 |
| `arduino/ESP32_Radar_Tracker/` | ESP32-S3 旧版 |
| `circuitpython/` | 历史实验脚本，与实际 ST7735 SPI 硬件不符 |

当前维护的固件：`src/master/`（主机 ESP32-C3）+ `src/slave/`（从机 STC8G）。
