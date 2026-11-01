# C6-MicroLinux (`c6os`) — Waveshare ESP32-C6-LCD-1.47

A custom, lightweight Linux-style micro-operating system and native **RFC-compliant SSHv2 server (`sshd`)** built from source for the **Waveshare ESP32-C6-LCD-1.47** (`ESP32-C6FH4` 160 MHz 32-bit RISC-V, 320 KB SRAM, 8 MB SPI Flash, 1.47" 320×172 ST7789 IPS LCD).

Users can **SSH directly into the ESP32-C6** over Wi-Fi (`ssh root@<ip>`) or attach over USB-Serial (`/dev/ttyACM0`) to control the 1.47" LCD screen and onboard WS2812 RGB LED through an **interactive ANSI CLI Menu (`c6-menu`)** or a **POSIX-like shell (`c6-sh`)** with virtual `/proc`, `/sys`, and `/dev` filesystems.

---

## Architecture & Key Features

| Subsystem | Implementation |
| :--- | :--- |
| **Target SoC & Display** | `ESP32-C6FH4` (RV32IMAC @ 160 MHz, 320 KB SRAM, 8 MB DIO Flash) + Waveshare 1.47" ST7789 (`320x172` RGB565 @ 80 MHz SPI2) |
| **SSHv2 Daemon (`sshd`)** | Built from source ([`src/ssh_server.cpp`](src/ssh_server.cpp), [`lib/c6os_core/src/ssh_packet.cpp`](lib/c6os_core/src/ssh_packet.cpp)) on TCP Port `22` using `libsodium` + `mbedtls` (`curve25519-sha256` KEX, `ssh-ed25519` host key in NVS, `aes128-ctr` encryption, `hmac-sha2-256` MAC, RFC 8308 `SSH_MSG_EXT_INFO`). Supports concurrent multi-session SSH, `password` (`root` / `esp32c6`), and `publickey` authentication (including `ProxyJump`). |
| **Interactive CLI Menu (`c6-menu`)** | Launches automatically upon SSH or Serial login ([`lib/c6os_core/src/c6os_shell.cpp`](lib/c6os_core/src/c6os_shell.cpp)). Single-keystroke control (`[1]`–`[9]`, `[+]`/`[-]`, `[m]`, `[k]`, `[a]`, `[t]`, `[s]`, `[q]`) over LCD modes, on-LCD graphical telemetry monitor, scrolling marquee ticker, flashing alert banner, custom headline/subtitle text, color themes, PWM backlight, 180° flip, WS2812 RGB LED, live `/dev/tty0` terminal mirroring, `htop`, and Wi-Fi setup. |
| **POSIX-like Shell (`c6-sh`) & System Tools** | Built-in commands: `top`, `htop`, `watch`, `crontab`, `sensors`, `pinout`, `gpio`, `fbgrab`, `fbi`, `edit`/`nano`, `sh`, `logger`, `date`, `ntpdate`, `ping`, `netstat`/`ss`, `lscpu`, `vmstat`, `neofetch`, `menu`, `screen`, `wifi`, `uname`, `uptime`, `free`, `df`, `ps`, `dmesg`, `ls`, `cd`, `pwd`, `cat`, `echo`, `rm`, `ip`/`ifconfig`, `hostname`, `passwd`, `reboot`, with output redirection (`>` / `>>`) and command chaining (`&&` / `;`). |
| **Virtual Filesystem (`VirtualFs`)** | Synthetic `/proc` (`version`, `cpuinfo`, `meminfo`, `uptime`, `loadavg`, `net/dev`, `net/wireless`), `/sys/class/*` (`thermal/thermal_zone0/temp`, `backlight/st7789/brightness`, `graphics/fb0/{mode,theme,text,ticker,alert}`, `leds/ws2812/{mode,rgb}`, `gpio/status`), `/dev/tty0` framebuffer console, and persistent `/etc/*` (`/etc/rc.local`, `/etc/crontab`) & `/root/*` on LittleFS ([`lib/c6os_core/src/c6os_vfs.cpp`](lib/c6os_core/src/c6os_vfs.cpp)). |
| **6 LCD Render Modes & Overlays** | `dashboard` (OS & SSH status cards), `console` (live 52×18 `/dev/tty0` Linux framebuffer terminal), `signboard` (full-screen custom typography banner), `test` (SMPTE color bars & hardware diagnostics), `monitor` (graphical on-LCD `htop` with CPU sparkline, SRAM bar, °C temp & process table), and `image` (`fbi` artwork viewer), plus a scrolling bottom marquee ticker and flashing alert banner ([`src/screen_manager.cpp`](src/screen_manager.cpp)). |
| **Network Daemons & Hardware Daemon** | `sshd` (`TCP :22`), `httpd` REST/Webhook API (`TCP :80` — `GET /api/status`, `GET /api/cmd?q=...`), `syslogd` listener (`UDP :514`), `ntpd` clock sync, and `buttond` (`GPIO 9` BOOT button short-press mode/alert ack, long-press backlight toggle) ([`src/main.cpp`](src/main.cpp)). |

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

## Controlling the Board & Screen via CLI, `/sys`, HTTP, and Syslog

Inside `c6-sh`, you can control hardware using built-in utilities or standard Linux device/sysfs redirection:

```bash
# Switch LCD rendering mode (dashboard | console | signboard | test | monitor | image)
screen mode monitor
echo signboard > /sys/class/graphics/fb0/mode

# Set scrolling bottom marquee ticker or flashing alert overlay
screen ticker "C6-MicroLinux 6.12 | Live Telemetry"
screen alert "HIGH CPU TEMP"

# Display graphical artwork on the 320x172 LCD or grab an ASCII/PPM screenshot
fbi tux "Linux on ESP32-C6"
fbgrab --ascii

# Hardware sensors, board pinout diagram, and GPIO/PWM control
sensors
pinout
gpio status
gpio pwm 22 128

# Schedule periodic cron jobs or watch a command live
crontab -a 30 logger "Cron heartbeat"
watch -n 2 sensors

# Edit startup scripts (/etc/rc.local) and run shell scripts
edit /etc/rc.local
sh /etc/rc.local

# Control the onboard WS2812 RGB LED and LCD PWM backlight
echo 80 > /sys/class/backlight/st7789/brightness
echo "0 255 128" > /sys/class/leds/ws2812/rgb
echo rainbow > /sys/class/leds/ws2812/mode
```

### Remote HTTP Webhook API (`:80`) & UDP Syslog (`:514`)

```bash
# Query full JSON system telemetry over HTTP
curl -s http://<BOARD_IP>/api/status

# Execute any c6-sh command remotely over HTTP
curl -s "http://<BOARD_IP>/api/cmd?q=sensors"

# Stream a syslog message to the ESP32-C6 (logged to dmesg and /dev/tty0)
echo "<14>Server deploy complete" > /dev/udp/<BOARD_IP>/514
```

---

## Building, Testing & Flashing

```bash
# Run host-native unit tests (VFS, SSHv2 packet framing, CLI menu & shell)
pio test -e native

# Compile firmware for Waveshare ESP32-C6-LCD-1.47
pio run -e waveshare_esp32c6_lcd_147

# Flash firmware to /dev/ttyACM0
pio run -e waveshare_esp32c6_lcd_147 -t upload
```

> **Note:** The previous Transit & Weather dashboard firmware is archived in [`legacy/transit-dashboard/`](legacy/transit-dashboard).
