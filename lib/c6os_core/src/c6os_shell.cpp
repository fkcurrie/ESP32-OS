#include "c6os_shell.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {

namespace {

void appendFmt(char* buf, size_t max_len, const char* fmt, ...) {
    if (!buf || max_len == 0 || !fmt) return;
    size_t cur = strlen(buf);
    if (cur >= max_len - 1) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + cur, max_len - cur, fmt, args);
    va_end(args);
}

void appendWithCrLf(char* buf, size_t max_len, const char* text) {
    if (!buf || max_len == 0 || !text) return;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            appendFmt(buf, max_len, "\r\n");
        } else {
            appendFmt(buf, max_len, "%c", text[i]);
        }
    }
}

void trimSpaces(char* str) {
    if (!str) return;
    char* start = str;
    while (*start && isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    size_t len = strlen(str);
    while (len > 0 && isspace(static_cast<unsigned char>(str[len - 1]))) {
        str[--len] = '\0';
    }
}

// Tokenize command respecting double and single quotes
size_t tokenizeArgs(char* input, char* argv[], size_t max_args) {
    size_t argc = 0;
    char* p = input;
    while (*p && argc < max_args) {
        while (*p && isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (!*p) break;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) {
                ++p;
            }
            if (*p == quote) {
                *p++ = '\0';
            }
        } else {
            argv[argc++] = p;
            while (*p && !isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (*p) {
                *p++ = '\0';
            }
        }
    }
    return argc;
}

}  // namespace

ShellSession::ShellSession()
    : mode_(SessionMode::MENU_MAIN),
      prev_mode_(SessionMode::SHELL_CLI),
      sort_mode_(MonitorSort::CPU),
      tree_view_(false),
      line_len_(0) {
    strncpy(cwd_, "/root", sizeof(cwd_) - 1);
    cwd_[sizeof(cwd_) - 1] = '\0';
    memset(line_buf_, 0, sizeof(line_buf_));
    memset(pending_ssid_, 0, sizeof(pending_ssid_));
    strncpy(menu_notice_, "Ready. Press [1-9] for screen, [t] for htop, [s] for shell.", sizeof(menu_notice_) - 1);
}

void ShellSession::begin(SystemState& state, VirtualFs& vfs, bool start_in_menu,
                         char* out_buf, size_t max_len) {
    (void)vfs;
    if (out_buf && max_len > 0) out_buf[0] = '\0';
    line_len_ = 0;
    if (start_in_menu) {
        mode_ = SessionMode::MENU_MAIN;
        renderMenu(state, out_buf, max_len);
    } else {
        mode_ = SessionMode::SHELL_CLI;
        char motd[384];
        if (vfs.readFile(state, "/etc/motd", motd, sizeof(motd))) {
            appendFmt(out_buf, max_len, "%s\r\n", motd);
        }
        renderPrompt(state, out_buf, max_len);
    }
}

void ShellSession::renderPrompt(const SystemState& state, char* out_buf, size_t max_len) const {
    const char* display_cwd = (strcmp(cwd_, "/root") == 0) ? "~" : cwd_;
    appendFmt(out_buf, max_len, "\033[1;32mroot@%s\033[0m:\033[1;34m%s\033[0m# ",
              state.hostname, display_cwd);
}

void ShellSession::renderMenu(const SystemState& state, char* out_buf, size_t max_len) const {
    char bar[12];
    int filled = (state.backlight_pct + 5) / 10;
    if (filled > 10) filled = 10;
    for (int i = 0; i < 10; ++i) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    bar[10] = '\0';

    const char* ip_str = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
    const char* net_tag = state.wifi_sta_connected ? state.wifi_ssid : "AP:C6-Linux-OS";

    appendFmt(out_buf, max_len,
              "\033[2J\033[H"
              "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;37mC6-MicroLinux (c6os 6.12-riscv32) — Interactive Screen Controller\033[0m \033[1;36m|\033[0m\r\n"
              "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
              "\033[1;36m|\033[0m  Host: \033[1;32mroot@%-14s\033[0m  Net: \033[1;33m%-15s\033[0m (\033[1;37m%-14s\033[0m) \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  SRAM: \033[1;37m%3u KB free\033[0m / %3u KB   Uptime: \033[1;37m%6u s\033[0m      SSH Sessions: \033[1;32m%u\033[0m \033[1;36m|\033[0m\r\n"
              "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
              "\033[1;36m|\033[0m  LCD Mode : \033[1;36m%-18s\033[0m  Theme    : \033[1;35m%-10s\033[0m Flip: \033[1;37m%-3s\033[0m    \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  Backlight: [\033[1;33m%s\033[0m] \033[1;37m%3u%%\033[0m   RGB LED  : \033[1;32m%-7s\033[0m (%3u,%3u,%3u)   \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  Headline : \033[1;37m\"%-48.48s\"\033[0m     \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  Subtitle : \033[0;37m\"%-48.48s\"\033[0m     \033[1;36m|\033[0m\r\n"
              "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[1]\033[0m Cycle LCD Mode   (Dashboard -> Console -> Signboard -> Test)  \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[2]\033[0m Set Screen Text  (Update Headline & Subtitle on LCD)          \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[3]\033[0m Cycle Color Theme (Cyber Cyan / Matrix / Amber / Red / Mono)  \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[4]\033[0m Set Backlight %%  (Or press \033[1;32m+\033[0m / \033[1;32m-\033[0m to step brightness +/-10%%)   \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[5]\033[0m Toggle 180 deg Landscape Flip                                 \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[6]\033[0m Control Onboard WS2812 RGB LED (Mode & Color)                 \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[7]\033[0m Live LCD Terminal Mirror (Type lines directly onto /dev/tty0) \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[8]\033[0m Configure Wi-Fi Network (Scan & Join 2.4 GHz SSID)            \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[9]\033[0m Kernel & Hardware Diagnostics (/proc/cpuinfo, meminfo, dmesg) \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;33m[t]\033[0m Launch \033[1;32mhtop\033[0m Live Task & Resource Monitor                      \033[1;36m|\033[0m\r\n"
              "\033[1;36m|\033[0m  \033[1;32m[s]\033[0m Drop to Linux Shell (c6-sh)            \033[1;31m[q]\033[0m Disconnect Session \033[1;36m|\033[0m\r\n"
              "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
              "  \033[1;32mStatus:\033[0m %s\r\n"
              "  \033[1;36mSelect option [1-9, +/-, t, s, q]:\033[0m ",
              state.hostname, net_tag, ip_str,
              static_cast<unsigned>(state.free_sram_bytes / 1024U),
              static_cast<unsigned>(state.total_sram_bytes / 1024U),
              static_cast<unsigned>(state.uptime_sec),
              static_cast<unsigned>(state.ssh_active_sessions),
              screenModeLabel(state.screen_mode), themeName(state.theme),
              state.landscape_flipped ? "YES" : "NO",
              bar, static_cast<unsigned>(state.backlight_pct),
              ledModeName(state.led_mode),
              static_cast<unsigned>(state.led_r),
              static_cast<unsigned>(state.led_g),
              static_cast<unsigned>(state.led_b),
              state.headline, state.subtitle,
              menu_notice_);
}

namespace {

struct TaskEntry {
    unsigned pid;
    const char* user;
    int pri;
    int ni;
    unsigned virt_kb;
    unsigned res_kb;
    unsigned shr_kb;
    char state_ch;
    unsigned cpu_tenths;
    unsigned time_cs;
    const char* cmd;
    const char* tree_cmd;
};

void buildBar(char* out, size_t width, unsigned pct, char fill_ch = '|') {
    if (pct > 100) pct = 100;
    size_t filled = (pct * width + 50) / 100;
    if (filled > width) filled = width;
    for (size_t i = 0; i < width; ++i) {
        out[i] = (i < filled) ? fill_ch : ' ';
    }
    out[width] = '\0';
}

}  // namespace

void ShellSession::renderTop(const SystemState& state, bool is_htop, bool clear_screen,
                             char* out_buf, size_t max_len) const {
    unsigned up = static_cast<unsigned>(state.uptime_sec);
    unsigned days = up / 86400U;
    unsigned hrs  = (up % 86400U) / 3600U;
    unsigned mins = (up % 3600U) / 60U;
    unsigned secs = up % 60U;

    unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
    if (total_kb == 0) total_kb = 320;
    unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
    unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 64U;
    unsigned cache_kb = 16U;
    unsigned mem_pct  = (used_kb * 100U) / total_kb;

    unsigned wifi_cpu = state.wifi_sta_connected ? (22U + (up % 5U)) : 8U;
    unsigned ssh_cpu  = (state.ssh_active_sessions > 0) ? (44U + ((up * 3U) % 9U)) : 4U;
    unsigned lcd_cpu  = 32U + ((up * 2U) % 7U);
    unsigned mon_cpu  = 12U + (up % 4U);

    TaskEntry tasks[9] = {
        {  1, "root", 20,  0,   4,   1,  1, 'S',  2, up * 2U + 4U,   "/sbin/init",                           "/sbin/init" },
        {  2, "root", 20,  0,   0,   0,  0, 'S',  1, up * 1U + 2U,   "[kthreadd]",                           "├─ [kthreadd]" },
        {  4, "root", -2,  0,   0,   0,  0, 'S',  5, up * 6U + 11U,  "[esp_timer]",                          "│  └─ [esp_timer]" },
        {  8, "root", 15, -5,  18,  12,  4, 'S', lcd_cpu, up * 34U + 19U, "/sbin/lcdd --driver=st7789 --spi=80M", "├─ /sbin/lcdd --driver=st7789" },
        { 12, "root", 20,  0,  32,  24,  8, 'S', wifi_cpu, up * 24U + 8U, "/sbin/wpa_supplicant -i wlan0",        "├─ /sbin/wpa_supplicant -i wlan0" },
        { 15, "root", 20,  0,   4,   2,  1, 'S',  6, up * 7U + 3U,   "/sbin/ws2812d --gpio=8",               "├─ /sbin/ws2812d --gpio=8" },
        { 18, "root", 20,  0,  40,  28, 10, 'S', ssh_cpu, up * 41U + 27U, "/usr/sbin/sshd -D -p 22",              "├─ /usr/sbin/sshd -D -p 22" },
        { 24, "root", 20,  0,   8,   4,  2, 'S',  3, up * 4U + 5U,   "/bin/c6-sh",                           "│  └─ /bin/c6-sh" },
        { 38, "root", 20,  0,   6,   3,  2, 'R', mon_cpu, 15U + (up % 90U), is_htop ? "/bin/htop" : "/bin/top", is_htop ? "│     └─ /bin/htop" : "│     └─ /bin/top" }
    };

    // Sort tasks if not in tree view
    if (!tree_view_ || !is_htop) {
        for (size_t i = 0; i < 9; ++i) {
            for (size_t j = i + 1; j < 9; ++j) {
                bool swap_needed = false;
                if (sort_mode_ == MonitorSort::MEM) {
                    swap_needed = (tasks[j].res_kb > tasks[i].res_kb);
                } else if (sort_mode_ == MonitorSort::TIME) {
                    swap_needed = (tasks[j].time_cs > tasks[i].time_cs);
                } else {
                    swap_needed = (tasks[j].cpu_tenths > tasks[i].cpu_tenths);
                }
                if (swap_needed) {
                    TaskEntry tmp = tasks[i];
                    tasks[i] = tasks[j];
                    tasks[j] = tmp;
                }
            }
        }
    }

    unsigned total_cpu_tenths = 0;
    for (size_t i = 0; i < 9; ++i) {
        total_cpu_tenths += tasks[i].cpu_tenths;
    }
    unsigned us_tenths = (total_cpu_tenths * 55U) / 100U;
    unsigned sy_tenths = total_cpu_tenths - us_tenths;
    unsigned id_tenths = (1000U > total_cpu_tenths + 12U) ? (1000U - total_cpu_tenths - 12U) : 0U;

    appendFmt(out_buf, max_len, "%s", clear_screen ? "\033[2J\033[H" : "\033[H");

    if (!is_htop) {
        appendFmt(out_buf, max_len,
                  "top - %02u:%02u:%02u up %u day%s, %02u:%02u,  %u user,  load average: 0.%02u, 0.09, 0.04\033[K\r\n"
                  "Tasks: \033[1m  9\033[0m total, \033[1m  1\033[0m running, \033[1m  8\033[0m sleeping,   0 stopped,   0 zombie\033[K\r\n"
                  "%%Cpu(s): \033[1m%2u.%u\033[0m us, \033[1m%2u.%u\033[0m sy,  0.0 ni, \033[1m%2u.%u\033[0m id,  0.0 wa,  0.4 hi,  0.8 si\033[K\r\n"
                  "KiB Mem : \033[1m%6u.0\033[0m total, \033[1m%6u.0\033[0m free, \033[1m%6u.0\033[0m used, \033[1m%6u.0\033[0m buff/cache\033[K\r\n"
                  "KiB Swap:      0.0 total,      0.0 free,      0.0 used. \033[1m%6u.0\033[0m avail Mem\033[K\r\n"
                  "\033[K\r\n"
                  "\033[7m  PID USER      PR  NI    VIRT    RES    SHR S  %%CPU  %%MEM     TIME+ COMMAND                  \033[0m\033[K\r\n",
                  hrs, mins, secs, days, (days == 1 ? "" : "s"), hrs, mins,
                  static_cast<unsigned>(state.ssh_active_sessions > 0 ? state.ssh_active_sessions : 1),
                  10U + (total_cpu_tenths / 15U),
                  us_tenths / 10U, us_tenths % 10U,
                  sy_tenths / 10U, sy_tenths % 10U,
                  id_tenths / 10U, id_tenths % 10U,
                  total_kb, free_kb, used_kb, cache_kb, free_kb + cache_kb);

        for (size_t i = 0; i < 9; ++i) {
            const TaskEntry& t = tasks[i];
            unsigned mem_t = (t.res_kb * 1000U) / total_kb;
            unsigned t_min = t.time_cs / 6000U;
            unsigned t_sec = (t.time_cs / 100U) % 60U;
            unsigned t_hun = t.time_cs % 100U;
            const char* row_style = (t.state_ch == 'R') ? "\033[1m" : "";
            const char* row_end   = (t.state_ch == 'R') ? "\033[0m" : "";
            appendFmt(out_buf, max_len,
                      "%s%5u %-8s %3d %3d %6uK %5uK %5uK %c %3u.%u %3u.%u %3u:%02u.%02u %-24s%s\033[K\r\n",
                      row_style, t.pid, t.user, t.pri, t.ni,
                      t.virt_kb, t.res_kb, t.shr_kb, t.state_ch,
                      t.cpu_tenths / 10U, t.cpu_tenths % 10U,
                      mem_t / 10U, mem_t % 10U,
                      t_min, t_sec, t_hun, t.cmd, row_end);
        }

        const char* sort_label = (sort_mode_ == MonitorSort::MEM) ? "%MEM" :
                                 (sort_mode_ == MonitorSort::TIME ? "TIME+" : "%CPU");
        appendFmt(out_buf, max_len,
                  "\033[K\r\n"
                  "\033[7m [q] Quit  [P] Sort:%%CPU  [M] Sort:%%MEM  [T] Sort:TIME+  [h] Switch to htop  (Sort: %-5s) \033[0m\033[K",
                  sort_label);
    } else {
        char cpu_bar[21], mem_bar[21], fsh_bar[21];
        buildBar(cpu_bar, 20, total_cpu_tenths / 10U + 4U, '|');
        buildBar(mem_bar, 20, mem_pct, '|');
        unsigned fsh_pct = (state.rootfs_total_kb > 0)
                               ? ((state.rootfs_used_kb * 100U) / state.rootfs_total_kb)
                               : 1U;
        if (fsh_pct < 5U) fsh_pct = 5U;
        buildBar(fsh_bar, 20, fsh_pct, '|');

        appendFmt(out_buf, max_len,
                  "  \033[1;36m0\033[0m  [\033[1;32m%s\033[0m \033[1;37m%2u.%u%%\033[0m]   \033[1;36mTasks:\033[0m \033[1;37m9\033[0m, 2 kthr; \033[1;32m1 running\033[0m\033[K\r\n"
                  "  \033[1;36mMem\033[0m[\033[1;33m%s\033[0m \033[1;37m%3u/%3uK\033[0m] \033[1;36mLoad average:\033[0m \033[1;37m0.%02u 0.09 0.04\033[0m\033[K\r\n"
                  "  \033[1;36mFsh\033[0m[\033[1;35m%s\033[0m \033[1;37m%3u/%4uK\033[0m] \033[1;36mUptime:\033[0m \033[1;37m%02u:%02u:%02u\033[0m (rv32imac @ %uMHz)\033[K\r\n"
                  "  \033[1;36mNet\033[0m[\033[1;32m%-13s %4ddBm\033[0m \033[1;37mwlan0\033[0m]   \033[1;36mLCD:\033[0m \033[1;37m%s\033[0m (%s, BL:%u%%)\033[K\r\n"
                  "\033[K\r\n"
                  "\033[30;42m  PID USER      PRI  NI  VIRT   RES   SHR S  CPU%%  MEM%%   TIME+  Command                        \033[0m\033[K\r\n",
                  cpu_bar, total_cpu_tenths / 10U, total_cpu_tenths % 10U,
                  mem_bar, used_kb, total_kb,
                  10U + (total_cpu_tenths / 15U),
                  fsh_bar, static_cast<unsigned>(state.rootfs_used_kb), static_cast<unsigned>(state.rootfs_total_kb),
                  hrs, mins, secs, static_cast<unsigned>(state.cpu_freq_mhz),
                  state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip,
                  static_cast<int>(state.wifi_rssi),
                  screenModeName(state.screen_mode), themeName(state.theme),
                  static_cast<unsigned>(state.backlight_pct));

        for (size_t i = 0; i < 9; ++i) {
            const TaskEntry& t = tasks[i];
            unsigned mem_t = (t.res_kb * 1000U) / total_kb;
            unsigned t_min = t.time_cs / 6000U;
            unsigned t_sec = (t.time_cs / 100U) % 60U;
            unsigned t_hun = t.time_cs % 100U;
            const char* cmd_str = tree_view_ ? t.tree_cmd : t.cmd;
            const char* state_col = (t.state_ch == 'R') ? "\033[1;32mR\033[0m" : "S";
            appendFmt(out_buf, max_len,
                      "%5u \033[1;32m%-8s\033[0m %4d %3d %4uK %4uK %4uK %s %3u.%u %3u.%u %2u:%02u.%02u \033[1;37m%-30s\033[0m\033[K\r\n",
                      t.pid, t.user, t.pri, t.ni,
                      t.virt_kb, t.res_kb, t.shr_kb, state_col,
                      t.cpu_tenths / 10U, t.cpu_tenths % 10U,
                      mem_t / 10U, mem_t % 10U,
                      t_min, t_sec, t_hun, cmd_str);
        }

        appendFmt(out_buf, max_len,
                  "\033[K\r\n"
                  "\033[1;37mF1\033[30;46mTop(h)\033[0m \033[1;37mF5\033[30;46mTree(t)\033[0m \033[1;37mF6\033[30;46mSortCPU(P)\033[0m \033[1;37mF7\033[30;46mSortMEM(M)\033[0m \033[1;37mF9\033[30;46mMenu(m)\033[0m \033[1;37mF10\033[30;46mQuit(q)\033[0m\033[K");
    }
}

void ShellSession::renderMonitorTick(const SystemState& state, char* out_buf, size_t max_len) const {
    if (out_buf && max_len > 0) out_buf[0] = '\0';
    if (mode_ == SessionMode::TOP_VIEW) {
        renderTop(state, /*is_htop=*/false, /*clear_screen=*/false, out_buf, max_len);
    } else if (mode_ == SessionMode::HTOP_VIEW) {
        renderTop(state, /*is_htop=*/true, /*clear_screen=*/false, out_buf, max_len);
    }
}

bool ShellSession::handleMonitorKey(SystemState& state, VirtualFs& vfs, char ch,
                                    char* out_buf, size_t max_len) {
    (void)vfs;
    if (ch == 'q' || ch == 'Q' || ch == 0x03) {
        mode_ = prev_mode_;
        if (mode_ == SessionMode::MENU_MAIN) {
            snprintf(menu_notice_, sizeof(menu_notice_), "Exited live monitor.");
            renderMenu(state, out_buf, max_len);
        } else {
            mode_ = SessionMode::SHELL_CLI;
            appendFmt(out_buf, max_len, "\r\n");
            renderPrompt(state, out_buf, max_len);
        }
        return true;
    }
    if (ch == 'p' || ch == 'P') {
        sort_mode_ = MonitorSort::CPU;
        tree_view_ = false;
    } else if (ch == 'M') {
        sort_mode_ = MonitorSort::MEM;
        tree_view_ = false;
    } else if (ch == 'm') {
        if (mode_ == SessionMode::HTOP_VIEW) {
            mode_ = SessionMode::MENU_MAIN;
            snprintf(menu_notice_, sizeof(menu_notice_), "Returned from htop to Screen Control Menu.");
            renderMenu(state, out_buf, max_len);
            return true;
        }
        sort_mode_ = MonitorSort::MEM;
    } else if (ch == 'T') {
        sort_mode_ = MonitorSort::TIME;
        tree_view_ = false;
    } else if (ch == 't') {
        if (mode_ == SessionMode::HTOP_VIEW) {
            tree_view_ = !tree_view_;
        } else {
            sort_mode_ = MonitorSort::TIME;
        }
    } else if (ch == 'h' || ch == 'H') {
        mode_ = (mode_ == SessionMode::HTOP_VIEW) ? SessionMode::TOP_VIEW : SessionMode::HTOP_VIEW;
        renderTop(state, mode_ == SessionMode::HTOP_VIEW, /*clear_screen=*/true, out_buf, max_len);
        return true;
    }
    renderTop(state, mode_ == SessionMode::HTOP_VIEW, /*clear_screen=*/false, out_buf, max_len);
    return true;
}

bool ShellSession::handleMenuKey(SystemState& state, VirtualFs& vfs, char ch,
                                 char* out_buf, size_t max_len) {
    if (ch == '\r' || ch == '\n' || ch == 0x03) {
        renderMenu(state, out_buf, max_len);
        return true;
    }

    switch (ch) {
        case '1': {
            uint8_t next = (static_cast<uint8_t>(state.screen_mode) + 1) %
                           static_cast<uint8_t>(ScreenMode::COUNT);
            state.screen_mode = static_cast<ScreenMode>(next);
            state.ui_dirty = true;
            snprintf(menu_notice_, sizeof(menu_notice_), "Switched LCD mode to: %s",
                     screenModeLabel(state.screen_mode));
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case '2': {
            mode_ = SessionMode::MENU_PROMPT_HEAD;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;33mEnter new Screen Headline\033[0m (current: \"%s\"): ",
                      state.headline);
            return true;
        }
        case '3': {
            uint8_t next = (static_cast<uint8_t>(state.theme) + 1) %
                           static_cast<uint8_t>(ThemeId::COUNT);
            state.theme = static_cast<ThemeId>(next);
            state.ui_dirty = true;
            snprintf(menu_notice_, sizeof(menu_notice_), "Changed LCD theme to: %s",
                     themeName(state.theme));
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case '4': {
            mode_ = SessionMode::MENU_PROMPT_BL;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;33mEnter Backlight Brightness [0-100]\033[0m (current: %u%%): ",
                      state.backlight_pct);
            return true;
        }
        case '+':
        case '=': {
            int bl = static_cast<int>(state.backlight_pct) + 10;
            if (bl > 100) bl = 100;
            state.backlight_pct = static_cast<uint8_t>(bl);
            state.ui_dirty = true;
            snprintf(menu_notice_, sizeof(menu_notice_), "Backlight increased to %u%%",
                     state.backlight_pct);
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case '-':
        case '_': {
            int bl = static_cast<int>(state.backlight_pct) - 10;
            if (bl < 0) bl = 0;
            state.backlight_pct = static_cast<uint8_t>(bl);
            state.ui_dirty = true;
            snprintf(menu_notice_, sizeof(menu_notice_), "Backlight decreased to %u%%",
                     state.backlight_pct);
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case '5': {
            state.landscape_flipped = !state.landscape_flipped;
            state.ui_dirty = true;
            snprintf(menu_notice_, sizeof(menu_notice_), "180 deg Landscape Flip: %s",
                     state.landscape_flipped ? "ENABLED" : "DISABLED");
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case '6': {
            mode_ = SessionMode::MENU_PROMPT_RGB;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;33mEnter RGB LED command\033[0m (off | rainbow | pulse | ssh | R G B): ");
            return true;
        }
        case '7': {
            state.screen_mode = ScreenMode::FB_CONSOLE;
            state.ui_dirty = true;
            mode_ = SessionMode::MENU_TTY0_MIRROR;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;36m=== Live LCD Terminal Mirror (/dev/tty0) ===\033[0m\r\n"
                      "Every line you type is rendered directly on the 1.47\" LCD screen.\r\n"
                      "Type \033[1;33m.clear\033[0m to clear screen or \033[1;33m.exit\033[0m to return to menu.\r\n"
                      "\033[1;32mtty0>\033[0m ");
            return true;
        }
        case '8': {
            state.wifi_scan_requested = true;
            mode_ = SessionMode::MENU_WIFI_SSID;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;36m=== Wi-Fi Configuration ===\033[0m\r\n"
                      "Current STA: %s (SSID: %s, IP: %s, RSSI: %d dBm)\r\n"
                      "Fallback AP: %s (SSID: %s, IP: %s)\r\n",
                      state.wifi_sta_connected ? "CONNECTED" : "DISCONNECTED",
                      state.wifi_ssid[0] ? state.wifi_ssid : "<none>",
                      state.wifi_ip, static_cast<int>(state.wifi_rssi),
                      state.wifi_ap_enabled ? "ACTIVE" : "OFF",
                      state.wifi_ap_ssid, state.wifi_ap_ip);
            if (state.wifi_scan_count > 0) {
                appendFmt(out_buf, max_len, "Recent 2.4 GHz Networks:\r\n");
                for (size_t i = 0; i < state.wifi_scan_count; ++i) {
                    appendFmt(out_buf, max_len, "  - %-24s (%4d dBm, ch %u)\r\n",
                              state.wifi_scan[i].ssid,
                              static_cast<int>(state.wifi_scan[i].rssi),
                              state.wifi_scan[i].channel);
                }
            }
            appendFmt(out_buf, max_len,
                      "\033[1;33mEnter Wi-Fi SSID to join\033[0m (or press Enter to cancel): ");
            return true;
        }
        case '9': {
            char buf[512];
            appendFmt(out_buf, max_len, "\r\n\033[1;36m=== /proc/version ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/version", buf, sizeof(buf))) {
                appendWithCrLf(out_buf, max_len, buf);
            }
            appendFmt(out_buf, max_len, "\033[1;36m=== /proc/cpuinfo ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/cpuinfo", buf, sizeof(buf))) {
                appendWithCrLf(out_buf, max_len, buf);
            }
            appendFmt(out_buf, max_len, "\033[1;36m=== /proc/meminfo ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/meminfo", buf, sizeof(buf))) {
                appendWithCrLf(out_buf, max_len, buf);
            }
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;33mPress Enter to return to menu...\033[0m");
            return true;
        }
        case '\r':
        case '\n':
        case ' ': {
            snprintf(menu_notice_, sizeof(menu_notice_), "Refreshed menu.");
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case 't':
        case 'T': {
            prev_mode_ = SessionMode::MENU_MAIN;
            mode_ = SessionMode::HTOP_VIEW;
            renderTop(state, true, true, out_buf, max_len);
            return true;
        }
        case 's':
        case 'S': {
            mode_ = SessionMode::SHELL_CLI;
            line_len_ = 0;
            appendFmt(out_buf, max_len,
                      "\r\n\033[1;32mEntering C6-MicroLinux POSIX Shell (c6-sh).\033[0m Type \033[1;33mmenu\033[0m to return to TUI.\r\n");
            renderPrompt(state, out_buf, max_len);
            return true;
        }
        case 'q':
        case 'Q': {
            appendFmt(out_buf, max_len, "\r\nClosing session. Goodbye!\r\n");
            return false;
        }
        default:
            snprintf(menu_notice_, sizeof(menu_notice_),
                     "Unknown key '%c'. Choose [1-9], [+/-], [t] htop, [s] shell, or [q] quit.",
                     isprint(static_cast<unsigned char>(ch)) ? ch : '?');
            renderMenu(state, out_buf, max_len);
            return true;
    }
}

bool ShellSession::consumeBytes(SystemState& state, VirtualFs& vfs,
                                const char* data, size_t len,
                                char* out_buf, size_t max_len) {
    if (out_buf && max_len > 0) out_buf[0] = '\0';
    if (!data || len == 0) return true;

    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];

        // Ignore bare LF if preceded by CR
        if (ch == '\n' && i > 0 && data[i - 1] == '\r') {
            continue;
        }

        // In live top / htop view, single keystrokes control sorting/refresh/exit
        if (isInteractiveMonitor()) {
            // Skip ANSI escape sequences (e.g. arrow keys or F10 \033[21~)
            if (ch == '\033') {
                // F10 sequence \033[21~ quits htop
                if (i + 4 < len && data[i + 1] == '[' && data[i + 2] == '2' &&
                    data[i + 3] == '1' && data[i + 4] == '~') {
                    i += 4;
                    handleMonitorKey(state, vfs, 'q', out_buf, max_len);
                    continue;
                }
                while (i + 1 < len && !isalpha(static_cast<unsigned char>(data[i + 1])) &&
                       data[i + 1] != '~') {
                    ++i;
                }
                if (i + 1 < len) ++i;
                continue;
            }
            if (!handleMonitorKey(state, vfs, ch, out_buf, max_len)) {
                return false;
            }
            continue;
        }

        // In MENU_MAIN, single keystrokes trigger immediate actions
        if (mode_ == SessionMode::MENU_MAIN) {
            // Ignore ANSI escape sequences (like arrow keys \033[A) in main menu
            if (ch == '\033') {
                if (i + 2 < len && data[i + 1] == '[') {
                    i += 2;
                }
                continue;
            }
            if (!handleMenuKey(state, vfs, ch, out_buf, max_len)) {
                return false;
            }
            continue;
        }

        // Ctrl+C cancels current input or returns to MENU_MAIN if in a sub-prompt
        if (ch == 0x03) {
            line_len_ = 0;
            line_buf_[0] = '\0';
            appendFmt(out_buf, max_len, "^C\r\n");
            if (mode_ != SessionMode::SHELL_CLI) {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Cancelled prompt.");
                renderMenu(state, out_buf, max_len);
            } else {
                renderPrompt(state, out_buf, max_len);
            }
            continue;
        }

        // Ctrl+D in empty shell line exits or returns to menu
        if (ch == 0x04 && line_len_ == 0) {
            appendFmt(out_buf, max_len, "exit\r\n");
            return false;
        }

        // Backspace / DEL
        if (ch == 0x7F || ch == 0x08) {
            if (line_len_ > 0) {
                --line_len_;
                line_buf_[line_len_] = '\0';
                appendFmt(out_buf, max_len, "\b \b");
            }
            continue;
        }

        // Enter (CR or LF)
        if (ch == '\r' || ch == '\n') {
            appendFmt(out_buf, max_len, "\r\n");
            line_buf_[line_len_] = '\0';
            char completed[192];
            strncpy(completed, line_buf_, sizeof(completed) - 1);
            completed[sizeof(completed) - 1] = '\0';
            line_len_ = 0;
            line_buf_[0] = '\0';

            if (!handleLineComplete(state, vfs, completed, out_buf, max_len)) {
                return false;
            }
            continue;
        }

        // Printable character
        if (isprint(static_cast<unsigned char>(ch)) && line_len_ + 1 < sizeof(line_buf_)) {
            line_buf_[line_len_++] = ch;
            line_buf_[line_len_] = '\0';
            // Mask password input if in Wi-Fi password prompt
            if (mode_ == SessionMode::MENU_WIFI_PASS) {
                appendFmt(out_buf, max_len, "*");
            } else {
                appendFmt(out_buf, max_len, "%c", ch);
            }
        }
    }
    return true;
}

bool ShellSession::handleLineComplete(SystemState& state, VirtualFs& vfs,
                                      const char* line,
                                      char* out_buf, size_t max_len) {
    switch (mode_) {
        case SessionMode::MENU_PROMPT_HEAD: {
            if (line[0] != '\0') {
                strncpy(state.headline, line, sizeof(state.headline) - 1);
                state.headline[sizeof(state.headline) - 1] = '\0';
                state.ui_dirty = true;
            }
            mode_ = SessionMode::MENU_PROMPT_SUB;
            appendFmt(out_buf, max_len,
                      "\033[1;33mEnter new Screen Subtitle\033[0m (current: \"%s\", Enter to keep): ",
                      state.subtitle);
            return true;
        }
        case SessionMode::MENU_PROMPT_SUB: {
            if (line[0] != '\0') {
                strncpy(state.subtitle, line, sizeof(state.subtitle) - 1);
                state.subtitle[sizeof(state.subtitle) - 1] = '\0';
            }
            state.ui_dirty = true;
            mode_ = SessionMode::MENU_MAIN;
            snprintf(menu_notice_, sizeof(menu_notice_), "Updated LCD text: \"%s\"", state.headline);
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case SessionMode::MENU_PROMPT_BL: {
            if (line[0] != '\0') {
                int val = atoi(line);
                if (val < 0) val = 0;
                if (val > 100) val = 100;
                state.backlight_pct = static_cast<uint8_t>(val);
                state.ui_dirty = true;
                snprintf(menu_notice_, sizeof(menu_notice_), "Backlight set to %u%%", state.backlight_pct);
            }
            mode_ = SessionMode::MENU_MAIN;
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case SessionMode::MENU_PROMPT_RGB: {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "screen led %s", line);
            char tmp[256] = {0};
            executeSingleCommand(state, vfs, cmd, tmp, sizeof(tmp));
            snprintf(menu_notice_, sizeof(menu_notice_), "RGB LED updated (%s)", ledModeName(state.led_mode));
            mode_ = SessionMode::MENU_MAIN;
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case SessionMode::MENU_TTY0_MIRROR: {
            if (strcmp(line, ".exit") == 0 || strcmp(line, "exit") == 0) {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Exited live LCD tty0 mirror.");
                renderMenu(state, out_buf, max_len);
                return true;
            }
            if (strcmp(line, ".clear") == 0) {
                state.clearFbConsole();
                appendFmt(out_buf, max_len, "[LCD cleared]\r\n\033[1;32mtty0>\033[0m ");
                return true;
            }
            state.appendFbConsoleLine(line);
            appendFmt(out_buf, max_len, "\033[1;32mtty0>\033[0m ");
            return true;
        }
        case SessionMode::MENU_WIFI_SSID: {
            if (line[0] == '\0') {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Wi-Fi setup cancelled.");
                renderMenu(state, out_buf, max_len);
                return true;
            }
            strncpy(pending_ssid_, line, sizeof(pending_ssid_) - 1);
            pending_ssid_[sizeof(pending_ssid_) - 1] = '\0';
            mode_ = SessionMode::MENU_WIFI_PASS;
            appendFmt(out_buf, max_len,
                      "\033[1;33mEnter Password for \"%s\"\033[0m (Enter for open network): ",
                      pending_ssid_);
            return true;
        }
        case SessionMode::MENU_WIFI_PASS: {
            strncpy(state.wifi_cfg_ssid, pending_ssid_, sizeof(state.wifi_cfg_ssid) - 1);
            state.wifi_cfg_ssid[sizeof(state.wifi_cfg_ssid) - 1] = '\0';
            strncpy(state.wifi_cfg_pass, line, sizeof(state.wifi_cfg_pass) - 1);
            state.wifi_cfg_pass[sizeof(state.wifi_cfg_pass) - 1] = '\0';
            state.wifi_sta_enabled = true;
            state.wifi_reconnect_requested = true;
            state.fs_sync_requested = true;
            mode_ = SessionMode::MENU_MAIN;
            snprintf(menu_notice_, sizeof(menu_notice_),
                     "Connecting to Wi-Fi SSID \"%s\"...", state.wifi_cfg_ssid);
            renderMenu(state, out_buf, max_len);
            return true;
        }
        case SessionMode::SHELL_CLI:
        default: {
            if (!executeCommandLine(state, vfs, line, out_buf, max_len)) {
                return false;
            }
            if (mode_ == SessionMode::SHELL_CLI) {
                renderPrompt(state, out_buf, max_len);
            }
            return true;
        }
    }
}

void ShellSession::formatStatusJson(const SystemState& state, char* out_buf, size_t max_len) {
    snprintf(out_buf, max_len,
             "{\"os\":\"C6-MicroLinux\",\"kernel\":\"6.12.0-c6os-riscv32\","
             "\"hostname\":\"%s\",\"board\":\"Waveshare-ESP32-C6-LCD-1.47\","
             "\"mode\":\"%s\",\"theme\":\"%s\",\"bl\":%u,\"flipped\":%s,"
             "\"headline\":\"%s\",\"subtitle\":\"%s\","
             "\"led_mode\":\"%s\",\"rgb\":[%u,%u,%u],"
             "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
             "\"ssh_port\":%u,\"ssh_sessions\":%u,"
             "\"free_sram\":%u,\"total_sram\":%u,\"uptime_s\":%u}\r\n",
             state.hostname,
             screenModeName(state.screen_mode),
             themeName(state.theme),
             state.backlight_pct,
             state.landscape_flipped ? "true" : "false",
             state.headline,
             state.subtitle,
             ledModeName(state.led_mode),
             state.led_r, state.led_g, state.led_b,
             state.wifi_sta_connected ? "true" : "false",
             state.wifi_ssid,
             state.wifi_ip,
             static_cast<int>(state.wifi_rssi),
             state.wifi_ap_ssid,
             state.wifi_ap_ip,
             static_cast<unsigned>(state.ssh_port),
             static_cast<unsigned>(state.ssh_active_sessions),
             static_cast<unsigned>(state.free_sram_bytes),
             static_cast<unsigned>(state.total_sram_bytes),
             static_cast<unsigned>(state.uptime_sec));
}

bool ShellSession::executeCommandLine(SystemState& state, VirtualFs& vfs,
                                      const char* cmd_line,
                                      char* out_buf, size_t max_len) {
    if (!cmd_line) return true;
    char work[256];
    strncpy(work, cmd_line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    // Support command chaining with '&&' or ';'
    char* cursor = work;
    while (cursor && *cursor) {
        char* next_sep = nullptr;
        char* p = cursor;
        bool in_quote = false;
        char quote_ch = 0;
        while (*p) {
            if ((*p == '"' || *p == '\'') && (p == cursor || *(p - 1) != '\\')) {
                if (!in_quote) {
                    in_quote = true;
                    quote_ch = *p;
                } else if (*p == quote_ch) {
                    in_quote = false;
                }
            } else if (!in_quote) {
                if (*p == ';') {
                    *p = '\0';
                    next_sep = p + 1;
                    break;
                }
                if (*p == '&' && *(p + 1) == '&') {
                    *p = '\0';
                    next_sep = p + 2;
                    break;
                }
            }
            ++p;
        }

        if (!executeSingleCommand(state, vfs, cursor, out_buf, max_len)) {
            return false;
        }
        cursor = next_sep;
    }
    return true;
}

bool ShellSession::executeSingleCommand(SystemState& state, VirtualFs& vfs,
                                        const char* raw_cmd,
                                        char* out_buf, size_t max_len) {
    char cmd_copy[256];
    strncpy(cmd_copy, raw_cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    trimSpaces(cmd_copy);
    if (cmd_copy[0] == '\0' || cmd_copy[0] == '#') {
        return true;
    }

    // Check for output redirection (> or >>)
    char* redir_path = nullptr;
    bool redir_append = false;
    {
        bool in_q = false;
        char q_ch = 0;
        for (size_t i = 0; cmd_copy[i] != '\0'; ++i) {
            char c = cmd_copy[i];
            if (c == '"' || c == '\'') {
                if (!in_q) { in_q = true; q_ch = c; }
                else if (c == q_ch) { in_q = false; }
            } else if (!in_q && c == '>') {
                cmd_copy[i] = '\0';
                if (cmd_copy[i + 1] == '>') {
                    redir_append = true;
                    redir_path = &cmd_copy[i + 2];
                } else {
                    redir_append = false;
                    redir_path = &cmd_copy[i + 1];
                }
                trimSpaces(redir_path);
                break;
            }
        }
    }

    char* argv[12] = {nullptr};
    size_t argc = tokenizeArgs(cmd_copy, argv, 12);
    if (argc == 0) return true;

    const char* prog = argv[0];

    // If redirected, capture command output into a temp buffer first
    char cmd_out[768];
    cmd_out[0] = '\0';
    char* target_buf = redir_path ? cmd_out : out_buf;
    size_t target_max = redir_path ? sizeof(cmd_out) : max_len;

    if (strcmp(prog, "s") == 0 || strcmp(prog, "true") == 0 || strcmp(prog, ":") == 0) {
        return true;
    }

    if (strcmp(prog, "exit") == 0 || strcmp(prog, "logout") == 0) {
        appendFmt(out_buf, max_len, "logout\r\n");
        return false;
    }

    if (strcmp(prog, "menu") == 0 || strcmp(prog, "c6-menu") == 0 || strcmp(prog, "/bin/c6-menu") == 0) {
        mode_ = SessionMode::MENU_MAIN;
        snprintf(menu_notice_, sizeof(menu_notice_), "Returned to interactive Screen Control Menu.");
        renderMenu(state, out_buf, max_len);
        return true;
    }

    if (strcmp(prog, "clear") == 0) {
        appendFmt(target_buf, target_max, "\033[2J\033[H");
    } else if (strcmp(prog, "help") == 0) {
        appendFmt(target_buf, target_max,
                  "C6-MicroLinux (c6os 6.12.0-riscv32) Built-in Commands:\r\n"
                  "  menu                         Launch interactive ANSI Screen Control Menu\r\n"
                  "  screen status                Print live screen & system state as JSON\r\n"
                  "  screen mode <mode>           Set mode: dashboard | console | signboard | test\r\n"
                  "  screen text <head> [sub]     Set screen headline and subtitle\r\n"
                  "  screen theme <theme>         Set theme: cyan | matrix | amber | red | mono\r\n"
                  "  screen bl <0..100>           Set LCD backlight brightness percentage\r\n"
                  "  screen flip <on|off|toggle>  Control 180-degree landscape flip\r\n"
                  "  screen console <text>        Append line to /dev/tty0 LCD terminal\r\n"
                  "  screen clear                 Clear /dev/tty0 LCD terminal\r\n"
                  "  screen led <mode> [r g b]    Set RGB LED: off | solid | pulse | rainbow | ssh\r\n"
                  "  wifi status|scan|connect     Manage 2.4 GHz Wi-Fi & SoftAP\r\n"
                  "  top [-b -n 1]                Live Linux process & CPU/SRAM monitor\r\n"
                  "  htop [-b -n 1]               Interactive color task manager & bar graphs\r\n"
                  "  lscpu, vmstat, neofetch      CPU architecture, VM stats & system summary\r\n"
                  "  uname, uptime, free, df, ps, dmesg, ls, cd, pwd, cat, echo, rm, ip, passwd, reboot\r\n");
    } else if (strcmp(prog, "uname") == 0) {
        bool all = false, rel = false, mach = false, node = false;
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-a") == 0) all = true;
            else if (strcmp(argv[i], "-r") == 0) rel = true;
            else if (strcmp(argv[i], "-m") == 0) mach = true;
            else if (strcmp(argv[i], "-n") == 0) node = true;
        }
        if (all) {
            appendFmt(target_buf, target_max,
                      "Linux %s 6.12.0-c6os-riscv32 #1 PREEMPT ESP32-C6FH4 rv32imac GNU/Linux\r\n",
                      state.hostname);
        } else if (rel) {
            appendFmt(target_buf, target_max, "6.12.0-c6os-riscv32\r\n");
        } else if (mach) {
            appendFmt(target_buf, target_max, "riscv32\r\n");
        } else if (node) {
            appendFmt(target_buf, target_max, "%s\r\n", state.hostname);
        } else {
            appendFmt(target_buf, target_max, "Linux\r\n");
        }
    } else if (strcmp(prog, "whoami") == 0) {
        appendFmt(target_buf, target_max, "root\r\n");
    } else if (strcmp(prog, "id") == 0) {
        appendFmt(target_buf, target_max, "uid=0(root) gid=0(root) groups=0(root)\r\n");
    } else if (strcmp(prog, "hostname") == 0) {
        if (argc >= 2) {
            vfs.writeFile(state, "/etc/hostname", argv[1], false);
        } else {
            appendFmt(target_buf, target_max, "%s\r\n", state.hostname);
        }
    } else if (strcmp(prog, "pwd") == 0) {
        appendFmt(target_buf, target_max, "%s\r\n", cwd_);
    } else if (strcmp(prog, "cd") == 0) {
        const char* dest = (argc >= 2) ? argv[1] : "/root";
        char norm[64];
        VirtualFs::normalizePath(cwd_, dest, norm, sizeof(norm));
        if (vfs.isDirectory(norm)) {
            strncpy(cwd_, norm, sizeof(cwd_) - 1);
            cwd_[sizeof(cwd_) - 1] = '\0';
        } else {
            appendFmt(target_buf, target_max, "cd: %s: No such directory\r\n", dest);
        }
    } else if (strcmp(prog, "ls") == 0) {
        const char* target = (argc >= 2 && argv[1][0] != '-') ? argv[1] :
                             (argc >= 3 ? argv[2] : cwd_);
        char norm[64];
        VirtualFs::normalizePath(cwd_, target, norm, sizeof(norm));
        char listing[384];
        if (vfs.listDirectory(norm, listing, sizeof(listing))) {
            appendWithCrLf(target_buf, target_max, listing);
        } else {
            appendFmt(target_buf, target_max, "ls: cannot access '%s': No such file or directory\r\n", target);
        }
    } else if (strcmp(prog, "cat") == 0) {
        if (argc < 2) {
            appendFmt(target_buf, target_max, "Usage: cat <path>\r\n");
        } else {
            char norm[64];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            char content[640];
            if (vfs.readFile(state, norm, content, sizeof(content))) {
                appendWithCrLf(target_buf, target_max, content);
            } else {
                appendFmt(target_buf, target_max, "cat: %s: No such file or directory\r\n", argv[1]);
            }
        }
    } else if (strcmp(prog, "rm") == 0) {
        if (argc < 2) {
            appendFmt(target_buf, target_max, "Usage: rm <path>\r\n");
        } else {
            char norm[64];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            if (!vfs.removeFile(state, norm)) {
                appendFmt(target_buf, target_max, "rm: cannot remove '%s': Operation not permitted or no such file\r\n", argv[1]);
            }
        }
    } else if (strcmp(prog, "echo") == 0) {
        for (size_t i = 1; i < argc; ++i) {
            appendFmt(target_buf, target_max, "%s%s", argv[i], (i + 1 < argc) ? " " : "");
        }
        appendFmt(target_buf, target_max, redir_path ? "\n" : "\r\n");
    } else if (strcmp(prog, "uptime") == 0) {
        unsigned up = static_cast<unsigned>(state.uptime_sec);
        unsigned days = up / 86400U;
        unsigned hrs  = (up % 86400U) / 3600U;
        unsigned mins = (up % 3600U) / 60U;
        unsigned secs = up % 60U;
        appendFmt(target_buf, target_max,
                  " up %u days, %02u:%02u:%02u,  %u user(s),  load average: 0.08, 0.05, 0.01\r\n",
                  days, hrs, mins, secs,
                  static_cast<unsigned>(state.ssh_active_sessions > 0 ? state.ssh_active_sessions : 1));
    } else if (strcmp(prog, "free") == 0) {
        unsigned total = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_m = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned used = (total > free_m) ? (total - free_m) : 0U;
        appendFmt(target_buf, target_max,
                  "               total        used        free      shared  buff/cache   available\r\n"
                  "Mem:          %6u      %6u      %6u           0          16      %6u (kB)\r\n"
                  "Swap:              0           0           0\r\n",
                  total, used, free_m, free_m);
    } else if (strcmp(prog, "df") == 0) {
        unsigned total = static_cast<unsigned>(state.rootfs_total_kb);
        unsigned used  = static_cast<unsigned>(state.rootfs_used_kb);
        unsigned avail = (total > used) ? (total - used) : 0U;
        unsigned pct   = (total > 0) ? ((used * 100U) / total) : 0U;
        appendFmt(target_buf, target_max,
                  "Filesystem      1K-blocks    Used Available Use%% Mounted on\r\n"
                  "/dev/mtdblock4       %4u    %4u      %4u  %2u%% /\r\n"
                  "proc                    0       0         0   0%% /proc\r\n"
                  "sysfs                   0       0         0   0%% /sys\r\n"
                  "devtmpfs               16       0        16   0%% /dev\r\n",
                  total, used, avail, pct);
        if (state.sd_mounted) {
            appendFmt(target_buf, target_max,
                      "/dev/mmcblk0p1   %8u     128  %8u   1%% /mnt/sd\r\n",
                      static_cast<unsigned>(state.sd_total_mb * 1024U),
                      static_cast<unsigned>(state.sd_total_mb * 1024U - 128U));
        }
    } else if (strcmp(prog, "ps") == 0) {
        appendFmt(target_buf, target_max,
                  "  PID USER       VSZ STAT COMMAND\r\n"
                  "    1 root      4096 S    /sbin/init\r\n"
                  "    2 root         0 SW   [kthreadd]\r\n"
                  "    8 root      6144 S    /sbin/lcdd --driver=st7789 --spi=80MHz\r\n"
                  "   12 root      8192 S    /sbin/wpa_supplicant -i wlan0\r\n"
                  "   18 root     12288 S    /usr/sbin/sshd -p %u [sessions=%u]\r\n"
                  "   24 root      6144 S    /bin/c6-sh (/dev/ttyACM0)\r\n"
                  "   31 root      6144 R    /bin/c6-sh (/dev/pts/0)\r\n",
                  state.ssh_port, state.ssh_active_sessions);
    } else if (strcmp(prog, "top") == 0 || strcmp(prog, "/bin/top") == 0 ||
               strcmp(prog, "htop") == 0 || strcmp(prog, "/bin/htop") == 0) {
        bool is_htop = (strcmp(prog, "htop") == 0 || strcmp(prog, "/bin/htop") == 0);
        bool batch = (redir_path != nullptr) || (mode_ != SessionMode::SHELL_CLI);
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-b") == 0 || strncmp(argv[i], "-n", 2) == 0 ||
                strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "-1") == 0) {
                batch = true;
            }
        }
        if (batch) {
            renderTop(state, is_htop, false, target_buf, target_max);
            appendFmt(target_buf, target_max, "\r\n");
        } else {
            prev_mode_ = mode_;
            mode_ = is_htop ? SessionMode::HTOP_VIEW : SessionMode::TOP_VIEW;
            renderTop(state, is_htop, true, target_buf, target_max);
        }
    } else if (strcmp(prog, "lscpu") == 0 || strcmp(prog, "/bin/lscpu") == 0) {
        appendFmt(target_buf, target_max,
                  "Architecture:          riscv32\r\n"
                  "  CPU op-mode(s):      32-bit\r\n"
                  "  Byte Order:          Little Endian\r\n"
                  "CPU(s):                1\r\n"
                  "  On-line CPU(s) list: 0\r\n"
                  "Vendor ID:             Espressif\r\n"
                  "  Model name:          ESP32-C6FH4 High-Performance RV32IMAC\r\n"
                  "  CPU max MHz:         %u.0000\r\n"
                  "  ISA extensions:      rv32imac_zicsr_zifencei\r\n"
                  "  L1 I-Cache:          32 KiB\r\n"
                  "  LP Co-Processor:     RV32IMAC @ 20 MHz (LP-SRAM 16 KiB)\r\n",
                  static_cast<unsigned>(state.cpu_freq_mhz));
    } else if (strcmp(prog, "vmstat") == 0 || strcmp(prog, "/bin/vmstat") == 0) {
        unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
        appendFmt(target_buf, target_max,
                  "procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----\r\n"
                  " r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st\r\n"
                  " %u  0      0 %6u     16     24    0    0     4     2  240  185  5  3 92  0  0\r\n",
                  1U + static_cast<unsigned>(state.ssh_active_sessions),
                  free_kb);
        (void)used_kb;
    } else if (strcmp(prog, "neofetch") == 0 || strcmp(prog, "/bin/neofetch") == 0) {
        unsigned up = static_cast<unsigned>(state.uptime_sec);
        unsigned mins = (up % 3600U) / 60U;
        unsigned secs = up % 60U;
        unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
        appendFmt(target_buf, target_max,
                  "\033[1;36m       .---.        \033[1;32mroot\033[0m@\033[1;32m%s\033[0m\r\n"
                  "\033[1;36m      /     \\       \033[0m-----------------\r\n"
                  "\033[1;36m     | (\033[1;33mC6\033[1;36m)  |      \033[1;36mOS\033[0m: C6-MicroLinux (c6os) riscv32\r\n"
                  "\033[1;36m      \\     /       \033[1;36mHost\033[0m: Waveshare ESP32-C6-LCD-1.47\r\n"
                  "\033[1;36m       '---'        \033[1;36mKernel\033[0m: 6.12.0-c6os-riscv32\r\n"
                  "\033[1;32m    RISC-V 32-bit   \033[1;36mUptime\033[0m: %um %us\r\n"
                  "                    \033[1;36mDisplay\033[0m: 172x320 ST7789 (%s, BL %u%%)\r\n"
                  "                    \033[1;36mCPU\033[0m: ESP32-C6FH4 (1) @ %uMHz\r\n"
                  "                    \033[1;36mMemory\033[0m: %uKiB / %uKiB (%s)\r\n",
                  state.hostname,
                  mins, secs,
                  screenModeName(state.screen_mode),
                  static_cast<unsigned>(state.backlight_pct),
                  static_cast<unsigned>(state.cpu_freq_mhz),
                  used_kb, total_kb,
                  state.wifi_ip);
    } else if (strcmp(prog, "dmesg") == 0) {
        for (size_t i = 0; i < state.dmesg_count; ++i) {
            appendFmt(target_buf, target_max, "%s\r\n", state.dmesg[i]);
        }
    } else if (strcmp(prog, "ip") == 0 || strcmp(prog, "ifconfig") == 0) {
        appendFmt(target_buf, target_max,
                  "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 1500\r\n"
                  "    inet 127.0.0.1/8 scope host lo\r\n"
                  "2: wlan0: <%s,BROADCAST,MULTICAST> mtu 1500\r\n"
                  "    link/ether %s\r\n"
                  "    inet %s/24 scope global wlan0 (SSID: %s, RSSI: %d dBm)\r\n"
                  "3: ap0: <%s,BROADCAST,MULTICAST> mtu 1500\r\n"
                  "    inet %s/24 scope global ap0 (SSID: %s)\r\n",
                  state.wifi_sta_connected ? "UP,LOWER_UP" : "NO-CARRIER",
                  state.wifi_mac, state.wifi_ip,
                  state.wifi_ssid[0] ? state.wifi_ssid : "none",
                  static_cast<int>(state.wifi_rssi),
                  state.wifi_ap_enabled ? "UP,LOWER_UP" : "DOWN",
                  state.wifi_ap_ip, state.wifi_ap_ssid);
    } else if (strcmp(prog, "passwd") == 0) {
        if (argc < 2) {
            appendFmt(target_buf, target_max, "Usage: passwd <new_password>\r\n");
        } else {
            strncpy(state.root_password, argv[1], sizeof(state.root_password) - 1);
            state.root_password[sizeof(state.root_password) - 1] = '\0';
            state.fs_sync_requested = true;
            appendFmt(target_buf, target_max, "passwd: password updated successfully for root\r\n");
        }
    } else if (strcmp(prog, "reboot") == 0) {
        state.reboot_requested = true;
        appendFmt(target_buf, target_max, "Broadcast message from root@%s: System is going down for reboot NOW!\r\n",
                  state.hostname);
    } else if (strcmp(prog, "wifi") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            appendFmt(target_buf, target_max,
                      "Wi-Fi Station : %s\r\n"
                      "  SSID        : %s\r\n"
                      "  IPv4        : %s\r\n"
                      "  MAC         : %s\r\n"
                      "  RSSI        : %d dBm\r\n"
                      "SoftAP Mode   : %s (SSID: %s, IP: %s)\r\n",
                      state.wifi_sta_connected ? "CONNECTED" : "DISCONNECTED",
                      state.wifi_ssid[0] ? state.wifi_ssid : state.wifi_cfg_ssid,
                      state.wifi_ip, state.wifi_mac, static_cast<int>(state.wifi_rssi),
                      state.wifi_ap_enabled ? "ENABLED" : "DISABLED",
                      state.wifi_ap_ssid, state.wifi_ap_ip);
        } else if (strcmp(argv[1], "scan") == 0) {
            state.wifi_scan_requested = true;
            appendFmt(target_buf, target_max, "Triggered 2.4 GHz Wi-Fi scan.\r\n");
            for (size_t i = 0; i < state.wifi_scan_count; ++i) {
                appendFmt(target_buf, target_max, "  [%u] %-24s  %4d dBm  ch %2u  %s\r\n",
                          static_cast<unsigned>(i + 1),
                          state.wifi_scan[i].ssid,
                          static_cast<int>(state.wifi_scan[i].rssi),
                          state.wifi_scan[i].channel,
                          state.wifi_scan[i].open ? "OPEN" : "WPA2/3");
            }
        } else if (strcmp(argv[1], "connect") == 0 && argc >= 3) {
            strncpy(state.wifi_cfg_ssid, argv[2], sizeof(state.wifi_cfg_ssid) - 1);
            state.wifi_cfg_ssid[sizeof(state.wifi_cfg_ssid) - 1] = '\0';
            if (argc >= 4) {
                strncpy(state.wifi_cfg_pass, argv[3], sizeof(state.wifi_cfg_pass) - 1);
                state.wifi_cfg_pass[sizeof(state.wifi_cfg_pass) - 1] = '\0';
            } else {
                state.wifi_cfg_pass[0] = '\0';
            }
            state.wifi_sta_enabled = true;
            state.wifi_reconnect_requested = true;
            state.fs_sync_requested = true;
            appendFmt(target_buf, target_max, "OK WIFI CONNECTING \"%s\"\r\n", state.wifi_cfg_ssid);
        } else if (strcmp(argv[1], "ap") == 0 && argc >= 3) {
            state.wifi_ap_enabled = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
            state.wifi_reconnect_requested = true;
            appendFmt(target_buf, target_max, "OK WIFI AP %s\r\n", state.wifi_ap_enabled ? "ON" : "OFF");
        } else {
            appendFmt(target_buf, target_max,
                      "Usage: wifi <status | scan | connect <ssid> [pass] | ap <on|off>>\r\n");
        }
    } else if (strcmp(prog, "screen") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            char json[512];
            formatStatusJson(state, json, sizeof(json));
            appendFmt(target_buf, target_max, "%s", json);
        } else if (strcmp(argv[1], "mode") == 0 && argc >= 3) {
            if (vfs.writeFile(state, "/sys/class/graphics/fb0/mode", argv[2], false)) {
                appendFmt(target_buf, target_max, "OK MODE %s\r\n", screenModeName(state.screen_mode));
            } else {
                appendFmt(target_buf, target_max, "ERR invalid mode '%s' (use: dashboard|console|signboard|test)\r\n", argv[2]);
            }
        } else if (strcmp(argv[1], "text") == 0 && argc >= 3) {
            strncpy(state.headline, argv[2], sizeof(state.headline) - 1);
            state.headline[sizeof(state.headline) - 1] = '\0';
            if (argc >= 4) {
                strncpy(state.subtitle, argv[3], sizeof(state.subtitle) - 1);
                state.subtitle[sizeof(state.subtitle) - 1] = '\0';
            }
            state.ui_dirty = true;
            appendFmt(target_buf, target_max, "OK TEXT \"%s\" \"%s\"\r\n", state.headline, state.subtitle);
        } else if (strcmp(argv[1], "theme") == 0 && argc >= 3) {
            if (vfs.writeFile(state, "/sys/class/graphics/fb0/theme", argv[2], false)) {
                appendFmt(target_buf, target_max, "OK THEME %s\r\n", themeName(state.theme));
            } else {
                appendFmt(target_buf, target_max, "ERR invalid theme '%s' (use: cyan|matrix|amber|red|mono)\r\n", argv[2]);
            }
        } else if (strcmp(argv[1], "bl") == 0 && argc >= 3) {
            vfs.writeFile(state, "/sys/class/backlight/st7789/brightness", argv[2], false);
            appendFmt(target_buf, target_max, "OK BL %u\r\n", state.backlight_pct);
        } else if (strcmp(argv[1], "flip") == 0 && argc >= 3) {
            if (strcmp(argv[2], "toggle") == 0) {
                state.landscape_flipped = !state.landscape_flipped;
            } else {
                state.landscape_flipped = (strcmp(argv[2], "on") == 0 ||
                                           strcmp(argv[2], "1") == 0 ||
                                           strcmp(argv[2], "true") == 0);
            }
            state.ui_dirty = true;
            appendFmt(target_buf, target_max, "OK FLIP %s\r\n", state.landscape_flipped ? "ON" : "OFF");
        } else if (strcmp(argv[1], "bar") == 0 && argc >= 3) {
            if (strcmp(argv[2], "off") == 0) {
                state.custom_bar_visible = false;
                state.ui_dirty = true;
                appendFmt(target_buf, target_max, "OK BAR OFF\r\n");
            } else {
                int pct = atoi(argv[2]);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                state.custom_bar_visible = true;
                state.custom_bar_pct = static_cast<uint8_t>(pct);
                if (argc >= 4) {
                    strncpy(state.custom_bar_label, argv[3], sizeof(state.custom_bar_label) - 1);
                    state.custom_bar_label[sizeof(state.custom_bar_label) - 1] = '\0';
                } else {
                    snprintf(state.custom_bar_label, sizeof(state.custom_bar_label), "LOAD %d%%", pct);
                }
                state.ui_dirty = true;
                appendFmt(target_buf, target_max, "OK BAR %u \"%s\"\r\n", state.custom_bar_pct, state.custom_bar_label);
            }
        } else if (strcmp(argv[1], "console") == 0 && argc >= 3) {
            state.appendFbConsoleLine(argv[2]);
            appendFmt(target_buf, target_max, "OK CONSOLE\r\n");
        } else if (strcmp(argv[1], "clear") == 0) {
            state.clearFbConsole();
            appendFmt(target_buf, target_max, "OK CLEAR\r\n");
        } else if (strcmp(argv[1], "led") == 0 && argc >= 3) {
            if (argc >= 5 && isdigit(static_cast<unsigned char>(argv[2][0]))) {
                state.led_mode = LedMode::SOLID;
                state.led_r = static_cast<uint8_t>(atoi(argv[2]) & 0xFF);
                state.led_g = static_cast<uint8_t>(atoi(argv[3]) & 0xFF);
                state.led_b = static_cast<uint8_t>(atoi(argv[4]) & 0xFF);
                state.ui_dirty = true;
                appendFmt(target_buf, target_max, "OK LED SOLID %u %u %u\r\n",
                          state.led_r, state.led_g, state.led_b);
            } else if (vfs.writeFile(state, "/sys/class/leds/ws2812/mode", argv[2], false)) {
                if (argc >= 6) {
                    state.led_r = static_cast<uint8_t>(atoi(argv[3]) & 0xFF);
                    state.led_g = static_cast<uint8_t>(atoi(argv[4]) & 0xFF);
                    state.led_b = static_cast<uint8_t>(atoi(argv[5]) & 0xFF);
                }
                appendFmt(target_buf, target_max, "OK LED %s (%u,%u,%u)\r\n",
                          ledModeName(state.led_mode), state.led_r, state.led_g, state.led_b);
            } else {
                appendFmt(target_buf, target_max, "ERR invalid led command\r\n");
            }
        } else {
            appendFmt(target_buf, target_max,
                      "Usage: screen <status | mode <m> | text <h> [s] | theme <t> | bl <0..100> | flip <on|off> | bar <pct> [lbl] | console <str> | clear | led <mode> [r g b]>\r\n");
        }
    } else {
        appendFmt(target_buf, target_max, "c6-sh: %s: command not found (type 'help' or 'menu')\r\n", prog);
    }

    if (redir_path) {
        char norm[64];
        VirtualFs::normalizePath(cwd_, redir_path, norm, sizeof(norm));
        if (!vfs.writeFile(state, norm, cmd_out, redir_append)) {
            appendFmt(out_buf, max_len, "c6-sh: %s: Permission denied or invalid device value\r\n", redir_path);
        }
    }

    return true;
}

}  // namespace c6os
