#ifndef C6OS_SHELL_H
#define C6OS_SHELL_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {

enum class SessionMode : uint8_t {
    SHELL_CLI = 0,           // Standard POSIX-style shell prompt (root@c6-linux:~# )
    MENU_MAIN = 1,           // Interactive ANSI TUI Screen Control Menu
    MENU_PROMPT_HEAD = 2,    // Prompting for new Screen Headline
    MENU_PROMPT_SUB = 3,     // Prompting for new Screen Subtitle
    MENU_PROMPT_BL = 4,      // Prompting for Backlight % (0..100)
    MENU_PROMPT_RGB = 5,     // Prompting for LED mode / RGB values
    MENU_TTY0_MIRROR = 6,    // Live typing directly onto the physical LCD console (/dev/tty0)
    MENU_WIFI_SSID = 7,      // Prompting for Wi-Fi SSID
    MENU_WIFI_PASS = 8,      // Prompting for Wi-Fi Password
    TOP_VIEW = 9,            // Interactive live-refreshing 'top' process monitor
    HTOP_VIEW = 10,          // Interactive live-refreshing 'htop' color TUI monitor
    WATCH_VIEW = 11,         // Interactive live-refreshing 'watch' command view
    EDIT_VIEW = 12,          // Interactive line-based text editor ('edit' / 'nano')
    MENU_PROMPT_TICKER = 13, // Prompting for scrolling bottom ticker text
    MENU_PROMPT_ALERT = 14   // Prompting for flashing alert overlay banner
};

enum class MonitorSort : uint8_t {
    CPU = 0,
    MEM = 1,
    TIME = 2
};

class ShellSession {
public:
    static constexpr uint8_t MAX_EXEC_DEPTH = 4;

    ShellSession();
    ~ShellSession();

    ShellSession(const ShellSession&) = delete;
    ShellSession& operator=(const ShellSession&) = delete;

    void begin(SystemState& state, VirtualFs& vfs, bool start_in_menu,
               char* out_buf, size_t max_len);

    // Feed raw incoming characters (handles single-keystroke menu navigation and line editing)
    // Returns false if session requested disconnect/logout ('q' in menu or 'exit' in shell)
    bool consumeBytes(SystemState& state, VirtualFs& vfs,
                      const char* data, size_t len,
                      char* out_buf, size_t max_len);

    // Execute a single command line directly (also supports '&&' and ';' chaining)
    // Returns false if command was 'exit' or 'logout'
    bool executeCommandLine(SystemState& state, VirtualFs& vfs,
                            const char* cmd_line,
                            char* out_buf, size_t max_len);

    void renderMenu(const SystemState& state, char* out_buf, size_t max_len) const;
    void renderPrompt(const SystemState& state, char* out_buf, size_t max_len) const;
    void renderTop(const SystemState& state, bool is_htop, bool clear_screen,
                   char* out_buf, size_t max_len) const;
    void renderMonitorTick(const SystemState& state, char* out_buf, size_t max_len) const;
    void renderMonitorTick(SystemState& state, VirtualFs& vfs, char* out_buf, size_t max_len);

    SessionMode mode() const { return mode_; }
    bool isInteractiveMonitor() const {
        return mode_ == SessionMode::TOP_VIEW ||
               mode_ == SessionMode::HTOP_VIEW ||
               mode_ == SessionMode::WATCH_VIEW;
    }
    uint16_t monitorIntervalMs() const {
        return (mode_ == SessionMode::WATCH_VIEW && watch_interval_sec_ > 0)
                   ? static_cast<uint16_t>(watch_interval_sec_ * 1000U)
                   : 1500U;
    }
    const char* cwd() const { return cwd_; }

    // Format screen & system status as JSON (used by 'screen status', '/api/status', and host tooling)
    static void formatStatusJson(const SystemState& state, char* out_buf, size_t max_len);

    // Format wall-clock date/time string via POSIX tzset() + localtime_r()
    static void formatDateStr(const SystemState& state, bool force_utc, char* out_buf, size_t max_len);

    // Sync /etc/crontab <-> state.cron_jobs
    static void syncCronFromVfs(SystemState& state, const VirtualFs& vfs);
    static void syncVfsFromCron(SystemState& state, VirtualFs& vfs);

private:
    void renderMenuTo(const SystemState& state, BufWriter& out) const;
    void renderPromptTo(const SystemState& state, BufWriter& out) const;
    void renderTopTo(const SystemState& state, bool is_htop, bool clear_screen, BufWriter& out) const;
    void renderWatchFrameTo(SystemState& state, VirtualFs& vfs, bool clear_screen, BufWriter& out);

    bool executeCommandLineTo(SystemState& state, VirtualFs& vfs,
                              const char* cmd_line, BufWriter& out);
    bool executeSingleCommand(SystemState& state, VirtualFs& vfs,
                              const char* cmd, BufWriter& out);
    bool runScriptFile(SystemState& state, VirtualFs& vfs,
                       const char* abs_path, BufWriter& out);

    bool handleMenuKey(SystemState& state, VirtualFs& vfs, char ch, BufWriter& out);
    bool handleMonitorKey(SystemState& state, VirtualFs& vfs, char ch, BufWriter& out);
    bool handleLineComplete(SystemState& state, VirtualFs& vfs, const char* line, BufWriter& out);

    bool ensureEditBuffer();
    void freeEditBuffer();

    SessionMode mode_;
    SessionMode prev_mode_;
    MonitorSort sort_mode_;
    bool tree_view_;
    uint8_t exec_depth_;
    char cwd_[64];
    char line_buf_[192];
    size_t line_len_;
    char pending_ssid_[33];
    char menu_notice_[96];
    char watch_cmd_[96];
    uint16_t watch_interval_sec_;
    char edit_path_[64];
    char* edit_buf_;  // Heap-allocated only while EDIT_VIEW is open (0 B idle SRAM)
};

}  // namespace c6os

#endif  // C6OS_SHELL_H
