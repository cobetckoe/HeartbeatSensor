# LOGO 设计与生成

屏幕开机先显示一块 **坎川工业** LOGO 1.5 秒，然后直接进雷达界面。

---

## 设计

- **外圈**：齿轮，只有一层轮缘（没有内圈同心环），16 齿
- **中心**：坎卦 ☵ —— 上断、中实、下断
- **底部**：坎川工业

「坎」为水，「川」为河，两字本义皆水，所以抽象成**卦象 + 几何**，
不用任何有机曲线。

配色：纯黑底 + **亮金 `#FFD800`** + **正红 `#E60012`**（中国红标准值）。

> **单色版本是为商标注册准备的** —— 申请时递黑白稿，保护范围比彩稿更宽。

---

## 输出的版本

全部由 `make_logo.py` 生成，落在 `utils/`（PNG 已入 Git，属于 LOGO 交付物，
不要 gitignore 掉）：

| 文件 | 尺寸 | 配色 | 用途 |
|---|---|---|---|
| `logo_abstract.png` | 800×800 | 亮金正红 | 彩色主图 |
| `logo_abstract_mono.png` | 800×800 | 黑底白图 | 印刷 |
| `logo_abstract_mono_inv.png` | 800×800 | 白底黑图 | 白纸 / 合同抬头 |
| `logo_abstract_icon.png` | 256×256 | 亮金正红 | 方形图标，无文字 |
| `logo_160x128.png` | 160×128 | 亮金正红 | **烧录用** |
| `logo_160x128_mono.png` | 160×128 | 黑白 | 屏幕尺寸单色版 |
| `*_seal.png` | 同上 | 同上 | 篆书字体版（需先下字体） |

另外 `logo_converter.py` 会输出 `utils/logo_preview.png` =
**屏幕上的实际效果**，烧之前先看它。

> `logo_160x128.png` 与 `logo_preview.png` 的内容等价（都是 160×128 彩稿），
> 分别由 `make_logo.py`（生成）和 `logo_converter.py`（预览）写出。

---

## 用法

```powershell
python utils/fetch_seal_font.py                        # 想要篆书版先下字体
python utils/make_logo.py                              # 生成内置几何 LOGO
python utils/logo_converter.py utils\logo_160x128.png  # 转成固件点阵
.\utils\build.ps1
```

`logo_converter.py` 做两件事：

1. 写出 `src/master/LogoImage.h`（固件点阵，**脚本生成，勿手改**）
2. 写出 `utils/logo_preview.png`（屏幕上实际会显示的样子）

**换成自己的图**——把图片存成 `utils/logo.png`（也认 `.jpg` / `.jpeg` / `.bmp`）
再跑 `logo_converter.py` 即可。**自己放的图 `make_logo.py` 永远不会动。**

---

## 实现注意

- 点阵在 `src/master/LogoImage.h`，由 `#if __has_include("LogoImage.h")` 保护，
  缺文件时固件仍能编译（只是没有 LOGO）
- 显示走 `radarDisplay.showBootLogo(1500)`，时长单位 ms
- **Adafruit GFX 经典 5×7 字模只有 ASCII（0x20–0x7F），没有中文也没有 `°`。**
  LOGO 底部的「坎川工业」是点阵图，不是文字渲染；屏上度数符号是用
  `drawCircle(x, y, 1)` 画的小圆点。两者都不要试图用字模解决。
