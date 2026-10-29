# utils/

工具脚本。按用途分四类：

| 脚本 | 用途 |
|---|---|
| [`build.ps1`](#构建与烧录) | 构建主机固件 |
| [`flash.ps1`](#构建与烧录) | 烧录主机固件 |
| [`build_slave.ps1`](#从机构建) | 构建从机固件（Keil UV4） |
| [`make_logo.py`](#logo) | 生成内置几何 LOGO |
| [`logo_converter.py`](#logo) | 把 PNG 转成固件点阵 |
| [`fetch_seal_font.py`](#logo) | 下载小篆字体 |
| [`make_qr_text.py`](#二维码说明文字) | 二维码说明文字 → `QrTextImage.h` |
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

## 从机构建

从机是 STC8G1K08A，需要 **Keil C51 V9.60+**（脚本调 `UV4.exe`）：

```powershell
.\utils\build_slave.ps1          # 增量编译
.\utils\build_slave.ps1 -Clean   # 清理后完整重建
```

产出 `src\slave\Objects\slave.hex`，用 **STC-ISP** 烧录。

**脚本会自动处理一个致命的坑**：`slave.uvproj` **不能带 UTF-8 BOM**，否则
UV4 静默失败——`Exit=15`、不写日志、不报错、无任何产物，极难察觉。
`build_slave.ps1` 每次构建前检测并剥离 BOM。

另一个反向的坑：`src\slave\Sources\**` 的 `.c` / `.h` **必须保持 GBK 无 BOM**。
Keil C51 按 GBK 解释源码，转成 UTF-8 会让中文注释炸成语法错误。

---

## LOGO

完整的设计说明、输出版本清单、字体实测结论见 **[docs/LOGO.md](../docs/LOGO.md)**，
这里只讲操作。

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

- `src/master/LogoImage.h` —— 固件编译时自动 `#include`
- `utils/logo_preview.png` —— **屏上实际效果**，烧之前先看它

`make_logo.py` 只写 `logo_abstract*.png` / `logo_160x128*.png`，
**永远不会覆盖 `logo.png`**，你自己放的图是安全的。

---

## 二维码说明文字

配网页上那段说明文字是点阵图（GFX 字模没有中文）：

```powershell
python utils\make_qr_text.py     # → src/master/QrTextImage.h + qr_text_preview.png
```

改文案就改脚本里的字符串，跑完先看 `utils/qr_text_preview.png` 再构建。
`QrTextImage.h` 是**脚本生成**的，不要手改。

---

## 串口监视

```powershell
python utils/serial_monitor.py            # 自动探测串口
python utils/serial_monitor.py COM30      # 指定串口
```

115200 baud。固件是事件驱动输出（配对 / 占用 / 防区离线），另每 60 秒一条 `[HEALTH]`。

**注意**：ESP32-C3 用原生 USB，打开串口这一动作本身就会让芯片复位（DTR/RTS）。
`serial_monitor.py` 已在打开前屏蔽 DTR/RTS，默认仅在首次连接时敲一次复位以报开机日志；
想看真实的连续运行时长（`[HEALTH] up=..m` 真正递增）加 `--no-reset`：

```powershell
python utils/serial_monitor.py COM30 --no-reset
```

需要图表 / 时间戳更细的场景，也可以用 `pio device monitor -d src/master`。

