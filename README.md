# C6-MicroLinux (`c6os`) — Waveshare ESP32-C6-LCD-1.47

**Status:** Active (`c6os 6.12`) • November 2026.

**TL;DR:** Standard ESP32 firmware requires a full recompile and USB reflash for every small script or tool change. We built `C6-MicroLinux` (`c6os`) for the Waveshare `ESP32-C6-LCD-1.47` (`ESP32-C6FH4`, `160 MHz` RISC-V, `320 KB` SRAM, `8 MB` QIO Flash). Engineers log in over SSHv2, push files via SFTP v3, and install Lua 5.4, RISC-V `.elf`, or shell apps via `epkg`. The QIO image uses `54,684 B` of static SRAM and `1,811,472 B` of flash. Because `ESP32-C6` lacks an MMU, apps run in bounded heap sandboxes, leaving multi-key `epkg` signing as our main open question.

---

## 1. System Overview & Measured Footprint

`C6-MicroLinux` turns a single-core RISC-V microcontroller into a live, network-accessible Linux-style node. Operators connect over 2.4 GHz Wi-Fi via `ssh root@<ip>` or attach over USB-Serial on `/dev/ttyACM0`. From there, they control the `320x172` IPS display, onboard `WS2812` LED, BLE 5.0 radio, and general-purpose pins.

At boot, `main.cpp` mounts `LittleFS` for `/etc`, `/root`, and `/opt/bin`, and mounts any FAT32 microSD card on `SPI2_HOST`. Static SRAM dropped from `64,928 B` to `54,684 B`, saving `10,244 B` after replacing `libsodium` with `mbedTLS` and scanline DMA. All user runtimes (`SFTP v3`, `Lua 5.4`, `RV32IMAC` ELF VM, and `NimBLE 5.0`) use `0 B` of idle heap until called.

| Subsystem | Implementation & Measured Bounds |
| :--- | :--- |
| Target Hardware | `ESP32-C6FH4` (`RV32IMAC` @ `160 MHz`, `320 KB` SRAM, `8 MB` QIO Flash @ `80 MHz`) + `1.47"` `ST7789` (`320x172` RGB565) |
| Firmware Size | Static SRAM: `54,684 B / 327,680 B` (`16.7%`); Flash: `1,811,472 B / 3,342,336 B` (`54.2%`) |
| SSHv2 & SFTP v3 (`:22`) | `curve25519-sha256` KEX, `aes128-ctr`, `hmac-sha2-256`, two-way window adjust, `ssh-ed25519` + `rsa-sha2-256/512` key verify against `/etc/ssh/authorized_keys`, 1024-round salted SHA-256 NVS `passwd`, and binary `sftp` |
| User App Runtime | `epkg` (`EPK1` + SHA-256 check), 32-bit `Lua 5.4` (`64 KB` heap cap, `300k`-op hook), `RV32IMAC` `.elf` VM (`48 KB` sandbox), and `.sh` scripts (recursion depth `4`) |
| Display Engine | `esp_lcd_panel_io_spi` @ `80 MHz` on `SPI2_HOST`, `20.4 KB` (`2 x 320 x 16 x 2 B`) DMA scanline buffers, dirty-region flags, and pixel-accurate `320x172` `.bmp`/`.ppm` `fbgrab` |
| Radios & Buses | Event-driven Wi-Fi STA + WPA2 SoftAP (`C6-Linux-OS`), lazy ESP-IDF `NimBLE 5.0` (`ble`), `gpio` (`0..23` non-reserved), `pwm`, `adc` (`GPIO 0..3`), `i2c`, and lwIP ICMP `ping` |
| Telemetry & HTTP (`:80`) | Live FreeRTOS `uxTaskGetSystemState()` in `ps`/`top`, `/proc` & `/sys` VFS, `America/Toronto` POSIX `tz`, read-only `/api/status`, and NVS bearer-gated `/api/cmd` |

---

## 2. Connecting Over Wi-Fi, SSH, and SFTP

On first boot without saved LAN credentials, the board starts a WPA2 SoftAP named `C6-Linux-OS` with password `esp32linux` at `192.168.4.1`. Once the board joins a 2.4 GHz LAN, it turns off the SoftAP unless you pin it on with `wifi ap on`. If LAN link drops for more than eight seconds, the fallback SoftAP turns back on automatically.

```bash
# Set 2.4 GHz LAN Wi-Fi over USB-Serial (/dev/ttyACM0)
python3 scripts/c6os_ctl.py wifi "<YOUR_SSID>" "<YOUR_PASSWORD>"

# Open an interactive login session (default login shell: c6-menu; initial password: esp32c6)
ssh root@<BOARD_IP>

# Run non-interactive shell commands over SSH
ssh root@<BOARD_IP> "uname -a && free && ps"

# Upload or download binary files over SFTP v3
sftp root@<BOARD_IP>
```

Public-key logins check client signatures against `/etc/ssh/authorized_keys` on `LittleFS`. Both standard OpenSSH `ssh-ed25519` or `ssh-rsa` lines and `SHA256:<base64>` key digests work. Run `passwd` inside `c6-sh` to rotate the salted root password in NVS, or `chsh /bin/sh` to boot straight into `c6-sh`.

---

## 3. Packaging & Running User Apps (`epkg`, Lua 5.4, RISC-V ELF)

Developers can ship tools to `/opt/bin` without rebuilding the kernel firmware. Build `.epkg` archives on your workstation with `scripts/mkepkg.py`, transfer them over `sftp`, and install them with `epkg`. Each package stores a SHA-256 payload digest verified before install.

```bash
# Pack a Lua 5.4 app on your host machine
python3 scripts/mkepkg.py -n blink -v 1.0.0 -a lua -e blink.lua -o blink.epkg

# Upload via SFTP and install on the ESP32-C6
ssh root@<BOARD_IP> "epkg install /root/blink.epkg && epkg list && blink"
```

Lua scripts run on a 32-bit `Lua 5.4` VM (`LUA_32BITS=1`) with C bindings for `sys`, `c6`, `gpio`, `wifi`, `ble`, and `fs`. Native C programs include `include/c6os_app_api.h` and compile as relocatable `RV32IMAC` ELF32 objects (`-march=rv32imac -mabi=ilp32 -Wl,-r`). They run inside a `48 KB` sandboxed VM.

```bash
# Run inline Lua 5.4 directly from c6-sh
lua -e 'c6.mode("signboard"); c6.text("LUA 5.4", "Free: " .. sys.free_ram())'
```

---

## 4. Hardware, Display, BLE 5.0, and HTTP API Controls

All display modes, radios, and pins are scriptable from `c6-sh` or `/sys`. Reserved flash, USB, SPI2, LED, and button pins (`GPIO 4..9, 12..15, 21, 22`) reject user writes so scripts cannot crash the bus.

```bash
# Switch LCD modes and capture a pixel-accurate 320x172 RGB BMP screenshot
screen mode monitor
fbgrab /root/screen.bmp

# Start NimBLE 5.0 on demand, scan for 5 seconds, then free all BLE RAM
ble on && ble scan 5 && ble off

# Sample 12-bit ADC millivolts, scan I2C, and ping a LAN host
adc read 0
i2c set 2 3 100000 && i2c scan
ping -c 3 1.1.1.1

# Set POSIX timezone (persists in /etc/timezone; default America/Toronto)
tz America/Toronto && date
```

Remote HTTP commands on port `80` require the 32-hex-character NVS bearer token printed by `apitoken` inside `c6-sh`. Meanwhile, `/api/status` stays read-only without a token.

```bash
# Read live JSON status (no token required)
curl -s http://<BOARD_IP>/api/status

# Run an authenticated shell command over HTTP
curl -s -H "Authorization: Bearer <TOKEN>" "http://<BOARD_IP>/api/cmd?c=sensors"
```

---

## 5. Building, Testing, and Flashing

Run the 10-suite host test harness on Linux or macOS before building the ESP32-C6 image.

```bash
# Run host unit tests (SSH crypto, SFTP v3, epkg, Lua 5.4, RV32IMAC ELF VM, fbgrab)
pio test -e native

# Build QIO firmware for Waveshare ESP32-C6-LCD-1.47
pio run -e waveshare_esp32c6_lcd_147

# Flash over USB-Serial/JTAG (/dev/ttyACM0)
pio run -e waveshare_esp32c6_lcd_147 -t upload
```

---

## 6. Open Questions

1. Which Ed25519 key ring format in `/etc/epkg/trusted_keys` should gate `.epkg` installs when packages come from shared HTTP mirrors rather than `sftp`?
2. What Physical Memory Protection region layout on `ESP32-C6` can fence `RV32IMAC` `.elf` sandboxes away from kernel SRAM without breaking FreeRTOS interrupt stacks?
