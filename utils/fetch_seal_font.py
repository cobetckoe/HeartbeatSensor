# -*- coding: utf-8 -*-
"""
fetch_seal_font.py - 下载小篆字体（崇羲篆體）到 utils/fonts/

为什么需要这个脚本
─────────────────
字体文件有 21.8 MB，不适合塞进 Git 仓库（见 .gitignore 里
`utils/fonts/*.otf` 规则）。所以克隆仓库后需要跑一次本脚本把它取回来，
utils/make_logo.py 才能生成篆书版 LOGO；没有它，其它版本（黑体）也照常生成。

字体来源
────────
崇羲篆體 (ChongXi Small Seal) —— 中央研究院 數位文化中心
通过 npm 包 @fontpkg/chong-xi-small-seal 分发（registry.npmjs.org）

用法
────
    python utils/fetch_seal_font.py            # 下载（已存在则跳过）
    python utils/fetch_seal_font.py --force    # 强制重新下载
    python utils/fetch_seal_font.py --version 1.0.0

依赖
────
仅标准库（urllib + tarfile），不需要 pip 安装任何东西。
"""

import argparse
import io
import json
import os
import sys
import tarfile
import urllib.request

# ── 配置 ──────────────────────────────────────────────────────────────
NPM_PACKAGE = "@fontpkg/chong-xi-small-seal"
REGISTRY = "https://registry.npmjs.org"

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "fonts")
OUT_NAME = "ChongXiSmallSeal.otf"
FONT_EXTS = (".otf", ".ttf", ".ttc")


def http_get(url, timeout=60):
    """带上 UA 的简单 GET，返回 bytes。"""
    req = urllib.request.Request(
        url, headers={"User-Agent": "kanchuan-fetch-seal-font/1.0"}
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def main():
    ap = argparse.ArgumentParser(
        description="下载小篆字体（崇羲篆體）到 utils/fonts/"
    )
    ap.add_argument("--force", action="store_true",
                    help="即使文件已存在也重新下载")
    ap.add_argument("--version", default=None,
                    help="指定 npm 包版本，默认取 latest")
    args = ap.parse_args()

    out_path = os.path.join(OUT_DIR, OUT_NAME)

    if os.path.isfile(out_path) and not args.force:
        print("已存在，跳过下载：")
        print("  %s  (%s 字节)" % (out_path, format(os.path.getsize(out_path), ",")))
        print("如需重新下载，加 --force")
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)

    meta_url = "%s/%s" % (REGISTRY, NPM_PACKAGE)
    print("查询 npm registry ...")
    print("  %s" % meta_url)
    try:
        meta = json.loads(http_get(meta_url).decode("utf-8"))
    except Exception as exc:
        print("查询失败：%s" % exc, file=sys.stderr)
        print("", file=sys.stderr)
        print("可以手动下载后放到 %s" % out_path, file=sys.stderr)
        print("（文件名必须叫 %s 才能被 make_logo.py 认出来）" % OUT_NAME,
              file=sys.stderr)
        return 1

    version = args.version or meta.get("dist-tags", {}).get("latest")
    if not version:
        print("registry 没有返回可用的版本号", file=sys.stderr)
        return 1
    if version not in meta.get("versions", {}):
        print("registry 里没有版本 %s" % version, file=sys.stderr)
        return 1

    tarball = meta["versions"][version]["dist"]["tarball"]
    print("版本    : %s" % version)
    print("下载    : %s" % tarball)

    try:
        blob = http_get(tarball, timeout=300)
    except Exception as exc:
        print("下载失败：%s" % exc, file=sys.stderr)
        return 1

    print("已下载  : %s 字节，解包中 ..." % format(len(blob), ","))

    try:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tf:
            members = [
                m for m in tf.getmembers()
                if m.isfile() and m.name.lower().endswith(FONT_EXTS)
            ]
            if not members:
                print("压缩包里没找到字体文件（%s）" % ", ".join(FONT_EXTS),
                      file=sys.stderr)
                return 1
            # 包里常有多个子集，取体积最大的那个（最完整）
            members.sort(key=lambda m: m.size, reverse=True)
            chosen = members[0]
            print("选用    : %s  (%s 字节)"
                  % (chosen.name, format(chosen.size, ",")))
            data = tf.extractfile(chosen).read()
    except Exception as exc:
        print("解包失败：%s" % exc, file=sys.stderr)
        return 1

    with open(out_path, "wb") as f:
        f.write(data)

    print("")
    print("已保存  : %s  (%s 字节)"
          % (out_path, format(len(data), ",")))
    print("下一步  : python utils/make_logo.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
