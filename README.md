# C6-MicroLinux (`c6os`) — Waveshare ESP32-C6-LCD-1.47

A custom, lightweight Linux-style micro-operating system and native **RFC-compliant SSHv2 server (`sshd`)** built from source for the **Waveshare ESP32-C6-LCD-1.47** (`ESP32-C6FH4` 160 MHz 32-bit RISC-V, 320 KB SRAM, 8 MB SPI Flash, 1.47" 320×172 ST7789 IPS LCD).

Users can **SSH directly into the ESP32-C6** over Wi-Fi (`ssh root@<ip>`) or attach over USB-Serial (`/dev/ttyACM0`) to control the 1.47" LCD screen and onboard WS2812 RGB LED through an **interactive ANSI CLI Menu (`c6-menu`)** or a **POSIX-like shell (`c6-sh`)** with virtual `/proc`, `/sys`, and `/dev` filesystems.

---

## Architecture & Key Features

| Subsystem | Implementation |
| :--- | :--- |
| **Target SoC & Display** | `ESP32-C6FH4` (RV32IMAC @ 160 MHz, 320 KB SRAM, 8 MB DIO Flash) + Waveshare 1.47" ST7789 (`320x172` RGB565 @ 80 MHz SPI2) |
| **SSHv2 Daemon (`sshd`)** | Built from source ([ssh_server.cpp](file:///home/fcurrie/Projects/small-led-screen/src/ssh_server.cpp), [ssh_packet.cpp](file:///home/fcurrie/Projects/small-led-screen/lib/c6os_core/src/ssh_packet.cpp)) on TCP Port `22` using `libsodium` + `mbedtls` (`curve25519-sha256` KEX, `ssh-ed25519` host key in NVS, `aes128-ctr` encryption, `hmac-sha2-256` MAC). Supports both `password` (`root` / `esp32c6`) and `publickey` authentication. |
| **Interactive CLI Menu (`c6-menu`)** | Launches automatically upon SSH or Serial login ([c6os_shell.cpp](file:///home/fcurrie/Projects/small-led-screen/lib/c6os_core/src/c6os_shell.cpp)). Single-keystroke control (`[1]`–`[9]`, `[+]`/`[-]`, `[s]`, `[q]`) over LCD modes, custom headline/subtitle text, color themes, PWM backlight, 180° flip, WS2812 RGB LED, live `/dev/tty0` terminal mirroring, and Wi-Fi setup. |
| **POSIX-like Shell (`c6-sh`)** | Built-in commands: `menu`, `screen`, `wifi`, `uname`, `uptime`, `free`, `df`, `ps`, `dmesg`, `ls`, `cd`, `pwd`, `cat`, `echo`, `rm`, `ip`/`ifconfig`, `hostname`, `passwd`, `reboot`, with output redirection (`>` / `>>`) and command chaining (`&&` / `;`). |
| **Virtual Filesystem (`VirtualFs`)** | Synthetic `/proc` (`version`, `cpuinfo`, `meminfo`, `uptime`, `loadavg`, `net/dev`, `net/wireless`), `/sys/class/*` (`backlight/st7789/brightness`, `graphics/fb0/{mode,theme,text}`, `leds/ws2812/{mode,rgb}`), `/dev/tty0` framebuffer console, and persistent `/etc/*` & `/root/*` on LittleFS ([c6os_vfs.cpp](file:///home/fcurrie/Projects/small-led-screen/lib/c6os_core/src/c6os_vfs.cpp)). |
| **4 LCD Render Modes** | `dashboard` (OS & SSH status cards), `console` (live 52×18 `/dev/tty0` Linux framebuffer terminal), `signboard` (full-screen custom typography banner), and `test` (SMPTE color bars & hardware diagnostics) ([screen_manager.cpp](file:///home/fcurrie/Projects/small-led-screen/src/screen_manager.cpp)). |

---

## Connecting via SSH or USB-Serial

### 1. Provision 2.4 GHz Wi-Fi (or Use Built-in SoftAP)

By default, `C6-MicroLinux` broadcasts a fallback SoftAP (`SSID: C6-Linux-OS`, Password: `esp32linux`, IP: `192.168.4.1`) and also supports connecting to your 2.4 GHz LAN Wi-Fi network:

```bash
# Configure 2.4 GHz Wi-Fi via USB-Serial (/dev/ttyACM0)
python3 scripts/c6os_ctl.py wifi "<YOUR_2.4GHZ_SSID>" "<YOUR_PASSWORD>"
```

Once connected, the LCD header and SSH card automatically display the board's IPv4 address.

### 2. SSH into the ESP32-C6

```bash
# Interactive CLI Screen Control Menu over SSH (Default password: esp32c6)
ssh root@<BOARD_IP>

# Or execute non-interactive commands over SSH
ssh root@<BOARD_IP> "screen mode signboard && screen text 'BUILD PASSED' 'All checks green'"
ssh root@<BOARD_IP> "echo 'Hello from SSH!' >> /dev/tty0 && screen mode console"
```

### 3. USB-Serial Console (`/dev/ttyACM0`)

```bash
# Attach to the interactive ANSI Screen Control Menu over USB-Serial
python3 scripts/c6os_ctl.py interactive

# Query live JSON status
python3 scripts/c6os_ctl.py status

# Execute a shell command over USB-Serial
python3 scripts/c6os_ctl.py exec "uname -a && cat /proc/meminfo"
```

---

## Controlling the Screen via `/sys` and `/dev`

Inside `c6-sh`, you can control hardware using either the `screen` utility or standard Linux device/sysfs redirection:

```bash
# Switch LCD rendering mode (dashboard | console | signboard | test)
echo signboard > /sys/class/graphics/fb0/mode

# Change color theme (cyan | matrix | amber | red | mono)
echo matrix > /sys/class/graphics/fb0/theme

# Adjust LCD PWM backlight (0 - 100%)
echo 80 > /sys/class/backlight/st7789/brightness

# Write lines directly to the LCD framebuffer terminal (/dev/tty0)
echo " console > /sys/class/graphics/fb0/mode"
echo "[OK] Deploying C6-MicroLinux..." > /dev/tty0
echo "Kernel: $(uname -r)" >> /dev/tty0

# Control the onboard WS2812 RGB LED
echo "0 255 128" > /sys/class/leds/ws2812/rgb
echo rainbow > /sys/class/leds/ws2812/mode
```

---

## Building, Testing & Flashing

```bash
# Run host-native unit tests (VFS, SSHv2 packet framing, CLI menu & shell)
pio test -e native

# Compile firmware for Waveshare ESP32-C6-LCD-1.47
pio run -e waveshare_esp32c6_lcd_1.47

# Flash firmware to /dev/ttyACM0
pio run -e waveshare_esp32c6_lcd_147 -t upload
```

> **Note:** The previous Transit & Weather dashboard firmware is archived in [legacy/transit-dashboard/](file:///home/fcurrie/Projects/small-led-screen/legacy/transit-dashboard).
