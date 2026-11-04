#ifndef C6OS_TYPES_H
#define C6OS_TYPES_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {

// Lightweight RAII heap buffer to keep large temporary arrays off FreeRTOS task stacks
template <typename T>
class HeapArray {
public:
    explicit HeapArray(size_t count)
        : ptr_(count > 0 ? static_cast<T*>(calloc(count, sizeof(T))) : nullptr),
          count_(count) {}
    ~HeapArray() {
        if (ptr_) free(ptr_);
    }
    HeapArray(const HeapArray&) = delete;
    HeapArray& operator=(const HeapArray&) = delete;

    T* get() const { return ptr_; }
    size_t size() const { return count_; }
    size_t bytes() const { return count_ * sizeof(T); }
    explicit operator bool() const { return ptr_ != nullptr; }
    T& operator[](size_t i) const { return ptr_[i]; }

private:
    T* ptr_;
    size_t count_;
};

// Display Modes for the 1.47" ST7789 (320x172) LCD
enum class ScreenMode : uint8_t {
    OS_DASHBOARD = 0,  // Hostname, SSH IP, CPU/SRAM telemetry, custom banner
    FB_CONSOLE   = 1,  // /dev/tty0 live scrolling kernel & terminal console
    SIGNBOARD    = 2,  // Full-screen scaled custom headline & subtitle signboard
    DIAGNOSTICS  = 3,  // RGB565 color bars, geometry grid, and hardware pin/bus test
    MONITOR      = 4,  // On-LCD htop telemetry: CPU sparkline, SRAM bar, temp, process table
    IMAGE_VIEW   = 5,  // Framebuffer artwork/sprite viewer (fbi)
    COUNT        = 6
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
    SSH_ACTIVITY = 4,  // Green when SSH active, Cyan idle pulse
    COUNT        = 5
};

// Dirty-region bitmask for scanline DMA partial screen updates
constexpr uint8_t DIRTY_NONE    = 0x00;
constexpr uint8_t DIRTY_HEADER  = 0x01;  // y: 0..24
constexpr uint8_t DIRTY_CARDS   = 0x02;  // y: 24..152
constexpr uint8_t DIRTY_TICKER  = 0x04;  // y: 152..172
constexpr uint8_t DIRTY_ALERT   = 0x08;  // y: 44..126
constexpr uint8_t DIRTY_CONSOLE = 0x10;  // y: 14..172
constexpr uint8_t DIRTY_FULL    = 0xFF;  // y: 0..172

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
        case ScreenMode::MONITOR:      return "monitor";
        case ScreenMode::IMAGE_VIEW:   return "image";
        default:                       return "dashboard";
    }
}

inline const char* screenModeLabel(ScreenMode mode) {
    switch (mode) {
        case ScreenMode::OS_DASHBOARD: return "OS Dashboard";
        case ScreenMode::FB_CONSOLE:   return "LCD Console (tty0)";
        case ScreenMode::SIGNBOARD:    return "Custom Signboard";
        case ScreenMode::DIAGNOSTICS:  return "Hardware Test";
        case ScreenMode::MONITOR:      return "Live htop Monitor";
        case ScreenMode::IMAGE_VIEW:   return "Framebuffer Image (fbi)";
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

// Reserved board pins on Waveshare ESP32-C6-LCD-1.47:
// GPIO 4 (SD_CS), 5 (MISO), 6 (MOSI), 7 (SCLK), 8 (WS2812), 9 (BOOT_BTN),
// 12/13 (USB D-/D+), 14 (LCD_CS), 15 (LCD_DC), 21 (LCD_RST), 22 (LCD_BL)
inline bool isReservedBoardPin(int pin) {
    switch (pin) {
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 12:
        case 13:
        case 14:
        case 15:
        case 21:
        case 22:
            return true;
        default:
            return false;
    }
}

// Linear O(1) cursor string writer replacing O(N^2) strlen() scans
class BufWriter {
public:
    BufWriter(char* buf, size_t cap, bool resume_existing = false)
        : buf_(buf), cap_(cap), pos_(0) {
        if (buf_ && cap_ > 0) {
            if (resume_existing) {
                pos_ = strnlen(buf_, cap_ - 1);
            } else {
                buf_[0] = '\0';
            }
        }
    }

    size_t length() const { return pos_; }
    size_t remaining() const { return (buf_ && pos_ + 1 < cap_) ? (cap_ - 1 - pos_) : 0; }
    char* data() const { return buf_; }

    void reset() {
        pos_ = 0;
        if (buf_ && cap_ > 0) buf_[0] = '\0';
    }

    void putChar(char c) {
        if (!buf_ || pos_ + 1 >= cap_) return;
        buf_[pos_++] = c;
        buf_[pos_] = '\0';
    }

    void putRaw(const char* data, size_t len) {
        if (!buf_ || !data || len == 0 || pos_ + 1 >= cap_) return;
        size_t avail = cap_ - 1 - pos_;
        if (len > avail) len = avail;
        memcpy(buf_ + pos_, data, len);
        pos_ += len;
        buf_[pos_] = '\0';
    }

    void putStr(const char* s) {
        if (s) putRaw(s, strlen(s));
    }

    void putWithCrLf(const char* text) {
        if (!buf_ || !text || pos_ + 1 >= cap_) return;
        for (size_t i = 0; text[i] != '\0' && pos_ + 1 < cap_; ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                if (pos_ + 2 >= cap_) break;
                buf_[pos_++] = '\r';
                buf_[pos_++] = '\n';
            } else {
                buf_[pos_++] = text[i];
            }
        }
        buf_[pos_] = '\0';
    }

    void fmt(const char* format, ...) {
        if (!buf_ || !format || pos_ + 1 >= cap_) return;
        va_list args;
        va_start(args, format);
        int w = vsnprintf(buf_ + pos_, cap_ - pos_, format, args);
        va_end(args);
        if (w > 0) {
            size_t added = static_cast<size_t>(w);
            if (pos_ + added >= cap_) {
                pos_ = cap_ - 1;
            } else {
                pos_ += added;
            }
            buf_[pos_] = '\0';
        }
    }

private:
    char* buf_;
    size_t cap_;
    size_t pos_;
};

// Constants for LCD Framebuffer Console (320x172 with 6x8 char cell -> 52 cols x 18 body rows)
constexpr size_t FB_CONSOLE_ROWS = 18;
constexpr size_t FB_CONSOLE_COLS = 52;
constexpr size_t DMESG_MAX_LINES = 32;
constexpr size_t DMESG_LINE_LEN  = 76;

struct WifiScanEntry {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool open;
};

constexpr size_t MAX_WIFI_SCAN_RESULTS = 10;
constexpr size_t MAX_CRON_JOBS = 4;

struct CronJob {
    bool active;
    bool enabled;
    uint32_t interval_sec;
    uint32_t last_run_sec;
    uint32_t run_count;
    char cmd[64];
};

// Hardware driver callback types (wired to ESP32-C6 peripherals on device, deterministic mocks on host)
struct BleScanResult {
    char addr[18];
    char name[24];
    int16_t rssi;
};

struct TaskInfoEntry {
    uint32_t pid;
    char name[20];
    char state_ch;
    int prio;
    uint32_t stack_free_bytes;
    uint32_t cpu_permille;
    uint32_t runtime_cs;
};

struct PingStats {
    bool reachable;
    uint16_t sent;
    uint16_t received;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t rtts_us[5];
};

struct HardwareOps {
    int (*gpio_mode)(int pin, uint8_t mode);  // 1=out, 2=in, 3=pullup, 4=pulldown
    int (*gpio_write)(int pin, int val);
    int (*gpio_read)(int pin);
    int (*gpio_pwm)(int pin, uint8_t duty, uint32_t freq_hz);
    int (*adc_read_mv)(int pin);
    int (*i2c_init)(uint8_t sda, uint8_t scl, uint32_t hz);
    int (*i2c_scan)(uint8_t* out_addrs, size_t max_addrs);
    int (*i2c_write_reg)(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len);
    int (*i2c_read_reg)(uint8_t addr, uint8_t reg, uint8_t* data, size_t len);
    int (*ble_enable)(bool enable);
    int (*ble_scan)(uint32_t duration_ms, BleScanResult* out_results, size_t max_results);
    int (*ble_advertise)(bool enable, const char* name);
    int (*icmp_ping)(const char* host, uint16_t count, uint32_t timeout_ms, PingStats* out_stats);
    size_t (*get_tasks)(TaskInfoEntry* out_tasks, size_t max_tasks, uint32_t* out_total_cpu_permille);
    void (*rotate_api_token)(char out_token[33]);
    uint32_t (*uptime_ms)();
    uint32_t (*free_heap)();
};

void setHardwareOps(const HardwareOps* ops);
const HardwareOps* getHardwareOps();

// Maps IANA timezone name (e.g. "America/Toronto", "UTC") or raw POSIX TZ string to POSIX TZ rule
bool resolvePosixTz(const char* tz_input, char* out_name, size_t name_cap, char* out_posix, size_t posix_cap);

struct SystemState {
    // Kernel & SoC Metadata
    char hostname[32];
    uint8_t pass_salt[16];
    uint8_t pass_hash[32];
    bool pass_configured;
    char api_token[33];
    char login_shell[24];

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

    // Thermal, CPU Load Sparkline, and Wall-Clock NTP State
    int16_t cpu_temp_deci_c;  // e.g., 415 -> 41.5 C
    uint8_t cpu_load_pct;
    uint8_t cpu_history[32];
    uint8_t cpu_history_idx;
    uint32_t epoch_offset_sec;
    char tz_name[32];
    char tz_posix[48];
    bool ntp_synced;
    bool ntp_sync_requested;

    // Network, BLE & SSH Daemon Status
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
    bool wifi_ap_pinned;
    char wifi_ap_ssid[33];
    char wifi_ap_ip[20];

    bool ble_enabled;
    bool ble_advertising;
    char ble_adv_name[24];

    uint8_t i2c_sda;
    uint8_t i2c_scl;
    uint32_t i2c_hz;

    uint16_t ssh_port;
    uint8_t ssh_active_sessions;
    uint32_t ssh_total_logins;
    char ssh_client_ip[24];
    char ssh_hostkey_fp[64];
    uint32_t net_rx_bytes;
    uint32_t net_tx_bytes;

    // HTTP REST API, Syslog Receiver, and Hardware Button Daemons
    uint16_t http_port;
    uint32_t http_requests;
    uint16_t syslog_port;
    uint32_t syslog_packets;
    uint32_t button_press_count;

    // Screen & Peripheral State
    ScreenMode screen_mode;
    ThemeId theme;
    uint8_t backlight_pct;
    bool landscape_flipped;
    char headline[48];
    char subtitle[64];
    char banner_footer[64];

    // Scrolling Ticker / Marquee & Alert Overlay Engine
    bool ticker_enabled;
    char ticker_text[96];
    uint16_t ticker_offset;
    bool alert_active;
    char alert_text[64];

    // Framebuffer Image Viewer (fbi) State
    uint8_t fbi_pattern;
    char fbi_caption[48];

    // Cron Periodic Scheduler Table
    CronJob cron_jobs[MAX_CRON_JOBS];

    // GPIO & PWM Hardware Control State
    uint32_t gpio_out_mask;
    uint32_t gpio_in_mask;
    int8_t gpio_cmd_pin;
    uint8_t gpio_cmd_action;
    uint8_t gpio_cmd_val;

    // Canvas Custom Draw Overlay (optional user-drawn box/bar from CLI)
    bool custom_bar_visible;
    uint8_t custom_bar_pct;
    char custom_bar_label[32];

    // Onboard WS2812 RGB LED State
    LedMode led_mode;
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;

    // Hardware Action & Dirty Flags
    bool ui_dirty;
    uint8_t dirty_regions;
    bool wifi_reconnect_requested;
    bool wifi_scan_requested;
    bool reboot_requested;
    bool fs_sync_requested;

    // Latest Wi-Fi Scan Results
    uint8_t wifi_scan_count;
    WifiScanEntry wifi_scan[MAX_WIFI_SCAN_RESULTS];

    // O(1) Ring-Indexed Framebuffer Console (/dev/tty0 on physical LCD)
    char fb_lines[FB_CONSOLE_ROWS][FB_CONSOLE_COLS + 1];
    uint8_t fb_head;
    uint8_t fb_line_count;

    // O(1) Ring-Indexed Kernel Ring Buffer (dmesg)
    char dmesg[DMESG_MAX_LINES][DMESG_LINE_LEN];
    uint8_t dmesg_head;
    uint8_t dmesg_count;

    void markDirty(uint8_t regions = DIRTY_FULL) {
        ui_dirty = true;
        dirty_regions |= regions;
    }

    const char* fbLineAt(size_t logical_idx) const {
        if (logical_idx >= fb_line_count) return "";
        size_t phys = (fb_head + logical_idx) % FB_CONSOLE_ROWS;
        return fb_lines[phys];
    }

    const char* getFbLine(size_t logical_idx) const { return fbLineAt(logical_idx); }

    const char* dmesgLineAt(size_t logical_idx) const {
        if (logical_idx >= dmesg_count) return "";
        size_t phys = (dmesg_head + logical_idx) % DMESG_MAX_LINES;
        return dmesg[phys];
    }

    const char* getDmesgLine(size_t logical_idx) const { return dmesgLineAt(logical_idx); }

    void initDefaults();

    size_t activeCronCount() const {
        size_t count = 0;
        for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
            if (cron_jobs[i].active) ++count;
        }
        return count;
    }

    void appendFbLine(const char* text) {
        appendFbConsoleLine(text);
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

            size_t slot = 0;
            if (fb_line_count < FB_CONSOLE_ROWS) {
                slot = (fb_head + fb_line_count) % FB_CONSOLE_ROWS;
                ++fb_line_count;
            } else {
                slot = fb_head;
                fb_head = static_cast<uint8_t>((fb_head + 1U) % FB_CONSOLE_ROWS);
            }
            memcpy(fb_lines[slot], line, len + 1);
            if (len == 0 && *text == '\0') break;
        }
        markDirty(DIRTY_CONSOLE);
    }

    void clearFbConsole() {
        memset(fb_lines, 0, sizeof(fb_lines));
        fb_head = 0;
        fb_line_count = 0;
        markDirty(DIRTY_CONSOLE | DIRTY_FULL);
    }

    void appendDmesg(uint32_t ms, const char* msg) {
        if (!msg) return;
        char formatted[DMESG_LINE_LEN];
        uint32_t sec = ms / 1000U;
        uint32_t rem_ms = ms % 1000U;
        int pos = 0;
        formatted[pos++] = '[';
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

        size_t slot = 0;
        if (dmesg_count < DMESG_MAX_LINES) {
            slot = (dmesg_head + dmesg_count) % DMESG_MAX_LINES;
            ++dmesg_count;
        } else {
            slot = dmesg_head;
            dmesg_head = static_cast<uint8_t>((dmesg_head + 1U) % DMESG_MAX_LINES);
        }
        memcpy(dmesg[slot], formatted, static_cast<size_t>(pos) + 1);
        appendFbConsoleLine(formatted);
    }
};

// Compact UI render snapshot so loop() never copies 3.5 KB of dmesg/scan/cron buffers
struct RenderSnapshot {
    char hostname[32];
    uint32_t cpu_freq_mhz;
    uint32_t total_sram_bytes;
    uint32_t free_sram_bytes;
    uint32_t flash_size_mb;
    uint32_t rootfs_total_kb;
    uint32_t rootfs_used_kb;
    bool sd_mounted;
    uint32_t uptime_sec;
    int16_t cpu_temp_deci_c;
    uint8_t cpu_load_pct;
    uint8_t cpu_history[32];
    uint8_t cpu_history_idx;
    bool wifi_sta_connected;
    char wifi_ssid[33];
    char wifi_ip[20];
    int32_t wifi_rssi;
    bool wifi_ap_enabled;
    char wifi_ap_ssid[33];
    char wifi_ap_ip[20];
    uint8_t ssh_active_sessions;
    uint32_t ssh_total_logins;
    char ssh_hostkey_fp[64];
    uint32_t net_rx_bytes;
    uint32_t net_tx_bytes;
    uint32_t button_press_count;
    uint8_t active_cron_count;
    ScreenMode screen_mode;
    ThemeId theme;
    uint8_t backlight_pct;
    bool landscape_flipped;
    char headline[48];
    char subtitle[64];
    bool ticker_enabled;
    char ticker_text[96];
    uint16_t ticker_offset;
    bool alert_active;
    char alert_text[64];
    uint8_t fbi_pattern;
    char fbi_caption[48];
    bool custom_bar_visible;
    uint8_t custom_bar_pct;
    char custom_bar_label[32];
    LedMode led_mode;
    uint8_t led_r;
    uint8_t led_g;
    uint8_t led_b;
    uint8_t dirty_regions;
    size_t fb_line_count;
    char fb_lines[FB_CONSOLE_ROWS][FB_CONSOLE_COLS + 1];

    void captureFrom(const SystemState& s, uint8_t dirty_mask);
};

}  // namespace c6os

#endif  // C6OS_TYPES_H
