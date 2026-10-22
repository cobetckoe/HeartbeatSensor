# -*- coding: utf-8 -*-
"""
logo_converter.py - 把你的 LOGO 图片转成固件内置点阵 (RGB565)

★ 这是「自己导入 PNG 替换 LOGO」的唯一入口。
  你自己放的图片永远不会被 make_logo.py 覆盖 (那个脚本只写 logo_abstract_*.png)。

用法 (三种, 任选其一):
  1) 放进 utils/logo.png 然后运行:
       python utils/logo_converter.py
     (也认 logo.jpg / logo.jpeg / logo.bmp)

  2) 直接指定任意路径的图片:
       python utils/logo_converter.py D:\\我的图\\brand.png

  3) 用生成器画好的图:
       python utils/logo_converter.py utils/logo_160x128.png

产物:
  src/master/LogoImage.h   固件可 include 的 PROGMEM 点阵
  utils/logo_preview.png                        实际会显示在屏上的效果预览 (先看这个再烧录!)

图片处理规则:
  - 等比缩放到不超过 160x128 (屏幕尺寸), 不裁切、不变形
  - 带透明通道的 PNG 会自动合成到屏幕底色上 (不会变黑块)
  - 深色/浅色自适应: 若图片整体偏暗, 背景填屏幕黑; 偏亮则白底保留
依赖: pip install Pillow
"""

import os
import sys

try:
    from PIL import Image
except ImportError:
    print("缺少 Pillow 库，请先安装: pip install Pillow")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SCRIPT_DIR)

# 输出到实际使用的工程目录 (src_dir = . 下同目录可直接 #include "LogoImage.h")
SKETCH_DIR = os.path.join(ROOT, "src", "master")
OUT_PATH = os.path.join(SKETCH_DIR, "LogoImage.h")
PREVIEW_PATH = os.path.join(SCRIPT_DIR, "logo_preview.png")

SCREEN_W, SCREEN_H = 160, 128
AUTO_CANDIDATES = ["logo.png", "logo.jpg", "logo.jpeg", "logo.bmp"]


def pick_source(argv):
    """优先级: 命令行参数 > utils/logo.* > 生成器输出"""
    if len(argv) > 1:
        p = argv[1]
        if not os.path.isabs(p):
            # 相对 utils/ 或相对工程根, 都试一下
            for base in (SCRIPT_DIR, ROOT, os.getcwd()):
                cand = os.path.normpath(os.path.join(base, p))
                if os.path.isfile(cand):
                    return cand
            return os.path.normpath(os.path.join(os.getcwd(), p))
        return p
    for name in AUTO_CANDIDATES:
        p = os.path.join(SCRIPT_DIR, name)
        if os.path.isfile(p):
            return p
    return None


def flatten(img, bg=(0, 0, 0)):
    """把任意图片统一成 RGB (透明通道合成到 bg 上)。"""
    if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        canvas = Image.new("RGB", rgba.size, bg)
        canvas.paste(rgba, mask=rgba.split()[3])
        return canvas
    return img.convert("RGB")


def guess_bg(img):
    """看四角平均亮度, 猜底色: 偏亮 -> 白底; 偏暗 -> 黑底。"""
    w, h = img.size
    pts = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    s = 0
    for x, y in pts:
        r, g, b = img.getpixel((x, y))
        s += (r + g + b) / 3.0
    return (255, 255, 255) if s / 4.0 > 128 else (0, 0, 0)


def to_rgb565(img, max_w=SCREEN_W, max_h=SCREEN_H):
    """等比缩放到不超过屏幕尺寸，再转 RGB565 像素列表。返回 (size, pixels)。"""
    w, h = img.size
    scale = min(max_w / float(w), max_h / float(h), 1.0)
    if scale < 1.0:
        img = img.resize((max(1, int(round(w * scale))),
                          max(1, int(round(h * scale)))), Image.LANCZOS)
    px = []
    for r, g, b in img.getdata():
        px.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return img.size, px


def main():
    src = pick_source(sys.argv)
    if src is None or not os.path.isfile(src):
        print("没找到图片。请任选一种方式:")
        print("  1) 把图片存成 utils/logo.png 后运行本脚本")
        print("  2) python utils/logo_converter.py <图片路径>")
        sys.exit(1)

    raw = Image.open(src)
    bg = guess_bg(flatten(raw))
    img = flatten(raw, bg)
    (w, h), pixels = to_rgb565(img)

    base = os.path.basename(src)
    print("源图片 : %s  %s" % (base, raw.size))
    print("处理为 : %dx%d RGB565  (底色 %s)"
          % (w, h, "白" if bg == (255, 255, 255) else "黑"))

    # ── 生成 include/LogoImage.h ──
    L = []
    L.append("// LogoImage.h - 由 utils/logo_converter.py 自动生成，请勿手工编辑")
    L.append("// 源图片: %s   尺寸: %dx%d   RGB565" % (base, w, h))
    L.append("#ifndef LOGO_IMAGE_H")
    L.append("#define LOGO_IMAGE_H")
    L.append("")
    L.append("#include <Arduino.h>")
    L.append("")
    L.append("#define LOGO_W %d" % w)
    L.append("#define LOGO_H %d" % h)
    L.append("")
    L.append("// 存储顺序与屏幕扫描一致，setAddrWindow + writePixels 逐行直推即可")
    L.append("static const uint16_t LOGO_BITMAP[%d] PROGMEM = {" % (w * h))
    for i in range(0, len(pixels), 12):
        L.append("    " + ", ".join("0x%04X" % v for v in pixels[i:i + 12]) + ",")
    L.append("};")
    L.append("")
    L.append("#endif  // LOGO_IMAGE_H")

    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L))

    # ── 生成预览图 (含屏幕黑边, 所见即所得) ──
    prev = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
    prev.paste(img, ((SCREEN_W - w) // 2, (SCREEN_H - h) // 2))
    prev.save(PREVIEW_PATH)

    print()
    print("已生成 : src/master/LogoImage.h  (%d 像素, 约 %d KB flash)"
          % (len(pixels), len(pixels) * 2 // 1024))
    print("预览图 : utils/logo_preview.png   ← 先看这个确认效果")
    print("下一步 : pio run -d \"src\\master\"  然后烧录")


if __name__ == "__main__":
    main()
