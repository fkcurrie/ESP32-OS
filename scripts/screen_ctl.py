#!/usr/bin/env python3
"""Host CLI controller for the Waveshare ESP32-C6-LCD-1.47 USB device."""

import argparse
import json
import math
import os
import socket
import sys
import termios
import time


def configure_tty(fd: int) -> None:
    """Configure /dev/ttyACM0 for raw 115200 8N1 non-blocking communication."""
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL  # cflag
    attrs[3] = 0  # lflag
    attrs[4] = termios.B115200  # ispeed
    attrs[5] = termios.B115200  # ospeed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def send_command_fd(fd: int, command: str, timeout_s: float = 1.0) -> str:
    """Send a single-line command on an already-open serial fd and return its reply."""
    while True:
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break

    payload = (command.strip() + "\n").encode("utf-8")
    os.write(fd, payload)

    t0 = time.monotonic()
    buf = b""
    while time.monotonic() - t0 < timeout_s:
        try:
            chunk = os.read(fd, 4096)
            if chunk:
                buf += chunk
                if b"\n" in buf:
                    break
        except BlockingIOError:
            time.sleep(0.002)

    for raw_line in buf.decode("utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if line:
            return line
    return ""


def send_command(port: str, command: str, timeout_s: float = 2.0) -> str:
    """Send a single-line command to the ESP32-C6 and return its single-line reply."""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_tty(fd)
        return send_command_fd(fd, command, timeout_s=timeout_s)
    finally:
        os.close(fd)


def read_cpu_times() -> tuple[int, int]:
    """Read cumulative (idle, total) CPU jiffies from /proc/stat."""
    with open("/proc/stat", "r", encoding="utf-8") as f:
        parts = [int(x) for x in f.readline().split()[1:]]
    idle = parts[3] + (parts[4] if len(parts) > 4 else 0)
    total = sum(parts)
    return idle, total


def read_ram_pct() -> int:
    """Read current Linux RAM usage percentage from /proc/meminfo."""
    mem_total = 1
    mem_avail = 0
    with open("/proc/meminfo", "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("MemTotal:"):
                mem_total = int(line.split()[1])
            elif line.startswith("MemAvailable:"):
                mem_avail = int(line.split()[1])
    return max(0, min(100, int(round(100.0 * (mem_total - mem_avail) / max(1, mem_total)))))


def get_primary_ip() -> str:
    """Resolve primary outbound IPv4 address."""
    ip_addr = "127.0.0.1"
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.168.100.200", 80))
            ip_addr = s.getsockname()[0]
    except OSError:
        pass
    return ip_addr


def read_host_metrics(prev_cpu: tuple[int, int] | None = None) -> tuple[int, int, str, tuple[int, int]]:
    """Read Linux host CPU usage %, RAM usage %, primary IPv4, and updated CPU counters."""
    if prev_cpu is None:
        idle1, total1 = read_cpu_times()
        time.sleep(0.08)
    else:
        idle1, total1 = prev_cpu

    idle2, total2 = read_cpu_times()
    d_total = max(1, total2 - total1)
    cpu_pct = max(0, min(100, int(round(100.0 * (d_total - (idle2 - idle1)) / d_total))))
    ram_pct = read_ram_pct()
    ip_addr = get_primary_ip()
    return cpu_pct, ram_pct, ip_addr, (idle2, total2)


def main() -> int:
    parser = argparse.ArgumentParser(description="Control Waveshare ESP32-C6-LCD-1.47 over USB Serial")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="Serial device path (default: /dev/ttyACM0)")

    sub = parser.add_subparsers(dest="subcmd", required=True)
    sub.add_parser("ping", help="Send PING and verify PONG")
    sub.add_parser("status", help="Query device JSON status")
    sub.add_parser("scan", help="Trigger an immediate Wi-Fi / BLE scan")
    p_flip = sub.add_parser("flip", help="Flip landscape orientation 180 degrees")
    p_flip.add_argument("state", nargs="?", choices=["0", "1"], help="Optional explicit flip state (0 or 1)")

    p_text = sub.add_parser("text", help="Display custom banner text on the LCD")
    p_text.add_argument("message", help="Message string to display")

    p_bl = sub.add_parser("bl", help="Set LCD backlight brightness (0..100)")
    p_bl.add_argument("percent", type=int, help="Backlight percentage (0..100)")

    p_view = sub.add_parser("view", help="Switch active LCD screen")
    p_view.add_argument("name", choices=["dashboard", "host", "wireless"], help="Target view")

    p_mode = sub.add_parser("mode", help="Set onboard WS2812 RGB LED mode")
    p_mode.add_argument("name", choices=["rainbow", "pulse", "solid", "off"], help="RGB LED mode")

    p_rgb = sub.add_parser("rgb", help="Set solid WS2812 RGB LED color")
    p_rgb.add_argument("r", type=int, help="Red (0..255)")
    p_rgb.add_argument("g", type=int, help="Green (0..255)")
    p_rgb.add_argument("b", type=int, help="Blue (0..255)")

    p_host = sub.add_parser("host", help="Send Linux host CPU/RAM/IP metrics to the screen")
    p_host.add_argument("--watch", action="store_true", help="Continuously stream metrics at high FPS")
    p_host.add_argument("--fps", type=float, default=15.0, help="Refresh rate in Hz when --watch is enabled (default: 15)")

    p_demo = sub.add_parser("demo", help="Run a smooth high-FPS live animation demo on the LCD")
    p_demo.add_argument("--seconds", type=float, default=5.0, help="Duration of the demo in seconds (default: 5.0)")
    p_demo.add_argument("--fps", type=float, default=20.0, help="Target animation frame rate (default: 20.0)")

    args = parser.parse_args()

    if args.subcmd == "ping":
        print(send_command(args.port, "PING"))
    elif args.subcmd == "status":
        reply = send_command(args.port, "STATUS")
        try:
            parsed = json.loads(reply)
            print(json.dumps(parsed, indent=2))
        except json.JSONDecodeError:
            print(reply)
    elif args.subcmd == "scan":
        print(send_command(args.port, "SCAN"))
    elif args.subcmd == "flip":
        cmd = f"FLIP {args.state}" if args.state is not None else "FLIP"
        print(send_command(args.port, cmd))
    elif args.subcmd == "text":
        print(send_command(args.port, f"TEXT {args.message}"))
    elif args.subcmd == "bl":
        print(send_command(args.port, f"BL {args.percent}"))
    elif args.subcmd == "view":
        print(send_command(args.port, f"VIEW {args.name}"))
    elif args.subcmd == "mode":
        print(send_command(args.port, f"MODE {args.name}"))
    elif args.subcmd == "rgb":
        print(send_command(args.port, f"RGB {args.r} {args.g} {args.b}"))
    elif args.subcmd == "host":
        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            configure_tty(fd)
            send_command_fd(fd, "VIEW host")
            prev_cpu = None
            interval = 1.0 / max(1.0, min(30.0, args.fps))
            while True:
                t_start = time.monotonic()
                cpu, ram, ip, prev_cpu = read_host_metrics(prev_cpu)
                reply = send_command_fd(fd, f"HOST {cpu} {ram} {ip}")
                if not args.watch:
                    print(reply)
                    break
                elapsed = time.monotonic() - t_start
                if elapsed < interval:
                    time.sleep(interval - elapsed)
        finally:
            os.close(fd)
    elif args.subcmd == "demo":
        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            configure_tty(fd)
            send_command_fd(fd, "VIEW host")
            ip = get_primary_ip()
            fps = max(1.0, min(30.0, args.fps))
            interval = 1.0 / fps
            t0 = time.monotonic()
            frames = 0
            while time.monotonic() - t0 < args.seconds:
                t_frame = time.monotonic()
                phase = (t_frame - t0) * 3.2
                cpu_sim = int(round(50.0 + 48.0 * math.sin(phase)))
                ram_sim = int(round(50.0 + 48.0 * math.cos(phase * 0.85)))
                send_command_fd(fd, f"HOST {cpu_sim} {ram_sim} {ip}")
                frames += 1
                elapsed = time.monotonic() - t_frame
                if elapsed < interval:
                    time.sleep(interval - elapsed)
            total_dt = max(0.001, time.monotonic() - t0)
            cpu, ram, ip, _ = read_host_metrics()
            send_command_fd(fd, f"HOST {cpu} {ram} {ip}")
            print(f"OK DEMO frames={frames} avg_fps={frames / total_dt:.1f}")
        finally:
            os.close(fd)

    return 0


if __name__ == "__main__":
    sys.exit(main())

