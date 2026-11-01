#include "c6os_display.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {
namespace display {

namespace {

constexpr uint16_t COL_BLACK  = 0x0000;
constexpr uint16_t COL_WHITE  = 0xFFFF;
constexpr uint16_t COL_CYAN   = 0x07FF;
constexpr uint16_t COL_GREEN  = 0x07E0;
constexpr uint16_t COL_AMBER  = 0xFFE0;
constexpr uint16_t COL_RED    = 0xF800;
constexpr uint16_t COL_PURPLE = 0xF81F;

// Standard 5x7 ASCII bitmap font (0x20..0x7E)
const uint8_t FONT_5X7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // 0x20 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // 0x21 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // 0x22 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // 0x23 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // 0x24 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // 0x25 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // 0x26 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // 0x27 '''
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // 0x28 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // 0x29 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // 0x2A '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // 0x2B '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // 0x2C ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // 0x2D '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // 0x2E '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // 0x2F '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0x30 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 0x31 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 0x32 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 0x33 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 0x34 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 0x35 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 0x36 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 0x37 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 0x38 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 0x39 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // 0x3A ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // 0x3B ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // 0x3C '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // 0x3D '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // 0x3E '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // 0x3F '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // 0x40 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 0x41 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 0x42 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 0x43 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 0x44 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 0x45 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // 0x46 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // 0x47 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 0x48 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 0x49 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 0x4A 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 0x4B 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 0x4C 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 0x4D 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 0x4E 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 0x4F 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 0x50 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 0x51 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 0x52 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 0x53 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 0x54 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 0x55 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 0x56 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // 0x57 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 0x58 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07},  // 0x59 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 0x5A 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // 0x5B '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // 0x5C '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // 0x5D ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // 0x5E '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // 0x5F '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // 0x60 '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 0x61 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // 0x62 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 0x63 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // 0x64 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 0x65 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // 0x66 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // 0x67 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // 0x68 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // 0x69 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // 0x6A 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // 0x6B 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // 0x6C 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // 0x6D 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // 0x6E 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 0x6F 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // 0x70 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // 0x71 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // 0x72 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 0x73 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // 0x74 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // 0x75 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // 0x76 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // 0x77 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 0x78 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // 0x79 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // 0x7A 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // 0x7B '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // 0x7C '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // 0x7D '}'
    {0x06, 0x09, 0x09, 0x06, 0x00}   // 0x7E '~'
};

class StripCanvas {
public:
    StripCanvas(uint16_t* buf, int16_t y0, int16_t strip_h, bool big_endian)
        : buf_(buf), y0_(y0), y1_(y0 + strip_h), strip_h_(strip_h), be_(big_endian) {}

    uint16_t encode(uint16_t c) const {
        return be_ ? static_cast<uint16_t>((c >> 8) | (c << 8)) : c;
    }

    void clear(uint16_t color) {
        const uint16_t px = encode(color);
        const size_t total = static_cast<size_t>(FB_WIDTH) * static_cast<size_t>(strip_h_);
        for (size_t i = 0; i < total; ++i) {
            buf_[i] = px;
        }
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if (w <= 0 || h <= 0) return;
        int16_t rx0 = (x < 0) ? 0 : x;
        int16_t rx1 = (x + w > FB_WIDTH) ? FB_WIDTH : (x + w);
        int16_t ry0 = (y < y0_) ? y0_ : y;
        int16_t ry1 = (y + h > y1_) ? y1_ : (y + h);
        if (rx0 >= rx1 || ry0 >= ry1) return;

        const uint16_t px = encode(color);
        for (int16_t py = ry0; py < ry1; ++py) {
            uint16_t* row = buf_ + static_cast<size_t>(py - y0_) * FB_WIDTH;
            for (int16_t px_x = rx0; px_x < rx1; ++px_x) {
                row[px_x] = px;
            }
        }
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if (w <= 0 || h <= 0) return;
        if (y + h <= y0_ || y >= y1_) return;
        fillRect(x, y, w, 1, color);
        fillRect(x, y + h - 1, w, 1, color);
        fillRect(x, y, 1, h, color);
        fillRect(x + w - 1, y, 1, h, color);
    }

    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg_color, uint16_t border_color) {
        if (y + h <= y0_ || y >= y1_) return;
        fillRect(x + 1, y + 1, w - 2, h - 2, bg_color);
        drawRect(x, y, w, h, border_color);
    }

    void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct,
                         uint16_t fill_color, uint16_t track_color) {
        if (y + h <= y0_ || y >= y1_) return;
        if (pct > 100) pct = 100;
        drawRect(x, y, w, h, COL_WHITE);
        const int16_t inner_w = w - 4;
        const int16_t inner_h = h - 4;
        if (inner_w <= 0 || inner_h <= 0) return;
        int16_t fill_w = static_cast<int16_t>((static_cast<int32_t>(inner_w) * pct) / 100);
        if (fill_w > 0) fillRect(x + 2, y + 2, fill_w, inner_h, fill_color);
        if (fill_w < inner_w) fillRect(x + 2 + fill_w, y + 2, inner_w - fill_w, inner_h, track_color);
    }

    void drawCharScaled(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg,
                        uint8_t scale_x, uint8_t scale_y) {
        if (scale_x == 0) scale_x = 1;
        if (scale_y == 0) scale_y = 1;
        const int16_t char_w = 6 * scale_x;
        const int16_t char_h = 8 * scale_y;
        if (y + char_h <= y0_ || y >= y1_ || x + char_w <= 0 || x >= FB_WIDTH) return;
        if (c < 0x20 || c > 0x7E) c = '?';

        const uint8_t* glyph = FONT_5X7[c - 0x20];
        const uint16_t fg_px = encode(fg);
        const uint16_t bg_px = encode(bg);

        int16_t ry0 = (y < y0_) ? y0_ : y;
        int16_t ry1 = (y + char_h > y1_) ? y1_ : (y + char_h);

        for (int16_t py = ry0; py < ry1; ++py) {
            uint8_t glyph_row = static_cast<uint8_t>((py - y) / scale_y);
            uint16_t* dst_row = buf_ + static_cast<size_t>(py - y0_) * FB_WIDTH;
            int16_t px_x = x;
            for (uint8_t col = 0; col < 6; ++col) {
                bool on = (col < 5 && glyph_row < 7) && (((glyph[col] >> glyph_row) & 0x01U) != 0);
                uint16_t px = on ? fg_px : bg_px;
                for (uint8_t sx = 0; sx < scale_x; ++sx) {
                    if (px_x >= 0 && px_x < FB_WIDTH) {
                        dst_row[px_x] = px;
                    }
                    ++px_x;
                }
            }
        }
    }

    void drawStringScaled(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg,
                          uint8_t scale_x, uint8_t scale_y, uint8_t max_chars = 0) {
        if (!str) return;
        if (scale_x == 0) scale_x = 1;
        if (scale_y == 0) scale_y = 1;
        const int16_t char_h = 8 * scale_y;
        if (y + char_h <= y0_ || y >= y1_) return;

        const int16_t step = 6 * scale_x;
        uint8_t count = 0;
        while (*str && (max_chars == 0 || count < max_chars)) {
            if (x + step > FB_WIDTH) break;
            drawCharScaled(x, y, *str++, fg, bg, scale_x, scale_y);
            x += step;
            ++count;
        }
        while (max_chars > 0 && count < max_chars && x + step <= FB_WIDTH) {
            drawCharScaled(x, y, ' ', fg, bg, scale_x, scale_y);
            x += step;
            ++count;
        }
    }

    void drawString(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg,
                    uint8_t scale = 1, uint8_t max_chars = 0) {
        drawStringScaled(x, y, str, fg, bg, scale, scale, max_chars);
    }

private:
    uint16_t* buf_;
    int16_t y0_;
    int16_t y1_;
    int16_t strip_h_;
    bool be_;
};

void renderDashboard(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.fillRect(0, 0, FB_WIDTH, 22, pal.bg);
    c.fillRect(0, 21, FB_WIDTH, 1, pal.border);

    char hdr_left[36];
    snprintf(hdr_left, sizeof(hdr_left), "root@%s", s.hostname);
    c.drawStringScaled(4, 3, hdr_left, pal.accent, pal.bg, 1, 2, 22);

    char hdr_right[28];
    if (s.wifi_sta_connected) {
        snprintf(hdr_right, sizeof(hdr_right), "WLAN %ddBm", static_cast<int>(s.wifi_rssi));
        c.drawStringScaled(224, 3, hdr_right, pal.ok, pal.bg, 1, 2, 15);
    } else if (s.wifi_ap_enabled) {
        snprintf(hdr_right, sizeof(hdr_right), "AP:%s", s.wifi_ap_ip);
        c.drawStringScaled(224, 3, hdr_right, pal.warn, pal.bg, 1, 2, 15);
    } else {
        c.drawStringScaled(224, 3, "WLAN DOWN", pal.alert, pal.bg, 1, 2, 15);
    }

    c.drawCard(2, 25, 316, 52, pal.bg, pal.border);
    char ssh_cmd[48];
    const char* active_ip = s.wifi_sta_connected ? s.wifi_ip : s.wifi_ap_ip;
    snprintf(ssh_cmd, sizeof(ssh_cmd), "ssh root@%s", active_ip);
    c.drawStringScaled(8, 30, ssh_cmd, pal.accent, pal.bg, 1, 2, 26);

    if (s.ssh_active_sessions > 0) {
        c.drawStringScaled(242, 30, "[SSH LIVE]", pal.ok, pal.bg, 1, 2, 11);
    } else {
        c.drawStringScaled(242, 30, "[PORT :22]", pal.muted, pal.bg, 1, 2, 11);
    }

    char net_sub[58];
    if (s.wifi_sta_connected) {
        snprintf(net_sub, sizeof(net_sub), "SSID: %-16.16s  AP: %-3s  Logins: %u",
                 s.wifi_ssid, s.wifi_ap_enabled ? "ON" : "OFF",
                 static_cast<unsigned>(s.ssh_total_logins));
    } else {
        snprintf(net_sub, sizeof(net_sub), "Join AP '%s' or USB /dev/ttyACM0 ('menu')",
                 s.wifi_ap_ssid);
    }
    c.drawString(8, 56, net_sub, pal.fg, pal.bg, 1, 50);

    c.drawCard(2, 80, 316, 41, pal.bg, pal.border);
    unsigned total_kb = static_cast<unsigned>(s.total_sram_bytes / 1024U);
    unsigned free_kb  = static_cast<unsigned>(s.free_sram_bytes / 1024U);
    unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
    uint8_t ram_pct   = (total_kb > 0) ? static_cast<uint8_t>((used_kb * 100U) / total_kb) : 0U;

    char ram_line[58];
    unsigned hrs  = static_cast<unsigned>(s.uptime_sec / 3600U);
    unsigned mins = static_cast<unsigned>((s.uptime_sec % 3600U) / 60U);
    unsigned secs = static_cast<unsigned>(s.uptime_sec % 60U);
    int temp_int  = static_cast<int>(s.cpu_temp_deci_c / 10);
    int temp_frac = abs(static_cast<int>(s.cpu_temp_deci_c % 10));
    snprintf(ram_line, sizeof(ram_line), "SRAM:%u/%uK(%u%%) %d.%dC BL:%u%% UP %02u:%02u:%02u",
             free_kb, total_kb, static_cast<unsigned>(100U - ram_pct),
             temp_int, temp_frac,
             static_cast<unsigned>(s.backlight_pct), hrs, mins, secs);
    c.drawString(8, 86, ram_line, pal.fg, pal.bg, 1, 50);
    c.drawProgressBar(8, 100, 304, 14, ram_pct, pal.accent, pal.bg);

    c.drawCard(2, 124, 316, 46, pal.bg, pal.accent);
    if (s.custom_bar_visible) {
        c.drawString(8, 130, s.custom_bar_label, pal.accent, pal.bg, 1, 50);
        c.drawProgressBar(8, 144, 304, 18, s.custom_bar_pct, pal.ok, pal.bg);
    } else {
        c.drawStringScaled(8, 129, s.headline, pal.fg, pal.bg, 1, 2, 50);
        c.drawString(8, 152, s.subtitle, pal.accent, pal.bg, 1, 50);
    }
}

void renderFbConsole(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.fillRect(0, 0, FB_WIDTH, 14, pal.accent);
    char top_bar[58];
    const char* ip = s.wifi_sta_connected ? s.wifi_ip : s.wifi_ap_ip;
    snprintf(top_bar, sizeof(top_bar), " /dev/tty0 | root@%s | %s ", s.hostname, ip);
    c.drawString(2, 3, top_bar, pal.bg, pal.accent, 1, 52);

    for (size_t i = 0; i < FB_CONSOLE_ROWS; ++i) {
        int16_t y = static_cast<int16_t>(17 + i * 8);
        const char* line = (i < s.fb_line_count) ? s.fb_lines[i] : "";
        uint16_t line_color = pal.fg;
        if (line[0] == '[') {
            line_color = pal.ok;
        } else if (strncmp(line, "root@", 5) == 0 || strncmp(line, "tty0>", 5) == 0) {
            line_color = pal.accent;
        }
        c.drawString(4, y, line, line_color, pal.bg, 1, FB_CONSOLE_COLS);
    }

    c.drawString(4, 162, "root@c6-linux:/dev/tty0# _", pal.accent, pal.bg, 1, 52);
}

void renderSignboard(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.drawRect(0, 0, FB_WIDTH, FB_HEIGHT, pal.border);
    c.drawRect(3, 3, FB_WIDTH - 6, FB_HEIGHT - 6, pal.accent);

    char badge[56];
    const char* ip = s.wifi_sta_connected ? s.wifi_ip : s.wifi_ap_ip;
    snprintf(badge, sizeof(badge), "[ %s | SSH %s ]", s.hostname, ip);
    c.drawString(12, 10, badge, pal.muted, pal.bg, 1, 48);
    c.fillRect(8, 22, FB_WIDTH - 16, 1, pal.border);

    size_t hlen = strlen(s.headline);
    if (hlen <= 12) {
        c.drawStringScaled(12, 42, s.headline, pal.fg, pal.bg, 2, 3, 25);
    } else if (hlen <= 25) {
        c.drawStringScaled(12, 46, s.headline, pal.fg, pal.bg, 2, 2, 25);
    } else {
        c.drawStringScaled(10, 50, s.headline, pal.fg, pal.bg, 1, 3, 50);
    }

    c.drawStringScaled(12, 106, s.subtitle, pal.accent, pal.bg, 1, 2, 48);

    c.fillRect(8, 144, FB_WIDTH - 16, 1, pal.border);
    char footer[56];
    snprintf(footer, sizeof(footer), "Mode: SIGNBOARD | Theme: %-6s | BL: %3u%%",
             themeName(s.theme), s.backlight_pct);
    c.drawString(12, 152, footer, pal.muted, pal.bg, 1, 48);
}

void renderDiagnostics(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.drawCard(0, 0, FB_WIDTH, FB_HEIGHT, pal.bg, pal.border);
    c.drawStringScaled(8, 6, "C6-MICROLINUX HARDWARE DIAGNOSTICS", pal.accent, pal.bg, 1, 2, 48);

    static const uint16_t BARS[8] = {
        COL_WHITE, COL_AMBER, COL_CYAN, COL_GREEN,
        COL_PURPLE, COL_RED, 0x001F, 0x7BEF
    };
    for (int i = 0; i < 8; ++i) {
        c.fillRect(8 + i * 38, 28, 36, 32, BARS[i]);
        c.drawRect(8 + i * 38, 28, 36, 32, COL_WHITE);
    }

    char line1[56], line2[56], line3[56], line4[56], line5[56], line6[56];
    snprintf(line1, sizeof(line1), "SoC   : ESP32-C6FH4 RISC-V @ %u MHz (8MB QIO)",
             static_cast<unsigned>(s.cpu_freq_mhz));
    snprintf(line2, sizeof(line2), "LCD   : ST7789 320x172 @ 80MHz SPI2 DMA (BL=%u%%)",
             static_cast<unsigned>(s.backlight_pct));
    snprintf(line3, sizeof(line3), "SRAM  : %u KB free / %u KB total",
             static_cast<unsigned>(s.free_sram_bytes / 1024U),
             static_cast<unsigned>(s.total_sram_bytes / 1024U));
    snprintf(line4, sizeof(line4), "RootFS: LittleFS %u/%u KB | SD: %s",
             static_cast<unsigned>(s.rootfs_used_kb),
             static_cast<unsigned>(s.rootfs_total_kb),
             s.sd_mounted ? "MOUNTED" : "NONE");
    snprintf(line5, sizeof(line5), "SSHKey: %.42s", s.ssh_hostkey_fp);
    snprintf(line6, sizeof(line6), "WS2812: mode=%s RGB=(%u,%u,%u) | Flip=%s",
             ledModeName(s.led_mode), s.led_r, s.led_g, s.led_b,
             s.landscape_flipped ? "YES" : "NO");

    c.drawString(8, 68, line1, pal.fg, pal.bg, 1, 50);
    c.drawString(8, 84, line2, pal.fg, pal.bg, 1, 50);
    c.drawString(8, 100, line3, pal.ok, pal.bg, 1, 50);
    c.drawString(8, 116, line4, pal.accent, pal.bg, 1, 50);
    c.drawString(8, 132, line5, pal.muted, pal.bg, 1, 50);
    c.drawString(8, 150, line6, pal.warn, pal.bg, 1, 50);
}

void renderMonitor(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.fillRect(0, 0, FB_WIDTH, 16, pal.accent);
    char hdr[58];
    int t_int = static_cast<int>(s.cpu_temp_deci_c / 10);
    int t_frc = abs(static_cast<int>(s.cpu_temp_deci_c % 10));
    snprintf(hdr, sizeof(hdr), " HTOP TELEMETRY | CPU:%2u%% | %d.%dC | %ddBm ",
             static_cast<unsigned>(s.cpu_load_pct), t_int, t_frc,
             static_cast<int>(s.wifi_rssi));
    c.drawString(2, 4, hdr, pal.bg, pal.accent, 1, 52);

    c.drawCard(2, 19, 156, 64, pal.bg, pal.border);
    c.drawString(6, 23, "CPU Load Sparkline", pal.accent, pal.bg, 1, 24);
    for (int i = 0; i < 32; ++i) {
        size_t idx = (s.cpu_history_idx + i) % 32;
        uint8_t val = s.cpu_history[idx];
        if (val > 100) val = 100;
        int16_t bar_h = static_cast<int16_t>((val * 44) / 100);
        if (bar_h < 2) bar_h = 2;
        int16_t bx = static_cast<int16_t>(6 + i * 4);
        int16_t by = static_cast<int16_t>(78 - bar_h);
        c.fillRect(bx, 34, 3, 44 - bar_h, pal.bg);
        uint16_t col = (val > 75) ? pal.warn : pal.ok;
        c.fillRect(bx, by, 3, bar_h, col);
    }

    c.drawCard(162, 19, 156, 64, pal.bg, pal.border);
    unsigned tot_kb = static_cast<unsigned>(s.total_sram_bytes / 1024U);
    unsigned fre_kb = static_cast<unsigned>(s.free_sram_bytes / 1024U);
    unsigned usd_kb = (tot_kb > fre_kb) ? (tot_kb - fre_kb) : 0U;
    uint8_t ram_pct = (tot_kb > 0) ? static_cast<uint8_t>((usd_kb * 100U) / tot_kb) : 0U;

    char mline[32];
    snprintf(mline, sizeof(mline), "MEM: %u/%uK (%u%%)", usd_kb, tot_kb, ram_pct);
    c.drawString(166, 23, mline, pal.fg, pal.bg, 1, 24);
    c.drawProgressBar(166, 33, 146, 10, ram_pct, pal.accent, pal.bg);

    uint8_t thm_pct = (t_int > 20) ? static_cast<uint8_t>((t_int - 20) * 100 / 65) : 5;
    if (thm_pct > 100) thm_pct = 100;
    snprintf(mline, sizeof(mline), "DIE: %d.%d C (BTN:%u)", t_int, t_frc,
             static_cast<unsigned>(s.button_press_count));
    c.drawString(166, 47, mline, pal.fg, pal.bg, 1, 24);
    c.drawProgressBar(166, 57, 146, 10, thm_pct, (t_int >= 65) ? pal.warn : pal.ok, pal.bg);

    snprintf(mline, sizeof(mline), "NET: RX %uK TX %uK",
             static_cast<unsigned>(s.net_rx_bytes / 1024U),
             static_cast<unsigned>(s.net_tx_bytes / 1024U));
    c.drawString(166, 71, mline, pal.muted, pal.bg, 1, 24);

    c.drawCard(2, 86, 316, 84, pal.bg, pal.border);
    c.drawString(6, 90, "PID USER  S  CPU%  MEM%  COMMAND", pal.accent, pal.bg, 1, 50);
    c.fillRect(4, 99, 312, 1, pal.border);

    char pline[56];
    c.drawString(6, 103, "  1 root  S   0.2   1.1  /sbin/init", pal.fg, pal.bg, 1, 50);
    c.drawString(6, 114, " 18 root  S   0.4   2.0  [st7789_dma]", pal.ok, pal.bg, 1, 50);
    snprintf(pline, sizeof(pline), " 29 root  S   %u.2   8.4  /usr/sbin/sshd (%u act)",
             static_cast<unsigned>(s.ssh_active_sessions > 0 ? 3 : 0),
             static_cast<unsigned>(s.ssh_active_sessions));
    c.drawString(6, 125, pline, pal.fg, pal.bg, 1, 50);
    c.drawString(6, 136, " 45 root  S   0.2   1.8  httpd:80 + syslogd:514", pal.fg, pal.bg, 1, 50);
    snprintf(pline, sizeof(pline), " 61 root  R   %u.5   3.2  /bin/c6-htop --lcd",
             static_cast<unsigned>(s.cpu_load_pct / 3));
    c.drawString(6, 147, pline, pal.accent, pal.bg, 1, 50);
    snprintf(pline, sizeof(pline), "UP: %us | Cron: %u | IP: %s",
             static_cast<unsigned>(s.uptime_sec),
             static_cast<unsigned>(s.active_cron_count),
             s.wifi_sta_connected ? s.wifi_ip : s.wifi_ap_ip);
    c.drawString(6, 158, pline, pal.muted, pal.bg, 1, 50);
}

void renderImageView(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    c.drawCard(0, 0, FB_WIDTH, FB_HEIGHT, pal.bg, pal.border);

    if (s.fbi_pattern == 1) {
        int16_t cx = 80, cy = 78;
        c.fillRect(cx - 26, cy - 38, 52, 74, 0x18E3);
        c.drawRect(cx - 26, cy - 38, 52, 74, pal.accent);
        c.fillRect(cx - 16, cy - 14, 32, 44, COL_WHITE);
        c.fillRect(cx - 12, cy - 30, 8, 8, COL_WHITE);
        c.fillRect(cx + 4,  cy - 30, 8, 8, COL_WHITE);
        c.fillRect(cx - 9,  cy - 27, 4, 4, COL_BLACK);
        c.fillRect(cx + 6,  cy - 27, 4, 4, COL_BLACK);
        c.fillRect(cx - 10, cy - 20, 20, 7, COL_AMBER);
        c.fillRect(cx - 22, cy + 32, 16, 7, COL_AMBER);
        c.fillRect(cx + 6,  cy + 32, 16, 7, COL_AMBER);

        c.drawStringScaled(140, 28, "TUX RISC-V", pal.accent, pal.bg, 2, 2, 14);
        c.drawStringScaled(140, 56, "Linux 6.12.0-c6os", pal.ok, pal.bg, 1, 2, 26);
        c.drawString(140, 82, "Arch: rv32imac_zicsr", pal.fg, pal.bg, 1, 26);
        c.drawString(140, 96, "CPU : ESP32-C6 @ 160MHz", pal.fg, pal.bg, 1, 26);
        c.drawString(140, 110, "FB0 : ST7789 320x172 DMA", pal.muted, pal.bg, 1, 26);
    } else if (s.fbi_pattern == 2) {
        for (int col = 0; col < 26; ++col) {
            int16_t x = static_cast<int16_t>(6 + col * 12);
            uint32_t rng = (static_cast<uint32_t>(col) * 37U + s.uptime_sec * 5U);
            for (int row = 0; row < 10; ++row) {
                int16_t y = static_cast<int16_t>(8 + row * 13);
                char glyph[2] = { static_cast<char>('0' + ((rng + row * 7) % 2)), '\0' };
                uint16_t col_px = ((row + rng) % 5 == 0) ? COL_WHITE
                                : ((row + rng) % 2 == 0) ? COL_GREEN : 0x03E0;
                c.drawString(x, y, glyph, col_px, pal.bg, 1, 1);
            }
        }
        c.drawCard(48, 48, 224, 52, pal.bg, COL_GREEN);
        c.drawStringScaled(62, 56, "WAKE UP, ROOT...", COL_GREEN, pal.bg, 2, 2, 16);
        c.drawString(62, 82, "C6-MicroLinux Matrix Engine", COL_WHITE, pal.bg, 1, 32);
    } else if (s.fbi_pattern == 3) {
        for (int16_t x = 0; x < FB_WIDTH; x += 32) {
            c.fillRect(x, 0, 1, FB_HEIGHT, pal.border);
        }
        for (int16_t y = 0; y < FB_HEIGHT; y += 28) {
            c.fillRect(0, y, FB_WIDTH, 1, pal.border);
        }
        c.drawRect(20, 20, 280, 110, pal.accent);
        c.drawRect(60, 40, 200, 70, pal.ok);
        c.fillRect(158, 0, 4, FB_HEIGHT, pal.warn);
        c.fillRect(0, 84, FB_WIDTH, 4, pal.warn);
        c.drawStringScaled(72, 62, "320x172 RGB565", COL_WHITE, pal.bg, 2, 2, 16);
    } else {
        c.drawRect(10, 10, 116, 116, pal.accent);
        c.drawRect(18, 18, 100, 100, pal.ok);
        c.fillRect(30, 30, 76, 76, pal.border);
        c.drawStringScaled(42, 48, "C6OS", COL_WHITE, pal.border, 2, 3, 6);
        c.drawStringScaled(140, 22, "C6-MicroLinux", pal.accent, pal.bg, 2, 2, 14);
        c.drawStringScaled(140, 50, "RISC-V 32-bit OS", pal.ok, pal.bg, 1, 2, 24);
        c.drawString(140, 76, "Framebuffer Image Viewer", pal.fg, pal.bg, 1, 26);
        c.drawString(140, 92, "fbi [logo|tux|matrix|grid]", pal.muted, pal.bg, 1, 26);
    }

    c.fillRect(2, 144, 316, 26, pal.bg);
    c.fillRect(2, 144, 316, 1, pal.accent);
    c.drawStringScaled(8, 149, s.fbi_caption, pal.fg, pal.bg, 1, 2, 50);
}

void renderOverlays(StripCanvas& c, const RenderSnapshot& s, const ThemePalette& pal) {
    if (s.ticker_enabled && s.ticker_text[0] != '\0') {
        c.fillRect(0, 153, FB_WIDTH, 1, pal.accent);
        c.fillRect(0, 154, FB_WIDTH, 18, pal.bg);

        char window[54];
        size_t len = strlen(s.ticker_text);
        if (len == 0) len = 1;
        size_t start = static_cast<size_t>(s.ticker_offset) % (len + 8);
        for (size_t i = 0; i < 52; ++i) {
            size_t idx = (start + i) % (len + 8);
            window[i] = (idx < len) ? s.ticker_text[idx] : ' ';
        }
        window[52] = '\0';
        c.drawStringScaled(4, 156, window, pal.warn, pal.bg, 1, 2, 52);
    }

    if (s.alert_active && s.alert_text[0] != '\0') {
        bool invert = ((s.uptime_sec % 2U) == 1U);
        uint16_t box_bg = invert ? COL_AMBER : COL_RED;
        uint16_t box_fg = invert ? COL_BLACK : COL_WHITE;
        c.fillRect(10, 44, 300, 80, box_bg);
        c.drawRect(10, 44, 300, 80, COL_WHITE);
        c.drawRect(13, 47, 294, 74, COL_BLACK);
        c.drawStringScaled(22, 54, "!! SYSTEM ALERT !!", box_fg, box_bg, 2, 2, 22);
        c.drawStringScaled(22, 82, s.alert_text, box_fg, box_bg, 1, 2, 46);
        c.drawString(22, 108, "Press BOOT button or 'screen alert off' to clear", box_fg, box_bg, 1, 46);
    }
}

bool endsWithIgnoreCase(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t slen = strlen(str);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return false;
    const char* tail = str + (slen - suflen);
    for (size_t i = 0; i < suflen; ++i) {
        if (tolower(static_cast<unsigned char>(tail[i])) !=
            tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

inline void rgb565ToRgb888(uint16_t px, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = static_cast<uint8_t>((px >> 11) & 0x1FU);
    uint8_t g6 = static_cast<uint8_t>((px >> 5) & 0x3FU);
    uint8_t b5 = static_cast<uint8_t>(px & 0x1FU);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void putLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void putLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

}  // namespace

bool stripOverlapsDirty(uint8_t dirty_mask, int16_t y0, int16_t y1) {
    if (dirty_mask == DIRTY_NONE) return false;
    if (dirty_mask == DIRTY_FULL) return true;
    auto overlaps = [&](int16_t a0, int16_t a1) {
        return (y0 < a1) && (y1 > a0);
    };
    if ((dirty_mask & DIRTY_HEADER) && overlaps(0, 24)) return true;
    if ((dirty_mask & DIRTY_CARDS) && overlaps(18, 172)) return true;
    if ((dirty_mask & DIRTY_TICKER) && overlaps(152, 172)) return true;
    if ((dirty_mask & DIRTY_ALERT) && overlaps(42, 128)) return true;
    if ((dirty_mask & DIRTY_CONSOLE) && overlaps(14, 172)) return true;
    return false;
}

void renderStrip(const RenderSnapshot& snap, int16_t y0, int16_t strip_h,
                 uint16_t* out_rgb565, bool big_endian_out) {
    if (!out_rgb565 || strip_h <= 0 || y0 >= FB_HEIGHT) return;
    if (y0 + strip_h > FB_HEIGHT) strip_h = FB_HEIGHT - y0;

    const ThemePalette pal = getThemePalette(snap.theme);
    StripCanvas canvas(out_rgb565, y0, strip_h, big_endian_out);
    canvas.clear(pal.bg);

    switch (snap.screen_mode) {
        case ScreenMode::FB_CONSOLE:
            renderFbConsole(canvas, snap, pal);
            break;
        case ScreenMode::SIGNBOARD:
            renderSignboard(canvas, snap, pal);
            break;
        case ScreenMode::DIAGNOSTICS:
            renderDiagnostics(canvas, snap, pal);
            break;
        case ScreenMode::MONITOR:
            renderMonitor(canvas, snap, pal);
            break;
        case ScreenMode::IMAGE_VIEW:
            renderImageView(canvas, snap, pal);
            break;
        case ScreenMode::OS_DASHBOARD:
        default:
            renderDashboard(canvas, snap, pal);
            break;
    }

    renderOverlays(canvas, snap, pal);
}

bool writeFramebufferImage(SystemState& state, VirtualFs& vfs,
                           const char* abs_path, size_t* out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (!abs_path) return false;

    FILE* f = vfs.openPosix(abs_path, "wb");
    if (!f) return false;

    auto* snap = static_cast<RenderSnapshot*>(malloc(sizeof(RenderSnapshot)));
    constexpr int16_t CHUNK_H = 8;
    auto* strip = static_cast<uint16_t*>(malloc(sizeof(uint16_t) * FB_WIDTH * CHUNK_H));
    auto* row_rgb = static_cast<uint8_t*>(malloc(FB_WIDTH * 3));
    if (!snap || !strip || !row_rgb) {
        free(snap);
        free(strip);
        free(row_rgb);
        fclose(f);
        return false;
    }
    snap->captureFrom(state, DIRTY_FULL);

    const bool is_bmp = endsWithIgnoreCase(abs_path, ".bmp");
    size_t total_written = 0;
    bool ok = true;

    if (is_bmp) {
        // 24-bit uncompressed Windows BITMAPINFOHEADER (320 * 3 = 960 bytes/row, already 4-byte aligned)
        const uint32_t img_bytes = static_cast<uint32_t>(FB_WIDTH) * FB_HEIGHT * 3U;
        const uint32_t file_size = 54U + img_bytes;
        uint8_t hdr[54];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 'B';
        hdr[1] = 'M';
        putLe32(hdr + 2, file_size);
        putLe32(hdr + 10, 54U);
        putLe32(hdr + 14, 40U);
        putLe32(hdr + 18, static_cast<uint32_t>(FB_WIDTH));
        // Negative height stores top-down scanlines cleanly in standard BMP viewers
        putLe32(hdr + 22, static_cast<uint32_t>(static_cast<int32_t>(-FB_HEIGHT)));
        putLe16(hdr + 26, 1U);
        putLe16(hdr + 28, 24U);
        putLe32(hdr + 34, img_bytes);
        if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) ok = false;
        total_written += sizeof(hdr);
    } else {
        char ppm_hdr[32];
        int hlen = snprintf(ppm_hdr, sizeof(ppm_hdr), "P6\n%d %d\n255\n", FB_WIDTH, FB_HEIGHT);
        if (hlen <= 0 || fwrite(ppm_hdr, 1, static_cast<size_t>(hlen), f) != static_cast<size_t>(hlen)) {
            ok = false;
        } else {
            total_written += static_cast<size_t>(hlen);
        }
    }

    for (int16_t y0 = 0; ok && y0 < FB_HEIGHT; y0 += CHUNK_H) {
        int16_t h = (y0 + CHUNK_H > FB_HEIGHT) ? (FB_HEIGHT - y0) : CHUNK_H;
        renderStrip(*snap, y0, h, strip, /*big_endian_out=*/false);
        for (int16_t r_idx = 0; r_idx < h; ++r_idx) {
            const uint16_t* src_row = strip + static_cast<size_t>(r_idx) * FB_WIDTH;
            for (int16_t x = 0; x < FB_WIDTH; ++x) {
                uint8_t r = 0, g = 0, b = 0;
                rgb565ToRgb888(src_row[x], r, g, b);
                if (is_bmp) {
                    row_rgb[x * 3 + 0] = b;
                    row_rgb[x * 3 + 1] = g;
                    row_rgb[x * 3 + 2] = r;
                } else {
                    row_rgb[x * 3 + 0] = r;
                    row_rgb[x * 3 + 1] = g;
                    row_rgb[x * 3 + 2] = b;
                }
            }
            if (fwrite(row_rgb, 1, FB_WIDTH * 3, f) != FB_WIDTH * 3) {
                ok = false;
                break;
            }
            total_written += FB_WIDTH * 3;
        }
    }

    free(row_rgb);
    free(strip);
    free(snap);
    fclose(f);

    if (ok) {
        state.fs_sync_requested = true;
        if (out_bytes) *out_bytes = total_written;
    }
    return ok;
}

}  // namespace display
}  // namespace c6os
