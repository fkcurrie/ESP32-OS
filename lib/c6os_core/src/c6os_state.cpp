#include "c6os_types.h"
#include "ssh_auth.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace c6os {

namespace {

HardwareOps g_hw_ops = {};
bool g_has_hw_ops = false;

struct TzMapping {
    const char* name;
    const char* posix;
};

constexpr TzMapping TZ_TABLE[] = {
    {"America/Toronto",     "EST5EDT,M3.2.0,M11.1.0"},
    {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
    {"US/Eastern",          "EST5EDT,M3.2.0,M11.1.0"},
    {"EST5EDT",             "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
    {"US/Central",          "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
    {"US/Mountain",         "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Vancouver",   "PST8PDT,M3.2.0,M11.1.0"},
    {"US/Pacific",          "PST8PDT,M3.2.0,M11.1.0"},
    {"PST8PDT",             "PST8PDT,M3.2.0,M11.1.0"},
    {"UTC",                 "UTC0"},
    {"Etc/UTC",             "UTC0"},
    {"GMT",                 "GMT0"},
    {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Asia/Tokyo",          "JST-9"},
    {"Asia/Singapore",      "SGT-8"},
    {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};

}  // namespace

void setHardwareOps(const HardwareOps* ops) {
    if (ops) {
        g_hw_ops = *ops;
        g_has_hw_ops = true;
    } else {
        memset(&g_hw_ops, 0, sizeof(g_hw_ops));
        g_has_hw_ops = false;
    }
}

const HardwareOps* getHardwareOps() {
    return g_has_hw_ops ? &g_hw_ops : nullptr;
}

bool resolvePosixTz(const char* tz_input, char* out_name, size_t name_cap, char* out_posix, size_t posix_cap) {
    if (!tz_input || tz_input[0] == '\0') return false;
    char clean[48];
    strncpy(clean, tz_input, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t n = strlen(clean);
    while (n > 0 && (clean[n - 1] == '\n' || clean[n - 1] == '\r' || clean[n - 1] == ' ')) {
        clean[--n] = '\0';
    }
    if (n == 0) return false;

    for (const auto& entry : TZ_TABLE) {
        if (strcmp(clean, entry.name) == 0) {
            if (out_name && name_cap > 0) {
                strncpy(out_name, entry.name, name_cap - 1);
                out_name[name_cap - 1] = '\0';
            }
            if (out_posix && posix_cap > 0) {
                strncpy(out_posix, entry.posix, posix_cap - 1);
                out_posix[posix_cap - 1] = '\0';
            }
            setenv("TZ", entry.posix, 1);
            tzset();
            return true;
        }
    }

    // Accept raw POSIX TZ strings (e.g., "EST5EDT,M3.2.0,M11.1.0" or "UTC0")
    bool has_digit = false;
    for (size_t i = 0; i < n; ++i) {
        if (clean[i] >= '0' && clean[i] <= '9') {
            has_digit = true;
            break;
        }
    }
    if (!has_digit) return false;

    if (out_name && name_cap > 0) {
        strncpy(out_name, clean, name_cap - 1);
        out_name[name_cap - 1] = '\0';
    }
    if (out_posix && posix_cap > 0) {
        strncpy(out_posix, clean, posix_cap - 1);
        out_posix[posix_cap - 1] = '\0';
    }
    setenv("TZ", clean, 1);
    tzset();
    return true;
}

void SystemState::initDefaults() {
    memset(this, 0, sizeof(*this));
    strncpy(hostname, "c6-linux", sizeof(hostname) - 1);

    static const uint8_t DEFAULT_SALT[16] = {
        0x43, 0x36, 0x2d, 0x4d, 0x69, 0x63, 0x72, 0x6f,
        0x4c, 0x69, 0x6e, 0x75, 0x78, 0x2d, 0x30, 0x31
    };
    memcpy(pass_salt, DEFAULT_SALT, sizeof(pass_salt));
    crypto::hashPassword("esp32c6", pass_salt, pass_hash);
    pass_configured = true;

    strncpy(api_token, "0123456789abcdef0123456789abcdef", sizeof(api_token) - 1);
    strncpy(login_shell, "/bin/c6-menu", sizeof(login_shell) - 1);

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

    cpu_temp_deci_c = 385;
    cpu_load_pct = 8;
    for (size_t i = 0; i < 32; ++i) {
        cpu_history[i] = static_cast<uint8_t>(6 + (i % 7));
    }
    epoch_offset_sec = 1793518800U;
    resolvePosixTz("America/Toronto", tz_name, sizeof(tz_name), tz_posix, sizeof(tz_posix));
    ntp_synced = false;
    ntp_sync_requested = false;

    wifi_sta_enabled = true;
    wifi_sta_connected = false;
    strncpy(wifi_ip, "0.0.0.0", sizeof(wifi_ip) - 1);
    strncpy(wifi_gateway, "0.0.0.0", sizeof(wifi_gateway) - 1);
    strncpy(wifi_mac, "14:c1:9f:05:7d:10", sizeof(wifi_mac) - 1);
    wifi_rssi = -100;

    wifi_ap_enabled = true;
    wifi_ap_pinned = false;
    strncpy(wifi_ap_ssid, "C6-Linux-OS", sizeof(wifi_ap_ssid) - 1);
    strncpy(wifi_ap_ip, "192.168.4.1", sizeof(wifi_ap_ip) - 1);

    ble_enabled = false;
    ble_advertising = false;
    strncpy(ble_adv_name, "C6-MicroLinux", sizeof(ble_adv_name) - 1);

    i2c_sda = 1;
    i2c_scl = 2;
    i2c_hz = 100000;

    ssh_port = 22;
    ssh_active_sessions = 0;
    ssh_total_logins = 0;
    strncpy(ssh_client_ip, "none", sizeof(ssh_client_ip) - 1);
    strncpy(ssh_hostkey_fp, "SHA256:Generating...", sizeof(ssh_hostkey_fp) - 1);

    http_port = 80;
    http_requests = 0;
    syslog_port = 514;
    syslog_packets = 0;
    button_press_count = 0;

    screen_mode = ScreenMode::OS_DASHBOARD;
    theme = ThemeId::CYBER_CYAN;
    backlight_pct = 100;
    landscape_flipped = false;
    strncpy(headline, "C6-MicroLinux Ready", sizeof(headline) - 1);
    strncpy(subtitle, "SSH Port 22 | CLI Screen Control", sizeof(subtitle) - 1);
    strncpy(banner_footer, "Use 'menu' over SSH or /dev/ttyACM0", sizeof(banner_footer) - 1);
    strncpy(fbi_caption, "C6-MicroLinux RISC-V", sizeof(fbi_caption) - 1);

    gpio_cmd_pin = -1;
    gpio_cmd_action = 0;
    gpio_cmd_val = 0;

    led_mode = LedMode::SSH_ACTIVITY;
    led_r = 0;
    led_g = 200;
    led_b = 255;

    markDirty(DIRTY_FULL);
}

void RenderSnapshot::captureFrom(const SystemState& s, uint8_t dirty_mask) {
    memcpy(hostname, s.hostname, sizeof(hostname));
    cpu_freq_mhz = s.cpu_freq_mhz;
    total_sram_bytes = s.total_sram_bytes;
    free_sram_bytes = s.free_sram_bytes;
    flash_size_mb = s.flash_size_mb;
    rootfs_total_kb = s.rootfs_total_kb;
    rootfs_used_kb = s.rootfs_used_kb;
    sd_mounted = s.sd_mounted;
    uptime_sec = s.uptime_sec;
    cpu_temp_deci_c = s.cpu_temp_deci_c;
    cpu_load_pct = s.cpu_load_pct;
    memcpy(cpu_history, s.cpu_history, sizeof(cpu_history));
    cpu_history_idx = s.cpu_history_idx;
    wifi_sta_connected = s.wifi_sta_connected;
    memcpy(wifi_ssid, s.wifi_ssid, sizeof(wifi_ssid));
    memcpy(wifi_ip, s.wifi_ip, sizeof(wifi_ip));
    wifi_rssi = s.wifi_rssi;
    wifi_ap_enabled = s.wifi_ap_enabled;
    memcpy(wifi_ap_ssid, s.wifi_ap_ssid, sizeof(wifi_ap_ssid));
    memcpy(wifi_ap_ip, s.wifi_ap_ip, sizeof(wifi_ap_ip));
    ssh_active_sessions = s.ssh_active_sessions;
    ssh_total_logins = s.ssh_total_logins;
    memcpy(ssh_hostkey_fp, s.ssh_hostkey_fp, sizeof(ssh_hostkey_fp));
    net_rx_bytes = s.net_rx_bytes;
    net_tx_bytes = s.net_tx_bytes;
    button_press_count = s.button_press_count;
    active_cron_count = static_cast<uint8_t>(s.activeCronCount());
    screen_mode = s.screen_mode;
    theme = s.theme;
    backlight_pct = s.backlight_pct;
    landscape_flipped = s.landscape_flipped;
    memcpy(headline, s.headline, sizeof(headline));
    memcpy(subtitle, s.subtitle, sizeof(subtitle));
    ticker_enabled = s.ticker_enabled;
    memcpy(ticker_text, s.ticker_text, sizeof(ticker_text));
    ticker_offset = s.ticker_offset;
    alert_active = s.alert_active;
    memcpy(alert_text, s.alert_text, sizeof(alert_text));
    fbi_pattern = s.fbi_pattern;
    memcpy(fbi_caption, s.fbi_caption, sizeof(fbi_caption));
    custom_bar_visible = s.custom_bar_visible;
    custom_bar_pct = s.custom_bar_pct;
    memcpy(custom_bar_label, s.custom_bar_label, sizeof(custom_bar_label));
    led_mode = s.led_mode;
    led_r = s.led_r;
    led_g = s.led_g;
    led_b = s.led_b;
    dirty_regions = dirty_mask;
    fb_line_count = s.fb_line_count;
    for (size_t i = 0; i < fb_line_count && i < FB_CONSOLE_ROWS; ++i) {
        memcpy(fb_lines[i], s.fbLineAt(i), FB_CONSOLE_COLS + 1);
    }
}

}  // namespace c6os
