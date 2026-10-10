# -*- coding: utf-8 -*-
"""
serial_monitor.py - 简易串口监视器

比 pio device monitor 轻量，直接看固件的统计行：
    T:目标数  F:帧率  RAW:原始字节数  BAUD:锁定的波特率
其中 RAW 一直是 0 就说明雷达根本没数据进来。

用法:
    python utils/serial_monitor.py            # 自动探测，只有一个串口时直接用
    python utils/serial_monitor.py COM30      # 指定串口
    python utils/serial_monitor.py COM30 115200

依赖: pip install pyserial
"""

import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial，请先安装: pip install pyserial")
    sys.exit(1)

BAUD_RATE = 115200


def list_available_ports():
    """返回 [(device, description), ...]，优先列出 USB 串口。"""
    ports = []
    for p in list_ports.comports():
        ports.append((p.device, p.description or ""))
    # ESP32 原生 USB-Serial/JTAG 一般描述里带 USB
    ports.sort(key=lambda x: ("usb" not in x[1].lower(), x[0]))
    return ports


def pick_port(argv):
    """命令行参数 > 唯一串口 > 交互选择。"""
    if len(argv) > 1:
        return argv[1]

    ports = list_available_ports()
    if not ports:
        print("没有探测到任何串口。")
        print("  · 检查 USB 线（必须是数据线，不是纯充电线）")
        print("  · Windows: 设备管理器 → 端口 (COM 和 LPT)")
        sys.exit(1)

    if len(ports) == 1:
        print("自动选择: %s  %s" % ports[0])
        return ports[0][0]

    print("检测到多个串口:")
    for i, (dev, desc) in enumerate(ports):
        print("  [%d] %-8s %s" % (i + 1, dev, desc))
    sel = input("请选择序号 (默认 1): ").strip() or "1"
    try:
        idx = int(sel) - 1
        if not 0 <= idx < len(ports):
            raise ValueError
    except ValueError:
        print("无效的序号: %s" % sel)
        sys.exit(1)
    return ports[idx][0]


def monitor(port, baud):
    print("连接 %s @ %d baud ... (Ctrl+C 退出)" % (port, baud))
    print("")
    try:
        with serial.Serial(port, baud, timeout=1) as ser:
            # 敲一下复位，保证从开机日志开始看
            try:
                ser.setDTR(False)
                ser.setRTS(True)
                time.sleep(0.1)
                ser.setRTS(False)
            except (OSError, IOError):
                pass

            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    print(line)
    except serial.SerialException as exc:
        print("")
        print("串口错误: %s" % exc)
        print("  · 确认没有别人占用这个串口（别同时开着 pio device monitor）")
        print("  · 确认串口号没变（拔插后编号可能改变）")
        sys.exit(1)
    except KeyboardInterrupt:
        print("")
        print("已停止监视。")


if __name__ == "__main__":
    port = pick_port(sys.argv)
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else BAUD_RATE
    monitor(port, baud)
