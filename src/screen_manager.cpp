#include "screen_manager.h"

#include <stdio.h>
#include <string.h>

namespace c6os {

void ScreenManager::begin() {
    lcd_.begin();
    lcd_.fillScreen(lcd::COLOR_BLACK);
    last_backlight_ = 100;
    last_flipped_ = false;
}

void ScreenManager::render(const SystemState& state, bool force_full_clear) {
    if (state.landscape_flipped != last_flipped_) {
        lcd_.setLandscapeFlip(state.landscape_flipped);
        last_flipped_ = state.landscape_flipped;
        force_full_clear = true;
    }

    if (state.backlight_pct != last_backlight_) {
        lcd_.setBacklight(state.backlight_pct);
        last_backlight_ = state.backlight_pct;
    }

    const ThemePalette pal = getThemePalette(state.theme);
    if (force_full_clear || state.screen_mode != last_mode_ || state.theme != last_theme_) {
        lcd_.fillScreen(pal.bg);
        last_mode_ = state.screen_mode;
        last_theme_ = state.theme;
    }

    switch (state.screen_mode) {
        case ScreenMode::FB_CONSOLE:
            renderFbConsole(state, pal);
            break;
        case ScreenMode::SIGNBOARD:
            renderSignboard(state, pal);
            break;
        case ScreenMode::DIAGNOSTICS:
            renderDiagnostics(state, pal);
            break;
        case ScreenMode::OS_DASHBOARD:
        default:
            renderDashboard(state, pal);
            break;
    }
}

void ScreenManager::renderDashboard(const SystemState& state, const ThemePalette& pal) {
    // 1. Header Bar (y: 0..22)
    lcd_.fillRect(0, 0, board::LCD_WIDTH, 22, pal.bg);
    lcd_.fillRect(0, 21, board::LCD_WIDTH, 1, pal.border);

    char hdr_left[32];
    snprintf(hdr_left, sizeof(hdr_left), "root@%s", state.hostname);
    lcd_.drawStringScaled(4, 3, hdr_left, pal.accent, pal.bg, 1, 2, 22);

    char hdr_right[28];
    if (state.wifi_sta_connected) {
        snprintf(hdr_right, sizeof(hdr_right), "WLAN %ddBm", static_cast<int>(state.wifi_rssi));
        lcd_.drawStringScaled(224, 3, hdr_right, pal.ok, pal.bg, 1, 2, 15);
    } else {
        snprintf(hdr_right, sizeof(hdr_right), "AP:192.168.4.1");
        lcd_.drawStringScaled(224, 3, hdr_right, pal.warn, pal.bg, 1, 2, 15);
    }

    // 2. Network & SSH Card (y: 25..77, h: 52)
    lcd_.drawCard(2, 25, 316, 52, pal.bg, pal.border);
    char ssh_cmd[48];
    const char* active_ip = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
    snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh root@%s", active_ip);
    lcd_.drawStringScaled(8, 30, ssh_cmd, pal.accent, pal.bg, 1, 2, 26);

    // SSH badge on right side of Card 1
    if (state.ssh_active_sessions > 0) {
        lcd_.drawStringScaled(242, 30, "[SSH LIVE]", pal.ok, pal.bg, 1, 2, 11);
    } else {
        lcd_.drawStringScaled(242, 30, "[PORT :22]", pal.muted, pal.bg, 1, 2, 11);
    }

    char net_sub[54];
    if (state.wifi_sta_connected) {
        snprintf(net_sub, sizeof(net_sub), "SSID: %-16.16s  AP: %s  Logins: %u",
                 state.wifi_ssid, state.wifi_ap_ip, static_cast<unsigned>(state.ssh_total_logins));
    } else {
        snprintf(net_sub, sizeof(net_sub), "Join AP '%s' or USB /dev/ttyACM0 ('menu')",
                 state.wifi_ap_ssid);
    }
    lcd_.drawString(8, 56, net_sub, pal.fg, pal.bg, 1, 50);

    // 3. Kernel & Hardware Telemetry Card (y: 80..121, h: 41)
    lcd_.drawCard(2, 80, 316, 41, pal.bg, pal.border);
    unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
    unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
    unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
    uint8_t ram_pct   = (total_kb > 0) ? static_cast<uint8_t>((used_kb * 100U) / total_kb) : 0U;

    char ram_line[54];
    unsigned hrs  = static_cast<unsigned>(state.uptime_sec / 3600U);
    unsigned mins = static_cast<unsigned>((state.uptime_sec % 3600U) / 60U);
    unsigned secs = static_cast<unsigned>(state.uptime_sec % 60U);
    snprintf(ram_line, sizeof(ram_line), "SRAM: %u/%uKB (%u%%)  BL:%u%%  UP %02u:%02u:%02u",
             free_kb, total_kb, static_cast<unsigned>(100U - ram_pct),
             static_cast<unsigned>(state.backlight_pct), hrs, mins, secs);
    lcd_.drawString(8, 86, ram_line, pal.fg, pal.bg, 1, 50);

    lcd_.drawProgressBar(8, 100, 304, 14, ram_pct, pal.accent, pal.bg);

    // 4. User Banner / CLI Controlled Message Card (y: 124..170, h: 46)
    lcd_.drawCard(2, 124, 316, 46, pal.bg, pal.accent);
    if (state.custom_bar_visible) {
        lcd_.drawString(8, 130, state.custom_bar_label, pal.accent, pal.bg, 1, 50);
        lcd_.drawProgressBar(8, 144, 304, 18, state.custom_bar_pct, pal.ok, pal.bg);
    } else {
        lcd_.drawStringScaled(8, 129, state.headline, pal.fg, pal.bg, 1, 2, 50);
        lcd_.drawString(8, 152, state.subtitle, pal.accent, pal.bg, 1, 50);
    }
}

void ScreenManager::renderFbConsole(const SystemState& state, const ThemePalette& pal) {
    // Top inverted status bar (y: 0..14)
    lcd_.fillRect(0, 0, board::LCD_WIDTH, 14, pal.accent);
    char top_bar[54];
    const char* ip = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
    snprintf(top_bar, sizeof(top_bar), " /dev/tty0 | root@%s | %s ", state.hostname, ip);
    lcd_.drawString(2, 3, top_bar, pal.bg, pal.accent, 1, 52);

    // Render 18 console lines (y: 17..161, 8px per row)
    for (size_t i = 0; i < FB_CONSOLE_ROWS; ++i) {
        int16_t y = static_cast<int16_t>(17 + i * 8);
        const char* line = (i < state.fb_line_count) ? state.fb_lines[i] : "";
        uint16_t line_color = pal.fg;
        if (line[0] == '[') {
            line_color = pal.ok;  // dmesg timestamps highlighted
        } else if (strncmp(line, "root@", 5) == 0 || strncmp(line, "tty0>", 5) == 0) {
            line_color = pal.accent;
        }
        lcd_.drawString(4, y, line, line_color, pal.bg, 1, FB_CONSOLE_COLS);
    }

    // Bottom prompt indicator
    lcd_.drawString(4, 162, "root@c6-linux:/dev/tty0# _", pal.accent, pal.bg, 1, 52);
}

void ScreenManager::renderSignboard(const SystemState& state, const ThemePalette& pal) {
    // Outer and inner decorative border
    lcd_.drawRect(0, 0, board::LCD_WIDTH, board::LCD_HEIGHT, pal.border);
    lcd_.drawRect(3, 3, board::LCD_WIDTH - 6, board::LCD_HEIGHT - 6, pal.accent);

    // Header badge
    char badge[50];
    const char* ip = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
    snprintf(badge, sizeof(badge), "[ %s | SSH %s ]", state.hostname, ip);
    lcd_.drawString(12, 10, badge, pal.muted, pal.bg, 1, 48);
    lcd_.fillRect(8, 22, board::LCD_WIDTH - 16, 1, pal.border);

    // Main Headline (large scaled font)
    size_t hlen = strlen(state.headline);
    if (hlen <= 12) {
        lcd_.drawStringScaled(12, 42, state.headline, pal.fg, pal.bg, 2, 3, 25);
    } else if (hlen <= 25) {
        lcd_.drawStringScaled(12, 46, state.headline, pal.fg, pal.bg, 2, 2, 25);
    } else {
        lcd_.drawStringScaled(10, 50, state.headline, pal.fg, pal.bg, 1, 3, 50);
    }

    // Subtitle
    lcd_.drawStringScaled(12, 106, state.subtitle, pal.accent, pal.bg, 1, 2, 48);

    // Footer bar
    lcd_.fillRect(8, 144, board::LCD_WIDTH - 16, 1, pal.border);
    char footer[52];
    snprintf(footer, sizeof(footer), "Mode: SIGNBOARD | Theme: %-6s | BL: %3u%%",
             themeName(state.theme), state.backlight_pct);
    lcd_.drawString(12, 152, footer, pal.muted, pal.bg, 1, 48);
}

void ScreenManager::renderDiagnostics(const SystemState& state, const ThemePalette& pal) {
    lcd_.drawCard(0, 0, board::LCD_WIDTH, board::LCD_HEIGHT, pal.bg, pal.border);
    lcd_.drawStringScaled(8, 6, "C6-MICROLINUX HARDWARE DIAGNOSTICS", pal.accent, pal.bg, 1, 2, 48);

    // 8 Color Bars (y: 28..64)
    static const uint16_t BARS[8] = {
        lcd::COLOR_WHITE, lcd::COLOR_AMBER, lcd::COLOR_CYAN, lcd::COLOR_GREEN,
        lcd::COLOR_PURPLE, lcd::COLOR_RED, 0x001F /*Blue*/, 0x7BEF /*Gray*/
    };
    for (int i = 0; i < 8; ++i) {
        lcd_.fillRect(8 + i * 38, 28, 36, 32, BARS[i]);
        lcd_.drawRect(8 + i * 38, 28, 36, 32, lcd::COLOR_WHITE);
    }

    char line1[54], line2[54], line3[54], line4[54], line5[54];
    snprintf(line1, sizeof(line1), "SoC   : ESP32-C6FH4 RISC-V @ %u MHz (8MB DIO)",
             static_cast<unsigned>(state.cpu_freq_mhz));
    snprintf(line2, sizeof(line2), "LCD   : ST7789 320x172 @ 80MHz SPI2 (BL=%u%%)",
             static_cast<unsigned>(state.backlight_pct));
    snprintf(line3, sizeof(line3), "SRAM  : %u KB free / %u KB total",
             static_cast<unsigned>(state.free_sram_bytes / 1024U),
             static_cast<unsigned>(state.total_sram_bytes / 1024U));
    snprintf(line4, sizeof(line4), "RootFS: LittleFS %u/%u KB | SD: %s",
             static_cast<unsigned>(state.rootfs_used_kb),
             static_cast<unsigned>(state.rootfs_total_kb),
             state.sd_mounted ? "MOUNTED" : "NONE");
    snprintf(line5, sizeof(line5), "SSHKey: %.42s", state.ssh_hostkey_fp);

    lcd_.drawString(8, 68, line1, pal.fg, pal.bg, 1, 50);
    lcd_.drawString(8, 84, line2, pal.fg, pal.bg, 1, 50);
    lcd_.drawString(8, 100, line3, pal.ok, pal.bg, 1, 50);
    lcd_.drawString(8, 116, line4, pal.accent, pal.bg, 1, 50);
    lcd_.drawString(8, 132, line5, pal.muted, pal.bg, 1, 50);

    char line6[54];
    snprintf(line6, sizeof(line6), "WS2812: mode=%s RGB=(%u,%u,%u) | Flip=%s",
             ledModeName(state.led_mode), state.led_r, state.led_g, state.led_b,
             state.landscape_flipped ? "YES" : "NO");
    lcd_.drawString(8, 150, line6, pal.warn, pal.bg, 1, 50);
}

}  // namespace c6os
