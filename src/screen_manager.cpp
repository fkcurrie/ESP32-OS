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
    bool full_redraw = false;
    if (force_full_clear || state.screen_mode != last_mode_ || state.theme != last_theme_ ||
        (last_alert_ && !state.alert_active) || (last_ticker_ && !state.ticker_enabled) ||
        (state.screen_mode == ScreenMode::IMAGE_VIEW && state.fbi_pattern != last_fbi_pattern_)) {
        lcd_.fillScreen(pal.bg);
        last_mode_ = state.screen_mode;
        last_theme_ = state.theme;
        last_fbi_pattern_ = state.fbi_pattern;
        full_redraw = true;
    }
    last_alert_ = state.alert_active;
    last_ticker_ = state.ticker_enabled;

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
        case ScreenMode::MONITOR:
            renderMonitor(state, pal, full_redraw);
            break;
        case ScreenMode::IMAGE_VIEW:
            renderImageView(state, pal);
            break;
        case ScreenMode::OS_DASHBOARD:
        default:
            renderDashboard(state, pal, full_redraw);
            break;
    }

    renderOverlays(state, pal);
}

void ScreenManager::renderTickerOnly(const SystemState& state) {
    if (!state.ticker_enabled || state.ticker_text[0] == '\0') {
        return;
    }
    const ThemePalette pal = getThemePalette(state.theme);
    renderTickerBand(state, pal);
}

void ScreenManager::renderDashboard(const SystemState& state, const ThemePalette& pal, bool full_redraw) {
    // 1. Header Bar (y: 0..22)
    if (full_redraw) {
        lcd_.fillRect(0, 0, board::LCD_WIDTH, 22, pal.bg);
        lcd_.fillRect(0, 21, board::LCD_WIDTH, 1, pal.border);
    }

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
    if (full_redraw) {
        lcd_.drawCard(2, 25, 316, 52, pal.bg, pal.border);
    }
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
    if (full_redraw) {
        lcd_.drawCard(2, 80, 316, 41, pal.bg, pal.border);
    }
    unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
    unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
    unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
    uint8_t ram_pct   = (total_kb > 0) ? static_cast<uint8_t>((used_kb * 100U) / total_kb) : 0U;

    char ram_line[54];
    unsigned hrs  = static_cast<unsigned>(state.uptime_sec / 3600U);
    unsigned mins = static_cast<unsigned>((state.uptime_sec % 3600U) / 60U);
    unsigned secs = static_cast<unsigned>(state.uptime_sec % 60U);
    int temp_int  = static_cast<int>(state.cpu_temp_deci_c / 10);
    int temp_frac = static_cast<int>(state.cpu_temp_deci_c % 10);
    if (temp_frac < 0) temp_frac = -temp_frac;
    snprintf(ram_line, sizeof(ram_line), "SRAM:%u/%uK(%u%%) %d.%dC BL:%u%% UP %02u:%02u:%02u",
             free_kb, total_kb, static_cast<unsigned>(100U - ram_pct),
             temp_int, temp_frac,
             static_cast<unsigned>(state.backlight_pct), hrs, mins, secs);
    lcd_.drawString(8, 86, ram_line, pal.fg, pal.bg, 1, 50);

    lcd_.drawProgressBar(8, 100, 304, 14, ram_pct, pal.accent, pal.bg);

    // 4. User Banner / CLI Controlled Message Card (y: 124..170, h: 46)
    if (full_redraw || state.custom_bar_visible != last_custom_bar_) {
        lcd_.drawCard(2, 124, 316, 46, pal.bg, pal.accent);
        last_custom_bar_ = state.custom_bar_visible;
    } else {
        lcd_.drawRect(2, 124, 316, 46, pal.accent);
    }
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
        const char* line = (i < state.fb_line_count) ? state.getFbLine(i) : "";
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

void ScreenManager::renderMonitor(const SystemState& state, const ThemePalette& pal, bool full_redraw) {
    // 1. Header Bar (y: 0..16)
    if (full_redraw) {
        lcd_.fillRect(0, 0, board::LCD_WIDTH, 16, pal.accent);
    }
    char hdr[54];
    int t_int = static_cast<int>(state.cpu_temp_deci_c / 10);
    int t_frc = static_cast<int>(state.cpu_temp_deci_c % 10);
    if (t_frc < 0) t_frc = -t_frc;
    snprintf(hdr, sizeof(hdr), " HTOP TELEMETRY | CPU:%2u%% | %d.%dC | %ddBm ",
             static_cast<unsigned>(state.cpu_load_pct), t_int, t_frc,
             static_cast<int>(state.wifi_rssi));
    lcd_.drawString(2, 4, hdr, pal.bg, pal.accent, 1, 52);

    // 2. CPU Sparkline Graph Card (x: 2..158, y: 19..83)
    if (full_redraw) {
        lcd_.drawCard(2, 19, 156, 64, pal.bg, pal.border);
        lcd_.drawString(6, 23, "CPU Load Sparkline", pal.accent, pal.bg, 1, 24);
    }
    for (int i = 0; i < 32; ++i) {
        size_t idx = (state.cpu_history_idx + i) % 32;
        uint8_t val = state.cpu_history[idx];
        if (val > 100) val = 100;
        int16_t bar_h = static_cast<int16_t>((val * 44) / 100);
        if (bar_h < 2) bar_h = 2;
        int16_t bx = static_cast<int16_t>(6 + i * 4);
        int16_t by = static_cast<int16_t>(78 - bar_h);
        lcd_.fillRect(bx, 34, 3, 44 - bar_h, pal.bg);
        uint16_t col = (val > 75) ? pal.warn : pal.ok;
        lcd_.fillRect(bx, by, 3, bar_h, col);
    }

    // 3. Memory, Thermal & Network Gauges (x: 162..318, y: 19..83)
    if (full_redraw) {
        lcd_.drawCard(162, 19, 156, 64, pal.bg, pal.border);
    }
    unsigned tot_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
    unsigned fre_kb = static_cast<unsigned>(state.free_sram_bytes / 1024U);
    unsigned usd_kb = (tot_kb > fre_kb) ? (tot_kb - fre_kb) : 0U;
    uint8_t ram_pct = (tot_kb > 0) ? static_cast<uint8_t>((usd_kb * 100U) / tot_kb) : 0U;

    char mline[32];
    snprintf(mline, sizeof(mline), "MEM: %u/%uK (%u%%)", usd_kb, tot_kb, ram_pct);
    lcd_.drawString(166, 23, mline, pal.fg, pal.bg, 1, 24);
    lcd_.drawProgressBar(166, 33, 146, 10, ram_pct, pal.accent, pal.bg);

    uint8_t thm_pct = (t_int > 20) ? static_cast<uint8_t>((t_int - 20) * 100 / 65) : 5;
    if (thm_pct > 100) thm_pct = 100;
    snprintf(mline, sizeof(mline), "DIE: %d.%d C (BTN:%u)", t_int, t_frc,
             static_cast<unsigned>(state.button_press_count));
    lcd_.drawString(166, 47, mline, pal.fg, pal.bg, 1, 24);
    lcd_.drawProgressBar(166, 57, 146, 10, thm_pct, (t_int >= 65) ? pal.warn : pal.ok, pal.bg);

    snprintf(mline, sizeof(mline), "NET: RX %uK TX %uK",
             static_cast<unsigned>(state.net_rx_bytes / 1024U),
             static_cast<unsigned>(state.net_tx_bytes / 1024U));
    lcd_.drawString(166, 71, mline, pal.muted, pal.bg, 1, 24);

    // 4. Live Process Table (y: 86..170)
    if (full_redraw) {
        lcd_.drawCard(2, 86, 316, 84, pal.bg, pal.border);
        lcd_.drawString(6, 90, "PID USER  S  CPU%  MEM%  COMMAND", pal.accent, pal.bg, 1, 50);
        lcd_.fillRect(4, 99, 312, 1, pal.border);
        lcd_.drawString(6, 103, "  1 root  S   0.2   1.1  /sbin/init", pal.fg, pal.bg, 1, 50);
        lcd_.drawString(6, 114, " 18 root  S   1.4   4.2  [st7789_fb0]", pal.ok, pal.bg, 1, 50);
        lcd_.drawString(6, 136, " 45 root  S   0.3   2.1  httpd:80 + syslogd:514", pal.fg, pal.bg, 1, 50);
    } else {
        lcd_.drawRect(2, 86, 316, 84, pal.border);
    }

    char pline[54];
    snprintf(pline, sizeof(pline), " 29 root  S   %u.2   8.4  /usr/sbin/sshd (%u act)",
             static_cast<unsigned>(state.ssh_active_sessions > 0 ? 3 : 0),
             static_cast<unsigned>(state.ssh_active_sessions));
    lcd_.drawString(6, 125, pline, pal.fg, pal.bg, 1, 50);
    snprintf(pline, sizeof(pline), " 61 root  R   %u.5   3.8  /bin/c6-htop --lcd",
             static_cast<unsigned>(state.cpu_load_pct / 3));
    lcd_.drawString(6, 147, pline, pal.accent, pal.bg, 1, 50);
    snprintf(pline, sizeof(pline), "UP: %us | Cron: %u | IP: %s",
             static_cast<unsigned>(state.uptime_sec),
             static_cast<unsigned>(state.activeCronCount()),
             state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip);
    lcd_.drawString(6, 158, pline, pal.muted, pal.bg, 1, 50);
}

void ScreenManager::renderImageView(const SystemState& state, const ThemePalette& pal) {
    lcd_.drawCard(0, 0, board::LCD_WIDTH, board::LCD_HEIGHT, pal.bg, pal.border);

    if (state.fbi_pattern == 1) {
        // Pattern 1: Linux Tux Penguin Graphical Art
        int16_t cx = 80, cy = 78;
        // Body (black/dark outline + white belly)
        lcd_.fillRect(cx - 26, cy - 38, 52, 74, 0x18E3);
        lcd_.drawRect(cx - 26, cy - 38, 52, 74, pal.accent);
        lcd_.fillRect(cx - 16, cy - 14, 32, 44, lcd::COLOR_WHITE);
        // Eyes
        lcd_.fillRect(cx - 12, cy - 30, 8, 8, lcd::COLOR_WHITE);
        lcd_.fillRect(cx + 4,  cy - 30, 8, 8, lcd::COLOR_WHITE);
        lcd_.fillRect(cx - 9,  cy - 27, 4, 4, lcd::COLOR_BLACK);
        lcd_.fillRect(cx + 6,  cy - 27, 4, 4, lcd::COLOR_BLACK);
        // Beak & Feet (Amber/Yellow)
        lcd_.fillRect(cx - 10, cy - 20, 20, 7, lcd::COLOR_AMBER);
        lcd_.fillRect(cx - 22, cy + 32, 16, 7, lcd::COLOR_AMBER);
        lcd_.fillRect(cx + 6,  cy + 32, 16, 7, lcd::COLOR_AMBER);

        lcd_.drawStringScaled(140, 28, "TUX RISC-V", pal.accent, pal.bg, 2, 2, 14);
        lcd_.drawStringScaled(140, 56, "Linux 6.12.0-c6os", pal.ok, pal.bg, 1, 2, 26);
        lcd_.drawString(140, 82, "Arch: rv32imac_zicsr", pal.fg, pal.bg, 1, 26);
        lcd_.drawString(140, 96, "CPU : ESP32-C6 @ 160MHz", pal.fg, pal.bg, 1, 26);
        lcd_.drawString(140, 110, "FB0 : ST7789 320x172", pal.muted, pal.bg, 1, 26);
    } else if (state.fbi_pattern == 2) {
        // Pattern 2: Digital Matrix Rain
        for (int col = 0; col < 26; ++col) {
            int16_t x = static_cast<int16_t>(6 + col * 12);
            uint32_t seed = (static_cast<uint32_t>(col) * 37U + state.uptime_sec * 5U);
            for (int row = 0; row < 10; ++row) {
                int16_t y = static_cast<int16_t>(8 + row * 13);
                char glyph[2] = { static_cast<char>('0' + ((seed + row * 7) % 2)), '\0' };
                uint16_t c = ((row + seed) % 5 == 0) ? lcd::COLOR_WHITE
                           : ((row + seed) % 2 == 0) ? lcd::COLOR_GREEN : 0x03E0;
                lcd_.drawString(x, y, glyph, c, pal.bg, 1, 1);
            }
        }
        lcd_.drawCard(48, 48, 224, 52, pal.bg, lcd::COLOR_GREEN);
        lcd_.drawStringScaled(62, 56, "WAKE UP, ROOT...", lcd::COLOR_GREEN, pal.bg, 2, 2, 16);
        lcd_.drawString(62, 82, "C6-MicroLinux Matrix Engine", lcd::COLOR_WHITE, pal.bg, 1, 32);
    } else if (state.fbi_pattern == 3) {
        // Pattern 3: High-Contrast Calibration Grid
        for (int16_t x = 0; x < board::LCD_WIDTH; x += 32) {
            lcd_.fillRect(x, 0, 1, board::LCD_HEIGHT, pal.border);
        }
        for (int16_t y = 0; y < board::LCD_HEIGHT; y += 28) {
            lcd_.fillRect(0, y, board::LCD_WIDTH, 1, pal.border);
        }
        lcd_.drawRect(20, 20, 280, 110, pal.accent);
        lcd_.drawRect(60, 40, 200, 70, pal.ok);
        lcd_.fillRect(158, 0, 4, board::LCD_HEIGHT, pal.warn);
        lcd_.fillRect(0, 84, board::LCD_WIDTH, 4, pal.warn);
        lcd_.drawStringScaled(72, 62, "320x172 RGB565", lcd::COLOR_WHITE, pal.bg, 2, 2, 16);
    } else {
        // Pattern 0 (Default): C6-MicroLinux Cyber Emblem
        lcd_.drawRect(10, 10, 116, 116, pal.accent);
        lcd_.drawRect(18, 18, 100, 100, pal.ok);
        lcd_.fillRect(30, 30, 76, 76, pal.border);
        lcd_.drawStringScaled(42, 48, "C6OS", lcd::COLOR_WHITE, pal.border, 2, 3, 6);
        lcd_.drawStringScaled(140, 22, "C6-MicroLinux", pal.accent, pal.bg, 2, 2, 14);
        lcd_.drawStringScaled(140, 50, "RISC-V 32-bit OS", pal.ok, pal.bg, 1, 2, 24);
        lcd_.drawString(140, 76, "Framebuffer Image Viewer", pal.fg, pal.bg, 1, 26);
        lcd_.drawString(140, 92, "fbi [logo|tux|matrix|grid]", pal.muted, pal.bg, 1, 26);
    }

    // Bottom caption bar
    lcd_.fillRect(2, 144, 316, 26, pal.bg);
    lcd_.fillRect(2, 144, 316, 1, pal.accent);
    lcd_.drawStringScaled(8, 149, state.fbi_caption, pal.fg, pal.bg, 1, 2, 50);
}

void ScreenManager::renderTickerBand(const SystemState& state, const ThemePalette& pal) {
    lcd_.fillRect(0, 153, board::LCD_WIDTH, 1, pal.accent);
    lcd_.fillRect(0, 154, board::LCD_WIDTH, 2, pal.bg);
    lcd_.fillRect(0, 156, 4, 16, pal.bg);
    lcd_.fillRect(316, 156, 4, 16, pal.bg);

    char window[54];
    size_t len = strlen(state.ticker_text);
    if (len == 0) len = 1;
    size_t start = static_cast<size_t>(state.ticker_offset) % (len + 8);
    for (size_t i = 0; i < 52; ++i) {
        size_t idx = (start + i) % (len + 8);
        window[i] = (idx < len) ? state.ticker_text[idx] : ' ';
    }
    window[52] = '\0';
    lcd_.drawStringScaled(4, 156, window, pal.warn, pal.bg, 1, 2, 52);
}

void ScreenManager::renderOverlays(const SystemState& state, const ThemePalette& pal) {
    // 1. Bottom Scrolling Marquee Ticker (y: 153..171)
    if (state.ticker_enabled && state.ticker_text[0] != '\0') {
        renderTickerBand(state, pal);
    }

    // 2. High-Visibility Flashing Alert Overlay Banner (y: 44..124)
    if (state.alert_active && state.alert_text[0] != '\0') {
        bool invert = ((state.uptime_sec % 2U) == 1U);
        uint16_t box_bg = invert ? lcd::COLOR_AMBER : lcd::COLOR_RED;
        uint16_t box_fg = invert ? lcd::COLOR_BLACK : lcd::COLOR_WHITE;
        lcd_.fillRect(10, 44, 300, 80, box_bg);
        lcd_.drawRect(10, 44, 300, 80, lcd::COLOR_WHITE);
        lcd_.drawRect(13, 47, 294, 74, lcd::COLOR_BLACK);
        lcd_.drawStringScaled(22, 54, "!! SYSTEM ALERT !!", box_fg, box_bg, 2, 2, 22);
        lcd_.drawStringScaled(22, 82, state.alert_text, box_fg, box_bg, 1, 2, 46);
        lcd_.drawString(22, 108, "Press BOOT button or 'screen alert off' to clear", box_fg, box_bg, 1, 46);
    }
}

}  // namespace c6os
