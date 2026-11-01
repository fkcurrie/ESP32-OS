#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <time.h>

#include "board_pins.h"
#include "c6os_shell.h"
#include "c6os_types.h"
#include "c6os_vfs.h"
#include "screen_manager.h"
#include "ssh_server.h"

namespace {

c6os::SystemState g_state;
c6os::VirtualFs g_vfs;
c6os::ScreenManager g_screen;
c6os::SshServer g_sshd(22);
WiFiServer g_httpd(80);
WiFiUDP g_syslog_udp;
SemaphoreHandle_t g_mutex = nullptr;

// Button debounce & press timing
bool g_btn_pressed = false;
uint32_t g_btn_press_start_ms = 0;
constexpr uint32_t BTN_DEBOUNCE_MS   = 40;
constexpr uint32_t BTN_LONG_PRESS_MS = 700;

uint8_t g_led_phase = 0;

void loadPersistentConfig() {
    Preferences prefs;
    prefs.begin("c6os_cfg", true);
    String host = prefs.getString("hostname", "c6-linux");
    String pass = prefs.getString("root_pass", "esp32c6");
    String ssid = prefs.getString("wifi_ssid", "");
    String wpass = prefs.getString("wifi_pass", "");
    prefs.end();

    strncpy(g_state.hostname, host.c_str(), sizeof(g_state.hostname) - 1);
    strncpy(g_state.root_password, pass.c_str(), sizeof(g_state.root_password) - 1);
    strncpy(g_state.wifi_cfg_ssid, ssid.c_str(), sizeof(g_state.wifi_cfg_ssid) - 1);
    strncpy(g_state.wifi_cfg_pass, wpass.c_str(), sizeof(g_state.wifi_cfg_pass) - 1);
}

void savePersistentConfig() {
    Preferences prefs;
    prefs.begin("c6os_cfg", false);
    prefs.putString("hostname", g_state.hostname);
    prefs.putString("root_pass", g_state.root_password);
    prefs.putString("wifi_ssid", g_state.wifi_cfg_ssid);
    prefs.putString("wifi_pass", g_state.wifi_cfg_pass);
    prefs.end();
}

void initStorage() {
    if (LittleFS.begin(true)) {
        g_state.rootfs_total_kb = static_cast<uint32_t>(LittleFS.totalBytes() / 1024ULL);
        g_state.rootfs_used_kb  = static_cast<uint32_t>(LittleFS.usedBytes() / 1024ULL);
        if (g_state.rootfs_used_kb == 0) g_state.rootfs_used_kb = 16;
        g_state.appendDmesg(millis(), "VFS: Mounted rootfs (LittleFS) read-write");
    } else {
        g_state.appendDmesg(millis(), "VFS: RootFS using ramfs fallback");
    }

    pinMode(board::PIN_SD_CS, OUTPUT);
    digitalWrite(board::PIN_SD_CS, HIGH);
    if (SD.begin(board::PIN_SD_CS, SPI, board::SD_SPI_HZ)) {
        if (SD.cardType() != CARD_NONE) {
            g_state.sd_mounted = true;
            g_state.sd_total_mb = static_cast<uint32_t>(SD.cardSize() / (1024ULL * 1024ULL));
            char msg[64];
            snprintf(msg, sizeof(msg), "mmcblk0: MicroSD card mounted (%lu MB)",
                     static_cast<unsigned long>(g_state.sd_total_mb));
            g_state.appendDmesg(millis(), msg);
        }
    }
}

void performWifiScanLocked() {
    int16_t n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        g_state.wifi_scan_count = 0;
        if (n > 0) {
            for (int16_t i = 0; i < n && g_state.wifi_scan_count < c6os::MAX_WIFI_SCAN_RESULTS; ++i) {
                String s = WiFi.SSID(i);
                if (s.length() == 0) continue;
                auto& entry = g_state.wifi_scan[g_state.wifi_scan_count++];
                strncpy(entry.ssid, s.c_str(), sizeof(entry.ssid) - 1);
                entry.ssid[sizeof(entry.ssid) - 1] = '\0';
                entry.rssi = WiFi.RSSI(i);
                entry.channel = static_cast<uint8_t>(WiFi.channel(i));
                entry.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "wlan0: Scan completed (%u networks found)",
                 static_cast<unsigned>(g_state.wifi_scan_count));
        g_state.appendDmesg(millis(), msg);
        xSemaphoreGive(g_mutex);
    }
    WiFi.scanDelete();
}

void applyWifiConfig() {
    char ssid[33] = {0};
    char pass[65] = {0};
    bool ap_en = true;
    char host[32] = {0};

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(ssid, g_state.wifi_cfg_ssid, sizeof(ssid) - 1);
        strncpy(pass, g_state.wifi_cfg_pass, sizeof(pass) - 1);
        ap_en = g_state.wifi_ap_enabled;
        strncpy(host, g_state.hostname, sizeof(host) - 1);
        xSemaphoreGive(g_mutex);
    }

    WiFi.setHostname(host);
    WiFi.mode(ap_en ? WIFI_AP_STA : WIFI_STA);

    if (ap_en) {
        WiFi.softAP("C6-Linux-OS");
        String ap_ip = WiFi.softAPIP().toString();
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strncpy(g_state.wifi_ap_ip, ap_ip.c_str(), sizeof(g_state.wifi_ap_ip) - 1);
            xSemaphoreGive(g_mutex);
        }
    }

    String mac = WiFi.macAddress();
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(g_state.wifi_mac, mac.c_str(), sizeof(g_state.wifi_mac) - 1);
        xSemaphoreGive(g_mutex);
    }

    if (ssid[0] != '\0') {
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (pass[0] != '\0') {
            WiFi.begin(ssid, pass);
        } else {
            WiFi.begin(ssid);
        }
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            char msg[72];
            snprintf(msg, sizeof(msg), "wlan0: Associating with '%s'...", ssid);
            g_state.appendDmesg(millis(), msg);
            g_state.ui_dirty = true;
            xSemaphoreGive(g_mutex);
        }
    }
}

void urlDecodeInPlace(char* str) {
    char* src = str;
    char* dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            ++src;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = static_cast<char>(strtol(hex, nullptr, 16));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void pollHttpAndSyslog() {
    // 1. Poll UDP 514 Syslog Listener
    int pkt_size = g_syslog_udp.parsePacket();
    if (pkt_size > 0) {
        char sbuf[96];
        int len = g_syslog_udp.read(sbuf, sizeof(sbuf) - 1);
        if (len > 0) {
            sbuf[len] = '\0';
            for (int i = 0; i < len; ++i) {
                if (sbuf[i] == '\r' || sbuf[i] == '\n') sbuf[i] = ' ';
            }
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.syslog_packets++;
                g_state.net_rx_bytes += static_cast<uint32_t>(len);
                char msg[112];
                snprintf(msg, sizeof(msg), "syslogd: %.80s", sbuf);
                g_state.appendDmesg(millis(), msg);
                g_state.appendFbLine(msg);
                g_state.ui_dirty = true;
                xSemaphoreGive(g_mutex);
            }
        }
    }

    // 2. Poll TCP 80 HTTP Webhook & REST API Server
    WiFiClient client = g_httpd.accept();
    if (!client) return;

    uint32_t t0 = millis();
    while (client.connected() && !client.available() && (millis() - t0 < 250)) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!client.available()) {
        client.stop();
        return;
    }

    String req_line = client.readStringUntil('\n');
    req_line.trim();
    while (client.available()) {
        String hdr = client.readStringUntil('\n');
        if (hdr.length() <= 1) break;
    }

    if (req_line.startsWith("GET /api/status")) {
        static char json[1024];
        json[0] = '\0';
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_state.http_requests++;
            c6os::ShellSession::formatStatusJson(g_state, json, sizeof(json));
            xSemaphoreGive(g_mutex);
        }
        client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
        client.print(json);
    } else if (req_line.startsWith("GET /api/cmd?q=")) {
        int q_start = req_line.indexOf("?q=") + 3;
        int q_end = req_line.indexOf(' ', q_start);
        String q_raw = (q_end > q_start) ? req_line.substring(q_start, q_end) : req_line.substring(q_start);
        char cmd_buf[160];
        strncpy(cmd_buf, q_raw.c_str(), sizeof(cmd_buf) - 1);
        cmd_buf[sizeof(cmd_buf) - 1] = '\0';
        urlDecodeInPlace(cmd_buf);

        char* out = static_cast<char*>(malloc(2048));
        if (out) {
            out[0] = '\0';
            c6os::ShellSession http_sh;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                g_state.http_requests++;
                http_sh.executeCommandLine(g_state, g_vfs, cmd_buf, out, 2048);
                xSemaphoreGive(g_mutex);
            }
            client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n");
            client.print(out);
            free(out);
        }
    } else {
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.http_requests++;
            xSemaphoreGive(g_mutex);
        }
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                     "C6-MicroLinux HTTP REST API (port 80)\n"
                     "Endpoints:\n"
                     "  GET /api/status\n"
                     "  GET /api/cmd?q=<url-encoded-command>\n");
    }
    client.stop();
}

void networkTask(void* param) {
    (void)param;
    bool was_connected = false;

    while (true) {
        bool do_reconnect = false;
        bool do_scan = false;
        bool do_sync = false;
        bool do_reboot = false;
        bool do_ntp = false;

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_state.wifi_reconnect_requested) {
                do_reconnect = true;
                g_state.wifi_reconnect_requested = false;
            }
            if (g_state.wifi_scan_requested) {
                do_scan = true;
                g_state.wifi_scan_requested = false;
            }
            if (g_state.fs_sync_requested) {
                do_sync = true;
                g_state.fs_sync_requested = false;
            }
            if (g_state.reboot_requested) {
                do_reboot = true;
                g_state.reboot_requested = false;
            }
            if (g_state.ntp_sync_requested) {
                do_ntp = true;
                g_state.ntp_sync_requested = false;
            }
            xSemaphoreGive(g_mutex);
        }

        if (do_sync) {
            savePersistentConfig();
        }
        if (do_reboot) {
            vTaskDelay(pdMS_TO_TICKS(400));
            ESP.restart();
        }
        if (do_reconnect) {
            applyWifiConfig();
        }
        if (do_scan) {
            performWifiScanLocked();
        }
        if (do_ntp && WiFi.status() == WL_CONNECTED) {
            configTzTime("AEDT-11", "pool.ntp.org", "time.nist.gov");
        }

        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected) {
            String ip = WiFi.localIP().toString();
            String gw = WiFi.gatewayIP().toString();
            String ssid = WiFi.SSID();
            int32_t rssi = WiFi.RSSI();

            if (!was_connected) {
                configTzTime("AEDT-11", "pool.ntp.org", "time.nist.gov");
            }

            time_t now_epoch = time(nullptr);

            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                bool ip_changed = (strcmp(g_state.wifi_ip, ip.c_str()) != 0);
                g_state.wifi_sta_connected = true;
                strncpy(g_state.wifi_ip, ip.c_str(), sizeof(g_state.wifi_ip) - 1);
                strncpy(g_state.wifi_gateway, gw.c_str(), sizeof(g_state.wifi_gateway) - 1);
                strncpy(g_state.wifi_ssid, ssid.c_str(), sizeof(g_state.wifi_ssid) - 1);
                g_state.wifi_rssi = rssi;

                if (now_epoch > 1700000000) {
                    g_state.epoch_offset_sec = static_cast<uint32_t>(now_epoch) - (millis() / 1000U);
                    g_state.ntp_synced = true;
                }

                if (!was_connected || ip_changed) {
                    char msg[96];
                    snprintf(msg, sizeof(msg), "wlan0: Bound to %s (SSID: %s)",
                             g_state.wifi_ip, g_state.wifi_ssid);
                    g_state.appendDmesg(millis(), msg);
                    g_state.ui_dirty = true;
                }
                xSemaphoreGive(g_mutex);
            }
            was_connected = true;
        } else {
            if (was_connected) {
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    g_state.wifi_sta_connected = false;
                    strncpy(g_state.wifi_ip, "0.0.0.0", sizeof(g_state.wifi_ip) - 1);
                    g_state.wifi_rssi = -100;
                    g_state.appendDmesg(millis(), "wlan0: Link lost");
                    g_state.ui_dirty = true;
                    xSemaphoreGive(g_mutex);
                }
                was_connected = false;
            }
        }

        pollHttpAndSyslog();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void sshdTask(void* param) {
    (void)param;
    while (true) {
        g_sshd.pollAcceptAndServe(g_state, g_vfs, g_mutex);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void serialConsoleTask(void* param) {
    (void)param;
    c6os::ShellSession serial_shell;
    char* out_buf = static_cast<char*>(malloc(4096));
    if (!out_buf) {
        vTaskDelete(nullptr);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        serial_shell.begin(g_state, g_vfs, /*start_in_menu=*/false, out_buf, 4096);
        xSemaphoreGive(g_mutex);
    }
    Serial.print(out_buf);
    uint32_t last_mon_tick = millis();

    while (true) {
        int avail = Serial.available();
        if (avail > 0) {
            char rx[64];
            size_t to_read = (static_cast<size_t>(avail) < sizeof(rx))
                                 ? static_cast<size_t>(avail)
                                 : sizeof(rx);
            size_t got = Serial.readBytes(rx, to_read);
            if (got > 0) {
                last_mon_tick = millis();
                out_buf[0] = '\0';
                bool keep = true;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    keep = serial_shell.consumeBytes(g_state, g_vfs, rx, got, out_buf, 4096);
                    if (!keep) {
                        // On serial console, exiting restarts the shell prompt
                        serial_shell.begin(g_state, g_vfs, /*start_in_menu=*/false, out_buf, 4096);
                    }
                    xSemaphoreGive(g_mutex);
                }
                if (out_buf[0] != '\0') {
                    Serial.print(out_buf);
                }
            }
        } else if (serial_shell.isInteractiveMonitor()) {
            uint32_t now = millis();
            if (now - last_mon_tick >= serial_shell.monitorIntervalMs()) {
                last_mon_tick = now;
                out_buf[0] = '\0';
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    serial_shell.renderMonitorTick(g_state, g_vfs, out_buf, 4096);
                    xSemaphoreGive(g_mutex);
                }
                if (out_buf[0] != '\0') {
                    Serial.print(out_buf);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void updateRgbLed(const c6os::SystemState& snap) {
    ++g_led_phase;
    switch (snap.led_mode) {
        case c6os::LedMode::OFF:
            rgbLedWrite(board::PIN_RGB_LED, 0, 0, 0);
            break;
        case c6os::LedMode::SOLID:
            rgbLedWrite(board::PIN_RGB_LED, snap.led_r, snap.led_g, snap.led_b);
            break;
        case c6os::LedMode::PULSE: {
            uint8_t level = (g_led_phase < 128) ? (g_led_phase * 2) : ((255 - g_led_phase) * 2);
            uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_r) * level) / 255U);
            uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_g) * level) / 255U);
            uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_b) * level) / 255U);
            rgbLedWrite(board::PIN_RGB_LED, r, g, b);
            break;
        }
        case c6os::LedMode::RAINBOW: {
            uint8_t p = g_led_phase;
            uint8_t r = 0, g = 0, b = 0;
            if (p < 85) {
                r = static_cast<uint8_t>(255 - p * 3);
                g = static_cast<uint8_t>(p * 3);
            } else if (p < 170) {
                p -= 85;
                g = static_cast<uint8_t>(255 - p * 3);
                b = static_cast<uint8_t>(p * 3);
            } else {
                p -= 170;
                b = static_cast<uint8_t>(255 - p * 3);
                r = static_cast<uint8_t>(p * 3);
            }
            rgbLedWrite(board::PIN_RGB_LED, r / 4, g / 4, b / 4);
            break;
        }
        case c6os::LedMode::SSH_ACTIVITY:
        default: {
            if (snap.ssh_active_sessions > 0) {
                // Bright emerald green when SSH client is connected
                rgbLedWrite(board::PIN_RGB_LED, 0, 64, 16);
            } else {
                // Gentle cyan heartbeat when idle
                uint8_t level = (g_led_phase < 128) ? (g_led_phase / 4) : ((255 - g_led_phase) / 4);
                rgbLedWrite(board::PIN_RGB_LED, 0, level / 2, level);
            }
            break;
        }
    }
}

void pollBootButton() {
    bool is_down = (digitalRead(board::PIN_BTN_BOOT) == LOW);
    uint32_t now = millis();

    if (is_down && !g_btn_pressed) {
        g_btn_pressed = true;
        g_btn_press_start_ms = now;
    } else if (!is_down && g_btn_pressed) {
        uint32_t held_ms = now - g_btn_press_start_ms;
        g_btn_pressed = false;
        if (held_ms >= BTN_DEBOUNCE_MS) {
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.button_press_count++;
                if (held_ms >= BTN_LONG_PRESS_MS) {
                    if (g_state.backlight_pct >= 90) g_state.backlight_pct = 50;
                    else if (g_state.backlight_pct >= 40) g_state.backlight_pct = 15;
                    else g_state.backlight_pct = 100;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "buttond: GPIO9 long press -> BL=%u%%",
                             static_cast<unsigned>(g_state.backlight_pct));
                    g_state.appendDmesg(now, msg);
                } else {
                    if (g_state.alert_active) {
                        g_state.alert_active = false;
                        g_state.appendDmesg(now, "buttond: GPIO9 short press -> cleared alert");
                    } else {
                        uint8_t next = (static_cast<uint8_t>(g_state.screen_mode) + 1) %
                                       static_cast<uint8_t>(c6os::ScreenMode::COUNT);
                        g_state.screen_mode = static_cast<c6os::ScreenMode>(next);
                        char msg[64];
                        snprintf(msg, sizeof(msg), "buttond: GPIO9 short press -> mode=%s",
                                 c6os::screenModeName(g_state.screen_mode));
                        g_state.appendDmesg(now, msg);
                    }
                }
                g_state.ui_dirty = true;
                xSemaphoreGive(g_mutex);
            }
        }
    }
}

void pollGpioHardware() {
    int8_t cmd_pin = -1;
    uint8_t cmd_act = 0;
    uint8_t cmd_val = 0;

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (g_state.gpio_cmd_pin >= 0) {
            cmd_pin = g_state.gpio_cmd_pin;
            cmd_act = g_state.gpio_cmd_action;
            cmd_val = g_state.gpio_cmd_val;
            g_state.gpio_cmd_pin = -1;
            g_state.gpio_cmd_action = 0;
        }
        xSemaphoreGive(g_mutex);
    }

    if (cmd_pin >= 0 && cmd_pin <= 23) {
        if (cmd_act == 1) {
            pinMode(cmd_pin, OUTPUT);
        } else if (cmd_act == 2) {
            pinMode(cmd_pin, INPUT);
        } else if (cmd_act == 3) {
            pinMode(cmd_pin, INPUT_PULLUP);
        } else if (cmd_act == 4) {
            pinMode(cmd_pin, OUTPUT);
            digitalWrite(cmd_pin, cmd_val ? HIGH : LOW);
        } else if (cmd_act == 5) {
            pinMode(cmd_pin, OUTPUT);
            analogWrite(cmd_pin, cmd_val);
        }
    }

    static const int SAFE_PINS[] = {0, 1, 2, 3, 9, 10, 11, 18, 19, 20};
    uint32_t in_mask = 0;
    for (size_t i = 0; i < sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]); ++i) {
        int p = SAFE_PINS[i];
        if (digitalRead(p) == HIGH) {
            in_mask |= (1UL << p);
        }
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_state.gpio_in_mask = in_mask;
        xSemaphoreGive(g_mutex);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    pinMode(board::PIN_BTN_BOOT, INPUT_PULLUP);
    rgbLedWrite(board::PIN_RGB_LED, 0, 16, 32);

    g_mutex = xSemaphoreCreateMutex();
    g_state.initDefaults();
    g_state.total_sram_bytes = ESP.getHeapSize();
    g_state.free_sram_bytes  = ESP.getFreeHeap();
    g_state.min_free_sram_bytes = ESP.getMinFreeHeap();
    g_state.flash_size_mb = static_cast<uint32_t>(ESP.getFlashChipSize() / (1024ULL * 1024ULL));
    g_state.cpu_temp_deci_c = static_cast<int16_t>(temperatureRead() * 10.0f);

    loadPersistentConfig();
    g_vfs.initDefaultFiles(g_state);

    // Boot in FB_CONSOLE mode so user sees live Linux kernel boot messages on the LCD
    g_screen.begin();
    g_state.screen_mode = c6os::ScreenMode::FB_CONSOLE;
    g_state.appendDmesg(millis(), "Linux version 6.12.0-c6os-riscv32 (gcc 13.2.0)");
    g_state.appendDmesg(millis(), "Machine: Waveshare ESP32-C6-LCD-1.47 (RV32IMAC)");
    g_state.appendDmesg(millis(), "fb0: ST7789 320x172 RGB565 @ 80MHz SPI2 ready");
    g_screen.render(g_state, true);

    initStorage();
    g_screen.render(g_state, false);

    applyWifiConfig();
    g_screen.render(g_state, false);

    performWifiScanLocked();
    g_sshd.begin(g_state, g_mutex);
    g_httpd.begin();
    g_syslog_udp.begin(514);
    g_state.appendDmesg(millis(), "httpd: REST API listening on 0.0.0.0:80");
    g_state.appendDmesg(millis(), "syslogd: UDP listener ready on 0.0.0.0:514");
    g_state.appendDmesg(millis(), "init: Executing /etc/rc.local & starting c6-sh");
    {
        char rc_out[512];
        c6os::ShellSession boot_sh;
        boot_sh.executeCommandLine(g_state, g_vfs, "sh /etc/rc.local", rc_out, sizeof(rc_out));
    }
    g_screen.render(g_state, false);

    delay(600);
    g_state.screen_mode = c6os::ScreenMode::OS_DASHBOARD;
    g_state.ui_dirty = true;
    g_screen.render(g_state, true);

    xTaskCreate(networkTask, "net_mgr", 6144, nullptr, 1, nullptr);
    xTaskCreate(sshdTask, "sshd", 10240, nullptr, 2, nullptr);
    xTaskCreate(serialConsoleTask, "ttyACM0", 6144, nullptr, 1, nullptr);
}

void loop() {
    static uint32_t last_sec_tick = 0;
    static uint32_t last_led_tick = 0;
    static uint32_t last_ticker_tick = 0;

    pollBootButton();

    uint32_t now = millis();
    if (now - last_ticker_tick >= 220) {
        last_ticker_tick = now;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_state.ticker_enabled) {
                g_state.ticker_offset++;
                g_state.ui_dirty = true;
            }
            xSemaphoreGive(g_mutex);
        }
    }

    if (now - last_sec_tick >= 1000) {
        last_sec_tick = now;
        pollGpioHardware();
        int16_t raw_deci_c = static_cast<int16_t>(temperatureRead() * 10.0f);

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.uptime_sec = now / 1000U;
            g_state.free_sram_bytes = ESP.getFreeHeap();
            g_state.min_free_sram_bytes = ESP.getMinFreeHeap();
            if (raw_deci_c > -200 && raw_deci_c < 1200) {
                g_state.cpu_temp_deci_c = raw_deci_c;
            }

            // Compute active CPU load % & record into 32-sample sparkline ring buffer
            uint8_t load = static_cast<uint8_t>(
                3U + (g_state.ssh_active_sessions * 5U) + ((g_state.uptime_sec * 3U) % 5U));
            if (load > 99) load = 99;
            g_state.cpu_load_pct = load;
            g_state.cpu_history[g_state.cpu_history_idx % 32] = load;
            g_state.cpu_history_idx = static_cast<uint8_t>((g_state.cpu_history_idx + 1) % 32);

            // Execute due cron jobs
            for (size_t i = 0; i < c6os::MAX_CRON_JOBS; ++i) {
                if (g_state.cron_jobs[i].active && g_state.cron_jobs[i].interval_sec > 0) {
                    if (g_state.uptime_sec - g_state.cron_jobs[i].last_run_sec >=
                        g_state.cron_jobs[i].interval_sec) {
                        g_state.cron_jobs[i].last_run_sec = g_state.uptime_sec;
                        g_state.cron_jobs[i].run_count++;
                        char cron_out[512];
                        c6os::ShellSession cron_sh;
                        cron_sh.executeCommandLine(
                            g_state, g_vfs, g_state.cron_jobs[i].cmd, cron_out, sizeof(cron_out));
                    }
                }
            }

            g_state.ui_dirty = true;
            xSemaphoreGive(g_mutex);
        }
    }

    static c6os::SystemState snapshot;
    bool should_render = false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (g_state.ui_dirty) {
            memcpy(&snapshot, &g_state, sizeof(snapshot));
            g_state.ui_dirty = false;
            should_render = true;
        }
        xSemaphoreGive(g_mutex);
    }

    if (should_render) {
        g_screen.render(snapshot, false);
    }

    if (now - last_led_tick >= 30) {
        last_led_tick = now;
        updateRgbLed(snapshot);
    }

    delay(15);
}
