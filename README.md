# Waveshare ESP32-C6-LCD-1.47 Firmware & Host Control Suite

Custom firmware, portable protocol library, and Linux host CLI for the **Waveshare ESP32-C6-LCD-1.47** development board attached over USB-Serial/JTAG (`/dev/ttyACM0`).

## Hardware Profile & Verified Pinout

| Feature | Specification |
| :--- | :--- |
| **Board** | Waveshare ESP32-C6-LCD-1.47 (non-touch) |
| **SoC** | Espressif `ESP32-C6FH4` (QFN32, revision `v0.2`), 160 MHz single-core 32-bit RISC-V |
| **Radios** | Wi-Fi 6 (802.11ax), Bluetooth 5 (LE), IEEE 802.15.4 (Zigbee / Thread) |
| **USB Interface** | Built-in USB-Serial/JTAG (`303a:1001`) at `/dev/ttyACM0`, Base MAC `14:c1:9f:05:7d:10` |
| **Flash & SRAM** | **8 MB SPI Flash** (`XMC 0x20/0x4017`, **DIO** mode), **320 KB SRAM** (`268 KB` usable heap), 0 PSRAM |
| **Display** | 1.47" IPS TFT LCD (`ST7789`, `172×320` RGB565, column offset `+34` px, 80 MHz SPI) |

### Onboard GPIO Pin Mapping

| Peripheral | Signal | GPIO | Notes |
| :--- | :--- | :--- | :--- |
| **SPI2 Bus** | `SCLK` | `GPIO 7` | Shared by ST7789 LCD and MicroSD slot |
| **SPI2 Bus** | `MOSI` | `GPIO 6` | Shared by ST7789 LCD and MicroSD slot |
| **SPI2 Bus** | `MISO` | `GPIO 5` | Used by MicroSD slot |
| **ST7789 LCD** | `LCD_CS` | `GPIO 14` | Active-low chip select (80 MHz SPI) |
| **ST7789 LCD** | `LCD_DC` | `GPIO 15` | Data (`1`) / Command (`0`) select |
| **ST7789 LCD** | `LCD_RST` | `GPIO 21` | Active-low hardware reset |
| **ST7789 LCD** | `LCD_BL` | `GPIO 22` | Backlight PWM (`1000 Hz`, 10-bit LEDC) |
| **MicroSD Slot** | `SD_CS` | `GPIO 4` | Active-low chip select (4 MHz SPI) |
| **RGB LED** | `RGB_DIN` | `GPIO 8` | Onboard addressable WS2812 RGB LED |
| **BOOT Button** | `BOOT` | `GPIO 9` | Active-low input (short press: view, long press: dim) |

---

## Building, Testing & Flashing

### 1. Run Unit Tests (Host Native)
```bash
timeout 30s pio test -e native
```

### 2. Build & Flash Firmware
```bash
timeout 30s pio run -e waveshare_esp32c6_lcd_147 -t upload
```

### 3. Restore Factory Firmware Backup
The original 4 MB factory flash dump (`0x000000..0x400000`) is stored in `backups/factory_firmware_4mb.bin` with its checksum tracked in [`backups/SHA256SUMS`](backups/SHA256SUMS):
```bash
./scripts/restore_factory.sh /dev/ttyACM0
```

---

## Host CLI Usage (`scripts/screen_ctl.py`)

```bash
# Query full hardware & UI state as JSON
python3 scripts/screen_ctl.py status

# Display custom text on the bottom card
python3 scripts/screen_ctl.py text "Cluster Healthy"

# Push live Linux host CPU, RAM, and IP telemetry to the Host Monitor view
python3 scripts/screen_ctl.py host

# Switch views (dashboard | host | wireless)
python3 scripts/screen_ctl.py view dashboard

# Control the onboard WS2812 RGB LED and LCD backlight
python3 scripts/screen_ctl.py rgb 0 255 128
python3 scripts/screen_ctl.py mode rainbow
python3 scripts/screen_ctl.py bl 75
```
