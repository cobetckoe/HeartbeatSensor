# utils/fonts/

第三方字体放这里。**字体文件不入 Git**（单个 20 MB+，见根目录 `.gitignore`），
克隆仓库后按需取回。

## 崇羲篆體 (ChongXiSmallSeal.otf)

- **来源**：中央研究院 數位文化中心「崇羲篆體」
- **分发**：npm 包 `@fontpkg/chong-xi-small-seal`
- **大小**：约 21.8 MB
- **取回**：

  ```powershell
  python utils/fetch_seal_font.py
  ```

  文件名必须是 `ChongXiSmallSeal.otf`，`make_logo.py` 才认得出。

### 已知限制

1. **缺「业」字。** 直接渲染「坎川工业」的「业」会变成方块。
   `make_logo.py` 已有兜底：`fallback_font_for_text()` 检测到文本含「业」时，
   整串自动换回系统黑体。判断缺字不能只靠 `textbbox()`，它对缺字照样返回一个
   看起来正常的方框，得去数实际像素。

2. **小屏不可用。** 篆书笔画细密，160×128 的屏上：

   | 字号 | 20 px | 26 px |
   |---|---|---|
   | 崇羲篆體 | 糊成一团 | 勉强可辨 |
   | 黑体 | 清晰 | 清晰 |

   所以篆书版只用于大幅面 / 印刷，设备开机画面用黑体。

## 其它字体

`make_logo.py` 还会按 `SEAL_FONT_CANDIDATES` 的顺序找本地篆体：方正小篆体、
汉仪篆书、叶根友篆书等。装了任意一个就会自动被用上。也可以直接把字体丢进本目录，
命名为 `seal.ttf` 或 `zhuanshu.ttf`。

> ?? 商用前请确认字体授权。方正常用字体个人非商用免费，商用需购买授权。
