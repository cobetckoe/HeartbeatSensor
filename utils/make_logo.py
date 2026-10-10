# -*- coding: utf-8 -*-
"""
make_logo.py - 坎川工业 LOGO 生成器 (v6: 齿轮 + 坎卦 + 亮金正红 + 黑白版)

设计 (全部只用 圆 + 矩形 + 多边形 基本图元, 无任何有机曲线):
  外圈  = 齿轮 (一层轮缘, 无内圈同心环)
  中心  = 坎卦  三横线: 上断 - 中实 - 下断
          "坎"为水, "川"为河, 两字本义皆水 -> 抽象为最纯粹的几何
  底部  = 坎川工业  (彩色版与黑白版都有)

v6 改动 (按用户要求):
  - 金色加亮: (255,200,40) -> (255,216,0)  亮金 #FFD800
  - 红色改为正红: (255,48,32) -> (230,0,18)  #E60012 (中国红)
  - 新增篆书字体自动探测 (SEAL_FONT_CANDIDATES), 装了篆体就自动用
    - 小屏 160x128 默认仍用黑体 (篆书在 20px 下会糊成一团, 见 README)

v5 改动:
  - 彩色版配色改为「红黄」: 纯黑底 + 金黄齿轮 + 朱红卦线
  - 黑白版两套: 黑底白图 (屏幕/印刷) + 白底黑图 (白纸/合同抬头)
  - 底部一律写「坎川工业」(含设备 160x128 版)
  - 去掉内圈同心环, 齿轮只有一层轮缘

映射到单片机 GFX 时: 齿轮 -> 多边形+圆, 横线 -> fillRect, 零曲线.

★ 本脚本不再生成 logo.png (避免覆盖你自己导入的图片!)
  你自己放的图请命名为 utils/logo.png / .jpg / .bmp, 永远不会被本脚本动。

输出:
  utils/logo_abstract.png          800x800  彩色主图 (红黄 + 文字)
  utils/logo_abstract_mono.png     800x800  黑白版 (黑底白图 + 文字)
  utils/logo_abstract_mono_inv.png 800x800  黑白反白版 (白底黑图 + 文字)
  utils/logo_abstract_icon.png     256x256  方形图标 (无文字)
  utils/logo_160x128.png           160x128  屏幕尺寸预览 (红黄 + 文字)
  utils/logo_160x128_mono.png      160x128  屏幕尺寸黑白版 (黑底白图 + 文字)

用法:
  python utils/make_logo.py
依赖:
  pip install Pillow
"""

import math
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("缺少 Pillow 库，请先安装: pip install Pillow")
    sys.exit(1)

HERE = os.path.dirname(os.path.abspath(__file__))

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\simhei.ttf",
    r"C:\Windows\Fonts\msyhbd.ttc",
    r"C:\Windows\Fonts\msyh.ttc",
]

# 篆书字体候选 (系统默认不带, 需自行安装或放到 utils/fonts/ 下)
#   免费/开源优先; 方正常用字体个人非商用免费, 商用需授权
SEAL_FONT_CANDIDATES = [
    os.path.join(HERE, "fonts", "ChongXiSmallSeal.otf"),  # 崇羲篆體 (已下载)
    os.path.join(HERE, "fonts", "seal.ttf"),            # 放这里最省事
    os.path.join(HERE, "fonts", "zhuanshu.ttf"),
    r"C:\Windows\Fonts\FZXiaoZhuanTi.ttf",               # 方正小篆体
    r"C:\Windows\Fonts\FZXiaoZhuanTi-CS.ttf",
    r"C:\Windows\Fonts\HYZhuanShu.ttf",                  # 汉仪篆书
    r"C:\Windows\Fonts\YeGenYouZhuanShu.ttf",            # 叶根友篆书
    r"C:\Windows\Fonts\seal.ttf",
    r"C:\Windows\Fonts\zhuanshu.ttf",
]


def find_seal_font():
    """返回可用的篆书字体路径, 没有则返回 None。"""
    for p in SEAL_FONT_CANDIDATES:
        if os.path.isfile(p):
            return p
    # 再扫一遍字体目录, 抓任何名字里带 zhuan/seal/篆 的
    for d in (r"C:\Windows\Fonts",
              os.path.join(os.environ.get("LOCALAPPDATA", ""),
                           r"Microsoft\Windows\Fonts"),
              os.path.join(HERE, "fonts")):
        if not os.path.isdir(d):
            continue
        try:
            for fn in os.listdir(d):
                low = fn.lower()
                if low.endswith((".ttf", ".otf", ".ttc")) and (
                        "zhuan" in low or "seal" in low or "篆" in fn):
                    return os.path.join(d, fn)
        except OSError:
            pass
    return None

# ---------------- 配色 ----------------
# 彩色版 = 红黄: 纯黑底 + 亮金齿轮 + 正红卦线 + 亮金文字
# v6: 金色加亮 (G 200->216, B 40->0), 红改为标准正红 (230,0,18)
GOLD_BRIGHT = (255, 216, 0)      # 亮金 (#FFD800)
RED_TRUE    = (230, 0, 18)       # 正红 (#E60012, 中国红标准值)

PAL_MAIN = {
    "bg":   (0, 0, 0),
    "ring": GOLD_BRIGHT,         # 亮金 (齿轮)
    "jin":  RED_TRUE,            # 正红 (坎卦)
    "text": GOLD_BRIGHT,         # 亮金 (文字)
}
# 黑白版: 纯黑底 + 纯白图形
PAL_MONO = {
    "bg":   (0, 0, 0),
    "ring": (255, 255, 255),
    "jin":  (255, 255, 255),
    "text": (255, 255, 255),
}
# 黑白反白版: 纯白底 + 纯黑图形 (打印 / 白纸 / 合同抬头用)
PAL_MONO_INV = {
    "bg":   (255, 255, 255),
    "ring": (0, 0, 0),
    "jin":  (0, 0, 0),
    "text": (0, 0, 0),
}
# 设备 160x128 彩色版 (与 PAL_MAIN 同色, 小屏下已足够亮)
PAL_DEV = {
    "bg":   (0, 0, 0),
    "ring": GOLD_BRIGHT,
    "jin":  RED_TRUE,
    "text": GOLD_BRIGHT,
}

# 齿轮比例 (相对外半径 R_tip, 与尺寸无关)
TOOTH_H_R = 0.130     # 齿高
BAND_R    = 0.100     # 轮缘厚度
TEETH_ODD = 0.52      # 齿占节距的比例 (0.52 -> 齿略窄于齿槽, 清晰)


# ---------------------------------------------------------------------------
#  字体
# ---------------------------------------------------------------------------
def load_font(size, path=None):
    """加载字体。path 指定时优先用它 (篆书版用)。"""
    if path and os.path.isfile(path):
        try:
            return ImageFont.truetype(path, int(size))
        except Exception:
            pass
    for p in FONT_CANDIDATES:
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, int(size))
            except Exception:
                continue
    return ImageFont.load_default()


def fit_font(text, target_h, max_w=None, path=None):
    """返回字号, 使 text 实际渲染高度接近 target_h。"""
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    f = load_font(100, path)
    bb = probe.textbbox((0, 0), text, font=f)
    h = max(1, bb[3] - bb[1])
    size = max(6, int(round(100.0 * target_h / h)))
    if max_w:
        f2 = load_font(size, path)
        bb2 = probe.textbbox((0, 0), text, font=f2)
        w = max(1, bb2[2] - bb2[0])
        if w > max_w:
            size = max(6, int(size * max_w / w))
    return load_font(size, path)


def fallback_font_for_text(font, text):
    """某些篆书字体对个别字（如「业」）缺字，兜底到系统黑体。"""
    path = getattr(font, "path", "") or ""
    if "ChongXiSmallSeal" not in os.path.basename(path) and "seal" not in path.lower():
        return font
    if "业" not in text:
        return font
    for p in [r"C:\Windows\Fonts\simhei.ttf", r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\msyh.ttc"]:
        if os.path.isfile(p):
            try:
                return ImageFont.truetype(p, int(getattr(font, "size", 24)))
            except Exception:
                continue
    return font


def draw_text_centered(d, text, cx, cy, font, fill):
    """以 (cx, cy) 为中心绘制文字。"""
    font = fallback_font_for_text(font, text)
    bb = d.textbbox((0, 0), text, font=font)
    w = bb[2] - bb[0]
    h = bb[3] - bb[1]
    d.text((cx - w / 2.0 - bb[0], cy - h / 2.0 - bb[1]), text, font=font, fill=fill)


# ---------------------------------------------------------------------------
#  核心图形: 齿轮 + 坎卦
# ---------------------------------------------------------------------------
def draw_gear(d, cx, cy, R_tip, teeth, color, bg):
    """画一个齿轮轮缘 (只有一层): 齿 + 轮缘环, 中心镂空为 bg。"""
    tooth_h = R_tip * TOOTH_H_R
    band = R_tip * BAND_R
    R_root = R_tip - tooth_h          # 齿根半径
    R_in = R_root - band              # 轮缘内径 (镂空半径)

    # ── 齿 (梯形/矩形齿, 径向排布) ──
    half_deg = (360.0 / teeth) * TEETH_ODD * 0.5
    for i in range(teeth):
        ac = 360.0 * i / teeth
        a0, a1 = ac - half_deg, ac + half_deg
        pts = []
        for r, a in ((R_root, a0), (R_tip, a0), (R_tip, a1), (R_root, a1)):
            rad = math.radians(a)
            pts.append((cx + r * math.cos(rad), cy + r * math.sin(rad)))
        d.polygon(pts, fill=color)

    # ── 轮缘实体 ──
    d.ellipse([cx - R_root, cy - R_root, cx + R_root, cy + R_root], fill=color)

    # ── 中心镂空 (露出黑底, 只留一层轮缘) ──
    d.ellipse([cx - R_in, cy - R_in, cx + R_in, cy + R_in], fill=bg)

    return R_in


def draw_mark(d, cx, cy, R_tip, teeth, pal):
    """在 (cx, cy) 画齿轮, 轮缘内部画坎卦三横线 (上断/中实/下断)。"""
    R_in = draw_gear(d, cx, cy, R_tip, teeth, pal["ring"], pal["bg"])

    # ── 坎卦 (相对镂空半径等比) ──
    half = R_in * 0.740           # 横线半长
    th = R_in * 0.135             # 线粗
    step = R_in * 0.335           # 上下爻行距 (相对中爻)
    gap = R_in * 0.340            # 断口宽度
    seg = half - gap * 0.5        # 断线单段长度

    jin = pal["jin"]

    def bar(x0, x1, y):
        d.rectangle([x0, y - th * 0.5, x1, y + th * 0.5], fill=jin)

    for i, y in enumerate((cy - step, cy, cy + step)):   # 上 / 中 / 下
        if i == 1:                                       # 中爻 = 实线
            bar(cx - half, cx + half, y)
        else:                                            # 上爻 / 下爻 = 断线
            bar(cx - half, cx - half + seg, y)
            bar(cx + half - seg, cx + half, y)


# ---------------------------------------------------------------------------
#  画布工具 (超采样后缩放, 保证边缘干净)
# ---------------------------------------------------------------------------
def supersample(w, h):
    """小图用更高倍率, 保证缩放后边缘不锯齿。"""
    return max(4, int(round(1200.0 / max(w, h))))


def render(w, h, pal, fn):
    ss = supersample(w, h)
    img = Image.new("RGB", (w * ss, h * ss), pal["bg"])
    d = ImageDraw.Draw(img)
    fn(d, w * ss, h * ss, ss)
    return img.resize((w, h), Image.LANCZOS)


def save(img, name):
    path = os.path.join(HERE, name)
    img.save(path)
    print("  %-30s %dx%d" % (name, img.size[0], img.size[1]))
    return path


# ---------------------------------------------------------------------------
#  各版本
# ---------------------------------------------------------------------------
def make_square(size, pal, with_text, R_ratio, teeth, name, text_font=None):
    def fn(d, W, H, ss):
        cx, cy = W / 2.0, H / 2.0
        if with_text:
            cy = H * 0.395                      # 有文字时图形上移
        draw_mark(d, cx, cy, W * R_ratio, teeth, pal)
        if with_text:
            f = fit_font("坎川工业", H * 0.115, max_w=W * 0.62, path=text_font)
            draw_text_centered(d, "坎川工业", cx, H * 0.845, f, pal["text"])

    save(render(size, size, pal, fn), name)


def make_device(name, pal, text_font=None):
    """160x128 屏幕尺寸预览 (设备配色, 底部写「坎川工业」)。

    小屏 4 个汉字要看得清, 字号必须够大 -> 齿轮缩小上移, 给文字留出
    约 20px 高的完整行。齿轮齿顶距屏幕上边 3px, 文字底距下边 8px。
    text_font: 传篆书字体路径则用篆书 (小屏下会很难认, 慎用)。
    """
    W, H = 160, 128

    # 篆书笔画细密, 小屏要放大才勉强可认 -> 齿轮缩小上移, 文字加高
    seal = text_font is not None
    if seal:
        cx_r, cy_r, txt_h, txt_y = 0.238, 0.300, 0.235, 0.815
    else:
        cx_r, cy_r, txt_h, txt_y = 0.272, 0.365, 0.160, 0.855

    def fn(d, w, h, ss):
        # 齿轮: 圆心上移 -> 齿顶不顶边, 底部留白给文字
        cx, cy = w / 2.0, h * cy_r
        R = w * cx_r
        draw_mark(d, cx, cy, R, 12, pal)
        # 底部文字: 宽度上限屏宽 88%
        f = fit_font("坎川工业", h * txt_h, max_w=w * 0.88, path=text_font)
        draw_text_centered(d, "坎川工业", cx, h * txt_y, f, pal["text"])

    save(render(W, H, pal, fn), name)


def main():
    print("坎川工业 LOGO (v6 齿轮 + 坎卦 + 亮金正红 / 黑白) 生成中...")
    print()
    print("[彩色版 亮金正红]")
    make_square(800, PAL_MAIN, True, 0.320, 16, "logo_abstract.png")
    make_square(256, PAL_MAIN, False, 0.405, 14, "logo_abstract_icon.png")
    make_device("logo_160x128.png", PAL_DEV)
    print()
    print("[黑白版]")
    make_square(800, PAL_MONO, True, 0.320, 16, "logo_abstract_mono.png")
    make_device("logo_160x128_mono.png", PAL_MONO)
    print()
    print("[黑白反白版 - 白底黑图]")
    make_square(800, PAL_MONO_INV, True, 0.320, 16, "logo_abstract_mono_inv.png")

    # ── 篆书版 (需自备篆体字体) ──
    print()
    seal = find_seal_font()
    if seal:
        print("[篆书版] 使用字体: %s" % seal)
        make_square(800, PAL_MAIN, True, 0.320, 16, "logo_abstract_seal.png",
                    text_font=seal)
        make_square(800, PAL_MONO, True, 0.320, 16, "logo_abstract_seal_mono.png",
                    text_font=seal)
        make_device("logo_160x128_seal.png", PAL_DEV, text_font=seal)
    else:
        print("[篆书版] 未找到篆体字体, 已跳过。")
        print("         系统默认不带小篆字体。安装方法:")
        print("         1) 下载任意小篆 ttf (方正小篆体 / 汉仪篆书 / 叶根友篆书 ...)")
        print("         2) 双击安装, 或直接放到 utils/fonts/seal.ttf")
        print("         3) 重跑本脚本, 会自动生成 *_seal.png")
    print()
    print("完成。注意: 本脚本不会生成/覆盖 logo.png。")
    print("   彩色版烧录: & \"C:/Program Files/Python311/python.exe\" utils/logo_converter.py utils/logo_160x128.png")
    print("   黑白版烧录: & \"C:/Program Files/Python311/python.exe\" utils/logo_converter.py utils/logo_160x128_mono.png")
    print("   自己的图:   & \"C:/Program Files/Python311/python.exe\" utils/logo_converter.py utils/logo.png")


if __name__ == "__main__":
    main()
