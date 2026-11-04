# C6-MicroLinux (`c6os`) — Waveshare ESP32-C6-LCD-1.47

**Status:** Active (`c6os 6.12`) • November 2026.

**TL;DR:** Standard ESP32 firmware requires a full recompile and USB reflash for every small script or tool change. We built `C6-MicroLinux` (`c6os`) for the Waveshare `ESP32-C6-LCD-1.47` (`ESP32-C6FH4`, `160 MHz` RISC-V, `320 KB` SRAM, `8 MB` QIO Flash). Engineers log in over SSHv2, push files via SFTP v3, and install signed `EPK2` packages carrying Lua 5.4, RISC-V `.elf`, or shell apps. The QIO image uses `54,812 B` of static SRAM and `1,847,552 B` of flash. It dims and sleeps idle ST7789 panels, streams `epkg` checks in `2 KB` chunks, and fences `.elf` apps inside `16–48 KB` `W^X` sandboxes, leaving signed OTA kernel images as our main open question.

---

## 1. System Overview & Measured Footprint

`C6-MicroLinux` turns a single-core RISC-V microcontroller into a live, network-accessible Linux-style node. Operators connect over 2.4 GHz Wi-Fi via `ssh root@<ip>` or attach over USB-Serial on `/dev/ttyACM0`. From there, they control the `320x172` IPS display, onboard `WS2812` LED, BLE 5.0 radio, and general-purpose pins.

At boot, `main.cpp` mounts `LittleFS` for `/etc`, `/root`, and `/apps`, and mounts any FAT32 microSD card on `SPI2_HOST`. Static SRAM sits at `54,812 B` (`16.7%` of `320 KB`), `8`-row DMA strip buffers hold `10,240 B`, and idle daemons block in `lwip_select()` rather than spin-polling sockets. All user runtimes (`SFTP v3`, `Lua 5.4`, `RV32IMAC` ELF VM, and `NimBLE 5.0`) use `0 B` of idle heap until called.

| Subsystem | Implementation & Measured Bounds |
| :--- | :--- |
| Target Hardware | `ESP32-C6FH4` (`RV32IMAC` @ `160 MHz`, `320 KB` SRAM, `8 MB` QIO Flash @ `80 MHz`) + `1.47"` `ST7789` (`320x172` RGB565) |
| Firmware Size | Static SRAM: `54,812 B / 327,680 B` (`16.7%`, CI gate `57,344 B`); Flash binary: `1,847,552 B / 3,342,336 B` (`55.3%`) |
| SSHv2 & SFTP v3 (`:22`) | Non-blocking `lwip_select()` sockets, `5,824 B` session workspace, `curve25519-sha256` KEX, `aes128-ctr`, `hmac-sha2-256`, `ssh-ed25519` + `rsa-sha2-256/512`, NVS `passwd`, and binary `sftp` |
| User App Runtime | `epkg` (`EPK1`/`EPK2` Ed25519 + `2 KB` streamed SHA-256), 32-bit `Lua 5.4` (`64 KB` heap cap), `RV32IMAC` `.elf` VM (`16–48 KB` `W^X` sandbox), and `.sh` scripts (depth `4`) |
| Display Engine | `esp_lcd_panel_io_spi` @ `80 MHz`, `10.2 KB` DMA scanlines, two-stage idle dim (`15%` @ `120 s`) & `SLPIN` sleep (`600 s`), and `320x172` `.bmp`/`.ppm` `fbgrab` |
| Radios & Buses | Event-driven Wi-Fi STA (`WIFI_PS_MIN_MODEM` idle, `WIFI_PS_NONE` on SSH) + SoftAP, adaptive BLE 5.0 scan (`25%` duty on active SSH/AP, `75%` idle), `gpio`, `pwm`, `adc`, `i2c`, `ping` |
| Telemetry & HTTP (`:80`) | O(1) `ulTaskGetIdleRunTimeCounter()` 1 Hz CPU load, `uxTaskGetSystemState()` in `ps`/`top`, dirty-gated `LittleFS` usage, `/proc` & `/sys`, and lwIP `/api/status` + `/api/cmd` |

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

Developers ship tools to `/apps` without rebuilding the kernel firmware. Build `.epkg` archives on your workstation with `scripts/mkepkg.py`, transfer them over `sftp`, and install them in streamed `2 KB` chunks via `epkg`. While `/etc/epkg/trusted_keys` holds zero keys, unsigned `EPK1` archives install after SHA-256 verification. Once you add an OpenSSH `ssh-ed25519` public key to `/etc/epkg/trusted_keys`, `epkg` requires a valid `EPK2` Ed25519 signature unless `--allow-unsigned` is passed.

```bash
# Generate an Ed25519 signing keypair and build a signed EPK2 package
python3 scripts/mkepkg.py --keygen release
python3 scripts/mkepkg.py -n blink -v 1.0.0 -a noarch -f blink.lua:/apps/blink.lua:0755 -k release.key -o blink.epkg

# Install on the ESP32-C6 after adding release.pub to /etc/epkg/trusted_keys
ssh root@<BOARD_IP> "epkg install /root/blink.epkg && epkg list && blink"
```

Lua scripts run on a 32-bit `Lua 5.4` VM (`LUA_32BITS=1`) with C bindings for `sys`, `c6`, `gpio`, `wifi`, `ble`, and `fs`. Native C programs include `include/c6os_app_api.h` and compile as relocatable `RV32IMAC` ELF32 objects (`-march=rv32imac -mabi=ilp32 -Wl,-r`). At launch, `c6os_elf.cpp` sizes each sandbox from `16 KB` to `48 KB` in `4 KB` steps and enforces `W^X` permissions across code (`RX`), read-only data (`R`), and heap/stack (`RW`).

```bash
# Run inline Lua 5.4 directly from c6-sh
lua -e 'c6.mode("signboard"); c6.text("LUA 5.4", "Free: " .. sys.free_ram())'
```

---

## 4. Hardware, Display, BLE 5.0, and HTTP API Controls

All display modes, idle timeouts, radios, and pins are scriptable from `c6-sh` or `/sys`. By default, the ST7789 backlight dims to `15%` after `120 s` of inactivity and enters panel sleep (`SLPIN`, `0%` backlight, `0` SPI2 DMA renders) after `600 s`. Pressing `GPIO 9` (`BOOT`), typing over SSH or Serial, calling `/api/cmd`, or raising an alert wakes the panel right away. When `ble scan` runs during an active SSH session or SoftAP, `main.cpp` drops BLE scan duty from `75%` to `25%` (`20 ms / 80 ms`) to keep SSH latency low on the shared 2.4 GHz radio.

```bash
# Configure ST7789 dim/sleep timeouts (seconds) and capture a 320x172 BMP screenshot
screen timeout 120 600
screen mode monitor && fbgrab /root/screen.bmp

# Start NimBLE 5.0 on demand, scan for 5 seconds, then free all BLE RAM
ble on && ble scan 5 && ble off

# Sample 12-bit ADC millivolts, scan I2C, and ping a LAN host
adc read 0
i2c set 2 3 100000 && i2c scan
ping -c 3 1.1.1.1
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

Run the 14-suite host test harness (including `ASan + LSan + UBSan`) on Linux or macOS before building the ESP32-C6 image.

```bash
# Run host unit tests (SSH crypto, SFTP v3, EPK2 Ed25519, Lua 5.4, W^X ELF VM, idle sleep)
pio test -e native
pio test -e native_asan

# Build QIO firmware for Waveshare ESP32-C6-LCD-1.47
pio run -e waveshare_esp32c6_lcd_147

# Flash over USB-Serial/JTAG (/dev/ttyACM0)
pio run -e waveshare_esp32c6_lcd_147 -t upload
```

---

## 6. Open Questions

1. What dual-slot partition layout on the `8 MB` flash can support atomic Ed25519-signed kernel OTA updates without shrinking the `LittleFS` `/apps` volume below `1 MB`?
2. At what ambient temperature or battery threshold on battery-backed carrier boards should `c6os` enter ESP-IDF light sleep between SSH keepalive packets?
