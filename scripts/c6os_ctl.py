#!/usr/bin/env python3
"""
C6-MicroLinux (c6os) Host Control & Provisioning Utility
========================================================
Interacts with the Waveshare ESP32-C6-LCD-1.47 running C6-MicroLinux over
USB-Serial (/dev/ttyACM0) or verifies SSH readiness.

Usage examples:
  python3 scripts/c6os_ctl.py status
  python3 scripts/c6os_ctl.py exec "uname -a && cat /proc/cpuinfo"
  python3 scripts/c6os_ctl.py exec "screen mode signboard && screen text 'C6 LINUX' 'SSH Ready'"
  python3 scripts/c6os_ctl.py menu-key 1
  python3 scripts/c6os_ctl.py wifi "My24GHzSSID" "MyPassword"
  python3 scripts/c6os_ctl.py interactive
"""

import argparse
import json
import os
import re
import select
import sys
import termios
import time
import tty

try:
    import serial
except ImportError:
    print("Error: pyserial is required (pip install pyserial)", file=sys.stderr)
    sys.exit(1)

ANSI_ESCAPE_RE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")


def strip_ansi(text: str) -> str:
    return ANSI_ESCAPE_RE.sub("", text)


def open_serial(port: str, baud: int = 115200, timeout: float = 0.25) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = timeout
    ser.dtr = True
    ser.rts = False
    ser.open()
    return ser


def drain(ser: serial.Serial, wait_s: float = 0.08) -> bytes:
    time.sleep(wait_s)
    chunks = []
    while ser.in_waiting:
        chunks.append(ser.read(ser.in_waiting))
        time.sleep(0.02)
    return b"".join(chunks)


def enter_shell_mode(ser: serial.Serial) -> None:
    """Deterministically transitions the session on /dev/ttyACM0 into c6-sh."""
    drain(ser, 0.03)
    ser.write(b"\x03")
    ser.flush()
    drain(ser, 0.06)
    ser.write(b"s\r")
    ser.flush()
    drain(ser, 0.10)


def enter_menu_mode(ser: serial.Serial) -> str:
    """Deterministically transitions the session into the interactive c6-menu TUI."""
    enter_shell_mode(ser)
    ser.write(b"menu\r")
    ser.flush()
    raw = drain(ser, 0.20)
    return raw.decode("utf-8", errors="replace")


def run_shell_command(ser: serial.Serial, cmd: str, wait_s: float = 0.35, return_to_menu: bool = True) -> str:
    enter_shell_mode(ser)
    ser.write(f"{cmd}\r".encode())
    ser.flush()

    deadline = time.time() + wait_s
    buf = bytearray()
    while time.time() < deadline:
        if ser.in_waiting:
            buf.extend(ser.read(ser.in_waiting))
        else:
            time.sleep(0.02)

    if return_to_menu:
        ser.write(b"menu\r")
        ser.flush()
        drain(ser, 0.08)

    text = buf.decode("utf-8", errors="replace")
    lines = text.splitlines()
    cleaned = []
    for idx, line in enumerate(lines):
        plain = strip_ansi(line).strip()
        if idx == 0 and plain == cmd.strip():
            continue
        if plain.startswith("root@") and plain.endswith("#"):
            continue
        cleaned.append(line)
    return "\n".join(cleaned).strip()


def cmd_status(port: str) -> int:
    with open_serial(port) as ser:
        out = run_shell_command(ser, "screen status", wait_s=0.35)
        for line in out.splitlines():
            line_s = strip_ansi(line).strip()
            if line_s.startswith("{") and line_s.endswith("}"):
                parsed = json.loads(line_s)
                print(json.dumps(parsed, indent=2))
                return 0
        print(out)
    return 0


def cmd_exec(port: str, command: str, raw_ansi: bool = False) -> int:
    with open_serial(port) as ser:
        out = run_shell_command(ser, command, wait_s=0.50)
        print(out if raw_ansi else strip_ansi(out))
    return 0


def cmd_menu_key(port: str, key: str, raw_ansi: bool = False) -> int:
    with open_serial(port) as ser:
        enter_menu_mode(ser)
        ser.write(key.encode("utf-8"))
        ser.flush()
        out = drain(ser, 0.25).decode("utf-8", errors="replace")
        print(out if raw_ansi else strip_ansi(out))
    return 0


def cmd_wifi(port: str, ssid: str, password: str) -> int:
    with open_serial(port) as ser:
        cmd = f'wifi connect "{ssid}" "{password}"' if password else f'wifi connect "{ssid}"'
        print(f"[*] Sending Wi-Fi configuration to {port}: SSID='{ssid}'...")
        out = run_shell_command(ser, cmd, wait_s=0.5, return_to_menu=False)
        print(strip_ansi(out))

        print("[*] Waiting for 2.4 GHz Wi-Fi station association & DHCP lease...")
        for _ in range(16):
            time.sleep(1.0)
            status_out = run_shell_command(ser, "screen status", wait_s=0.3, return_to_menu=False)
            for line in status_out.splitlines():
                line_s = strip_ansi(line).strip()
                if line_s.startswith("{") and line_s.endswith("}"):
                    data = json.loads(line_s)
                    if data.get("wifi_connected") and data.get("ip") != "0.0.0.0":
                        ip = data["ip"]
                        print(f"[+] Connected! IPv4: {ip} (RSSI: {data.get('rssi')} dBm)")
                        print("[+] You can now SSH into the board:")
                        print(f"    ssh root@{ip}   (password: esp32c6, or use your SSH key)")
                        enter_menu_mode(ser)
                        return 0
        enter_menu_mode(ser)
        print("[!] Timed out waiting for Wi-Fi connection. Check 2.4 GHz SSID/password or use SoftAP 192.168.4.1.")
        return 1


def cmd_interactive(port: str) -> int:
    with open_serial(port, timeout=0.05) as ser:
        print(f"Connected to {port} (C6-MicroLinux). Press Ctrl+] to detach.\r")
        ser.write(b"\r")
        ser.flush()
        old_tty = termios.tcgetattr(sys.stdin.fileno())
        try:
            tty.setraw(sys.stdin.fileno())
            while True:
                rlist, _, _ = select.select([sys.stdin, ser], [], [], 0.05)
                if sys.stdin in rlist:
                    ch = os.read(sys.stdin.fileno(), 32)
                    if b"\x1d" in ch:  # Ctrl+]
                        break
                    ser.write(ch)
                    ser.flush()
                if ser in rlist:
                    data = ser.read(ser.in_waiting or 1)
                    if data:
                        os.write(sys.stdout.fileno(), data)
        finally:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_tty)
            print("\nDetached from C6-MicroLinux serial console.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="C6-MicroLinux (c6os) Host Control CLI")
    parser.add_argument("-p", "--port", default="/dev/ttyACM0", help="Serial device port (default: /dev/ttyACM0)")
    parser.add_argument("--ansi", action="store_true", help="Preserve ANSI color codes in output")

    sub = parser.add_subparsers(dest="subcmd", required=True)
    sub.add_parser("status", help="Fetch live JSON status from c6os")

    p_exec = sub.add_parser("exec", help="Execute a shell command in c6-sh")
    p_exec.add_argument("command", help="Command line to execute (supports ; and && and >)")

    p_key = sub.add_parser("menu-key", help="Send a keystroke to the interactive Screen Control Menu")
    p_key.add_argument("key", help="Key to send (e.g. 1, 3, +, -, 9)")

    p_wifi = sub.add_parser("wifi", help="Configure and connect to a 2.4 GHz Wi-Fi network")
    p_wifi.add_argument("ssid", help="2.4 GHz Wi-Fi SSID")
    p_wifi.add_argument("password", nargs="?", default="", help="Wi-Fi password (optional for open networks)")

    sub.add_parser("interactive", help="Attach interactively to the serial Screen Control Menu / shell")

    args = parser.parse_args()
    if args.subcmd == "status":
        return cmd_status(args.port)
    if args.subcmd == "exec":
        return cmd_exec(args.port, args.command, raw_ansi=args.ansi)
    if args.subcmd == "menu-key":
        return cmd_menu_key(args.port, args.key, raw_ansi=args.ansi)
    if args.subcmd == "wifi":
        return cmd_wifi(args.port, args.ssid, args.password)
    if args.subcmd == "interactive":
        return cmd_interactive(args.port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
