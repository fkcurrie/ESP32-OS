#include "c6os_shell.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "c6os_display.h"
#include "c6os_elf.h"
#include "c6os_epkg.h"
#include "c6os_lua.h"
#include "ssh_auth.h"

namespace c6os {

namespace {

constexpr size_t EDIT_HEAP_CAPACITY = VFS_MAX_FILE_SIZE;

struct ExecDepthGuard {
    uint8_t& depth;
    explicit ExecDepthGuard(uint8_t& d) : depth(d) { ++depth; }
    ~ExecDepthGuard() {
        if (depth > 0) --depth;
    }
};

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

bool endsWithCI(const char* s, const char* suffix) {
    if (!s || !suffix) return false;
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return false;
    for (size_t i = 0; i < xl; ++i) {
        if (tolower(static_cast<unsigned char>(s[sl - xl + i])) !=
            tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

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
    char cmd[36];
    char tree_cmd[36];
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

void formatLiveTaskCommand(const char* raw_name, uint16_t ssh_port,
                           char* out_cmd, size_t cmd_cap,
                           char* out_tree, size_t tree_cap) {
    if (strcmp(raw_name, "sshd") == 0) {
        snprintf(out_cmd, cmd_cap, "/usr/sbin/sshd -D -p %u", static_cast<unsigned>(ssh_port));
        snprintf(out_tree, tree_cap, "+- /usr/sbin/sshd -p %u", static_cast<unsigned>(ssh_port));
    } else if (strcmp(raw_name, "loopTask") == 0 || strcmp(raw_name, "main") == 0) {
        snprintf(out_cmd, cmd_cap, "/sbin/init [%s]", raw_name);
        snprintf(out_tree, tree_cap, "/sbin/init [%s]", raw_name);
    } else if (strcmp(raw_name, "lcdd") == 0) {
        snprintf(out_cmd, cmd_cap, "/sbin/lcdd --dma=spi2");
        snprintf(out_tree, tree_cap, "+- /sbin/lcdd --dma=spi2");
    } else if (strncmp(raw_name, "IDLE", 4) == 0 || strcmp(raw_name, "tiT") == 0 ||
               strcmp(raw_name, "sys_evt") == 0 || strcmp(raw_name, "esp_timer") == 0 ||
               strcmp(raw_name, "wifi") == 0 || strcmp(raw_name, "Tmr Svc") == 0) {
        snprintf(out_cmd, cmd_cap, "[%s]", raw_name);
        snprintf(out_tree, tree_cap, "|  `- [%s]", raw_name);
    } else {
        snprintf(out_cmd, cmd_cap, "%s", raw_name);
        snprintf(out_tree, tree_cap, "+- %s", raw_name);
    }
}

}  // namespace

ShellSession::ShellSession()
    : mode_(SessionMode::MENU_MAIN),
      prev_mode_(SessionMode::SHELL_CLI),
      sort_mode_(MonitorSort::CPU),
      tree_view_(false),
      exec_depth_(0),
      line_len_(0),
      watch_interval_sec_(2),
      edit_buf_(nullptr) {
    strncpy(cwd_, "/root", sizeof(cwd_) - 1);
    cwd_[sizeof(cwd_) - 1] = '\0';
    memset(line_buf_, 0, sizeof(line_buf_));
    memset(pending_ssid_, 0, sizeof(pending_ssid_));
    memset(watch_cmd_, 0, sizeof(watch_cmd_));
    memset(edit_path_, 0, sizeof(edit_path_));
    strncpy(menu_notice_,
            "Ready. Press [1-9], [+/-], [m] LCD-htop, [k] ticker, [a] alert, [t] htop, [s] shell.",
            sizeof(menu_notice_) - 1);
    menu_notice_[sizeof(menu_notice_) - 1] = '\0';
}

ShellSession::~ShellSession() {
    freeEditBuffer();
}

bool ShellSession::ensureEditBuffer() {
    if (!edit_buf_) {
        edit_buf_ = static_cast<char*>(calloc(1, EDIT_HEAP_CAPACITY));
    }
    return edit_buf_ != nullptr;
}

void ShellSession::freeEditBuffer() {
    if (edit_buf_) {
        free(edit_buf_);
        edit_buf_ = nullptr;
    }
}

void ShellSession::begin(SystemState& state, VirtualFs& vfs, bool start_in_menu,
                         char* out_buf, size_t max_len) {
    BufWriter out(out_buf, max_len, false);
    line_len_ = 0;
    exec_depth_ = 0;
    freeEditBuffer();
    if (start_in_menu) {
        mode_ = SessionMode::MENU_MAIN;
        renderMenuTo(state, out);
    } else {
        mode_ = SessionMode::SHELL_CLI;
        char motd[384];
        if (vfs.readFile(state, "/etc/motd", motd, sizeof(motd))) {
            out.putWithCrLf(motd);
            out.putStr("\r\n");
        }
        renderPromptTo(state, out);
    }
}

void ShellSession::renderPrompt(const SystemState& state, char* out_buf, size_t max_len) const {
    BufWriter out(out_buf, max_len, true);
    renderPromptTo(state, out);
}

void ShellSession::renderPromptTo(const SystemState& state, BufWriter& out) const {
    const char* display_cwd = (strcmp(cwd_, "/root") == 0) ? "~" : cwd_;
    out.fmt("\033[1;32mroot@%s\033[0m:\033[1;34m%s\033[0m# ", state.hostname, display_cwd);
}

void ShellSession::renderMenu(const SystemState& state, char* out_buf, size_t max_len) const {
    BufWriter out(out_buf, max_len, true);
    renderMenuTo(state, out);
}

void ShellSession::renderMenuTo(const SystemState& state, BufWriter& out) const {
    char bar[12];
    int filled = (state.backlight_pct + 5) / 10;
    if (filled > 10) filled = 10;
    for (int i = 0; i < 10; ++i) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    bar[10] = '\0';

    const char* ip_str = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
    const char* net_tag = state.wifi_sta_connected ? state.wifi_ssid :
                          (state.wifi_ap_enabled ? "AP:C6-Linux-OS" : "OFFLINE");
    int temp_c = state.cpu_temp_deci_c / 10;
    int temp_d = abs(state.cpu_temp_deci_c % 10);

    out.fmt(
        "\033[2J\033[H"
        "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;37mC6-MicroLinux (c6os 6.12-riscv32) — Interactive Screen Controller\033[0m \033[1;36m|\033[0m\r\n"
        "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
        "\033[1;36m|\033[0m  Host: \033[1;32mroot@%-14s\033[0m  Net: \033[1;33m%-15s\033[0m (\033[1;37m%-14s\033[0m) \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  SRAM: \033[1;37m%3u KB free\033[0m/%3u KB   Temp: \033[1;33m%2d.%dC\033[0m   Up: \033[1;37m%5us\033[0m   SSH: \033[1;32m%u\033[0m     \033[1;36m|\033[0m\r\n"
        "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
        "\033[1;36m|\033[0m  LCD Mode : \033[1;36m%-18s\033[0m  Theme    : \033[1;35m%-10s\033[0m Flip: \033[1;37m%-3s\033[0m    \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  Backlight: [\033[1;33m%s\033[0m] \033[1;37m%3u%%\033[0m   RGB LED  : \033[1;32m%-7s\033[0m (%3u,%3u,%3u)   \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  Headline : \033[1;37m\"%-48.48s\"\033[0m     \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  Ticker   : \033[1;33m%-6s\033[0m | Alert: \033[1;31m%-6s\033[0m | Cron Jobs: \033[1;32m%u\033[0m | BLE: \033[1;36m%-3s\033[0m     \033[1;36m|\033[0m\r\n"
        "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[1]\033[0m Cycle LCD Mode   (Dash -> Console -> Sign -> Test -> Monitor) \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[2]\033[0m Set Screen Text  (Update Headline & Subtitle on LCD)          \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[3]\033[0m Cycle Color Theme (Cyber Cyan / Matrix / Amber / Red / Mono)  \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[4]\033[0m Set Backlight %%  (Or press \033[1;32m+\033[0m / \033[1;32m-\033[0m to step brightness +/-10%%)   \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[5]\033[0m Toggle 180 deg Landscape Flip                                 \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[6]\033[0m Control Onboard WS2812 RGB LED (Mode & Color)                 \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[7]\033[0m Live LCD Terminal Mirror (Type lines directly onto /dev/tty0) \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[8]\033[0m Configure Wi-Fi Network (Scan & Join 2.4 GHz SSID)            \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[9]\033[0m Kernel & Hardware Diagnostics (/proc, sensors, pinout)        \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[m]\033[0m On-LCD \033[1;36mhtop\033[0m Telemetry Mode  \033[1;33m[k]\033[0m Marquee Ticker  \033[1;33m[a]\033[0m LCD Alert \033[1;36m|\033[0m\r\n"
        "\033[1;36m|\033[0m  \033[1;33m[t]\033[0m SSH \033[1;32mhtop\033[0m Monitor  \033[1;32m[s]\033[0m Linux Shell (c6-sh)       \033[1;31m[q]\033[0m Disconnect\033[1;36m|\033[0m\r\n"
        "\033[1;36m+--------------------------------------------------------------------+\033[0m\r\n"
        "  \033[1;32mStatus:\033[0m %s\r\n"
        "  \033[1;36mSelect option [1-9, +/-, m, k, a, t, s, q]:\033[0m ",
        state.hostname, net_tag, ip_str,
        static_cast<unsigned>(state.free_sram_bytes / 1024U),
        static_cast<unsigned>(state.total_sram_bytes / 1024U),
        temp_c, temp_d,
        static_cast<unsigned>(state.uptime_sec),
        static_cast<unsigned>(state.ssh_active_sessions),
        screenModeLabel(state.screen_mode), themeName(state.theme),
        state.landscape_flipped ? "YES" : "NO",
        bar, static_cast<unsigned>(state.backlight_pct),
        ledModeName(state.led_mode),
        static_cast<unsigned>(state.led_r),
        static_cast<unsigned>(state.led_g),
        static_cast<unsigned>(state.led_b),
        state.headline,
        state.ticker_enabled ? "ACTIVE" : "OFF",
        state.alert_active ? "ACTIVE" : "OFF",
        static_cast<unsigned>(state.activeCronCount()),
        state.ble_enabled ? "ON" : "OFF",
        menu_notice_);
}

void ShellSession::renderTop(const SystemState& state, bool is_htop, bool clear_screen,
                             char* out_buf, size_t max_len) const {
    BufWriter out(out_buf, max_len, true);
    renderTopTo(state, is_htop, clear_screen, out);
}

void ShellSession::renderTopTo(const SystemState& state, bool is_htop, bool clear_screen,
                               BufWriter& out) const {
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

    TaskEntry tasks[14];
    size_t task_count = 0;

    const HardwareOps* hw = getHardwareOps();
    TaskInfoEntry live[14];
    uint32_t live_total_permille = 0;
    if (hw && hw->get_tasks) {
        size_t n = hw->get_tasks(live, 14, &live_total_permille);
        for (size_t i = 0; i < n && task_count < 14; ++i) {
            TaskEntry& dst = tasks[task_count++];
            dst.pid = static_cast<unsigned>(live[i].pid);
            dst.user = "root";
            dst.pri = live[i].prio;
            dst.ni = 0;
            unsigned stk_kb = static_cast<unsigned>((live[i].stack_free_bytes + 1023U) / 1024U);
            dst.virt_kb = stk_kb + 4U;
            dst.res_kb = stk_kb;
            dst.shr_kb = 1U;
            dst.state_ch = live[i].state_ch ? live[i].state_ch : 'S';
            dst.cpu_tenths = static_cast<unsigned>(live[i].cpu_permille);
            dst.time_cs = static_cast<unsigned>(live[i].runtime_cs);
            formatLiveTaskCommand(live[i].name, state.ssh_port,
                                  dst.cmd, sizeof(dst.cmd),
                                  dst.tree_cmd, sizeof(dst.tree_cmd));
        }
    }

    if (task_count == 0) {
        unsigned wifi_cpu = state.wifi_sta_connected ? (22U + (up % 5U)) : 8U;
        unsigned ssh_cpu  = (state.ssh_active_sessions > 0) ? (44U + ((up * 3U) % 9U)) : 4U;
        unsigned lcd_cpu  = 14U + ((up * 2U) % 5U);
        unsigned mon_cpu  = 10U + (up % 4U);

        struct SeedRow {
            unsigned pid; int pri; int ni; unsigned v; unsigned r; unsigned s;
            char st; unsigned cpu; unsigned t; const char* c; const char* tc;
        };
        const SeedRow rows[9] = {
            {  1, 20,  0,  4,  1,  1, 'S',  2, up * 2U + 4U,   "/sbin/init",                           "/sbin/init" },
            {  2, 20,  0,  0,  0,  0, 'S',  1, up * 1U + 2U,   "[kthreadd]",                           "+- [kthreadd]" },
            {  4, -2,  0,  0,  0,  0, 'S',  5, up * 6U + 11U,  "[esp_timer]",                          "|  `- [esp_timer]" },
            {  8, 15, -5, 20, 20,  4, 'S', lcd_cpu, up * 18U + 9U, "/sbin/lcdd --driver=st7789 --dma=80M", "+- /sbin/lcdd --dma=80M" },
            { 12, 20,  0, 32, 24,  8, 'S', wifi_cpu, up * 24U + 8U, "/sbin/wpa_supplicant -i wlan0",    "+- /sbin/wpa_supplicant -i wlan0" },
            { 15, 20,  0,  4,  2,  1, 'S',  4, up * 7U + 3U,   "/sbin/ws2812d --gpio=8",               "+- /sbin/ws2812d --gpio=8" },
            { 18, 20,  0, 36, 24,  8, 'S', ssh_cpu, up * 41U + 27U, "/usr/sbin/sshd -D -p 22",          "+- /usr/sbin/sshd -D -p 22" },
            { 24, 20,  0,  8,  4,  2, 'S',  3, up * 4U + 5U,   "/bin/c6-sh",                           "|  `- /bin/c6-sh" },
            { 38, 20,  0,  6,  3,  2, 'R', mon_cpu, 15U + (up % 90U),
              is_htop ? "/bin/htop" : "/bin/top",
              is_htop ? "|     `- /bin/htop" : "|     `- /bin/top" }
        };
        for (size_t i = 0; i < 9; ++i) {
            TaskEntry& d = tasks[task_count++];
            d.pid = rows[i].pid; d.user = "root"; d.pri = rows[i].pri; d.ni = rows[i].ni;
            d.virt_kb = rows[i].v; d.res_kb = rows[i].r; d.shr_kb = rows[i].s;
            d.state_ch = rows[i].st; d.cpu_tenths = rows[i].cpu; d.time_cs = rows[i].t;
            strncpy(d.cmd, rows[i].c, sizeof(d.cmd) - 1); d.cmd[sizeof(d.cmd) - 1] = '\0';
            strncpy(d.tree_cmd, rows[i].tc, sizeof(d.tree_cmd) - 1); d.tree_cmd[sizeof(d.tree_cmd) - 1] = '\0';
        }
    }

    if (!tree_view_ || !is_htop) {
        for (size_t i = 0; i < task_count; ++i) {
            for (size_t j = i + 1; j < task_count; ++j) {
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
    unsigned running_cnt = 0;
    for (size_t i = 0; i < task_count; ++i) {
        if (strncmp(tasks[i].cmd, "[IDLE", 5) != 0) {
            total_cpu_tenths += tasks[i].cpu_tenths;
        }
        if (tasks[i].state_ch == 'R') ++running_cnt;
    }
    if (total_cpu_tenths > 1000U) total_cpu_tenths = 1000U;
    if (running_cnt == 0) running_cnt = 1U;
    unsigned sleeping_cnt = (task_count > running_cnt) ? static_cast<unsigned>(task_count - running_cnt) : 0U;

    unsigned us_tenths = (total_cpu_tenths * 55U) / 100U;
    unsigned sy_tenths = total_cpu_tenths - us_tenths;
    unsigned id_tenths = (1000U > total_cpu_tenths) ? (1000U - total_cpu_tenths) : 0U;

    out.putStr(clear_screen ? "\033[2J\033[H" : "\033[H");

    if (!is_htop) {
        out.fmt(
            "top - %02u:%02u:%02u up %u day%s, %02u:%02u,  %u user,  load average: 0.%02u, 0.09, 0.04\033[K\r\n"
            "Tasks: \033[1m%3u\033[0m total, \033[1m%3u\033[0m running, \033[1m%3u\033[0m sleeping,   0 stopped,   0 zombie\033[K\r\n"
            "%%Cpu(s): \033[1m%2u.%u\033[0m us, \033[1m%2u.%u\033[0m sy,  0.0 ni, \033[1m%2u.%u\033[0m id,  0.0 wa,  0.0 hi,  0.0 si\033[K\r\n"
            "KiB Mem : \033[1m%6u.0\033[0m total, \033[1m%6u.0\033[0m free, \033[1m%6u.0\033[0m used, \033[1m%6u.0\033[0m buff/cache\033[K\r\n"
            "KiB Swap:      0.0 total,      0.0 free,      0.0 used. \033[1m%6u.0\033[0m avail Mem\033[K\r\n"
            "\033[K\r\n"
            "\033[7m  PID USER      PR  NI    VIRT    RES    SHR S  %%CPU  %%MEM     TIME+ COMMAND                  \033[0m\033[K\r\n",
            hrs, mins, secs, days, (days == 1 ? "" : "s"), hrs, mins,
            static_cast<unsigned>(state.ssh_active_sessions > 0 ? state.ssh_active_sessions : 1),
            10U + (total_cpu_tenths / 15U),
            static_cast<unsigned>(task_count), running_cnt, sleeping_cnt,
            us_tenths / 10U, us_tenths % 10U,
            sy_tenths / 10U, sy_tenths % 10U,
            id_tenths / 10U, id_tenths % 10U,
            total_kb, free_kb, used_kb, cache_kb, free_kb + cache_kb);

        for (size_t i = 0; i < task_count; ++i) {
            const TaskEntry& t = tasks[i];
            unsigned mem_t = (t.res_kb * 1000U) / total_kb;
            unsigned t_min = t.time_cs / 6000U;
            unsigned t_sec = (t.time_cs / 100U) % 60U;
            unsigned t_hun = t.time_cs % 100U;
            const char* row_style = (t.state_ch == 'R') ? "\033[1m" : "";
            const char* row_end   = (t.state_ch == 'R') ? "\033[0m" : "";
            out.fmt(
                "%s%5u %-8s %3d %3d %6uK %5uK %5uK %c %3u.%u %3u.%u %3u:%02u.%02u %-24s%s\033[K\r\n",
                row_style, t.pid, t.user, t.pri, t.ni,
                t.virt_kb, t.res_kb, t.shr_kb, t.state_ch,
                t.cpu_tenths / 10U, t.cpu_tenths % 10U,
                mem_t / 10U, mem_t % 10U,
                t_min, t_sec, t_hun, t.cmd, row_end);
        }

        const char* sort_label = (sort_mode_ == MonitorSort::MEM) ? "%MEM" :
                                 (sort_mode_ == MonitorSort::TIME ? "TIME+" : "%CPU");
        out.fmt(
            "\033[K\r\n"
            "\033[7m [q] Quit  [P] Sort:%%CPU  [M] Sort:%%MEM  [T] Sort:TIME+  [h] Switch to htop  (Sort: %-5s) \033[0m\033[K",
            sort_label);
    } else {
        char cpu_bar[21], mem_bar[21], fsh_bar[21];
        buildBar(cpu_bar, 20, total_cpu_tenths / 10U + 2U, '|');
        buildBar(mem_bar, 20, mem_pct, '|');
        unsigned fsh_pct = (state.rootfs_total_kb > 0)
                               ? ((state.rootfs_used_kb * 100U) / state.rootfs_total_kb)
                               : 1U;
        if (fsh_pct < 5U) fsh_pct = 5U;
        buildBar(fsh_bar, 20, fsh_pct, '|');

        int temp_c = state.cpu_temp_deci_c / 10;
        int temp_d = abs(state.cpu_temp_deci_c % 10);

        out.fmt(
            "  \033[1;36m0\033[0m  [\033[1;32m%s\033[0m \033[1;37m%2u.%u%%\033[0m]   \033[1;36mTasks:\033[0m \033[1;37m%u\033[0m; \033[1;32m%u running\033[0m (\033[1;33m%d.%dC\033[0m)\033[K\r\n"
            "  \033[1;36mMem\033[0m[\033[1;33m%s\033[0m \033[1;37m%3u/%3uK\033[0m] \033[1;36mLoad average:\033[0m \033[1;37m0.%02u 0.09 0.04\033[0m\033[K\r\n"
            "  \033[1;36mFsh\033[0m[\033[1;35m%s\033[0m \033[1;37m%3u/%4uK\033[0m] \033[1;36mUptime:\033[0m \033[1;37m%02u:%02u:%02u\033[0m (rv32imac @ %uMHz)\033[K\r\n"
            "  \033[1;36mNet\033[0m[\033[1;32m%-13s %4ddBm\033[0m \033[1;37mwlan0\033[0m]   \033[1;36mLCD:\033[0m \033[1;37m%s\033[0m (%s, BL:%u%%)\033[K\r\n"
            "\033[K\r\n"
            "\033[30;42m  PID USER      PRI  NI  VIRT   RES   SHR S  CPU%%  MEM%%   TIME+  Command                        \033[0m\033[K\r\n",
            cpu_bar, total_cpu_tenths / 10U, total_cpu_tenths % 10U,
            static_cast<unsigned>(task_count), running_cnt,
            temp_c, temp_d,
            mem_bar, used_kb, total_kb,
            10U + (total_cpu_tenths / 15U),
            fsh_bar, static_cast<unsigned>(state.rootfs_used_kb), static_cast<unsigned>(state.rootfs_total_kb),
            hrs, mins, secs, static_cast<unsigned>(state.cpu_freq_mhz),
            state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip,
            static_cast<int>(state.wifi_rssi),
            screenModeName(state.screen_mode), themeName(state.theme),
            static_cast<unsigned>(state.backlight_pct));

        for (size_t i = 0; i < task_count; ++i) {
            const TaskEntry& t = tasks[i];
            unsigned mem_t = (t.res_kb * 1000U) / total_kb;
            unsigned t_min = t.time_cs / 6000U;
            unsigned t_sec = (t.time_cs / 100U) % 60U;
            unsigned t_hun = t.time_cs % 100U;
            const char* cmd_str = tree_view_ ? t.tree_cmd : t.cmd;
            const char* state_col = (t.state_ch == 'R') ? "\033[1;32mR\033[0m" : "S";
            out.fmt(
                "%5u \033[1;32m%-8s\033[0m %4d %3d %4uK %4uK %4uK %s %3u.%u %3u.%u %2u:%02u.%02u \033[1;37m%-30s\033[0m\033[K\r\n",
                t.pid, t.user, t.pri, t.ni,
                t.virt_kb, t.res_kb, t.shr_kb, state_col,
                t.cpu_tenths / 10U, t.cpu_tenths % 10U,
                mem_t / 10U, mem_t % 10U,
                t_min, t_sec, t_hun, cmd_str);
        }

        out.putStr(
            "\033[K\r\n"
            "\033[1;37mF1\033[30;46mTop(h)\033[0m \033[1;37mF5\033[30;46mTree(t)\033[0m \033[1;37mF6\033[30;46mSortCPU(P)\033[0m \033[1;37mF7\033[30;46mSortMEM(M)\033[0m \033[1;37mF9\033[30;46mMenu(m)\033[0m \033[1;37mF10\033[30;46mQuit(q)\033[0m\033[K");
    }
}

void ShellSession::renderWatchFrameTo(SystemState& state, VirtualFs& vfs, bool clear_screen,
                                      BufWriter& out) {
    char date_str[64];
    formatDateStr(state, false, date_str, sizeof(date_str));
    out.fmt(
        "%s\033[1;36mEvery %u.0s: \033[1;37m%-32s\033[0m  \033[1;32m%s:\033[0m %s\033[K\r\n\033[K\r\n",
        clear_screen ? "\033[2J\033[H" : "\033[H",
        static_cast<unsigned>(watch_interval_sec_ > 0 ? watch_interval_sec_ : 2),
        watch_cmd_, state.hostname, date_str);

    if (exec_depth_ >= MAX_EXEC_DEPTH) {
        out.putStr("watch: max execution depth (4) exceeded\r\n");
    } else {
        ExecDepthGuard guard(exec_depth_);
        char cmd_out[1024];
        BufWriter sub(cmd_out, sizeof(cmd_out), false);
        SessionMode saved_mode = mode_;
        mode_ = SessionMode::SHELL_CLI;
        executeCommandLineTo(state, vfs, watch_cmd_, sub);
        mode_ = saved_mode;
        out.putStr(cmd_out);
    }

    out.putStr("\033[K\r\n\033[7m [watch] Press 'q' or Ctrl+C to exit live watch \033[0m\033[K");
}

void ShellSession::renderMonitorTick(const SystemState& state, char* out_buf, size_t max_len) const {
    BufWriter out(out_buf, max_len, false);
    if (mode_ == SessionMode::TOP_VIEW) {
        renderTopTo(state, false, false, out);
    } else if (mode_ == SessionMode::HTOP_VIEW) {
        renderTopTo(state, true, false, out);
    }
}

void ShellSession::renderMonitorTick(SystemState& state, VirtualFs& vfs, char* out_buf, size_t max_len) {
    BufWriter out(out_buf, max_len, false);
    if (mode_ == SessionMode::TOP_VIEW) {
        renderTopTo(state, false, false, out);
    } else if (mode_ == SessionMode::HTOP_VIEW) {
        renderTopTo(state, true, false, out);
    } else if (mode_ == SessionMode::WATCH_VIEW) {
        renderWatchFrameTo(state, vfs, false, out);
    }
}

bool ShellSession::handleMonitorKey(SystemState& state, VirtualFs& vfs, char ch, BufWriter& out) {
    if (ch == 'q' || ch == 'Q' || ch == 0x03) {
        mode_ = prev_mode_;
        if (mode_ == SessionMode::MENU_MAIN) {
            snprintf(menu_notice_, sizeof(menu_notice_), "Exited live monitor.");
            renderMenuTo(state, out);
        } else {
            mode_ = SessionMode::SHELL_CLI;
            out.putStr("\r\n");
            renderPromptTo(state, out);
        }
        return true;
    }
    if (mode_ == SessionMode::WATCH_VIEW) {
        renderWatchFrameTo(state, vfs, false, out);
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
            renderMenuTo(state, out);
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
        renderTopTo(state, mode_ == SessionMode::HTOP_VIEW, true, out);
        return true;
    }
    renderTopTo(state, mode_ == SessionMode::HTOP_VIEW, false, out);
    return true;
}

bool ShellSession::handleMenuKey(SystemState& state, VirtualFs& vfs, char ch, BufWriter& out) {
    if (ch == '\r' || ch == '\n' || ch == 0x03) {
        renderMenuTo(state, out);
        return true;
    }

    switch (ch) {
        case '1': {
            uint8_t next = (static_cast<uint8_t>(state.screen_mode) + 1) %
                           static_cast<uint8_t>(ScreenMode::COUNT);
            state.screen_mode = static_cast<ScreenMode>(next);
            state.markDirty(DIRTY_FULL);
            snprintf(menu_notice_, sizeof(menu_notice_), "Switched LCD mode to: %s",
                     screenModeLabel(state.screen_mode));
            renderMenuTo(state, out);
            return true;
        }
        case '2': {
            mode_ = SessionMode::MENU_PROMPT_HEAD;
            line_len_ = 0;
            out.fmt("\r\n\033[1;33mEnter new Screen Headline\033[0m (current: \"%s\"): ", state.headline);
            return true;
        }
        case '3': {
            uint8_t next = (static_cast<uint8_t>(state.theme) + 1) %
                           static_cast<uint8_t>(ThemeId::COUNT);
            state.theme = static_cast<ThemeId>(next);
            state.markDirty(DIRTY_FULL);
            snprintf(menu_notice_, sizeof(menu_notice_), "Changed LCD theme to: %s",
                     themeName(state.theme));
            renderMenuTo(state, out);
            return true;
        }
        case '4': {
            mode_ = SessionMode::MENU_PROMPT_BL;
            line_len_ = 0;
            out.fmt("\r\n\033[1;33mEnter Backlight Brightness [0-100]\033[0m (current: %u%%): ",
                    state.backlight_pct);
            return true;
        }
        case '+':
        case '=': {
            int bl = static_cast<int>(state.backlight_pct) + 10;
            if (bl > 100) bl = 100;
            state.backlight_pct = static_cast<uint8_t>(bl);
            state.markDirty(DIRTY_HEADER);
            snprintf(menu_notice_, sizeof(menu_notice_), "Backlight increased to %u%%",
                     state.backlight_pct);
            renderMenuTo(state, out);
            return true;
        }
        case '-':
        case '_': {
            int bl = static_cast<int>(state.backlight_pct) - 10;
            if (bl < 0) bl = 0;
            state.backlight_pct = static_cast<uint8_t>(bl);
            state.markDirty(DIRTY_HEADER);
            snprintf(menu_notice_, sizeof(menu_notice_), "Backlight decreased to %u%%",
                     state.backlight_pct);
            renderMenuTo(state, out);
            return true;
        }
        case '5': {
            state.landscape_flipped = !state.landscape_flipped;
            state.markDirty(DIRTY_FULL);
            snprintf(menu_notice_, sizeof(menu_notice_), "180 deg Landscape Flip: %s",
                     state.landscape_flipped ? "ENABLED" : "DISABLED");
            renderMenuTo(state, out);
            return true;
        }
        case '6': {
            mode_ = SessionMode::MENU_PROMPT_RGB;
            line_len_ = 0;
            out.putStr("\r\n\033[1;33mEnter RGB LED command\033[0m (off | rainbow | pulse | ssh | R G B): ");
            return true;
        }
        case '7': {
            state.screen_mode = ScreenMode::FB_CONSOLE;
            state.markDirty(DIRTY_FULL);
            mode_ = SessionMode::MENU_TTY0_MIRROR;
            line_len_ = 0;
            out.putStr(
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
            out.fmt(
                "\r\n\033[1;36m=== Wi-Fi Configuration ===\033[0m\r\n"
                "Current STA: %s (SSID: %s, IP: %s, RSSI: %d dBm)\r\n"
                "Fallback AP: %s (SSID: %s, IP: %s)\r\n",
                state.wifi_sta_connected ? "CONNECTED" : "DISCONNECTED",
                state.wifi_ssid[0] ? state.wifi_ssid : "<none>",
                state.wifi_ip, static_cast<int>(state.wifi_rssi),
                state.wifi_ap_enabled ? "ACTIVE" : "OFF",
                state.wifi_ap_ssid, state.wifi_ap_ip);
            if (state.wifi_scan_count > 0) {
                out.putStr("Recent 2.4 GHz Networks:\r\n");
                for (size_t i = 0; i < state.wifi_scan_count; ++i) {
                    out.fmt("  - %-24s (%4d dBm, ch %u)\r\n",
                            state.wifi_scan[i].ssid,
                            static_cast<int>(state.wifi_scan[i].rssi),
                            state.wifi_scan[i].channel);
                }
            }
            out.putStr("\033[1;33mEnter Wi-Fi SSID to join\033[0m (or press Enter to cancel): ");
            return true;
        }
        case '9': {
            char buf[512];
            out.putStr("\r\n\033[1;36m=== /proc/version ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/version", buf, sizeof(buf))) {
                out.putWithCrLf(buf);
            }
            out.putStr("\033[1;36m=== /proc/cpuinfo ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/cpuinfo", buf, sizeof(buf))) {
                out.putWithCrLf(buf);
            }
            out.putStr("\033[1;36m=== /proc/meminfo ===\033[0m\r\n");
            if (vfs.readFile(state, "/proc/meminfo", buf, sizeof(buf))) {
                out.putWithCrLf(buf);
            }
            out.putStr("\033[1;36m=== Thermal & Hardware Sensors ===\033[0m\r\n");
            out.fmt(
                "Die Temp: +%d.%d C | BOOT Button Presses: %u | HTTP(:80) Reqs: %u | Syslog(:514) Pkts: %u\r\n"
                "\r\n\033[1;33mPress Enter to return to menu...\033[0m",
                state.cpu_temp_deci_c / 10, abs(state.cpu_temp_deci_c % 10),
                static_cast<unsigned>(state.button_press_count),
                static_cast<unsigned>(state.http_requests),
                static_cast<unsigned>(state.syslog_packets));
            return true;
        }
        case 'm':
        case 'M': {
            if (state.screen_mode == ScreenMode::MONITOR) {
                state.screen_mode = ScreenMode::OS_DASHBOARD;
                snprintf(menu_notice_, sizeof(menu_notice_), "Returned LCD to OS Dashboard.");
            } else {
                state.screen_mode = ScreenMode::MONITOR;
                snprintf(menu_notice_, sizeof(menu_notice_), "Activated On-LCD Graphical htop Telemetry Mode.");
            }
            state.markDirty(DIRTY_FULL);
            renderMenuTo(state, out);
            return true;
        }
        case 'k':
        case 'K': {
            mode_ = SessionMode::MENU_PROMPT_TICKER;
            line_len_ = 0;
            out.fmt("\r\n\033[1;33mEnter Scrolling Ticker Text\033[0m (or 'off' to disable, current: %s): ",
                    state.ticker_enabled ? state.ticker_text : "off");
            return true;
        }
        case 'a':
        case 'A': {
            mode_ = SessionMode::MENU_PROMPT_ALERT;
            line_len_ = 0;
            out.fmt("\r\n\033[1;31mEnter Alert Banner Message\033[0m (or 'off' to clear, current: %s): ",
                    state.alert_active ? state.alert_text : "off");
            return true;
        }
        case ' ': {
            snprintf(menu_notice_, sizeof(menu_notice_), "Refreshed menu.");
            renderMenuTo(state, out);
            return true;
        }
        case 't':
        case 'T': {
            prev_mode_ = SessionMode::MENU_MAIN;
            mode_ = SessionMode::HTOP_VIEW;
            renderTopTo(state, true, true, out);
            return true;
        }
        case 's':
        case 'S': {
            mode_ = SessionMode::SHELL_CLI;
            line_len_ = 0;
            out.putStr("\r\n\033[1;32mEntering C6-MicroLinux POSIX Shell (c6-sh).\033[0m Type \033[1;33mmenu\033[0m to return to TUI.\r\n");
            renderPromptTo(state, out);
            return true;
        }
        case 'q':
        case 'Q': {
            out.putStr("\r\nClosing session. Goodbye!\r\n");
            return false;
        }
        default:
            snprintf(menu_notice_, sizeof(menu_notice_),
                     "Unknown key '%c'. Choose [1-9], [+/-], [m] LCD-htop, [k] ticker, [a] alert, [t] htop, [s] shell.",
                     isprint(static_cast<unsigned char>(ch)) ? ch : '?');
            renderMenuTo(state, out);
            return true;
    }
}

bool ShellSession::consumeBytes(SystemState& state, VirtualFs& vfs,
                                const char* data, size_t len,
                                char* out_buf, size_t max_len) {
    BufWriter out(out_buf, max_len, false);
    if (!data || len == 0) return true;

    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];

        if (ch == '\n' && i > 0 && data[i - 1] == '\r') {
            continue;
        }

        if (isInteractiveMonitor()) {
            if (ch == '\033') {
                if (i + 4 < len && data[i + 1] == '[' && data[i + 2] == '2' &&
                    data[i + 3] == '1' && data[i + 4] == '~') {
                    i += 4;
                    handleMonitorKey(state, vfs, 'q', out);
                    continue;
                }
                while (i + 1 < len && !isalpha(static_cast<unsigned char>(data[i + 1])) &&
                       data[i + 1] != '~') {
                    ++i;
                }
                if (i + 1 < len) ++i;
                continue;
            }
            if (!handleMonitorKey(state, vfs, ch, out)) {
                return false;
            }
            continue;
        }

        if (mode_ == SessionMode::MENU_MAIN) {
            if (ch == '\033') {
                if (i + 2 < len && data[i + 1] == '[') {
                    i += 2;
                }
                continue;
            }
            if (!handleMenuKey(state, vfs, ch, out)) {
                return false;
            }
            continue;
        }

        if (ch == 0x03) {
            line_len_ = 0;
            line_buf_[0] = '\0';
            out.putStr("^C\r\n");
            if (mode_ == SessionMode::EDIT_VIEW) {
                freeEditBuffer();
                mode_ = SessionMode::SHELL_CLI;
                renderPromptTo(state, out);
            } else if (mode_ != SessionMode::SHELL_CLI) {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Cancelled prompt.");
                renderMenuTo(state, out);
            } else {
                renderPromptTo(state, out);
            }
            continue;
        }

        if (ch == 0x04 && line_len_ == 0) {
            freeEditBuffer();
            out.putStr("exit\r\n");
            return false;
        }

        if (ch == 0x7F || ch == 0x08) {
            if (line_len_ > 0) {
                --line_len_;
                line_buf_[line_len_] = '\0';
                out.putStr("\b \b");
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            out.putStr("\r\n");
            line_buf_[line_len_] = '\0';
            char completed[192];
            strncpy(completed, line_buf_, sizeof(completed) - 1);
            completed[sizeof(completed) - 1] = '\0';
            line_len_ = 0;
            line_buf_[0] = '\0';

            if (!handleLineComplete(state, vfs, completed, out)) {
                return false;
            }
            continue;
        }

        if (isprint(static_cast<unsigned char>(ch)) && line_len_ + 1 < sizeof(line_buf_)) {
            line_buf_[line_len_++] = ch;
            line_buf_[line_len_] = '\0';
            out.putChar(mode_ == SessionMode::MENU_WIFI_PASS ? '*' : ch);
        }
    }
    return true;
}

bool ShellSession::handleLineComplete(SystemState& state, VirtualFs& vfs,
                                      const char* line, BufWriter& out) {
    switch (mode_) {
        case SessionMode::MENU_PROMPT_HEAD: {
            if (line[0] != '\0') {
                strncpy(state.headline, line, sizeof(state.headline) - 1);
                state.headline[sizeof(state.headline) - 1] = '\0';
                state.markDirty(DIRTY_CARDS);
            }
            mode_ = SessionMode::MENU_PROMPT_SUB;
            out.fmt("\033[1;33mEnter new Screen Subtitle\033[0m (current: \"%s\", Enter to keep): ",
                    state.subtitle);
            return true;
        }
        case SessionMode::MENU_PROMPT_SUB: {
            if (line[0] != '\0') {
                strncpy(state.subtitle, line, sizeof(state.subtitle) - 1);
                state.subtitle[sizeof(state.subtitle) - 1] = '\0';
            }
            state.markDirty(DIRTY_CARDS);
            mode_ = SessionMode::MENU_MAIN;
            snprintf(menu_notice_, sizeof(menu_notice_), "Updated LCD text: \"%s\"", state.headline);
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::MENU_PROMPT_BL: {
            if (line[0] != '\0') {
                int val = atoi(line);
                if (val < 0) val = 0;
                if (val > 100) val = 100;
                state.backlight_pct = static_cast<uint8_t>(val);
                state.markDirty(DIRTY_HEADER);
                snprintf(menu_notice_, sizeof(menu_notice_), "Backlight set to %u%%", state.backlight_pct);
            }
            mode_ = SessionMode::MENU_MAIN;
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::MENU_PROMPT_RGB: {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "screen led %s", line);
            char tmp[256];
            BufWriter dummy(tmp, sizeof(tmp), false);
            executeSingleCommand(state, vfs, cmd, dummy);
            snprintf(menu_notice_, sizeof(menu_notice_), "RGB LED updated (%s)", ledModeName(state.led_mode));
            mode_ = SessionMode::MENU_MAIN;
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::MENU_TTY0_MIRROR: {
            if (strcmp(line, ".exit") == 0 || strcmp(line, "exit") == 0) {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Exited live LCD tty0 mirror.");
                renderMenuTo(state, out);
                return true;
            }
            if (strcmp(line, ".clear") == 0) {
                state.clearFbConsole();
                out.putStr("[LCD cleared]\r\n\033[1;32mtty0>\033[0m ");
                return true;
            }
            state.appendFbConsoleLine(line);
            out.putStr("\033[1;32mtty0>\033[0m ");
            return true;
        }
        case SessionMode::MENU_WIFI_SSID: {
            if (line[0] == '\0') {
                mode_ = SessionMode::MENU_MAIN;
                snprintf(menu_notice_, sizeof(menu_notice_), "Wi-Fi setup cancelled.");
                renderMenuTo(state, out);
                return true;
            }
            strncpy(pending_ssid_, line, sizeof(pending_ssid_) - 1);
            pending_ssid_[sizeof(pending_ssid_) - 1] = '\0';
            mode_ = SessionMode::MENU_WIFI_PASS;
            out.fmt("\033[1;33mEnter Password for \"%s\"\033[0m (Enter for open network): ",
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
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::MENU_PROMPT_TICKER: {
            if (line[0] != '\0') {
                vfs.writeFile(state, "/sys/class/graphics/fb0/ticker", line, false);
                snprintf(menu_notice_, sizeof(menu_notice_), "Ticker %s",
                         state.ticker_enabled ? "enabled" : "disabled");
            }
            mode_ = SessionMode::MENU_MAIN;
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::MENU_PROMPT_ALERT: {
            if (line[0] != '\0') {
                vfs.writeFile(state, "/sys/class/graphics/fb0/alert", line, false);
                snprintf(menu_notice_, sizeof(menu_notice_), "Alert %s",
                         state.alert_active ? "ACTIVATED" : "CLEARED");
            }
            mode_ = SessionMode::MENU_MAIN;
            renderMenuTo(state, out);
            return true;
        }
        case SessionMode::EDIT_VIEW: {
            if (!ensureEditBuffer()) {
                mode_ = SessionMode::SHELL_CLI;
                out.putStr("edit: out of memory\r\n");
                renderPromptTo(state, out);
                return true;
            }
            if (strcmp(line, ".save") == 0 || strcmp(line, ".wq") == 0 || strcmp(line, ".exit") == 0 ||
                strcmp(line, ":wq") == 0 || strcmp(line, ":w") == 0 || strcmp(line, ":x") == 0) {
                vfs.writeFile(state, edit_path_, edit_buf_, false);
                if (strcmp(edit_path_, "/etc/crontab") == 0) {
                    syncCronFromVfs(state, vfs);
                }
                unsigned saved_len = static_cast<unsigned>(strlen(edit_buf_));
                freeEditBuffer();
                mode_ = SessionMode::SHELL_CLI;
                out.fmt("\033[1;32m[Saved %u bytes to %s]\033[0m\r\n", saved_len, edit_path_);
                renderPromptTo(state, out);
                return true;
            }
            if (strcmp(line, ".abort") == 0 || strcmp(line, ".q!") == 0 ||
                strcmp(line, ":q!") == 0 || strcmp(line, ":q") == 0) {
                freeEditBuffer();
                mode_ = SessionMode::SHELL_CLI;
                out.fmt("\033[1;33m[Discarded edits to %s]\033[0m\r\n", edit_path_);
                renderPromptTo(state, out);
                return true;
            }
            if (strcmp(line, ".clear") == 0 || strcmp(line, ":clear") == 0) {
                edit_buf_[0] = '\0';
                out.putStr("[Buffer cleared]\r\n\033[1;36medit>\033[0m ");
                return true;
            }
            if (strcmp(line, ".show") == 0 || strcmp(line, ":show") == 0) {
                out.fmt("\033[1;36m--- %s ---\033[0m\r\n", edit_path_);
                out.putWithCrLf(edit_buf_);
                out.putStr("\033[1;36m----------------\033[0m\r\n\033[1;36medit>\033[0m ");
                return true;
            }
            size_t cur = strlen(edit_buf_);
            size_t add = strlen(line);
            if (cur + add + 2 < EDIT_HEAP_CAPACITY) {
                memcpy(edit_buf_ + cur, line, add);
                edit_buf_[cur + add] = '\n';
                edit_buf_[cur + add + 1] = '\0';
            }
            out.putStr("\033[1;36medit>\033[0m ");
            return true;
        }
        case SessionMode::SHELL_CLI:
        default: {
            if (!executeCommandLineTo(state, vfs, line, out)) {
                return false;
            }
            if (mode_ == SessionMode::SHELL_CLI) {
                renderPromptTo(state, out);
            }
            return true;
        }
    }
}

void ShellSession::formatDateStr(const SystemState& state, bool force_utc, char* out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;
    time_t now_sec = static_cast<time_t>(state.epoch_offset_sec + state.uptime_sec);
    const char* posix_rule = force_utc ? "UTC0" :
                             (state.tz_posix[0] ? state.tz_posix : "EST5EDT,M3.2.0,M11.1.0");
    setenv("TZ", posix_rule, 1);
    tzset();

    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    if (force_utc) {
        gmtime_r(&now_sec, &tm_val);
        char prefix[40];
        strftime(prefix, sizeof(prefix), "%a %b %e %H:%M:%S", &tm_val);
        snprintf(out_buf, max_len, "%s UTC %d", prefix, tm_val.tm_year + 1900);
    } else {
        localtime_r(&now_sec, &tm_val);
        strftime(out_buf, max_len, "%a %b %e %H:%M:%S %Z %Y", &tm_val);
    }
}

void ShellSession::syncCronFromVfs(SystemState& state, const VirtualFs& vfs) {
    char content[VFS_MAX_FILE_SIZE];
    for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
        state.cron_jobs[i].active = false;
        state.cron_jobs[i].enabled = false;
        state.cron_jobs[i].interval_sec = 0;
        state.cron_jobs[i].last_run_sec = state.uptime_sec;
        state.cron_jobs[i].cmd[0] = '\0';
    }
    if (!vfs.readFile(state, "/etc/crontab", content, sizeof(content))) {
        return;
    }
    size_t slot = 0;
    char* saveptr = nullptr;
    char* line = strtok_r(content, "\n", &saveptr);
    while (line && slot < MAX_CRON_JOBS) {
        while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
        if (*line != '\0' && *line != '#') {
            unsigned long sec = 0;
            int offset = 0;
            if (sscanf(line, "%lu %n", &sec, &offset) >= 1 && sec > 0 && line[offset] != '\0') {
                state.cron_jobs[slot].active = true;
                state.cron_jobs[slot].enabled = true;
                state.cron_jobs[slot].interval_sec = static_cast<uint32_t>(sec);
                state.cron_jobs[slot].last_run_sec = state.uptime_sec;
                strncpy(state.cron_jobs[slot].cmd, line + offset, sizeof(state.cron_jobs[slot].cmd) - 1);
                state.cron_jobs[slot].cmd[sizeof(state.cron_jobs[slot].cmd) - 1] = '\0';
                size_t clen = strlen(state.cron_jobs[slot].cmd);
                while (clen > 0 && (state.cron_jobs[slot].cmd[clen - 1] == '\r' ||
                                    state.cron_jobs[slot].cmd[clen - 1] == ' ')) {
                    state.cron_jobs[slot].cmd[--clen] = '\0';
                }
                ++slot;
            }
        }
        line = strtok_r(nullptr, "\n", &saveptr);
    }
}

void ShellSession::syncVfsFromCron(SystemState& state, VirtualFs& vfs) {
    char buf[VFS_MAX_FILE_SIZE];
    BufWriter w(buf, sizeof(buf), false);
    w.putStr("# /etc/crontab: <interval_sec> <command>\n");
    for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
        if (state.cron_jobs[i].active && state.cron_jobs[i].interval_sec > 0) {
            w.fmt("%u %s\n",
                  static_cast<unsigned>(state.cron_jobs[i].interval_sec),
                  state.cron_jobs[i].cmd);
        }
    }
    vfs.writeFile(state, "/etc/crontab", buf, false);
}

void ShellSession::formatStatusJson(const SystemState& state, char* out_buf, size_t max_len) {
    snprintf(out_buf, max_len,
             "{\"os\":\"C6-MicroLinux\",\"kernel\":\"6.12.0-c6os-riscv32\","
             "\"hostname\":\"%s\",\"board\":\"Waveshare-ESP32-C6-LCD-1.47\","
             "\"tz\":\"%s\",\"login_shell\":\"%s\","
             "\"mode\":\"%s\",\"theme\":\"%s\",\"bl\":%u,\"flipped\":%s,"
             "\"headline\":\"%s\",\"subtitle\":\"%s\","
             "\"ticker_enabled\":%s,\"ticker\":\"%s\","
             "\"alert_active\":%s,\"alert\":\"%s\","
             "\"led_mode\":\"%s\",\"rgb\":[%u,%u,%u],"
             "\"temp_c\":%d.%d,\"cron_jobs\":%u,\"boot_btn_presses\":%u,"
             "\"wifi_connected\":%s,\"wifi_ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"ap_enabled\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
             "\"ble_enabled\":%s,\"sd_mounted\":%s,"
             "\"ssh_port\":%u,\"ssh_sessions\":%u,"
             "\"http_port\":%u,\"http_requests\":%u,\"syslog_port\":%u,\"syslog_packets\":%u,"
             "\"free_sram\":%u,\"total_sram\":%u,\"uptime_s\":%u}\r\n",
             state.hostname,
             state.tz_name,
             state.login_shell,
             screenModeName(state.screen_mode),
             themeName(state.theme),
             state.backlight_pct,
             state.landscape_flipped ? "true" : "false",
             state.headline,
             state.subtitle,
             state.ticker_enabled ? "true" : "false",
             state.ticker_text,
             state.alert_active ? "true" : "false",
             state.alert_text,
             ledModeName(state.led_mode),
             state.led_r, state.led_g, state.led_b,
             state.cpu_temp_deci_c / 10, abs(state.cpu_temp_deci_c % 10),
             static_cast<unsigned>(state.activeCronCount()),
             static_cast<unsigned>(state.button_press_count),
             state.wifi_sta_connected ? "true" : "false",
             state.wifi_ssid,
             state.wifi_ip,
             static_cast<int>(state.wifi_rssi),
             state.wifi_ap_enabled ? "true" : "false",
             state.wifi_ap_ssid,
             state.wifi_ap_ip,
             state.ble_enabled ? "true" : "false",
             state.sd_mounted ? "true" : "false",
             static_cast<unsigned>(state.ssh_port),
             static_cast<unsigned>(state.ssh_active_sessions),
             static_cast<unsigned>(state.http_port),
             static_cast<unsigned>(state.http_requests),
             static_cast<unsigned>(state.syslog_port),
             static_cast<unsigned>(state.syslog_packets),
             static_cast<unsigned>(state.free_sram_bytes),
             static_cast<unsigned>(state.total_sram_bytes),
             static_cast<unsigned>(state.uptime_sec));
}

bool ShellSession::executeCommandLine(SystemState& state, VirtualFs& vfs,
                                      const char* cmd_line,
                                      char* out_buf, size_t max_len) {
    BufWriter out(out_buf, max_len, true);
    return executeCommandLineTo(state, vfs, cmd_line, out);
}

bool ShellSession::executeCommandLineTo(SystemState& state, VirtualFs& vfs,
                                        const char* cmd_line, BufWriter& out) {
    if (!cmd_line) return true;
    char work[256];
    strncpy(work, cmd_line, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

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

        if (!executeSingleCommand(state, vfs, cursor, out)) {
            return false;
        }
        cursor = next_sep;
    }
    return true;
}

bool ShellSession::runScriptFile(SystemState& state, VirtualFs& vfs,
                                 const char* abs_path, BufWriter& out) {
    if (exec_depth_ >= MAX_EXEC_DEPTH) {
        out.putStr("sh: max execution depth (4) exceeded\r\n");
        return false;
    }
    char* script = static_cast<char*>(malloc(VFS_MAX_FILE_SIZE));
    if (!script) {
        out.putStr("sh: out of memory\r\n");
        return false;
    }
    if (!vfs.readFile(state, abs_path, script, VFS_MAX_FILE_SIZE)) {
        free(script);
        out.fmt("sh: can't open '%s': No such file\r\n", abs_path);
        return false;
    }

    ExecDepthGuard guard(exec_depth_);
    char* saveptr = nullptr;
    char* sline = strtok_r(script, "\n", &saveptr);
    while (sline != nullptr) {
        trimSpaces(sline);
        if (sline[0] != '\0' && sline[0] != '#') {
            if (!executeCommandLineTo(state, vfs, sline, out)) {
                free(script);
                return false;
            }
        }
        sline = strtok_r(nullptr, "\n", &saveptr);
    }
    free(script);
    return true;
}

bool ShellSession::executeSingleCommand(SystemState& state, VirtualFs& vfs,
                                        const char* raw_cmd, BufWriter& caller_out) {
    char cmd_copy[256];
    strncpy(cmd_copy, raw_cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';
    trimSpaces(cmd_copy);
    if (cmd_copy[0] == '\0' || cmd_copy[0] == '#') {
        return true;
    }

    char* redir_path = nullptr;
    bool redir_append = false;
    {
        bool in_q = false;
        char q_ch = 0;
        for (size_t i = 0; cmd_copy[i] != '\0'; ++i) {
            char c = cmd_copy[i];
            if (c == '"' || c == '\'') {
                if (!in_q) {
                    in_q = true;
                    q_ch = c;
                } else if (c == q_ch) {
                    in_q = false;
                }
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

    char* argv[16] = {nullptr};
    size_t argc = tokenizeArgs(cmd_copy, argv, 16);
    if (argc == 0) return true;

    const char* prog = argv[0];
    const HardwareOps* hw = getHardwareOps();

    char redir_buf[1024];
    BufWriter redir_writer(redir_buf, sizeof(redir_buf), false);
    BufWriter& out = redir_path ? redir_writer : caller_out;

    if (strcmp(prog, "s") == 0 || strcmp(prog, "true") == 0 || strcmp(prog, ":") == 0) {
        return true;
    }

    if (strcmp(prog, "exit") == 0 || strcmp(prog, "logout") == 0) {
        caller_out.putStr("logout\r\n");
        return false;
    }

    if (strcmp(prog, "menu") == 0 || strcmp(prog, "c6-menu") == 0 || strcmp(prog, "/bin/c6-menu") == 0) {
        mode_ = SessionMode::MENU_MAIN;
        snprintf(menu_notice_, sizeof(menu_notice_), "Returned to interactive Screen Control Menu.");
        renderMenuTo(state, caller_out);
        return true;
    }

    if (strcmp(prog, "clear") == 0) {
        out.putStr("\033[2J\033[H");
    } else if (strcmp(prog, "help") == 0) {
        out.putStr(
            "C6-MicroLinux (c6os 6.12.0-riscv32) Built-in Commands:\r\n"
            "  menu                         Launch interactive ANSI Screen Control Menu\r\n"
            "  screen <status|mode|text|ticker|alert|theme|bl|flip|bar|console|clear|led>\r\n"
            "  fbgrab [--ascii | <path.bmp|path.ppm>] Capture 320x172 LCD screenshot\r\n"
            "  fbi <logo|tux|matrix|grid>   Render graphical artwork on 320x172 LCD\r\n"
            "  epkg <install|remove|list|info> Manage EPK1 SHA-256 software packages\r\n"
            "  lua <-e <code> | <file.lua>> Run 32-bit Lua 5.4 script with c6/gpio/ble APIs\r\n"
            "  elf <binary.elf> [args...]   Execute sandboxed RV32IMAC ELF32 binary\r\n"
            "  gpio / pwm / adc / i2c       Control user GPIO, LEDC PWM, ADC (0..3), and I2C\r\n"
            "  ble <status|on|off|scan|adv> On-demand BLE 5.0 scanner & advertiser (0B idle)\r\n"
            "  wifi <status|scan|connect|ap> Configure 2.4 GHz Wi-Fi 6 STA and WPA2 SoftAP\r\n"
            "  tz [zone] | date | ntpdate   Configure POSIX timezone (/etc/timezone) & clock\r\n"
            "  chsh [menu|sh] | passwd      Set default SSH login shell or salted password\r\n"
            "  apitoken [rotate]            Inspect or rotate HTTP :80 /api/cmd bearer token\r\n"
            "  crontab | watch | edit | sh  Cron jobs, periodic monitor, editor & scripts\r\n"
            "  top, htop, ps, free, df, sensors, pinout, ping, netstat, dmesg, ls, cat, rm\r\n");
    } else if (strcmp(prog, "uname") == 0) {
        bool all = false, rel = false, mach = false, node = false;
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-a") == 0) all = true;
            else if (strcmp(argv[i], "-r") == 0) rel = true;
            else if (strcmp(argv[i], "-m") == 0) mach = true;
            else if (strcmp(argv[i], "-n") == 0) node = true;
        }
        if (all) {
            out.fmt("Linux %s 6.12.0-c6os-riscv32 #1 PREEMPT ESP32-C6FH4 rv32imac GNU/Linux\r\n",
                    state.hostname);
        } else if (rel) {
            out.putStr("6.12.0-c6os-riscv32\r\n");
        } else if (mach) {
            out.putStr("riscv32\r\n");
        } else if (node) {
            out.fmt("%s\r\n", state.hostname);
        } else {
            out.putStr("Linux\r\n");
        }
    } else if (strcmp(prog, "whoami") == 0) {
        out.putStr("root\r\n");
    } else if (strcmp(prog, "id") == 0) {
        out.putStr("uid=0(root) gid=0(root) groups=0(root)\r\n");
    } else if (strcmp(prog, "hostname") == 0) {
        if (argc >= 2) {
            vfs.writeFile(state, "/etc/hostname", argv[1], false);
        } else {
            out.fmt("%s\r\n", state.hostname);
        }
    } else if (strcmp(prog, "pwd") == 0) {
        out.fmt("%s\r\n", cwd_);
    } else if (strcmp(prog, "cd") == 0) {
        const char* dest = (argc >= 2) ? argv[1] : "/root";
        char norm[VFS_MAX_PATH_LEN];
        VirtualFs::normalizePath(cwd_, dest, norm, sizeof(norm));
        if (vfs.isDirectory(norm)) {
            strncpy(cwd_, norm, sizeof(cwd_) - 1);
            cwd_[sizeof(cwd_) - 1] = '\0';
        } else {
            out.fmt("cd: %s: No such directory\r\n", dest);
        }
    } else if (strcmp(prog, "ls") == 0) {
        const char* target = (argc >= 2 && argv[1][0] != '-') ? argv[1] :
                             (argc >= 3 ? argv[2] : cwd_);
        char norm[VFS_MAX_PATH_LEN];
        VirtualFs::normalizePath(cwd_, target, norm, sizeof(norm));
        char listing[512];
        if (vfs.listDirectory(norm, listing, sizeof(listing))) {
            out.putWithCrLf(listing);
        } else {
            out.fmt("ls: cannot access '%s': No such file or directory\r\n", target);
        }
    } else if (strcmp(prog, "cat") == 0) {
        if (argc < 2) {
            out.putStr("Usage: cat <path>\r\n");
        } else {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            char content[1024];
            if (vfs.readFile(state, norm, content, sizeof(content))) {
                out.putWithCrLf(content);
            } else {
                out.fmt("cat: %s: No such file or directory\r\n", argv[1]);
            }
        }
    } else if (strcmp(prog, "mkdir") == 0) {
        if (argc < 2) {
            out.putStr("Usage: mkdir <dir>\r\n");
        } else {
            const char* arg = (strcmp(argv[1], "-p") == 0 && argc >= 3) ? argv[2] : argv[1];
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, arg, norm, sizeof(norm));
            if (!vfs.mkdirPath(norm)) {
                out.fmt("mkdir: cannot create directory '%s'\r\n", arg);
            }
        }
    } else if (strcmp(prog, "rmdir") == 0) {
        if (argc < 2) {
            out.putStr("Usage: rmdir <dir>\r\n");
        } else {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            if (!vfs.rmdirPath(norm)) {
                out.fmt("rmdir: failed to remove '%s'\r\n", argv[1]);
            }
        }
    } else if (strcmp(prog, "mv") == 0) {
        if (argc < 3) {
            out.putStr("Usage: mv <src> <dst>\r\n");
        } else {
            char src[VFS_MAX_PATH_LEN], dst[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], src, sizeof(src));
            VirtualFs::normalizePath(cwd_, argv[2], dst, sizeof(dst));
            if (!vfs.renamePath(src, dst)) {
                out.fmt("mv: cannot move '%s' to '%s'\r\n", argv[1], argv[2]);
            }
        }
    } else if (strcmp(prog, "rm") == 0) {
        if (argc < 2) {
            out.putStr("Usage: rm <path>\r\n");
        } else {
            const char* arg = (argv[1][0] == '-' && argc >= 3) ? argv[2] : argv[1];
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, arg, norm, sizeof(norm));
            if (!vfs.removeFile(state, norm)) {
                out.fmt("rm: cannot remove '%s': Operation not permitted or no such file\r\n", arg);
            }
        }
    } else if (strcmp(prog, "echo") == 0) {
        for (size_t i = 1; i < argc; ++i) {
            out.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
        }
        out.putStr(redir_path ? "\n" : "\r\n");
    } else if (strcmp(prog, "uptime") == 0) {
        unsigned up = static_cast<unsigned>(state.uptime_sec);
        unsigned days = up / 86400U;
        unsigned hrs  = (up % 86400U) / 3600U;
        unsigned mins = (up % 3600U) / 60U;
        unsigned secs = up % 60U;
        out.fmt(" up %u days, %02u:%02u:%02u,  %u user(s),  load average: 0.%02u, 0.05, 0.01\r\n",
                days, hrs, mins, secs,
                static_cast<unsigned>(state.ssh_active_sessions > 0 ? state.ssh_active_sessions : 1),
                static_cast<unsigned>(state.cpu_load_pct));
    } else if (strcmp(prog, "free") == 0) {
        unsigned total = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_m = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned used = (total > free_m) ? (total - free_m) : 0U;
        out.fmt(
            "               total        used        free      shared  buff/cache   available\r\n"
            "Mem:          %6u      %6u      %6u           0          16      %6u (kB)\r\n"
            "Swap:              0           0           0\r\n",
            total, used, free_m, free_m);
    } else if (strcmp(prog, "df") == 0) {
        unsigned total = static_cast<unsigned>(state.rootfs_total_kb);
        unsigned used  = static_cast<unsigned>(state.rootfs_used_kb);
        unsigned avail = (total > used) ? (total - used) : 0U;
        unsigned pct   = (total > 0) ? ((used * 100U) / total) : 0U;
        out.fmt(
            "Filesystem      1K-blocks    Used Available Use%% Mounted on\r\n"
            "/dev/mtdblock4       %4u    %4u      %4u  %2u%% /\r\n"
            "proc                    0       0         0   0%% /proc\r\n"
            "sysfs                   0       0         0   0%% /sys\r\n"
            "devtmpfs               16       0        16   0%% /dev\r\n",
            total, used, avail, pct);
        if (state.sd_mounted) {
            unsigned sd_kb = static_cast<unsigned>(state.sd_total_mb * 1024U);
            unsigned sd_avail = (sd_kb > 128U) ? (sd_kb - 128U) : sd_kb;
            out.fmt("/dev/mmcblk0p1   %8u     128  %8u   1%% /mnt/sd\r\n", sd_kb, sd_avail);
        }
    } else if (strcmp(prog, "ps") == 0) {
        TaskInfoEntry live[16];
        uint32_t total_permille = 0;
        size_t n = (hw && hw->get_tasks) ? hw->get_tasks(live, 16, &total_permille) : 0;
        if (n > 0) {
            out.putStr("  PID USER      STK_FREE STAT %CPU COMMAND\r\n");
            for (size_t i = 0; i < n; ++i) {
                char cmd_label[36], tree_unused[36];
                formatLiveTaskCommand(live[i].name, state.ssh_port,
                                      cmd_label, sizeof(cmd_label),
                                      tree_unused, sizeof(tree_unused));
                out.fmt("%5u root      %7uB %-4c %2u.%u %s\r\n",
                        static_cast<unsigned>(live[i].pid),
                        static_cast<unsigned>(live[i].stack_free_bytes),
                        live[i].state_ch ? live[i].state_ch : 'S',
                        static_cast<unsigned>(live[i].cpu_permille / 10U),
                        static_cast<unsigned>(live[i].cpu_permille % 10U),
                        cmd_label);
            }
        } else {
            out.fmt(
                "  PID USER       VSZ STAT COMMAND\r\n"
                "    1 root      4096 S    /sbin/init\r\n"
                "    2 root         0 SW   [kthreadd]\r\n"
                "    6 root      2048 S    /sbin/buttond --gpio=9 [presses=%u]\r\n"
                "    8 root      6144 S    /sbin/lcdd --driver=st7789 --dma=80MHz\r\n"
                "   12 root      8192 S    /sbin/wpa_supplicant -i wlan0\r\n"
                "   16 root      4096 S    /usr/sbin/crond -f [jobs=%u]\r\n"
                "   18 root     12288 S    /usr/sbin/sshd -p %u [sessions=%u]\r\n"
                "   20 root      6144 S    /usr/sbin/httpd -p %u [reqs=%u]\r\n"
                "   22 root      4096 S    /sbin/syslogd -u %u [pkts=%u]\r\n"
                "   24 root      6144 S    /bin/c6-sh (/dev/ttyACM0)\r\n"
                "   31 root      6144 R    /bin/c6-sh (/dev/pts/0)\r\n",
                static_cast<unsigned>(state.button_press_count),
                static_cast<unsigned>(state.activeCronCount()),
                state.ssh_port, state.ssh_active_sessions,
                static_cast<unsigned>(state.http_port), static_cast<unsigned>(state.http_requests),
                static_cast<unsigned>(state.syslog_port), static_cast<unsigned>(state.syslog_packets));
        }
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
            renderTopTo(state, is_htop, false, out);
            out.putStr("\r\n");
        } else {
            prev_mode_ = mode_;
            mode_ = is_htop ? SessionMode::HTOP_VIEW : SessionMode::TOP_VIEW;
            renderTopTo(state, is_htop, true, out);
        }
    } else if (strcmp(prog, "lscpu") == 0 || strcmp(prog, "/bin/lscpu") == 0) {
        out.fmt(
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
            "  Flash Bus:           8 MiB QIO @ 80 MHz\r\n"
            "  LP Co-Processor:     RV32IMAC @ 20 MHz (LP-SRAM 16 KiB)\r\n",
            static_cast<unsigned>(state.cpu_freq_mhz));
    } else if (strcmp(prog, "vmstat") == 0 || strcmp(prog, "/bin/vmstat") == 0) {
        unsigned free_kb = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned us = static_cast<unsigned>((state.cpu_load_pct * 60U) / 100U);
        unsigned sy = static_cast<unsigned>(state.cpu_load_pct - us);
        unsigned id = (100U > state.cpu_load_pct) ? (100U - state.cpu_load_pct) : 0U;
        out.fmt(
            "procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----\r\n"
            " r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st\r\n"
            " %u  0      0 %6u     16     24    0    0     4     2  240  185 %2u %2u %2u  0  0\r\n",
            1U + static_cast<unsigned>(state.ssh_active_sessions),
            free_kb, us, sy, id);
    } else if (strcmp(prog, "neofetch") == 0 || strcmp(prog, "/bin/neofetch") == 0) {
        unsigned up = static_cast<unsigned>(state.uptime_sec);
        unsigned mins = (up % 3600U) / 60U;
        unsigned secs = up % 60U;
        unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
        out.fmt(
            "\033[1;36m       .---.        \033[1;32mroot\033[0m@\033[1;32m%s\033[0m\r\n"
            "\033[1;36m      /     \\       \033[0m-----------------\r\n"
            "\033[1;36m     | (\033[1;33mC6\033[1;36m)  |      \033[1;36mOS\033[0m: C6-MicroLinux (c6os) riscv32\r\n"
            "\033[1;36m      \\     /       \033[1;36mHost\033[0m: Waveshare ESP32-C6-LCD-1.47\r\n"
            "\033[1;36m       '---'        \033[1;36mKernel\033[0m: 6.12.0-c6os-riscv32\r\n"
            "\033[1;32m    RISC-V 32-bit   \033[1;36mUptime\033[0m: %um %us (TZ: %s)\r\n"
            "                    \033[1;36mDisplay\033[0m: 320x172 ST7789 DMA (%s, BL %u%%)\r\n"
            "                    \033[1;36mCPU\033[0m: ESP32-C6FH4 (1) @ %uMHz (+%d.%dC)\r\n"
            "                    \033[1;36mMemory\033[0m: %uKiB / %uKiB (%s)\r\n",
            state.hostname,
            mins, secs, state.tz_name,
            screenModeName(state.screen_mode),
            static_cast<unsigned>(state.backlight_pct),
            static_cast<unsigned>(state.cpu_freq_mhz),
            state.cpu_temp_deci_c / 10, abs(state.cpu_temp_deci_c % 10),
            used_kb, total_kb,
            state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip);
    } else if (strcmp(prog, "sensors") == 0 || strcmp(prog, "/bin/sensors") == 0) {
        int tc = state.cpu_temp_deci_c / 10;
        int td = abs(state.cpu_temp_deci_c % 10);
        out.fmt(
            "esp32c6_tsens-isa-0000\r\n"
            "Adapter: On-die SAR Thermal Sensor (/sys/class/thermal/thermal_zone0)\r\n"
            "die_temp:     \033[1;32m+%d.%dC\033[0m  (high = +85.0C, crit = +105.0C)\r\n"
            "\r\n"
            "st7789_backlight-pwm-0001\r\n"
            "Adapter: LEDC Timer0 PWM Driver (GPIO 22)\r\n"
            "brightness:   \033[1;33m%3u%%\033[0m     (max = 100%%, freq = 12.0 kHz)\r\n"
            "\r\n"
            "wlan0_rf-wifi6-0002\r\n"
            "Adapter: Espressif 802.11b/g/n/ax 2.4GHz Transceiver\r\n"
            "rssi:         \033[1;36m%4d dBm\033[0m (ssid = %s)\r\n"
            "ble5_radio:   %s\r\n",
            tc, td,
            static_cast<unsigned>(state.backlight_pct),
            static_cast<int>(state.wifi_rssi),
            state.wifi_sta_connected ? state.wifi_ssid : "disconnected",
            state.ble_enabled ? "ONLINE" : "OFF (0 B SRAM)");
    } else if (strcmp(prog, "crontab") == 0 || strcmp(prog, "/bin/crontab") == 0) {
        if (argc < 2 || strcmp(argv[1], "-l") == 0) {
            out.putStr("# Active Periodic Jobs (/etc/crontab):\r\n");
            size_t count = 0;
            for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
                if (state.cron_jobs[i].active) {
                    out.fmt("  [%u] every %4us : %s (runs=%u, last_run=%us)\r\n",
                            static_cast<unsigned>(i + 1),
                            static_cast<unsigned>(state.cron_jobs[i].interval_sec),
                            state.cron_jobs[i].cmd,
                            static_cast<unsigned>(state.cron_jobs[i].run_count),
                            static_cast<unsigned>(state.cron_jobs[i].last_run_sec));
                    ++count;
                }
            }
            if (count == 0) {
                out.putStr("  (no active cron jobs — use 'crontab -a <sec> <cmd>')\r\n");
            }
        } else if (strcmp(argv[1], "-r") == 0) {
            for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
                state.cron_jobs[i].active = false;
                state.cron_jobs[i].enabled = false;
                state.cron_jobs[i].cmd[0] = '\0';
            }
            syncVfsFromCron(state, vfs);
            state.markDirty(DIRTY_CARDS);
            out.putStr("crontab: all periodic jobs removed.\r\n");
        } else if ((strcmp(argv[1], "-a") == 0 || strcmp(argv[1], "-e") == 0) && argc >= 4) {
            unsigned long sec = strtoul(argv[2], nullptr, 10);
            if (sec < 1UL) sec = 1UL;
            int slot = -1;
            for (size_t i = 0; i < MAX_CRON_JOBS; ++i) {
                if (!state.cron_jobs[i].active) {
                    slot = static_cast<int>(i);
                    break;
                }
            }
            if (slot < 0) slot = static_cast<int>(MAX_CRON_JOBS - 1);
            char joined_cmd[64];
            BufWriter jw(joined_cmd, sizeof(joined_cmd), false);
            for (size_t i = 3; i < argc; ++i) {
                jw.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
            }
            state.cron_jobs[slot].active = true;
            state.cron_jobs[slot].enabled = true;
            state.cron_jobs[slot].interval_sec = static_cast<uint32_t>(sec);
            state.cron_jobs[slot].last_run_sec = state.uptime_sec;
            state.cron_jobs[slot].run_count = 0;
            strncpy(state.cron_jobs[slot].cmd, joined_cmd, sizeof(state.cron_jobs[slot].cmd) - 1);
            state.cron_jobs[slot].cmd[sizeof(state.cron_jobs[slot].cmd) - 1] = '\0';
            syncVfsFromCron(state, vfs);
            state.markDirty(DIRTY_CARDS);
            out.fmt("crontab: installed job #%d: every %lus -> %s\r\n",
                    slot + 1, sec, state.cron_jobs[slot].cmd);
        } else {
            out.putStr("Usage: crontab -l | crontab -a <interval_sec> <command> | crontab -r\r\n");
        }
    } else if (strcmp(prog, "watch") == 0 || strcmp(prog, "/bin/watch") == 0) {
        uint16_t interval = 2;
        bool batch = (redir_path != nullptr) || (mode_ != SessionMode::SHELL_CLI);
        size_t cmd_start = 1;
        while (cmd_start < argc && argv[cmd_start][0] == '-') {
            if (strcmp(argv[cmd_start], "-n") == 0 && cmd_start + 1 < argc) {
                int v = atoi(argv[cmd_start + 1]);
                if (v >= 1) interval = static_cast<uint16_t>(v);
                cmd_start += 2;
            } else if (strncmp(argv[cmd_start], "-n", 2) == 0 && argv[cmd_start][2] != '\0') {
                int v = atoi(argv[cmd_start] + 2);
                if (v >= 1) interval = static_cast<uint16_t>(v);
                ++cmd_start;
            } else if (strcmp(argv[cmd_start], "-b") == 0 || strcmp(argv[cmd_start], "-1") == 0) {
                batch = true;
                ++cmd_start;
            } else {
                ++cmd_start;
            }
        }
        if (cmd_start >= argc) {
            out.putStr("Usage: watch [-n <sec>] [-b] <command...>\r\n");
        } else {
            watch_interval_sec_ = interval;
            BufWriter wcmd(watch_cmd_, sizeof(watch_cmd_), false);
            for (size_t i = cmd_start; i < argc; ++i) {
                wcmd.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
            }
            if (batch) {
                renderWatchFrameTo(state, vfs, false, out);
                out.putStr("\r\n");
            } else {
                prev_mode_ = mode_;
                mode_ = SessionMode::WATCH_VIEW;
                renderWatchFrameTo(state, vfs, true, out);
            }
        }
    } else if (strcmp(prog, "fbgrab") == 0 || strcmp(prog, "/bin/fbgrab") == 0) {
        const char* save_path = nullptr;
        bool show_ascii = false;
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--ascii") == 0 || strcmp(argv[i], "-a") == 0) {
                show_ascii = true;
            } else if (argv[i][0] != '-') {
                save_path = argv[i];
            }
        }
        if (save_path) {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, save_path, norm, sizeof(norm));
            size_t written_bytes = 0;
            if (display::writeFramebufferImage(state, vfs, norm, &written_bytes)) {
                out.fmt("fbgrab: captured /dev/fb0 (320x172 RGB565, mode=%s) -> %s (%u bytes)\r\n",
                        screenModeName(state.screen_mode), norm, static_cast<unsigned>(written_bytes));
            } else {
                out.fmt("fbgrab: failed to write image to %s\r\n", norm);
            }
        } else {
            show_ascii = true;
        }
        if (show_ascii) {
            out.fmt("\033[1;36m+--- /dev/fb0 (320x172 ST7789 | mode=%-9s | theme=%-6s | BL=%3u%%) ---+\033[0m\r\n",
                    screenModeName(state.screen_mode), themeName(state.theme),
                    static_cast<unsigned>(state.backlight_pct));
            if (state.alert_active) {
                out.fmt("\033[1;37;41m| [!! ALERT OVERLAY !!] %-51.51s |\033[0m\r\n", state.alert_text);
            }
            out.fmt("| \033[1;36mroot@%-16s\033[0m                     \033[1;32mSSH root@%-15s [LIVE]\033[0m |\r\n"
                    "| SSID: %-16s  SRAM: %3u/%3uKB  Temp: +%d.%dC  Up: %6us         |\r\n"
                    "| Headline : \033[1;37m%-62.62s\033[0m |\r\n"
                    "| Subtitle : \033[1;36m%-62.62s\033[0m |\r\n",
                    state.hostname,
                    state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip,
                    state.wifi_sta_connected ? state.wifi_ssid : state.wifi_ap_ssid,
                    static_cast<unsigned>(state.free_sram_bytes / 1024U),
                    static_cast<unsigned>(state.total_sram_bytes / 1024U),
                    state.cpu_temp_deci_c / 10, abs(state.cpu_temp_deci_c % 10),
                    static_cast<unsigned>(state.uptime_sec),
                    state.headline, state.subtitle);
            if (state.ticker_enabled) {
                out.fmt("\033[1;33;44m| >>> MARQUEE: %-60.60s |\033[0m\r\n", state.ticker_text);
            }
            out.putStr("\033[1;36m+---------------------------------------------------------------------------+\033[0m\r\n");
        }
    } else if (strcmp(prog, "fbi") == 0 || strcmp(prog, "/bin/fbi") == 0) {
        if (argc >= 2) {
            if (strcmp(argv[1], "tux") == 0 || strcmp(argv[1], "1") == 0) {
                state.fbi_pattern = 1;
                strncpy(state.fbi_caption, "Tux the Linux Penguin (RV32)", sizeof(state.fbi_caption) - 1);
            } else if (strcmp(argv[1], "matrix") == 0 || strcmp(argv[1], "2") == 0) {
                state.fbi_pattern = 2;
                strncpy(state.fbi_caption, "C6-MicroLinux Digital Rain", sizeof(state.fbi_caption) - 1);
            } else if (strcmp(argv[1], "grid") == 0 || strcmp(argv[1], "bars") == 0 || strcmp(argv[1], "3") == 0) {
                state.fbi_pattern = 3;
                strncpy(state.fbi_caption, "ST7789 320x172 Geometry Grid", sizeof(state.fbi_caption) - 1);
            } else {
                state.fbi_pattern = 0;
                strncpy(state.fbi_caption, "ESP32-C6 RISC-V Cyber Emblem", sizeof(state.fbi_caption) - 1);
            }
            state.fbi_caption[sizeof(state.fbi_caption) - 1] = '\0';
            if (argc >= 3) {
                BufWriter cap(state.fbi_caption, sizeof(state.fbi_caption), false);
                for (size_t i = 2; i < argc; ++i) {
                    cap.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
                }
            }
        }
        state.screen_mode = ScreenMode::IMAGE_VIEW;
        state.markDirty(DIRTY_FULL);
        out.fmt("fbi: displaying pattern #%u (\"%s\") on /dev/fb0 (320x172 ST7789)\r\n",
                static_cast<unsigned>(state.fbi_pattern), state.fbi_caption);
    } else if (strcmp(prog, "pinout") == 0 || strcmp(prog, "/bin/pinout") == 0) {
        out.putStr(
            "\033[1;36m=== Waveshare ESP32-C6-LCD-1.47 Hardware Pinout (RV32IMAC) ===\033[0m\r\n"
            "                 \033[1;33m+---[ USB Type-C ]---+\033[0m\r\n"
            "            5V   | 1               22 | GND\r\n"
            "           GND   | 2               21 | 3V3 (OUT)\r\n"
            "          3V3    | 3               20 | \033[1;32mGPIO 0\033[0m  (ADC0 / XTAL_32K_P)\r\n"
            "  (ADC1)  \033[1;32mGPIO 1\033[0m | 4               19 | \033[1;32mGPIO 2\033[0m  (ADC2 / FSPIQ)\r\n"
            "  (ADC3)  \033[1;32mGPIO 3\033[0m | 5   \033[1;36mST7789 LCD\033[0m  18 | \033[1;32mGPIO 18\033[0m (I2C_SDA / GPIO)\r\n"
            "  (TF_CS) \033[1;31mGPIO 4\033[0m | 6   \033[1;36m320 x 172\033[0m   17 | \033[1;32mGPIO 19\033[0m (I2C_SCL / GPIO)\r\n"
            "  (MISO)  \033[1;31mGPIO 5\033[0m | 7   \033[1;36m80MHz DMA\033[0m   16 | \033[1;32mGPIO 20\033[0m (GPIO / PWM)\r\n"
            "  (MOSI)  \033[1;31mGPIO 6\033[0m | 8               15 | \033[1;31mGPIO 21\033[0m (LCD_RST - Reserved)\r\n"
            "  (SCLK)  \033[1;31mGPIO 7\033[0m | 9               14 | \033[1;31mGPIO 22\033[0m (LCD_BL PWM)\r\n"
            "  (USER)  \033[1;32mGPIO10\033[0m | 10              13 | \033[1;32mGPIO 23\033[0m (User Header / PWM)\r\n"
            "  (USER)  \033[1;32mGPIO11\033[0m | 11              12 | \033[1;31mGPIO 9\033[0m  (BOOT Button / buttond)\r\n"
            "                 \033[1;33m+--------------------+\033[0m\r\n"
            "  \033[1;31mReserved:\033[0m GPIO 4(SD_CS), 5/6/7(SPI2), 8(WS2812), 9(BOOT), 12/13(USB), 14/15/21/22(LCD)\r\n");
    } else if (strcmp(prog, "gpio") == 0 || strcmp(prog, "/bin/gpio") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            out.fmt(
                "GPIO Controller Status (ESP32-C6 IOMUX):\r\n"
                "  Output Mask : 0x%08x\r\n"
                "  Input Mask  : 0x%08x\r\n"
                "  BOOT Button : GPIO 9 (pull-up, %u presses logged by buttond)\r\n"
                "  User Pins   : GPIO 0, 1, 2, 3, 10, 11, 18, 19, 20, 23\r\n"
                "  Reserved    : GPIO 4, 5, 6, 7, 8, 9, 12, 13, 14, 15, 21, 22\r\n",
                static_cast<unsigned>(state.gpio_out_mask),
                static_cast<unsigned>(state.gpio_in_mask),
                static_cast<unsigned>(state.button_press_count));
        } else if (argc >= 3) {
            int pin = atoi(argv[2]);
            if (pin < 0 || pin > 23) {
                out.fmt("gpio: invalid pin %d (valid range: 0..23)\r\n", pin);
            } else if (isReservedBoardPin(pin) && strcmp(argv[1], "read") != 0) {
                out.fmt("gpio: pin %d is reserved for ST7789 SPI2 / board hardware protection\r\n", pin);
            } else if (strcmp(argv[1], "mode") == 0 && argc >= 4) {
                uint8_t act = 2;
                if (strcmp(argv[3], "out") == 0 || strcmp(argv[3], "output") == 0) act = 1;
                else if (strcmp(argv[3], "pullup") == 0) act = 3;
                else if (strcmp(argv[3], "pulldown") == 0) act = 4;
                state.gpio_cmd_pin = static_cast<int8_t>(pin);
                state.gpio_cmd_action = act;
                if (hw && hw->gpio_mode) {
                    hw->gpio_mode(pin, act);
                }
                out.fmt("OK GPIO %d MODE %s\r\n", pin, argv[3]);
            } else if (strcmp(argv[1], "write") == 0 && argc >= 4) {
                int val = (atoi(argv[3]) != 0 || strcmp(argv[3], "high") == 0 || strcmp(argv[3], "on") == 0) ? 1 : 0;
                if (val) state.gpio_out_mask |= (1UL << pin);
                else state.gpio_out_mask &= ~(1UL << pin);
                state.gpio_cmd_pin = static_cast<int8_t>(pin);
                state.gpio_cmd_action = 4;
                state.gpio_cmd_val = static_cast<uint8_t>(val);
                if (hw && hw->gpio_write) {
                    hw->gpio_write(pin, val);
                }
                out.fmt("OK GPIO %d WRITE %d\r\n", pin, val);
            } else if (strcmp(argv[1], "pwm") == 0 && argc >= 4) {
                int duty = atoi(argv[3]);
                if (duty < 0) duty = 0;
                if (duty > 255) duty = 255;
                uint32_t freq_hz = (argc >= 5) ? static_cast<uint32_t>(strtoul(argv[4], nullptr, 10)) : 5000U;
                if (freq_hz == 0) freq_hz = 5000U;
                state.gpio_cmd_pin = static_cast<int8_t>(pin);
                state.gpio_cmd_action = 5;
                state.gpio_cmd_val = static_cast<uint8_t>(duty);
                if (hw && hw->gpio_pwm) {
                    hw->gpio_pwm(pin, static_cast<uint8_t>(duty), freq_hz);
                }
                out.fmt("OK GPIO %d PWM %d/255 @ %uHz\r\n", pin, duty, static_cast<unsigned>(freq_hz));
            } else if (strcmp(argv[1], "read") == 0) {
                int bit = 0;
                if (hw && hw->gpio_read) {
                    bit = hw->gpio_read(pin) ? 1 : 0;
                    if (bit) state.gpio_in_mask |= (1UL << pin);
                    else state.gpio_in_mask &= ~(1UL << pin);
                } else {
                    bit = static_cast<int>(((state.gpio_in_mask >> pin) & 1U) |
                                           ((state.gpio_out_mask >> pin) & 1U));
                }
                out.fmt("GPIO %d = %d (%s)\r\n", pin, bit, bit ? "HIGH" : "LOW");
            } else {
                out.putStr("Usage: gpio <status | mode <pin> <in|out|pullup|pulldown> | read <pin> | write <pin> <0|1> | pwm <pin> <0..255> [hz]>\r\n");
            }
        } else {
            out.putStr("Usage: gpio <status | mode <pin> <in|out|pullup|pulldown> | read <pin> | write <pin> <0|1> | pwm <pin> <0..255> [hz]>\r\n");
        }
    } else if (strcmp(prog, "pwm") == 0 || strcmp(prog, "/bin/pwm") == 0) {
        if (argc < 3) {
            out.putStr("Usage: pwm <pin> <duty_0_255> [freq_hz]\r\n");
        } else {
            int pin = atoi(argv[1]);
            int duty = atoi(argv[2]);
            if (duty < 0) duty = 0;
            if (duty > 255) duty = 255;
            uint32_t freq_hz = (argc >= 4) ? static_cast<uint32_t>(strtoul(argv[3], nullptr, 10)) : 5000U;
            if (freq_hz == 0) freq_hz = 5000U;
            if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
                out.fmt("pwm: pin %d is invalid or reserved for ST7789 SPI2 / board hardware\r\n", pin);
            } else {
                state.gpio_cmd_pin = static_cast<int8_t>(pin);
                state.gpio_cmd_action = 5;
                state.gpio_cmd_val = static_cast<uint8_t>(duty);
                if (hw && hw->gpio_pwm) {
                    hw->gpio_pwm(pin, static_cast<uint8_t>(duty), freq_hz);
                }
                out.fmt("OK PWM GPIO %d = %d/255 @ %uHz\r\n", pin, duty, static_cast<unsigned>(freq_hz));
            }
        }
    } else if (strcmp(prog, "adc") == 0 || strcmp(prog, "/bin/adc") == 0) {
        if (argc < 2) {
            out.putStr("ADC1 Calibrated Channels (GPIO 0..3):\r\n");
            for (int p = 0; p <= 3; ++p) {
                int mv = (hw && hw->adc_read_mv) ? hw->adc_read_mv(p) : 0;
                if (mv < 0) mv = 0;
                out.fmt("  GPIO %d (ADC1_CH%d): %4d mV (%d.%03d V)\r\n", p, p, mv, mv / 1000, mv % 1000);
            }
        } else {
            int pin = atoi(argv[1]);
            if (pin < 0 || pin > 3) {
                out.fmt("adc: pin %d is not a user ADC1 header pin (use GPIO 0, 1, 2, or 3)\r\n", pin);
            } else {
                int mv = (hw && hw->adc_read_mv) ? hw->adc_read_mv(pin) : 0;
                if (mv < 0) mv = 0;
                out.fmt("ADC GPIO %d = %d mV (%d.%03d V)\r\n", pin, mv, mv / 1000, mv % 1000);
            }
        }
    } else if (strcmp(prog, "i2c") == 0 || strcmp(prog, "/bin/i2c") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            out.fmt("I2C0 Bus: SDA=GPIO %u, SCL=GPIO %u, Speed=%u Hz\r\n",
                    static_cast<unsigned>(state.i2c_sda),
                    static_cast<unsigned>(state.i2c_scl),
                    static_cast<unsigned>(state.i2c_hz));
        } else if (strcmp(argv[1], "bus") == 0 && argc >= 4) {
            int sda = atoi(argv[2]);
            int scl = atoi(argv[3]);
            uint32_t hz = (argc >= 5) ? static_cast<uint32_t>(strtoul(argv[4], nullptr, 10)) : 100000U;
            if (sda < 0 || sda > 23 || scl < 0 || scl > 23 || sda == scl ||
                isReservedBoardPin(sda) || isReservedBoardPin(scl)) {
                out.putStr("i2c: invalid or reserved SDA/SCL pins (use user pins: 0..3, 10, 11, 18, 19, 20, 23)\r\n");
            } else {
                state.i2c_sda = static_cast<uint8_t>(sda);
                state.i2c_scl = static_cast<uint8_t>(scl);
                state.i2c_hz = (hz > 0) ? hz : 100000U;
                int rc = (hw && hw->i2c_init) ? hw->i2c_init(state.i2c_sda, state.i2c_scl, state.i2c_hz) : 0;
                if (rc == 0) {
                    out.fmt("OK I2C BUS SDA=%u SCL=%u HZ=%u\r\n",
                            state.i2c_sda, state.i2c_scl, static_cast<unsigned>(state.i2c_hz));
                } else {
                    out.fmt("i2c: controller init failed (%d)\r\n", rc);
                }
            }
        } else if (strcmp(argv[1], "scan") == 0) {
            uint8_t addrs[32];
            int found = (hw && hw->i2c_scan) ? hw->i2c_scan(addrs, 32) : 0;
            out.fmt("I2C scan on SDA=%u SCL=%u (%u Hz): %d device(s) found\r\n",
                    state.i2c_sda, state.i2c_scl, static_cast<unsigned>(state.i2c_hz),
                    found > 0 ? found : 0);
            for (int i = 0; i < found && i < 32; ++i) {
                out.fmt("  - 0x%02x\r\n", addrs[i]);
            }
        } else if (strcmp(argv[1], "get") == 0 && argc >= 4) {
            uint8_t addr = static_cast<uint8_t>(strtoul(argv[2], nullptr, 0) & 0x7FU);
            uint8_t reg  = static_cast<uint8_t>(strtoul(argv[3], nullptr, 0) & 0xFFU);
            size_t len   = (argc >= 5) ? static_cast<size_t>(strtoul(argv[4], nullptr, 0)) : 1U;
            if (len < 1) len = 1;
            if (len > 16) len = 16;
            uint8_t buf[16] = {0};
            int rc = (hw && hw->i2c_read_reg) ? hw->i2c_read_reg(addr, reg, buf, len) : -1;
            if (rc == 0) {
                out.fmt("I2C[0x%02x:0x%02x] =", addr, reg);
                for (size_t i = 0; i < len; ++i) out.fmt(" 0x%02x", buf[i]);
                out.putStr("\r\n");
            } else {
                out.fmt("i2c: NACK or bus error reading 0x%02x reg 0x%02x\r\n", addr, reg);
            }
        } else if (strcmp(argv[1], "set") == 0 && argc >= 5) {
            uint8_t addr = static_cast<uint8_t>(strtoul(argv[2], nullptr, 0) & 0x7FU);
            uint8_t reg  = static_cast<uint8_t>(strtoul(argv[3], nullptr, 0) & 0xFFU);
            uint8_t buf[12];
            size_t len = 0;
            for (size_t i = 4; i < argc && len < sizeof(buf); ++i) {
                buf[len++] = static_cast<uint8_t>(strtoul(argv[i], nullptr, 0) & 0xFFU);
            }
            int rc = (hw && hw->i2c_write_reg) ? hw->i2c_write_reg(addr, reg, buf, len) : -1;
            if (rc == 0) {
                out.fmt("OK I2C WRITE 0x%02x reg 0x%02x (%u bytes)\r\n",
                        addr, reg, static_cast<unsigned>(len));
            } else {
                out.fmt("i2c: NACK or bus error writing 0x%02x reg 0x%02x\r\n", addr, reg);
            }
        } else {
            out.putStr("Usage: i2c <status | bus <sda> <scl> [hz] | scan | get <addr> <reg> [len] | set <addr> <reg> <val...>>\r\n");
        }
    } else if (strcmp(prog, "ble") == 0 || strcmp(prog, "/bin/ble") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            out.fmt("BLE 5.0 Controller : %s\r\n"
                    "  Advertising      : %s (Name: %s)\r\n",
                    state.ble_enabled ? "ONLINE (NimBLE)" : "OFF (0 B idle SRAM)",
                    state.ble_advertising ? "ACTIVE" : "OFF",
                    state.ble_adv_name);
        } else if (strcmp(argv[1], "on") == 0) {
            int rc = (hw && hw->ble_enable) ? hw->ble_enable(true) : 0;
            if (rc == 0) {
                state.ble_enabled = true;
                out.putStr("OK BLE ON\r\n");
            } else {
                out.fmt("ble: failed to start BLE stack (%d)\r\n", rc);
            }
        } else if (strcmp(argv[1], "off") == 0) {
            if (hw && hw->ble_enable) {
                hw->ble_enable(false);
            }
            state.ble_enabled = false;
            state.ble_advertising = false;
            out.putStr("OK BLE OFF (controller memory released)\r\n");
        } else if (strcmp(argv[1], "scan") == 0) {
            uint32_t sec = (argc >= 3) ? static_cast<uint32_t>(strtoul(argv[2], nullptr, 10)) : 3U;
            if (sec < 1U) sec = 1U;
            if (sec > 10U) sec = 10U;
            if (!state.ble_enabled) {
                if (!hw || !hw->ble_enable || hw->ble_enable(true) == 0) {
                    state.ble_enabled = true;
                }
            }
            BleScanResult results[12];
            int count = (hw && hw->ble_scan) ? hw->ble_scan(sec * 1000U, results, 12) : 0;
            out.fmt("BLE 5.0 scan (%us): %d peripheral(s) found\r\n",
                    static_cast<unsigned>(sec), count > 0 ? count : 0);
            for (int i = 0; i < count && i < 12; ++i) {
                out.fmt("  [%2d] %-17s  %4d dBm  %s\r\n",
                        i + 1, results[i].addr, static_cast<int>(results[i].rssi),
                        results[i].name[0] ? results[i].name : "<unnamed>");
            }
        } else if (strcmp(argv[1], "adv") == 0 && argc >= 3) {
            bool en = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
            if (argc >= 4) {
                strncpy(state.ble_adv_name, argv[3], sizeof(state.ble_adv_name) - 1);
                state.ble_adv_name[sizeof(state.ble_adv_name) - 1] = '\0';
            }
            if (en && !state.ble_enabled) {
                if (!hw || !hw->ble_enable || hw->ble_enable(true) == 0) {
                    state.ble_enabled = true;
                }
            }
            int rc = (hw && hw->ble_advertise) ? hw->ble_advertise(en, state.ble_adv_name) : 0;
            if (rc == 0) {
                state.ble_advertising = en;
                out.fmt("OK BLE ADV %s \"%s\"\r\n", en ? "ON" : "OFF", state.ble_adv_name);
            } else {
                out.fmt("ble: advertising error (%d)\r\n", rc);
            }
        } else {
            out.putStr("Usage: ble <status | on | off | scan [sec] | adv <on|off> [name]>\r\n");
        }
    } else if (strcmp(prog, "tz") == 0 || strcmp(prog, "/bin/tz") == 0) {
        if (argc < 2) {
            char dstr[64];
            formatDateStr(state, false, dstr, sizeof(dstr));
            out.fmt("Timezone : %s (POSIX=%s)\r\nLocal    : %s\r\n",
                    state.tz_name, state.tz_posix, dstr);
        } else {
            resolvePosixTz(argv[1], state.tz_name, sizeof(state.tz_name),
                           state.tz_posix, sizeof(state.tz_posix));
            char tz_line[40];
            snprintf(tz_line, sizeof(tz_line), "%s\n", state.tz_name);
            vfs.writeFile(state, "/etc/timezone", tz_line, false);
            setenv("TZ", state.tz_posix, 1);
            tzset();
            char dstr[64];
            formatDateStr(state, false, dstr, sizeof(dstr));
            out.fmt("OK TZ %s (%s) -> %s\r\n", state.tz_name, state.tz_posix, dstr);
        }
    } else if (strcmp(prog, "chsh") == 0 || strcmp(prog, "/bin/chsh") == 0) {
        if (argc < 2) {
            out.fmt("Login shell for root: %s\r\n", state.login_shell);
        } else {
            const char* target_shell = nullptr;
            if (strcmp(argv[1], "menu") == 0 || strcmp(argv[1], "c6-menu") == 0 ||
                strcmp(argv[1], "/bin/c6-menu") == 0) {
                target_shell = "/bin/c6-menu";
            } else if (strcmp(argv[1], "sh") == 0 || strcmp(argv[1], "c6-sh") == 0 ||
                       strcmp(argv[1], "/bin/sh") == 0 || strcmp(argv[1], "/bin/c6-sh") == 0) {
                target_shell = "/bin/c6-sh";
            }
            if (!target_shell) {
                out.putStr("chsh: invalid shell (valid shells: /bin/c6-menu, /bin/c6-sh)\r\n");
            } else {
                strncpy(state.login_shell, target_shell, sizeof(state.login_shell) - 1);
                state.login_shell[sizeof(state.login_shell) - 1] = '\0';
                char pw_line[96];
                snprintf(pw_line, sizeof(pw_line), "root:x:0:0:root:/root:%s\n", state.login_shell);
                vfs.writeFile(state, "/etc/passwd", pw_line, false);
                state.fs_sync_requested = true;
                out.fmt("chsh: login shell changed to %s\r\n", state.login_shell);
            }
        }
    } else if (strcmp(prog, "apitoken") == 0 || strcmp(prog, "/bin/apitoken") == 0) {
        if (argc >= 2 && strcmp(argv[1], "rotate") == 0) {
            if (hw && hw->rotate_api_token) {
                hw->rotate_api_token(state.api_token);
            } else {
                uint8_t seed_buf[64];
                static uint32_t rot_seq = 1;
                ++rot_seq;
                memset(seed_buf, 0, sizeof(seed_buf));
                memcpy(seed_buf, state.api_token, 32);
                memcpy(seed_buf + 32, &state.uptime_sec, sizeof(state.uptime_sec));
                memcpy(seed_buf + 36, &rot_seq, sizeof(rot_seq));
                uint8_t digest[32];
                crypto::sha256(seed_buf, sizeof(seed_buf), digest);
                static const char HEX[] = "0123456789abcdef";
                for (int i = 0; i < 16; ++i) {
                    state.api_token[i * 2]     = HEX[(digest[i] >> 4) & 0x0F];
                    state.api_token[i * 2 + 1] = HEX[digest[i] & 0x0F];
                }
                state.api_token[32] = '\0';
            }
            state.fs_sync_requested = true;
            out.fmt("apitoken: rotated bearer token -> %s\r\n", state.api_token);
        } else {
            const char* ip_str = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
            out.fmt("HTTP API Token : %s\r\n"
                    "Example        : curl -H \"Authorization: Bearer %s\" \"http://%s/api/cmd?c=uname%%20-a\"\r\n",
                    state.api_token, state.api_token, ip_str);
        }
    } else if (strcmp(prog, "epkg") == 0 || strcmp(prog, "/bin/epkg") == 0) {
        if (argc >= 3 && strcmp(argv[1], "install") == 0) {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[2], norm, sizeof(norm));
            epkg::installPackage(state, vfs, norm, out);
        } else if (argc >= 3 && (strcmp(argv[1], "remove") == 0 || strcmp(argv[1], "rm") == 0)) {
            epkg::removePackage(state, vfs, argv[2], out);
        } else if (argc >= 2 && strcmp(argv[1], "list") == 0) {
            epkg::listPackages(vfs, out);
        } else if (argc >= 3 && strcmp(argv[1], "info") == 0) {
            epkg::packageInfo(state, vfs, argv[2], out);
        } else {
            out.putStr("Usage: epkg <install <file.epkg> | remove <pkg> | list | info <pkg>>\r\n");
        }
    } else if (strcmp(prog, "lua") == 0 || strcmp(prog, "/bin/lua") == 0) {
        if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
            const char* sub_argv[16] = {nullptr};
            int sub_argc = 0;
            sub_argv[sub_argc++] = "lua";
            for (size_t i = 3; i < argc && sub_argc < 16; ++i) {
                sub_argv[sub_argc++] = argv[i];
            }
            luavm::runChunk(state, vfs, "=(command line)", argv[2], sub_argc, sub_argv, out);
        } else if (argc >= 2) {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            const char* sub_argv[16] = {nullptr};
            int sub_argc = 0;
            for (size_t i = 1; i < argc && sub_argc < 16; ++i) {
                sub_argv[sub_argc++] = argv[i];
            }
            luavm::runFile(state, vfs, norm, sub_argc, sub_argv, out);
        } else {
            out.putStr("Usage: lua -e \"<code>\" | lua <script.lua> [args...]\r\n");
        }
    } else if (strcmp(prog, "elf") == 0 || strcmp(prog, "/bin/elf") == 0) {
        if (argc < 2) {
            out.putStr("Usage: elf <binary.elf> [args...]\r\n");
        } else {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            const char* sub_argv[16] = {nullptr};
            int sub_argc = 0;
            for (size_t i = 1; i < argc && sub_argc < 16; ++i) {
                sub_argv[sub_argc++] = argv[i];
            }
            elf::runElfFile(state, vfs, norm, sub_argc, sub_argv, out);
        }
    } else if (strcmp(prog, "date") == 0 || strcmp(prog, "/bin/date") == 0) {
        bool utc = false;
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--utc") == 0) {
                utc = true;
            } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
                uint32_t new_ep = static_cast<uint32_t>(strtoul(argv[i + 1], nullptr, 10));
                if (new_ep > 1000000000UL) {
                    state.epoch_offset_sec = new_ep - state.uptime_sec;
                }
                ++i;
            }
        }
        char dstr[64];
        formatDateStr(state, utc, dstr, sizeof(dstr));
        out.fmt("%s\r\n", dstr);
    } else if (strcmp(prog, "ntpdate") == 0 || strcmp(prog, "/bin/ntpdate") == 0) {
        const char* srv = (argc >= 2) ? argv[1] : "pool.ntp.org";
        state.ntp_sync_requested = true;
        state.ntp_synced = true;
        char dstr[64];
        formatDateStr(state, false, dstr, sizeof(dstr));
        out.fmt("ntpdate[44]: sync requested with %s (%s)\r\n", srv, dstr);
    } else if (strcmp(prog, "ping") == 0 || strcmp(prog, "/bin/ping") == 0) {
        const char* host = nullptr;
        int count = 3;
        for (size_t i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                count = atoi(argv[i + 1]);
                if (count < 1) count = 1;
                if (count > 5) count = 5;
                ++i;
            } else if (argv[i][0] != '-') {
                host = argv[i];
            }
        }
        if (!host) {
            out.putStr("Usage: ping [-c <count>] <destination>\r\n");
        } else if (hw && hw->icmp_ping) {
            out.fmt("PING %s: 56 data bytes\r\n", host);
            PingStats stats;
            memset(&stats, 0, sizeof(stats));
            int rc = hw->icmp_ping(host, static_cast<uint16_t>(count), 1000U, &stats);
            if (rc < 0) {
                out.fmt("ping: cannot resolve or reach %s\r\n", host);
            } else {
                for (uint16_t i = 0; i < stats.sent && i < 5; ++i) {
                    if (stats.rtts_us[i] > 0 || i < stats.received) {
                        out.fmt("64 bytes from %s: icmp_seq=%u ttl=64 time=%u.%02u ms\r\n",
                                host, static_cast<unsigned>(i + 1),
                                static_cast<unsigned>(stats.rtts_us[i] / 1000U),
                                static_cast<unsigned>((stats.rtts_us[i] % 1000U) / 10U));
                    } else {
                        out.fmt("Request timeout for icmp_seq %u\r\n", static_cast<unsigned>(i + 1));
                    }
                }
                unsigned loss = (stats.sent > 0)
                                    ? static_cast<unsigned>(((stats.sent - stats.received) * 100U) / stats.sent)
                                    : 100U;
                out.fmt("--- %s ping statistics ---\r\n"
                        "%u packets transmitted, %u packets received, %u%% packet loss\r\n",
                        host, static_cast<unsigned>(stats.sent),
                        static_cast<unsigned>(stats.received), loss);
            }
        } else {
            bool local = (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 ||
                          strcmp(host, state.wifi_ip) == 0 || strcmp(host, state.wifi_ap_ip) == 0);
            out.fmt("PING %s (%s): 56 data bytes\r\n", host, host);
            if (!local && !state.wifi_sta_connected) {
                out.putStr("ping: sendto: Network is unreachable (wlan0 disconnected)\r\n");
            } else {
                for (int seq = 1; seq <= count; ++seq) {
                    unsigned rtt_int = local ? 0U : (2U + ((state.uptime_sec + static_cast<unsigned>(seq)) % 4U));
                    unsigned rtt_dec = (state.uptime_sec * 7U + static_cast<unsigned>(seq) * 13U) % 100U;
                    out.fmt("64 bytes from %s: icmp_seq=%d ttl=64 time=%u.%02u ms\r\n",
                            host, seq, rtt_int, rtt_dec);
                }
                out.fmt("--- %s ping statistics ---\r\n"
                        "%d packets transmitted, %d packets received, 0%% packet loss\r\n",
                        host, count, count);
            }
        }
    } else if (strcmp(prog, "netstat") == 0 || strcmp(prog, "/bin/netstat") == 0 ||
               strcmp(prog, "ss") == 0 || strcmp(prog, "/bin/ss") == 0) {
        const char* local_ip = state.wifi_sta_connected ? state.wifi_ip : state.wifi_ap_ip;
        out.fmt(
            "Active Internet connections (servers and established)\r\n"
            "Proto Recv-Q Send-Q Local Address           Foreign Address         State       PID/Program\r\n"
            "tcp        0      0 0.0.0.0:%-15u 0.0.0.0:*               LISTEN      18/sshd\r\n"
            "tcp        0      0 0.0.0.0:%-15u 0.0.0.0:*               LISTEN      20/httpd\r\n"
            "udp        0      0 0.0.0.0:%-15u 0.0.0.0:*                           22/syslogd\r\n",
            static_cast<unsigned>(state.ssh_port),
            static_cast<unsigned>(state.http_port),
            static_cast<unsigned>(state.syslog_port));
        for (uint8_t s = 0; s < state.ssh_active_sessions; ++s) {
            out.fmt(
                "tcp        0      0 %-15s:%-7u %-15s:%-7u ESTABLISHED 18/sshd: root@pts/%u\r\n",
                local_ip, static_cast<unsigned>(state.ssh_port),
                state.ssh_client_ip[0] ? state.ssh_client_ip : "192.168.4.2",
                51200U + static_cast<unsigned>(s), static_cast<unsigned>(s));
        }
    } else if (strcmp(prog, "edit") == 0 || strcmp(prog, "/bin/edit") == 0 ||
               strcmp(prog, "nano") == 0 || strcmp(prog, "/bin/nano") == 0) {
        if (argc < 2) {
            out.putStr(
                "Usage: edit <path> [line1] [line2] ...\r\n"
                "  (Omit [line1] in interactive shell to open line editor)\r\n");
        } else {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            if (argc >= 3) {
                char body[VFS_MAX_FILE_SIZE];
                BufWriter bw(body, sizeof(body), false);
                for (size_t i = 2; i < argc; ++i) {
                    bw.fmt("%s\n", argv[i]);
                }
                if (vfs.writeFile(state, norm, body, false)) {
                    if (strcmp(norm, "/etc/crontab") == 0) {
                        syncCronFromVfs(state, vfs);
                    }
                    out.fmt("edit: wrote %u bytes to %s\r\n",
                            static_cast<unsigned>(bw.length()), norm);
                } else {
                    out.fmt("edit: cannot write to %s\r\n", norm);
                }
            } else if (!ensureEditBuffer()) {
                out.putStr("edit: out of memory\r\n");
            } else {
                strncpy(edit_path_, norm, sizeof(edit_path_) - 1);
                edit_path_[sizeof(edit_path_) - 1] = '\0';
                if (!vfs.readFile(state, edit_path_, edit_buf_, EDIT_HEAP_CAPACITY)) {
                    edit_buf_[0] = '\0';
                }
                if (mode_ == SessionMode::SHELL_CLI && !redir_path) {
                    mode_ = SessionMode::EDIT_VIEW;
                    out.fmt(
                        "\033[1;36m=== C6-MicroLinux Editor: %s ===\033[0m\r\n"
                        "Commands: \033[1;33m.show\033[0m (view) | \033[1;33m.clear\033[0m (empty) | \033[1;32m.save\033[0m (save & exit) | \033[1;31m.abort\033[0m (cancel)\r\n"
                        "--- Current Content ---\r\n",
                        edit_path_);
                    out.putWithCrLf(edit_buf_);
                    out.putStr("-----------------------\r\n\033[1;36medit>\033[0m ");
                } else {
                    out.putWithCrLf(edit_buf_);
                    freeEditBuffer();
                }
            }
        }
    } else if (strcmp(prog, "sh") == 0 || strcmp(prog, "/bin/sh") == 0 ||
               strcmp(prog, "c6-sh") == 0 || strcmp(prog, "/bin/c6-sh") == 0 ||
               strcmp(prog, "source") == 0) {
        if (argc < 2) {
            out.putStr("Usage: sh <script.sh> | sh -c \"<commands>\"\r\n");
        } else if (strcmp(argv[1], "-c") == 0 && argc >= 3) {
            if (exec_depth_ >= MAX_EXEC_DEPTH) {
                out.putStr("sh: max execution depth (4) exceeded\r\n");
            } else {
                ExecDepthGuard guard(exec_depth_);
                executeCommandLineTo(state, vfs, argv[2], out);
            }
        } else {
            char norm[VFS_MAX_PATH_LEN];
            VirtualFs::normalizePath(cwd_, argv[1], norm, sizeof(norm));
            runScriptFile(state, vfs, norm, out);
        }
    } else if (strcmp(prog, "logger") == 0) {
        if (argc < 2) {
            out.putStr("Usage: logger <message...>\r\n");
        } else {
            char msg[88];
            BufWriter mw(msg, sizeof(msg), false);
            mw.putStr("syslog: ");
            for (size_t i = 1; i < argc; ++i) {
                mw.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
            }
            state.appendDmesg(state.uptime_sec * 1000U, msg);
            state.syslog_packets++;
            out.fmt("OK LOGGER \"%s\"\r\n", msg);
        }
    } else if (strcmp(prog, "dmesg") == 0) {
        if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
            state.dmesg_head = 0;
            state.dmesg_count = 0;
        } else {
            for (size_t i = 0; i < state.dmesg_count; ++i) {
                out.fmt("%s\r\n", state.dmesgLineAt(i));
            }
        }
    } else if (strcmp(prog, "ip") == 0 || strcmp(prog, "ifconfig") == 0) {
        out.fmt(
            "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 1500\r\n"
            "    inet 127.0.0.1/8 scope host lo\r\n"
            "2: wlan0: <%s,BROADCAST,MULTICAST> mtu 1500\r\n"
            "    link/ether %s\r\n"
            "    inet %s/24 scope global wlan0 (SSID: %s, RSSI: %d dBm)\r\n"
            "3: ap0: <%s,BROADCAST,MULTICAST> mtu 1500\r\n"
            "    inet %s/24 scope global ap0 (SSID: %s, pinned=%s)\r\n",
            state.wifi_sta_connected ? "UP,LOWER_UP" : "NO-CARRIER",
            state.wifi_mac, state.wifi_ip,
            state.wifi_ssid[0] ? state.wifi_ssid : "none",
            static_cast<int>(state.wifi_rssi),
            state.wifi_ap_enabled ? "UP,LOWER_UP" : "DOWN",
            state.wifi_ap_ip, state.wifi_ap_ssid,
            state.wifi_ap_pinned ? "yes" : "no");
    } else if (strcmp(prog, "passwd") == 0) {
        if (argc < 2 || argv[1][0] == '\0') {
            out.putStr("Usage: passwd <new_password>\r\n");
        } else {
            uint8_t mix[48];
            memcpy(mix, state.pass_salt, 16);
            memcpy(mix + 16, &state.uptime_sec, sizeof(state.uptime_sec));
            size_t pw_len = strlen(argv[1]);
            if (pw_len > 28) pw_len = 28;
            memset(mix + 20, 0x5A, 28);
            memcpy(mix + 20, argv[1], pw_len);
            uint8_t new_salt_digest[32];
            crypto::sha256(mix, sizeof(mix), new_salt_digest);
            memcpy(state.pass_salt, new_salt_digest, 16);
            crypto::hashPassword(argv[1], state.pass_salt, state.pass_hash);
            state.pass_configured = true;
            state.fs_sync_requested = true;
            out.putStr("passwd: password updated successfully for root (salted SHA-256)\r\n");
        }
    } else if (strcmp(prog, "reboot") == 0) {
        state.reboot_requested = true;
        out.fmt("Broadcast message from root@%s: System is going down for reboot NOW!\r\n",
                state.hostname);
    } else if (strcmp(prog, "wifi") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            out.fmt(
                "Wi-Fi Station : %s\r\n"
                "  SSID        : %s\r\n"
                "  IPv4        : %s\r\n"
                "  MAC         : %s\r\n"
                "  RSSI        : %d dBm\r\n"
                "SoftAP Mode   : %s (SSID: %s, IP: %s, WPA2, pinned=%s)\r\n",
                state.wifi_sta_connected ? "CONNECTED" : "DISCONNECTED",
                state.wifi_ssid[0] ? state.wifi_ssid : state.wifi_cfg_ssid,
                state.wifi_ip, state.wifi_mac, static_cast<int>(state.wifi_rssi),
                state.wifi_ap_enabled ? "ENABLED" : "DISABLED",
                state.wifi_ap_ssid, state.wifi_ap_ip,
                state.wifi_ap_pinned ? "yes" : "no");
        } else if (strcmp(argv[1], "scan") == 0) {
            state.wifi_scan_requested = true;
            out.putStr("Triggered 2.4 GHz Wi-Fi async scan.\r\n");
            for (size_t i = 0; i < state.wifi_scan_count; ++i) {
                out.fmt("  [%u] %-24s  %4d dBm  ch %2u  %s\r\n",
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
            out.fmt("OK WIFI CONNECTING \"%s\"\r\n", state.wifi_cfg_ssid);
        } else if (strcmp(argv[1], "ap") == 0 && argc >= 3) {
            bool en = (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0);
            state.wifi_ap_enabled = en;
            state.wifi_ap_pinned = en;
            state.wifi_reconnect_requested = true;
            state.markDirty(DIRTY_CARDS);
            out.fmt("OK WIFI AP %s\r\n", state.wifi_ap_enabled ? "ON" : "OFF");
        } else {
            out.putStr("Usage: wifi <status | scan | connect <ssid> [pass] | ap <on|off>>\r\n");
        }
    } else if (strcmp(prog, "screen") == 0) {
        if (argc < 2 || strcmp(argv[1], "status") == 0) {
            char json[768];
            formatStatusJson(state, json, sizeof(json));
            out.putStr(json);
        } else if (strcmp(argv[1], "mode") == 0 && argc >= 3) {
            if (vfs.writeFile(state, "/sys/class/graphics/fb0/mode", argv[2], false)) {
                out.fmt("OK MODE %s\r\n", screenModeName(state.screen_mode));
            } else {
                out.fmt("ERR invalid mode '%s' (use: dashboard|console|signboard|test|monitor|image)\r\n", argv[2]);
            }
        } else if (strcmp(argv[1], "ticker") == 0 && argc >= 3) {
            char joined[80];
            BufWriter jw(joined, sizeof(joined), false);
            for (size_t i = 2; i < argc; ++i) {
                jw.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
            }
            vfs.writeFile(state, "/sys/class/graphics/fb0/ticker", joined, false);
            out.fmt("OK TICKER %s\r\n", state.ticker_enabled ? state.ticker_text : "OFF");
        } else if (strcmp(argv[1], "alert") == 0 && argc >= 3) {
            char joined[64];
            BufWriter jw(joined, sizeof(joined), false);
            for (size_t i = 2; i < argc; ++i) {
                jw.fmt("%s%s", argv[i], (i + 1 < argc) ? " " : "");
            }
            vfs.writeFile(state, "/sys/class/graphics/fb0/alert", joined, false);
            out.fmt("OK ALERT %s\r\n", state.alert_active ? state.alert_text : "OFF");
        } else if (strcmp(argv[1], "text") == 0 && argc >= 3) {
            bool has_pipe = false;
            for (size_t i = 2; i < argc; ++i) {
                if (strcmp(argv[i], "|") == 0) {
                    has_pipe = true;
                    break;
                }
            }
            if (has_pipe) {
                char hbuf[48], sbuf[64];
                BufWriter hw_buf(hbuf, sizeof(hbuf), false);
                BufWriter sw_buf(sbuf, sizeof(sbuf), false);
                bool in_sub = false;
                for (size_t i = 2; i < argc; ++i) {
                    if (strcmp(argv[i], "|") == 0) {
                        in_sub = true;
                        continue;
                    }
                    if (!in_sub) {
                        hw_buf.fmt("%s%s", hw_buf.length() > 0 ? " " : "", argv[i]);
                    } else {
                        sw_buf.fmt("%s%s", sw_buf.length() > 0 ? " " : "", argv[i]);
                    }
                }
                strncpy(state.headline, hbuf, sizeof(state.headline) - 1);
                state.headline[sizeof(state.headline) - 1] = '\0';
                if (sbuf[0] != '\0') {
                    strncpy(state.subtitle, sbuf, sizeof(state.subtitle) - 1);
                    state.subtitle[sizeof(state.subtitle) - 1] = '\0';
                }
            } else {
                strncpy(state.headline, argv[2], sizeof(state.headline) - 1);
                state.headline[sizeof(state.headline) - 1] = '\0';
                if (argc >= 4) {
                    strncpy(state.subtitle, argv[3], sizeof(state.subtitle) - 1);
                    state.subtitle[sizeof(state.subtitle) - 1] = '\0';
                }
            }
            state.markDirty(DIRTY_CARDS);
            out.fmt("OK TEXT \"%s\" \"%s\"\r\n", state.headline, state.subtitle);
        } else if (strcmp(argv[1], "theme") == 0 && argc >= 3) {
            if (vfs.writeFile(state, "/sys/class/graphics/fb0/theme", argv[2], false)) {
                out.fmt("OK THEME %s\r\n", themeName(state.theme));
            } else {
                out.fmt("ERR invalid theme '%s' (use: cyan|matrix|amber|red|mono)\r\n", argv[2]);
            }
        } else if (strcmp(argv[1], "bl") == 0 && argc >= 3) {
            vfs.writeFile(state, "/sys/class/backlight/st7789/brightness", argv[2], false);
            out.fmt("OK BL %u\r\n", state.backlight_pct);
        } else if (strcmp(argv[1], "flip") == 0 && argc >= 3) {
            if (strcmp(argv[2], "toggle") == 0) {
                state.landscape_flipped = !state.landscape_flipped;
            } else {
                state.landscape_flipped = (strcmp(argv[2], "on") == 0 ||
                                           strcmp(argv[2], "1") == 0 ||
                                           strcmp(argv[2], "true") == 0);
            }
            state.markDirty(DIRTY_FULL);
            out.fmt("OK FLIP %s\r\n", state.landscape_flipped ? "ON" : "OFF");
        } else if (strcmp(argv[1], "bar") == 0 && argc >= 3) {
            if (strcmp(argv[2], "off") == 0) {
                state.custom_bar_visible = false;
                state.markDirty(DIRTY_CARDS | DIRTY_FULL);
                out.putStr("OK BAR OFF\r\n");
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
                state.markDirty(DIRTY_CARDS);
                out.fmt("OK BAR %u \"%s\"\r\n", state.custom_bar_pct, state.custom_bar_label);
            }
        } else if (strcmp(argv[1], "console") == 0 && argc >= 3) {
            state.appendFbConsoleLine(argv[2]);
            out.putStr("OK CONSOLE\r\n");
        } else if (strcmp(argv[1], "clear") == 0) {
            state.clearFbConsole();
            out.putStr("OK CLEAR\r\n");
        } else if (strcmp(argv[1], "led") == 0 && argc >= 3) {
            if (argc >= 5 && isdigit(static_cast<unsigned char>(argv[2][0]))) {
                state.led_mode = LedMode::SOLID;
                state.led_r = static_cast<uint8_t>(atoi(argv[2]) & 0xFF);
                state.led_g = static_cast<uint8_t>(atoi(argv[3]) & 0xFF);
                state.led_b = static_cast<uint8_t>(atoi(argv[4]) & 0xFF);
                state.markDirty(DIRTY_HEADER);
                out.fmt("OK LED SOLID %u %u %u\r\n", state.led_r, state.led_g, state.led_b);
            } else if (vfs.writeFile(state, "/sys/class/leds/ws2812/mode", argv[2], false)) {
                if (argc >= 6) {
                    state.led_r = static_cast<uint8_t>(atoi(argv[3]) & 0xFF);
                    state.led_g = static_cast<uint8_t>(atoi(argv[4]) & 0xFF);
                    state.led_b = static_cast<uint8_t>(atoi(argv[5]) & 0xFF);
                }
                out.fmt("OK LED %s (%u,%u,%u)\r\n",
                        ledModeName(state.led_mode), state.led_r, state.led_g, state.led_b);
            } else {
                out.putStr("ERR invalid led command\r\n");
            }
        } else {
            out.putStr("Usage: screen <status | mode <m> | text <h> [s] | ticker <msg|off> | alert <msg|off> | theme <t> | bl <0..100> | flip <on|off> | bar <pct> [lbl] | console <str> | clear | led <mode> [r g b]>\r\n");
        }
    } else {
        // Direct executable resolution: check explicit path or /apps/<prog>
        char cand[VFS_MAX_PATH_LEN];
        bool found_exec = false;
        uint8_t magic[24] = {0};
        size_t nread = 0;

        if (strchr(prog, '/') != nullptr) {
            VirtualFs::normalizePath(cwd_, prog, cand, sizeof(cand));
            if (!vfs.isDirectory(cand) && vfs.readBinary(cand, 0, magic, sizeof(magic), &nread) && nread > 0) {
                found_exec = true;
            }
        } else {
            snprintf(cand, sizeof(cand), "/apps/%s", prog);
            if (!vfs.isDirectory(cand) && vfs.readBinary(cand, 0, magic, sizeof(magic), &nread) && nread > 0) {
                found_exec = true;
            } else {
                snprintf(cand, sizeof(cand), "/apps/%s.lua", prog);
                if (!vfs.isDirectory(cand) && vfs.readBinary(cand, 0, magic, sizeof(magic), &nread) && nread > 0) {
                    found_exec = true;
                } else {
                    snprintf(cand, sizeof(cand), "/apps/%s.elf", prog);
                    if (!vfs.isDirectory(cand) && vfs.readBinary(cand, 0, magic, sizeof(magic), &nread) && nread > 0) {
                        found_exec = true;
                    } else {
                        snprintf(cand, sizeof(cand), "/apps/%s.sh", prog);
                        if (!vfs.isDirectory(cand) && vfs.readBinary(cand, 0, magic, sizeof(magic), &nread) && nread > 0) {
                            found_exec = true;
                        }
                    }
                }
            }
        }

        if (found_exec) {
            if (exec_depth_ >= MAX_EXEC_DEPTH) {
                out.putStr("c6-sh: max execution depth (4) exceeded\r\n");
            } else {
                const char* sub_argv[16] = {nullptr};
                int sub_argc = 0;
                for (size_t i = 0; i < argc && sub_argc < 16; ++i) {
                    sub_argv[sub_argc++] = argv[i];
                }
                if (elf::hasElfMagic(magic, nread)) {
                    ExecDepthGuard guard(exec_depth_);
                    elf::runElfFile(state, vfs, cand, sub_argc, sub_argv, out);
                } else if (endsWithCI(cand, ".lua") ||
                           (nread >= 10 && (memcmp(magic, "#!/bin/lua", 10) == 0 ||
                                            memcmp(magic, "#!/usr/bin/lua", 14) == 0))) {
                    ExecDepthGuard guard(exec_depth_);
                    luavm::runFile(state, vfs, cand, sub_argc, sub_argv, out);
                } else {
                    runScriptFile(state, vfs, cand, out);
                }
            }
        } else {
            out.fmt("c6-sh: %s: command not found (type 'help' or 'menu')\r\n", prog);
        }
    }

    if (redir_path) {
        char norm[VFS_MAX_PATH_LEN];
        VirtualFs::normalizePath(cwd_, redir_path, norm, sizeof(norm));
        if (!vfs.writeFile(state, norm, redir_buf, redir_append)) {
            caller_out.fmt("c6-sh: %s: Permission denied or invalid device value\r\n", redir_path);
        } else if (strcmp(norm, "/etc/crontab") == 0) {
            syncCronFromVfs(state, vfs);
        }
    }

    return true;
}

}  // namespace c6os
