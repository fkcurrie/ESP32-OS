#ifndef C6OS_SHELL_H
#define C6OS_SHELL_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {

enum class SessionMode : uint8_t {
    SHELL_CLI = 0,         // Standard POSIX-style shell prompt (root@c6-linux:~# )
    MENU_MAIN = 1,         // Interactive ANSI TUI Screen Control Menu
    MENU_PROMPT_HEAD = 2,  // Prompting for new Screen Headline
    MENU_PROMPT_SUB = 3,   // Prompting for new Screen Subtitle
    MENU_PROMPT_BL = 4,    // Prompting for Backlight % (0..100)
    MENU_PROMPT_RGB = 5,   // Prompting for LED mode / RGB values
    MENU_TTY0_MIRROR = 6,  // Live typing directly onto the physical LCD console (/dev/tty0)
    MENU_WIFI_SSID = 7,    // Prompting for Wi-Fi SSID
    MENU_WIFI_PASS = 8     // Prompting for Wi-Fi Password
};

class ShellSession {
public:
    ShellSession();

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

    SessionMode mode() const { return mode_; }
    const char* cwd() const { return cwd_; }

    // Format screen & system status as JSON (used by 'screen status' and host tooling)
    static void formatStatusJson(const SystemState& state, char* out_buf, size_t max_len);

private:
    bool executeSingleCommand(SystemState& state, VirtualFs& vfs,
                              const char* cmd,
                              char* out_buf, size_t max_len);

    bool handleMenuKey(SystemState& state, VirtualFs& vfs, char ch,
                       char* out_buf, size_t max_len);

    bool handleLineComplete(SystemState& state, VirtualFs& vfs,
                            const char* line,
                            char* out_buf, size_t max_len);

    SessionMode mode_;
    char cwd_[64];
    char line_buf_[192];
    size_t line_len_;
    char pending_ssid_[33];
    char menu_notice_[96];
};

}  // namespace c6os

#endif  // C6OS_SHELL_H
