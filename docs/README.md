# 文档索引

本目录存放所有「细节性」文档。根目录 [`README.md`](../README.md) 只保留
简介 / 硬件 / 接线 / 快速开始 / 工程结构，其余内容按主题拆到这里。

## 按主题

| 文档 | 内容 | 什么时候看 |
|---|---|---|
| [DISPLAY.md](DISPLAY.md) | 屏幕渲染：SPI 配置、脏矩形推帧、分级强制放行、配色分级 | 屏幕花屏 / 帧率低 / 想改画面时 |
| [SERIAL-LOG.md](SERIAL-LOG.md) | 串口日志的每一条格式与判读方法 | 排查任何问题，**第一个该看的地方** |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | 常见问题速查表 | 出问题了先查这张表 |
| [PROTOCOL-433M.md](PROTOCOL-433M.md) | KC433 无线报文协议（脉宽编码 / 帧结构 / 发送节奏） | 改从机通信、或要兼容第三方从机时 |
| [TUYA-DP-SETUP.md](TUYA-DP-SETUP.md) | 涂鸦云 DP 建点、三元组、配网绑定全流程 | 接入涂鸦云时 |
| [LOGO.md](LOGO.md) | LOGO 设计理念、输出清单、脚本用法 | 换 LOGO / 商标注册时 |
| [legacy-snapshots/](legacy-snapshots/) | 已废弃的历史代码快照，**不参与编译** | 只在考古时看 |

## 脚本用法

工具脚本的参数、环境准备、两个 `pio` 的坑：见 [`../utils/README.md`](../utils/README.md)。
从机（Keil C51）构建：`.\utils\build_slave.ps1`，详见根 README 的「工程结构」。

## 改文档的规矩

1. **一份内容只写在一个地方**。其他位置一律用链接，不要复制粘贴。
   复制出来的第二份迟早会和第一份说法不一致 —— 这个仓库已经踩过
   「README 让人去串口看 `RAW:` 计数，而固件从来不打这条日志」的坑。
2. **所有 `.md` 必须是 UTF-8 带 BOM**。PowerShell 5.1 在没有 BOM 时会按
   系统代码页（GBK）读取，中文全是乱码。写完用下面的方式落盘：

   ```powershell
   [System.IO.File]::WriteAllText($p, $s, (New-Object System.Text.UTF8Encoding($true)))
   ```

3. **从机源码（`src/slave/Sources/**`）反过来：必须保持 GBK 无 BOM**。
   Keil C51 按 GBK 解释，加 BOM 或转 UTF-8 都会让中文注释炸成语法错误。
4. 写日志格式说明时，请**直接从代码里粘真实格式串**，不要凭记忆写。
