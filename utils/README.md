# utils/

工具脚本。按用途分四类：

| 脚本 | 用途 |
|---|---|
| [`build.ps1`](#构建与烧录) | 构建固件 |
| [`flash.ps1`](#构建与烧录) | 烧录固件 |
| [`make_logo.py`](#logo) | 生成内置几何 LOGO |
| [`logo_converter.py`](#logo) | 把 PNG 转成固件点阵 |
| [`fetch_seal_font.py`](#logo) | 下载小篆字体 |
| [`serial_monitor.py`](#串口监视) | 简易串口监视 |
| `fonts/` | 第三方字体（不入 Git，见其 README） |

---

## 环境准备

```powershell
pip install -r requirements.txt   # pyserial
pip install Pillow                # 只有改 LOGO 才需要
```

---

## 构建与烧录

```powershell
.\utils\build.ps1                        # 增量构建
.\utils\build.ps1 -Clean                 # fullclean 后重建
.\utils\build.ps1 -Monitor               # 构建完进串口
.\utils\flash.ps1                        # 自动找串口 → 编译 → 烧录
.\utils\flash.ps1 -Port COM30 -Monitor   # 指定串口，烧完看输出
.\utils\flash.ps1 -Raw                   # 绕过 pio，用 esptool 手动烧
```

**为什么脚本要自己挑 pio**：本机有两个 pio，其中
`C:\.platformio\penv\Scripts\pio.exe` 缺 `intelhex`，链接阶段会报
`ModuleNotFoundError: No module named 'intelhex'`。两个脚本都会优先选不在
`penv` 下的那个，不用你手动处理。

`-Raw` 走的是 esptool 手动写 4 个分区（bootloader / partitions / boot_app0 /
firmware），这条路径实测可用，`pio upload` 抽风时拿它兜底。

---

## LOGO

完整的设计说明和字体实测结论见[根目录 README](../README.md#logo)，这里只讲操作。

### 用内置几何 LOGO

```powershell
python utils/make_logo.py        # → utils/logo_*.png
python utils/logo_converter.py utils\logo_160x128.png   # → 固件点阵
```

### 用自己的图片

```powershell
# 方式一：存到 utils/logo.png 然后直接跑
python utils/logo_converter.py

# 方式二：指定任意路径
python utils/logo_converter.py D:\我的图\brand.png
```

### 篆书版

```powershell
python utils/fetch_seal_font.py   # 先下 21.8 MB 字体（不入 Git）
python utils/make_logo.py         # 会自动多出 *_seal.png
```

> 篆书在 160×128 的屏上认不出来，只用于大幅面 / 印刷。
> 见 [utils/fonts/README.md](fonts/README.md)。

转换脚本的两个产物：

- `arduino/ESP32_C3_Radar_Tracker/LogoImage.h` —— 固件编译时自动 `#include`
- `utils/logo_preview.png` —— **屏上实际效果**，烧之前先看它

`make_logo.py` 只写 `logo_abstract*.png` / `logo_160x128*.png`，
**永远不会覆盖 `logo.png`**，你自己放的图是安全的。

---

## 串口监视

```powershell
python utils/serial_monitor.py            # 自动探测串口
python utils/serial_monitor.py COM30      # 指定串口
```

115200 baud，每秒打印一行 `T:目标数 F:帧率 RAW:原始字节 BAUD:锁定波特率`。

需要更完整的功能（图表、自动重连）时用 `pio device monitor -d "arduino\ESP32_C3_Radar_Tracker"`。

---

## 诊断

SPI 频率扫描是独立工程，不在本目录：

```powershell
pio run -d "diag\spi_speed_test" -t upload
```

会依次用 1 / 2 / 4 / 6 / 8 / 12 / 16 / 20 / 27 / 40 MHz 推整帧，每档 6 秒，
用来在真实杜邦线上找不花屏的最高频率。
