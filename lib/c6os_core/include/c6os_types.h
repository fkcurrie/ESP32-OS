#ifndef C6OS_TYPES_H
#define C6OS_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace c6os {

// Display Modes for the 1.47" ST7789 (320x172) LCD
enum class ScreenMode : uint8_t {
    OS_DASHBOARD = 0,  // Hostname, SSH IP, CPU/SRAM telemetry, custom banner
    FB_CONSOLE   = 1,  // /dev/tty0 live scrolling kernel & terminal console (53x21 chars)
    SIGNBOARD    = 2,  // Full-screen scaled custom headline & subtitle signboard
    DIAGNOSTICS  = 3,  // RGB565 color bars, geometry grid, and hardware pin/bus test
    COUNT        = 4
};

// High-Contrast RGB565 Color Themes
enum class ThemeId : uint8_t {
    CYBER_CYAN   = 0,  // Black bg, Cyan accents, White text
    MATRIX_GREEN = 1,  // Black bg, Vibrant Green accents & text
    AMBER_CRT    = 2,  // Black bg, Warm Amber/Gold accents & text
    RED_ALERT    = 3,  // Black bg, Crimson Red borders & Yellow/White text
    MONO_WHITE   = 4,  // Pure Black & Crisp White high-contrast monochrome
    COUNT        = 5
};

// Onboard WS2812 RGB LED Operating Modes
enum class LedMode : uint8_t {
    OFF          = 0,
    SOLID        = 1,
    PULSE        = 2,
    RAINBOW      = 3,
    SSH_ACTIVITY = 4,  // Green when SSH active, Cyan idle pulse, Amber on auth
    COUNT        = 5
};

struct ThemePalette {
    uint16_t bg;
    uint16_t fg;
    uint16_t accent;
    uint16_t border;
    uint16_t muted;
    uint16_t ok;
    uint16_t warn;
    uint16_t alert;
};

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((static_cast<uint16_t>(r & 0xF8) << 8) |
                                  (static_cast<uint16_t>(g & 0xFC) << 3) |
                                  (static_cast<uint16_t>(b) >> 3)));
}

inline ThemePalette getThemePalette(ThemeId theme) {
    switch (theme) {
        case ThemeId::MATRIX_GREEN:
            return {0x0000, 0x07E0, 0x07FF, 0x07E0, 0x03E0, 0x07E0, 0xFFE0, 0xF800};
        case ThemeId::AMBER_CRT:
            return {0x0000, 0xFFE0, 0xFD20, 0xFD20, 0xC618, 0x07E0, 0xFFE0, 0xF800};
        case ThemeId::RED_ALERT:
            return {0x0000, 0xFFFF, 0xF800, 0xF800, 0xFFE0, 0xFFE0, 0xFD20, 0xF800};
        case ThemeId::MONO_WHITE:
            return {0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xC618, 0xFFFF, 0xC618, 0xFFFF};
        case ThemeId::CYBER_CYAN:
        default:
            return {0x0000, 0xFFFF, 0x07FF, 0xFFFF, 0xC618, 0x07E0, 0xFFE0, 0xF800};
    }
}

inline const char* screenModeName(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::OS_DASHBOARD: return "dashboard";
        case ScreenMode::FB_CONSOLE:   return "console";
        case ScreenMode::SIGNBOARD:    return "signboard";
        case ScreenMode::DIAGNOSTICS:  return "test";
        default:                       return "dashboard";
    }
}

inline const char* screenModeLabel(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::OS_DASHBOARD: return "OS Dashboard";
        case ScreenMode::FB_CONSOLE:   return "LCD Console (tty0)";
        case ScreenMode::SIGNBOARD:    return "Custom Signboard";
        case ScreenMode::DIAGNOSTICS:  return "Hardware Test";
        default:                       return "OS Dashboard";
    }
}

inline const char* themeName(ThemeId theme) {
    switch (theme) {
        case ThemeId::CYBER_CYAN:   return "cyan";
        case ThemeId::MATRIX_GREEN: return "matrix";
        case ThemeId::AMBER_CRT:    return "amber";
        case ThemeId::RED_ALERT:    return "red";
        case ThemeId::MONO_WHITE:   return "mono";
        default:                    return "cyan";
    }
}

inline const char* ledModeName(LedMode mode) {
    switch (mode) {
        case LedMode::OFF:          return "off";
        case LedMode::SOLID:        return "solid";
        case LedMode::PULSE:        return "pulse";
        case LedMode::RAINBOW:      return "rainbow";
        case LedMode::SSH_ACTIVITY: return "ssh";
        default:                    return "off";
    }
}

// Constants for LCD Framebuffer Console (320x172 with 6x8 char cell -> 53 cols x 18 body rows)
constexpr size_t FB_CONSOLE_ROWS = 18;
constexpr size_t FB_CONSOLE_COLS = 52;
constexpr size_t DMESG_MAX_LINES = 32;
constexpr size_t DMESG_LINE_LEN  = 76;

struct WifiScanEntry {
    char ssid[33];
    int32_t rssi;
    uint8_t channel;
    bool open;
};

constexpr size_t MAX_WIFI_SCAN_RESULTS = 10;

struct SystemState {
    // Kernel & SoC Metadata
    char hostname[32];
    char root_password[48];
    uint32_t cpu_freq_mhz;
    uint32_t total_sram_bytes;
    uint32_t free_sram_bytes;
    uint32_t min_free_sram_bytes;
    uint32_t flash_size_mb;
    uint32_t rootfs_total_kb;
    uint32_t rootfs_used_kb;
    bool sd_mounted;
    uint32_t sd_total_mb;
    uint32_t uptime_sec;

    // Network & SSH Daemon Status
    bool wifi_sta_enabled;
    bool wifi_sta_connected;
    char wifi_cfg_ssid[33];
    char wifi_cfg_pass[65];
    char wifi_ssid[33];
    char wifi_ip[20];
    char wifi_gateway[20];
    char wifi_mac[20];
    int32_t wifi_rssi;

    bool wifi_ap_enabled;
    char wifi_ap_ssid[33];
    char wifi_ap_ip[20];

    uint16_t ssh_port;
    uint8_t ssh_active_sessions;
    uint32_t ssh_total_logins;
    char ssh_client_ip[24];
    char ssh_hostkey_fp[64];
    uint32_t net_rx_bytes;
    uint32_t net_tx_bytes;

    // Screen & Peripheral State
    ScreenMode screen_mode;
    ThemeId theme;
    uint8_t backlight_pct;
    bool landscape_flipped;
    char headline[48];
    char subtitle[64];
    char banner_footer[64];

    // Canvas Custom Draw Overlay (optional user-drawn box/bar from CLI)
    bool custom_bar_visible;
    uint8_t custom_bar_pct;
    char custom_bar_label[32];

    // Onboard WS2812 RGB LED State
    LedMode led_mode;
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;

    // Hardware Action Flags (set by shell/menu, consumed by hardware tasks)
    bool ui_dirty;
    bool wifi_reconnect_requested;
    bool wifi_scan_requested;
    bool reboot_requested;
    bool fs_sync_requested;

    // Latest Wi-Fi Scan Results
    size_t wifi_scan_count;
    WifiScanEntry wifi_scan[MAX_WIFI_SCAN_RESULTS];

    // Framebuffer Console (/dev/tty0 on physical LCD)
    char fb_lines[FB_CONSOLE_ROWS][FB_CONSOLE_COLS + 1];
    size_t fb_line_count;

    // Kernel Ring Buffer (dmesg)
    char dmesg[DMESG_MAX_LINES][DMESG_LINE_LEN];
    size_t dmesg_count;

    void initDefaults() {
        memset(this, 0, sizeof(*this));
        strncpy(hostname, "c6-linux", sizeof(hostname) - 1);
        strncpy(root_password, "esp32c6", sizeof(root_password) - 1);
        cpu_freq_mhz = 160;
        total_sram_bytes = 327680;
        free_sram_bytes = 256000;
        min_free_sram_bytes = 240000;
        flash_size_mb = 8;
        rootfs_total_kb = 1536;
        rootfs_used_kb = 24;
        sd_mounted = false;
        sd_total_mb = 0;
        uptime_sec = 0;

        wifi_sta_enabled = true;
        wifi_sta_connected = false;
        strncpy(wifi_ip, "0.0.0.0", sizeof(wifi_ip) - 1);
        strncpy(wifi_gateway, "0.0.0.0", sizeof(wifi_gateway) - 1);
        strncpy(wifi_mac, "14:c1:9f:05:7d:10", sizeof(wifi_mac) - 1);
        wifi_rssi = -100;

        wifi_ap_enabled = true;
        strncpy(wifi_ap_ssid, "C6-Linux-OS", sizeof(wifi_ap_ssid) - 1);
        strncpy(wifi_ap_ip, "192.168.4.1", sizeof(wifi_ap_ip) - 1);

        ssh_port = 22;
        ssh_active_sessions = 0;
        ssh_total_logins = 0;
        strncpy(ssh_client_ip, "none", sizeof(ssh_client_ip) - 1);
        strncpy(ssh_hostkey_fp, "SHA256:Generating...", sizeof(ssh_hostkey_fp) - 1);

        screen_mode = ScreenMode::OS_DASHBOARD;
        theme = ThemeId::CYBER_CYAN;
        backlight_pct = 100;
        landscape_flipped = false;
        strncpy(headline, "C6-MicroLinux Ready", sizeof(headline) - 1);
        strncpy(subtitle, "SSH Port 22 | CLI Screen Control", sizeof(subtitle) - 1);
        strncpy(banner_footer, "Use 'menu' over SSH or /dev/ttyACM0", sizeof(banner_footer) - 1);

        led_mode = LedMode::SSH_ACTIVITY;
        led_r = 0;
        led_g = 200;
        led_b = 255;

        ui_dirty = true;
    }

    void appendFbConsoleLine(const char* text) {
        if (!text) return;
        while (*text) {
            char line[FB_CONSOLE_COLS + 1];
            size_t len = 0;
            while (*text && *text != '\n' && *text != '\r' && len < FB_CONSOLE_COLS) {
                line[len++] = *text++;
            }
            line[len] = '\0';
            if (*text == '\r') ++text;
            if (*text == '\n') ++text;

            if (fb_line_count < FB_CONSOLE_ROWS) {
                strncpy(fb_lines[fb_line_count], line, FB_CONSOLE_COLS);
                fb_lines[fb_line_count][FB_CONSOLE_COLS] = '\0';
                ++fb_line_count;
            } else {
                for (size_t i = 1; i < FB_CONSOLE_ROWS; ++i) {
                    memcpy(fb_lines[i - 1], fb_lines[i], FB_CONSOLE_COLS + 1);
                }
                strncpy(fb_lines[FB_CONSOLE_ROWS - 1], line, FB_CONSOLE_COLS);
                fb_lines[FB_CONSOLE_ROWS - 1][FB_CONSOLE_COLS] = '\0';
            }
            if (len == 0 && *text == '\0') break;
        }
        ui_dirty = true;
    }

    void clearFbConsole() {
        memset(fb_lines, 0, sizeof(fb_lines));
        fb_line_count = 0;
        ui_dirty = true;
    }

    void appendDmesg(uint32_t ms, const char* msg) {
        if (!msg) return;
        char formatted[DMESG_LINE_LEN];
        uint32_t sec = ms / 1000U;
        uint32_t rem_ms = ms % 1000U;
        // Format: [    0.123] msg
        int pos = 0;
        formatted[pos++] = '[';
        // Pad seconds to 5 chars
        char sec_buf[12];
        int s_idx = 0;
        uint32_t tmp = sec;
        if (tmp == 0) {
            sec_buf[s_idx++] = '0';
        } else {
            while (tmp > 0 && s_idx < 10) {
                sec_buf[s_idx++] = static_cast<char>('0' + (tmp % 10));
                tmp /= 10;
            }
        }
        for (int pad = 5 - s_idx; pad > 0; --pad) {
            formatted[pos++] = ' ';
        }
        for (int i = s_idx - 1; i >= 0; --i) {
            formatted[pos++] = sec_buf[i];
        }
        formatted[pos++] = '.';
        formatted[pos++] = static_cast<char>('0' + ((rem_ms / 100U) % 10U));
        formatted[pos++] = static_cast<char>('0' + ((rem_ms / 10U) % 10U));
        formatted[pos++] = static_cast<char>('0' + (rem_ms % 10U));
        formatted[pos++] = ']';
        formatted[pos++] = ' ';
        while (*msg && pos < static_cast<int>(DMESG_LINE_LEN) - 1) {
            formatted[pos++] = *msg++;
        }
        formatted[pos] = '\0';

        if (dmesg_count < DMESG_MAX_LINES) {
            strncpy(dmesg[dmesg_count], formatted, DMESG_LINE_LEN - 1);
            dmesg[dmesg_count][DMESG_LINE_LEN - 1] = '\0';
            ++dmesg_count;
        } else {
            for (size_t i = 1; i < DMESG_MAX_LINES; ++i) {
                memcpy(dmesg[i - 1], dmesg[i], DMESG_LINE_LEN);
            }
            strncpy(dmesg[DMESG_MAX_LINES - 1], formatted, DMESG_LINE_LEN - 1);
            dmesg[DMESG_MAX_LINES - 1][DMESG_LINE_LEN - 1] = '\0';
        }
        appendFbConsoleLine(formatted);
    }
};

}  // namespace c6os

#endif  // C6OS_TYPES_H
