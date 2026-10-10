# ESP32 Radar Tracker Utilities

Python utility scripts for working with the ESP32 Radar Tracker.

## Requirements

```bash
pip install pyserial
pip install Pillow
```

## 开机 LOGO（可用自己的图片替换）

### 用自己的图片替换 LOGO（推荐）

```bash
# 1) 把图片放到 utils/ 下，命名 logo.png（也认 .jpg/.jpeg/.bmp）
# 2) 转换
python utils/logo_converter.py
# 3) 先看预览确认效果，再编译烧录
#    utils/logo_preview.png  ← 屏上实际效果
pio run -d "arduino\ESP32_C3_Radar_Tracker"
```

也可以直接指定任意路径的图：

```bash
python utils/logo_converter.py D:\我的图\brand.png
```

图片会自动等比缩放到不超过 160×128，带透明通道的 PNG 会自动合成底色。
产物是 `arduino/ESP32_C3_Radar_Tracker/LogoImage.h`，编译时自动启用。

### 用内置的抽象几何 LOGO

```bash
python utils/make_logo.py     # 生成 logo_abstract*.png + logo_160x128*.png
python utils/logo_converter.py utils/logo_160x128.png        # 彩色版 → 固件
python utils/logo_converter.py utils/logo_160x128_mono.png   # 黑白版 → 固件
```

`make_logo.py` 只输出 `logo_abstract*.png` / `logo_160x128*.png`，**不会**动 `logo.png`，
所以你自己的图永远安全。

当前内置设计（v5）：**齿轮外圈** + 中心**坎卦 ☵**（上断/中实/下断），
底部一律写「**坎川工业**」（含 160×128 设备版）。
外圈只有一层轮缘（无内圈同心环），线条清晰不杂乱。
齿数/齿高/轮缘厚等比例常量都在 `make_logo.py` 顶部，可自行微调。

输出的 5 个版本：

| 文件 | 尺寸 | 配色 | 说明 |
|---|---|---|---|
| `logo_abstract.png` | 800×800 | **红黄**（黑底 + 金黄齿轮 + 朱红卦线） | 彩色主图，含文字 |
| `logo_abstract_mono.png` | 800×800 | 黑白（黑底白图） | 印刷 / 单色屏 |
| `logo_abstract_mono_inv.png` | 800×800 | 黑白反白（白底黑图） | 白纸 / 合同抬头 |
| `logo_abstract_icon.png` | 256×256 | 红黄 | 方形图标，无文字 |
| `logo_160x128.png` | 160×128 | 红黄 | **烧录用**（屏幕尺寸） |
| `logo_160x128_mono.png` | 160×128 | 黑白 | 屏幕尺寸黑白版 |

烧录哪个版本就转换哪个：`logo_160x128.png` → 彩色开机画面，
`logo_160x128_mono.png` → 黑白开机画面。

## Scripts

### serial_monitor.py

Monitor real-time radar data from the ESP32:

```bash
python utils/serial_monitor.py
```

You'll be prompted to enter your COM port (or press Enter for default COM5).

### deploy_circuitpython.py

Deploy CircuitPython code to your device:

```bash
python utils/deploy_circuitpython.py
```

This script will:
1. Prompt for your COM port
2. Stop the running program
3. Upload the CircuitPython code
4. Restart the device

## Configuration

If you have a consistent COM port, you can modify the default in each script:

```python
DEFAULT_PORT = "COM5"  # Change to your port
```

## Troubleshooting

**Can't find COM port:**
- Windows: Check Device Manager → Ports (COM & LPT)
- List all ports: `python -m serial.tools.list_ports`

**Permission denied:**
- Close Arduino IDE or other serial monitors
- Only one program can access the port at a time

**No data received:**
- Check baud rate (should be 115200)
- Verify ESP32 is powered and programmed
- Check USB cable (must support data transfer)
