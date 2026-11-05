# -*- coding: utf-8 -*-
"""
make_qr_text.py - 把配网二维码页上的中文提示预渲染成固件点阵

★ 为什么需要这一步
  Adafruit_GFX 自带字体是 5x7/6x8 的 ASCII 位图，**根本不含汉字**。
  想在 160x128 的屏上显示「系统相机 / 扫码入网 / 勿用微信」，
  只有两条路：
    1) 塞一个完整的中文字库 —— 至少几十 KB flash，而且 C3 只剩不到 300KB，
       为几句话付这个代价不值；
    2) 把这几个固定字符串【离线渲染成点阵】烧进固件 —— 每行 48x12 只要 72 字节。
  这里走第 2 条。这些文案是写死的、不会变，所以离线渲染完全够用。

★ 输出 1bit 掩膜而不是彩色点阵
  掩膜只有 0/1，颜色在【画的时候】由代码指定 —— 这样同一份点阵
  既能画红色强调，也能画黑色正文，不用为每种颜色各存一份。
  （用户明确要求「可以用彩色字体」，掩膜方案正好配合。）

输出:
  src/master/QrTextImage.h   固件 include 的点阵头文件
  utils/qr_text_preview.png  1:1 预览（先看这个再烧录）

用法:
  python utils/make_qr_text.py
依赖:
  pip install Pillow
"""

import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("缺少 Pillow 库，请先安装: pip install Pillow")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKETCH = os.path.join(ROOT, "src", "master")
OUT_H = os.path.join(SKETCH, "QrTextImage.h")
OUT_PNG = os.path.join(HERE, "qr_text_preview.png")

# ── 字体：优先黑体（笔画粗、小字号最清楚），退到雅黑 ──
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
]

# ── 要渲染的文案 ──
#   名字 → (文字, 字号)
#   ★ 字号取 12：这是 160px 宽屏上汉字还能保持笔画不粘连的下限
#     （试过 11px，「系」的绞丝旁会和右半边糊在一起，见 git 历史里的预览图）。
#     12px 的四个汉字实测 53px 宽，而右栏可用 55px，正好放下还有余量。
#     再大就溢出了，所以文案一律压到 4 字以内。
GLYPHS = [
    ("HINT1", "系统相机", 12),   # 第 1 步：用什么扫（金色强调）
    ("HINT2", "扫码入网", 12),   # 第 2 步：扫完会怎样
    ("HINT3", "勿用微信", 12),   # 反面强调：微信扫不出（红色警告）
]

SCREEN_W, SCREEN_H = 160, 128


def find_font():
    for p in FONT_CANDIDATES:
        if os.path.isfile(p):
            return p
    print("找不到中文字体，请确认系统里有 simhei.ttf 或 msyh.ttc")
    sys.exit(1)


def render_mask(text, px, font_path, ss=4):
    """把一句话渲染成 1bit 掩膜（1 = 有墨），返回 (w, h, bits[])。

    ★ 采用【固定字距】排版：每个汉字占 px 宽的一格，整行宽 = px × 字数。
      这样 4 个 12px 汉字恰好 48px 宽，比按墨迹外框拼接（53px）窄 5px ——
      正好塞得进右栏。汉字的墨迹并不填满字身框，12px 字距是标准排版间距，
      压缩后笔画之间仍有空隙，不会粘字。

    ★ 渲染流程：先在 ss 倍尺寸上渲染（ss=4），再用 LANCZOS 缩回目标字号。
      不能直接在目标字号渲染再二值化：12px 的汉字笔画只有 1~2 px 宽，
      TrueType 的抗锯齿会把它糊成一团灰，直接阈值化会断笔、缺角。
      先大后小相当于自己做了一次高质量降采样，笔画能保住。
    """
    font = ImageFont.truetype(font_path, px * ss)

    # ★ 单字渲染用的「画布格」必须比目标字距宽：
    #   字号 = px*ss 时，一个汉字墨迹本身就接近 px*ss 宽，
    #   若把画布也只开成 cell 宽再从中间落笔，字会被裁掉一半。
    #   所以画布开 2*cell，量完墨迹再缩放拼到 cell 宽的最终行里。
    cell = px * ss
    pad_cell = cell * 2

    # 逐字渲染，各自居中到自己那一格，最后拼成整行
    n = len(text)
    w_big = cell * n
    row = Image.new("L", (w_big, px * ss * 2), 255)
    for i, ch in enumerate(text):
        one = Image.new("L", (pad_cell, px * ss * 2), 255)
        ImageDraw.Draw(one).text((pad_cell // 2, cell // 2), ch, font=font, fill=0)
        bb = one.point(lambda v: 255 - v).getbbox()
        if bb is None:
            continue
        # ★ crop(bb) 已经把墨迹从 bb[0]/bb[1] 处裁出来了，
        #   所以粘贴坐标是「目标格子的左边 + 居中偏移」，
        #   绝不能再减一次 bb[0]（那样每个字都会左移一个墨迹偏移量，
        #   几个字就会互相叠压成一团乱码）。
        glyph = one.crop(bb)
        gw, gh = glyph.size
        # 墨迹若比格子还宽（字距被压得比字身窄），先等比缩到格内
        if gw > cell:
            scale = cell / float(gw)
            glyph = glyph.resize((cell, max(1, int(round(gh * scale)))), Image.LANCZOS)
            gw, gh = glyph.size
        dst_x = i * cell + max(0, (cell - gw) // 2)
        row.paste(glyph, (dst_x, bb[1]))

    ink = row.point(lambda v: 255 - v).getbbox()
    if ink is None:
        return 0, 0, b""
    # 垂直裁到墨迹，水平保留满宽（固定字距），这样整行宽度可预测
    crop = row.crop((0, ink[1], w_big, ink[3]))

    w = max(1, int(round(crop.width / float(ss))))
    h = max(1, int(round(crop.height / float(ss))))
    small = crop.resize((w, h), Image.LANCZOS)

    bits = bytearray((w * h + 7) // 8)
    p = small.load()
    for y in range(h):
        for x in range(w):
            if p[x, y] < 128:                      # 够黑就算笔画
                idx = y * w + x
                bits[idx >> 3] |= (0x80 >> (idx & 7))   # MSB first，与 GFX 习惯一致
    return w, h, bits


def emit_c_array(name, data):
    lines = []
    lines.append("static const uint8_t %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02X" % b for b in data[i:i + 16])
        lines.append("  " + chunk + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    font_path = find_font()
    print("字体 : %s" % font_path)

    results = []
    for name, text, px in GLYPHS:
        w, h, bits = render_mask(text, px, font_path)
        if w == 0:
            print("!! 渲染失败: %s" % text)
            sys.exit(1)
        results.append((name, text, w, h, bits))
        print("  %-6s %-6s %2dx%-2d  %d 字节" % (name, text, w, h, len(bits)))

    # ── 写头文件 ──
    out = []
    out.append("// QrTextImage.h - 由 utils/make_qr_text.py 自动生成，请勿手工编辑")
    out.append("// 中文提示点阵（1bit 掩膜，颜色在绘制时指定）")
    out.append("//")
    out.append("// Copyright (C) 2026 Kanchuan Industrial")
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("#ifndef QR_TEXT_IMAGE_H")
    out.append("#define QR_TEXT_IMAGE_H")
    out.append("")
    out.append("#include <Arduino.h>")
    out.append("")
    for name, text, w, h, bits in results:
        out.append("// %s  %s  %dx%d" % (name, text, w, h))
        out.append("#define QRT_%s_W %d" % (name, w))
        out.append("#define QRT_%s_H %d" % (name, h))
        out.append(emit_c_array("QRT_%s" % name, bits))
        out.append("")
    out.append("#define QR_TEXT_IMAGE_AVAILABLE 1")
    out.append("")
    out.append("#endif")
    out.append("")

    # ★ 显式写 UTF-8 无 BOM：本机 PowerShell 默认写 GBK，
    #   而注释里有中文，写错编码会让 gcc 报 invalid UTF-8。
    with open(OUT_H, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("写出 : %s" % OUT_H)

    # ── 预览图：把点阵按 1:1 摆到 160x128 的黑底上，模拟真实观感 ──
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    colors = [(255, 216, 0), (255, 216, 0), (230, 0, 18), (255, 216, 0)]
    x, y = 4, 4
    for (name, text, w, h, bits), col in zip(results, colors):
        for yy in range(h):
            for xx in range(w):
                idx = yy * w + xx
                if bits[idx >> 3] & (0x80 >> (idx & 7)):
                    d.point((x + xx, y + yy), fill=col)
        x += w + 6
        if x > SCREEN_W - 40:
            x = 4
            y += h + 4
    img.resize((SCREEN_W * 3, SCREEN_H * 3), Image.NEAREST).save(OUT_PNG)
    print("预览 : %s  (3 倍放大)" % OUT_PNG)


if __name__ == "__main__":
    main()
