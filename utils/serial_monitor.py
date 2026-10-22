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


def monitor(port, baud, reset_on_start=True):
    print("连接 %s @ %d baud ... (Ctrl+C 退出)" % (port, baud))
    if reset_on_start:
        print("首次连接会复位芯片，以捕获完整开机日志。"
              "若只想看真实连续运行时间，加 --no-reset")
    print("")
    first = True
    while True:
        try:
            # ★ 必须先在构造时屏蔽 DTR/RTS：
            #   ESP32-C3 原生 USB 上，打开端口的瞬间拉 DTR/RTS 就会复位芯片。
            #   不复位时才能看到真实递增的 [HEALTH] up=..m。
            ser = serial.Serial()
            ser.port = port
            ser.baudrate = baud
            ser.timeout = 1
            ser.dtr = False
            ser.rts = False
            ser.open()
            try:
                if first and reset_on_start:
                    # 只在第一次敲复位，保证从开机日志开始看；
                    # 之后重连【千万不要】再敲——那会掩盖真实的重启原因。
                    try:
                        ser.setDTR(False)
                        ser.setRTS(True)
                        time.sleep(0.1)
                        ser.setRTS(False)
                    except (OSError, IOError):
                        pass
                elif not first:
                    print("[monitor] 已重新连上 %s" % port)
                first = False

                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    if line:
                        # 带时间戳，便于判断「跑了多久才死机」
                        print("%s %s" % (time.strftime("%H:%M:%S"), line))
            finally:
                ser.close()
        except serial.SerialException as exc:
            # ★ 关键：ESP32-C3 用原生 USB-Serial/JTAG 时，芯片一旦复位
            #   USB 设备会【整体消失再枚举】——串口句柄失效，连接断开。
            #   旧版脚本在这里直接 sys.exit 退出，正好把「死机重启」
            #   那一刻最关键的 [BOOT] reset= 日志丢掉了。
            #   现在改为自动重连，等 USB 重新枚举后继续抓。
            print("[monitor] 串口断开 (%s)，等待设备重新枚举..." % exc)
            time.sleep(1.0)
            continue
        except KeyboardInterrupt:
            print("")
            print("已停止监视。")
            return


if __name__ == "__main__":
    args = [a for a in sys.argv if a != "--no-reset"]
    no_reset = "--no-reset" in sys.argv
    port = pick_port(args)
    baud = int(args[2]) if len(args) > 2 else BAUD_RATE
    monitor(port, baud, reset_on_start=not no_reset)
