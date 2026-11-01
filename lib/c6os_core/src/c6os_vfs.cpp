#include "c6os_vfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {

namespace {

void trimTrailingNewline(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

}  // namespace

VirtualFs::VirtualFs() {
    memset(user_files_, 0, sizeof(user_files_));
}

void VirtualFs::initDefaultFiles(const SystemState& state) {
    memset(user_files_, 0, sizeof(user_files_));

    auto setFile = [this](const char* path, const char* content) {
        for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
            if (!user_files_[i].in_use) {
                user_files_[i].in_use = true;
                strncpy(user_files_[i].path, path, VFS_MAX_PATH_LEN - 1);
                strncpy(user_files_[i].content, content, VFS_MAX_FILE_SIZE - 1);
                break;
            }
        }
    };

    setFile("/etc/hostname", "c6-linux\n");
    setFile("/etc/os-release",
            "NAME=\"C6-MicroLinux\"\n"
            "VERSION=\"6.12.0-riscv32 (Waveshare ESP32-C6-LCD-1.47)\"\n"
            "ID=c6os\n"
            "PRETTY_NAME=\"C6-MicroLinux 6.12.0-c6os-riscv32\"\n"
            "HOME_URL=\"https://espressif.com/\"\n");
    setFile("/etc/motd",
            "Welcome to C6-MicroLinux 6.12.0-c6os-riscv32 (rv32imac)\n"
            " * Board:   Waveshare ESP32-C6-LCD-1.47 (ST7789 320x172 RGB565)\n"
            " * Menu:    Type 'menu' to launch the interactive Screen Control TUI\n"
            " * Control: Type 'screen help' or write to /dev/tty0 & /sys/class/*\n");
    setFile("/etc/passwd", "root:x:0:0:root:/root:/bin/c6-sh\n");

    char wpa_buf[192];
    snprintf(wpa_buf, sizeof(wpa_buf),
             "ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"%s\"\n  psk=\"***\"\n}\n",
             state.wifi_cfg_ssid[0] ? state.wifi_cfg_ssid : "unconfigured");
    setFile("/etc/wpa_supplicant.conf", wpa_buf);

    setFile("/root/README.txt",
            "C6-MicroLinux Screen Quick Reference:\n"
            "  menu                         - Open interactive ANSI TUI menu\n"
            "  screen mode dashboard        - Show OS & SSH status dashboard\n"
            "  screen mode console          - Show live /dev/tty0 console on LCD\n"
            "  screen mode signboard        - Show full-screen custom signboard\n"
            "  screen text \"Hello\" \"Sub\"    - Update screen headline & subtitle\n"
            "  screen bl 80                 - Set LCD backlight to 80%\n"
            "  echo \"Hi LCD\" > /dev/tty0    - Print line to LCD framebuffer terminal\n");
}

void VirtualFs::normalizePath(const char* cwd, const char* input_path, char* out_path, size_t max_len) {
    if (!out_path || max_len == 0) return;
    if (!input_path || input_path[0] == '\0') {
        strncpy(out_path, cwd ? cwd : "/", max_len - 1);
        out_path[max_len - 1] = '\0';
        return;
    }

    char combined[160];
    if (input_path[0] == '/') {
        strncpy(combined, input_path, sizeof(combined) - 1);
        combined[sizeof(combined) - 1] = '\0';
    } else if (input_path[0] == '~') {
        snprintf(combined, sizeof(combined), "/root%s", input_path + 1);
    } else {
        const char* base = (cwd && cwd[0] == '/') ? cwd : "/";
        if (strcmp(base, "/") == 0) {
            snprintf(combined, sizeof(combined), "/%s", input_path);
        } else {
            snprintf(combined, sizeof(combined), "%s/%s", base, input_path);
        }
    }

    const char* segments[16];
    size_t seg_count = 0;

    char* saveptr = nullptr;
    char* token = strtok_r(combined, "/", &saveptr);
    while (token != nullptr) {
        if (strcmp(token, ".") == 0) {
            // Ignore
        } else if (strcmp(token, "..") == 0) {
            if (seg_count > 0) --seg_count;
        } else if (seg_count < 16) {
            segments[seg_count++] = token;
        }
        token = strtok_r(nullptr, "/", &saveptr);
    }

    if (seg_count == 0) {
        strncpy(out_path, "/", max_len - 1);
        out_path[max_len - 1] = '\0';
        return;
    }

    out_path[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < seg_count; ++i) {
        int written = snprintf(out_path + pos, max_len - pos, "/%s", segments[i]);
        if (written < 0 || static_cast<size_t>(written) >= max_len - pos) {
            break;
        }
        pos += static_cast<size_t>(written);
    }
}

bool VirtualFs::isDirectory(const char* abs_path) const {
    if (!abs_path) return false;
    static const char* const DIRS[] = {
        "/",
        "/bin",
        "/dev",
        "/etc",
        "/etc/ssh",
        "/proc",
        "/proc/net",
        "/root",
        "/sys",
        "/sys/class",
        "/sys/class/backlight",
        "/sys/class/backlight/st7789",
        "/sys/class/graphics",
        "/sys/class/graphics/fb0",
        "/sys/class/leds",
        "/sys/class/leds/ws2812",
        "/var",
        "/var/log"
    };
    for (const char* d : DIRS) {
        if (strcmp(abs_path, d) == 0) return true;
    }
    return false;
}

bool VirtualFs::listDirectory(const char* abs_path, char* out_buf, size_t max_len) const {
    if (!abs_path || !out_buf || max_len == 0) return false;
    out_buf[0] = '\0';

    if (strcmp(abs_path, "/") == 0) {
        snprintf(out_buf, max_len, "bin/  dev/  etc/  proc/  root/  sys/  var/\n");
        return true;
    }
    if (strcmp(abs_path, "/bin") == 0) {
        snprintf(out_buf, max_len, "c6-menu*  c6-sh*  cat*  df*  dmesg*  echo*  free*  ip*  ls*  passwd*  ps*  reboot*  screen*  uname*  uptime*  wifi*\n");
        return true;
    }
    if (strcmp(abs_path, "/proc") == 0) {
        snprintf(out_buf, max_len, "cpuinfo  loadavg  meminfo  net/  uptime  version\n");
        return true;
    }
    if (strcmp(abs_path, "/proc/net") == 0) {
        snprintf(out_buf, max_len, "dev  wireless\n");
        return true;
    }
    if (strcmp(abs_path, "/dev") == 0) {
        snprintf(out_buf, max_len, "fb0  input0  led0  null  pts/  tty0  ttyACM0  ttyS0  zero\n");
        return true;
    }
    if (strcmp(abs_path, "/sys") == 0) {
        snprintf(out_buf, max_len, "class/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class") == 0) {
        snprintf(out_buf, max_len, "backlight/  graphics/  leds/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/backlight") == 0) {
        snprintf(out_buf, max_len, "st7789/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/backlight/st7789") == 0) {
        snprintf(out_buf, max_len, "brightness  max_brightness\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics") == 0) {
        snprintf(out_buf, max_len, "fb0/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0") == 0) {
        snprintf(out_buf, max_len, "mode  name  stride  text  theme  virtual_size\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds") == 0) {
        snprintf(out_buf, max_len, "ws2812/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812") == 0) {
        snprintf(out_buf, max_len, "mode  rgb\n");
        return true;
    }
    if (strcmp(abs_path, "/var") == 0) {
        snprintf(out_buf, max_len, "log/\n");
        return true;
    }
    if (strcmp(abs_path, "/var/log") == 0) {
        snprintf(out_buf, max_len, "dmesg  sshd.log\n");
        return true;
    }
    if (strcmp(abs_path, "/etc/ssh") == 0) {
        snprintf(out_buf, max_len, "ssh_host_ed25519_key  ssh_host_ed25519_key.pub  sshd_config\n");
        return true;
    }

    if (strcmp(abs_path, "/etc") == 0 || strcmp(abs_path, "/root") == 0) {
        size_t prefix_len = strlen(abs_path);
        size_t pos = 0;
        if (strcmp(abs_path, "/etc") == 0) {
            pos += static_cast<size_t>(snprintf(out_buf + pos, max_len - pos, "ssh/  "));
        }
        for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
            if (!user_files_[i].in_use) continue;
            if (strncmp(user_files_[i].path, abs_path, prefix_len) == 0 &&
                user_files_[i].path[prefix_len] == '/') {
                const char* sub = user_files_[i].path + prefix_len + 1;
                if (strchr(sub, '/') == nullptr) {
                    int w = snprintf(out_buf + pos, max_len - pos, "%s  ", sub);
                    if (w > 0) pos += static_cast<size_t>(w);
                }
            }
        }
        snprintf(out_buf + pos, max_len - pos, "\n");
        return true;
    }

    return false;
}

bool VirtualFs::readFile(const SystemState& state, const char* abs_path, char* out_buf, size_t max_len) const {
    if (!abs_path || !out_buf || max_len == 0) return false;
    out_buf[0] = '\0';

    // Synthetic /proc files
    if (strcmp(abs_path, "/proc/version") == 0) {
        snprintf(out_buf, max_len,
                 "Linux version 6.12.0-c6os-riscv32 (root@%s) "
                 "(riscv32-esp-elf-gcc (crosstool-NG esp-13.2.0) 13.2.0, GNU ld 2.41) "
                 "#1 PREEMPT ESP32-C6FH4\n",
                 state.hostname);
        return true;
    }
    if (strcmp(abs_path, "/proc/cpuinfo") == 0) {
        snprintf(out_buf, max_len,
                 "processor\t: 0\n"
                 "hart\t\t: 0\n"
                 "isa\t\t: rv32imac_zicsr_zifencei\n"
                 "mmu\t\t: none (PMP)\n"
                 "uarch\t\t: espressif,esp32-c6fh4\n"
                 "model name\t: Espressif ESP32-C6FH4 RISC-V @ %u MHz\n"
                 "cpu MHz\t\t: %u.000\n"
                 "flash size\t: %u MB (XMC DIO)\n"
                 "display\t\t: Waveshare 1.47\" ST7789 (320x172 RGB565 @ 80MHz SPI2)\n",
                 state.cpu_freq_mhz, state.cpu_freq_mhz, state.flash_size_mb);
        return true;
    }
    if (strcmp(abs_path, "/proc/meminfo") == 0) {
        uint32_t total_kb = state.total_sram_bytes / 1024U;
        uint32_t free_kb  = state.free_sram_bytes / 1024U;
        uint32_t min_kb   = state.min_free_sram_bytes / 1024U;
        uint32_t used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
        snprintf(out_buf, max_len,
                 "MemTotal:         %6u kB\n"
                 "MemFree:          %6u kB\n"
                 "MemAvailable:     %6u kB\n"
                 "MemUsed:          %6u kB\n"
                 "LowWaterMark:     %6u kB\n"
                 "SwapTotal:             0 kB\n"
                 "SwapFree:              0 kB\n",
                 total_kb, free_kb, free_kb, used_kb, min_kb);
        return true;
    }
    if (strcmp(abs_path, "/proc/uptime") == 0) {
        snprintf(out_buf, max_len, "%u.00 %u.00\n", state.uptime_sec, state.uptime_sec);
        return true;
    }
    if (strcmp(abs_path, "/proc/loadavg") == 0) {
        snprintf(out_buf, max_len, "0.08 0.05 0.01 1/6 42\n");
        return true;
    }
    if (strcmp(abs_path, "/proc/net/dev") == 0) {
        snprintf(out_buf, max_len,
                 "Inter-|   Receive                                                |  Transmit\n"
                 " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop\n"
                 "    lo:       0       0    0    0    0     0          0         0        0       0    0    0\n"
                 " wlan0:%8u     120    0    0    0     0          0         0 %8u     115    0    0\n"
                 "   ap0:     512       8    0    0    0     0          0         0     1024      10    0    0\n",
                 state.net_rx_bytes, state.net_tx_bytes);
        return true;
    }
    if (strcmp(abs_path, "/proc/net/wireless") == 0) {
        snprintf(out_buf, max_len,
                 "Inter-| sta-|   Quality        |   Discarded packets\n"
                 " face | tus | link level noise |  nwid  crypt   frag\n"
                 " wlan0: %04x   70.  %4d.  -95.      0      0      0\n",
                 state.wifi_sta_connected ? 1 : 0, static_cast<int>(state.wifi_rssi));
        return true;
    }

    // Synthetic /sys files
    if (strcmp(abs_path, "/sys/class/backlight/st7789/brightness") == 0) {
        snprintf(out_buf, max_len, "%u\n", state.backlight_pct);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/backlight/st7789/max_brightness") == 0) {
        snprintf(out_buf, max_len, "100\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/name") == 0) {
        snprintf(out_buf, max_len, "st7789_spi2_320x172\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/virtual_size") == 0) {
        snprintf(out_buf, max_len, "320,172\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/mode") == 0) {
        snprintf(out_buf, max_len, "%s\n", screenModeName(state.screen_mode));
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/theme") == 0) {
        snprintf(out_buf, max_len, "%s\n", themeName(state.theme));
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/text") == 0) {
        snprintf(out_buf, max_len, "%s\n%s\n", state.headline, state.subtitle);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/mode") == 0) {
        snprintf(out_buf, max_len, "%s\n", ledModeName(state.led_mode));
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/rgb") == 0) {
        snprintf(out_buf, max_len, "%u %u %u\n", state.led_r, state.led_g, state.led_b);
        return true;
    }
    if (strcmp(abs_path, "/etc/ssh/ ssh_host_ed25519_key.pub") == 0 ||
        strcmp(abs_path, "/etc/ssh/ssh_host_ed25519_key.pub") == 0) {
        snprintf(out_buf, max_len, "ssh-ed25519 (%s) root@%s\n", state.ssh_hostkey_fp, state.hostname);
        return true;
    }
    if (strcmp(abs_path, "/etc/ssh/sshd_config") == 0) {
        snprintf(out_buf, max_len,
                 "Port %u\n"
                 "HostKey /etc/ssh/ssh_host_ed25519_key\n"
                 "KexAlgorithms curve25519-sha256,curve25519-sha256@libssh.org\n"
                 "Ciphers aes128-ctr,aes256-ctr\n"
                 "MACs hmac-sha2-256\n"
                 "PermitRootLogin yes\n",
                 state.ssh_port);
        return true;
    }
    if (strcmp(abs_path, "/var/log/dmesg") == 0) {
        size_t pos = 0;
        for (size_t i = 0; i < state.dmesg_count; ++i) {
            int w = snprintf(out_buf + pos, max_len - pos, "%s\n", state.dmesg[i]);
            if (w < 0 || static_cast<size_t>(w) >= max_len - pos) break;
            pos += static_cast<size_t>(w);
        }
        return true;
    }
    if (strcmp(abs_path, "/dev/tty0") == 0 || strcmp(abs_path, "/dev/fb0") == 0) {
        size_t pos = 0;
        for (size_t i = 0; i < state.fb_line_count; ++i) {
            int w = snprintf(out_buf + pos, max_len - pos, "%s\n", state.fb_lines[i]);
            if (w < 0 || static_cast<size_t>(w) >= max_len - pos) break;
            pos += static_cast<size_t>(w);
        }
        return true;
    }
    if (strcmp(abs_path, "/dev/null") == 0) {
        out_buf[0] = '\0';
        return true;
    }

    // Check user files (/etc/*, /root/*)
    for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
        if (user_files_[i].in_use && strcmp(user_files_[i].path, abs_path) == 0) {
            strncpy(out_buf, user_files_[i].content, max_len - 1);
            out_buf[max_len - 1] = '\0';
            return true;
        }
    }

    return false;
}

bool VirtualFs::writeFile(SystemState& state, const char* abs_path, const char* data, bool append) {
    if (!abs_path || !data) return false;

    if (strcmp(abs_path, "/dev/null") == 0) {
        return true;
    }

    // Device & sysfs write hooks
    if (strcmp(abs_path, "/dev/tty0") == 0 || strcmp(abs_path, "/dev/fb0") == 0) {
        if (!append) state.clearFbConsole();
        state.appendFbConsoleLine(data);
        return true;
    }

    char clean[128];
    strncpy(clean, data, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    trimTrailingNewline(clean);

    if (strcmp(abs_path, "/sys/class/backlight/st7789/brightness") == 0) {
        int val = atoi(clean);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        state.backlight_pct = static_cast<uint8_t>(val);
        state.ui_dirty = true;
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/mode") == 0) {
        if (strcmp(clean, "dashboard") == 0 || strcmp(clean, "0") == 0) {
            state.screen_mode = ScreenMode::OS_DASHBOARD;
        } else if (strcmp(clean, "console") == 0 || strcmp(clean, "tty0") == 0 || strcmp(clean, "1") == 0) {
            state.screen_mode = ScreenMode::FB_CONSOLE;
        } else if (strcmp(clean, "signboard") == 0 || strcmp(clean, "2") == 0) {
            state.screen_mode = ScreenMode::SIGNBOARD;
        } else if (strcmp(clean, "test") == 0 || strcmp(clean, "diagnostics") == 0 || strcmp(clean, "3") == 0) {
            state.screen_mode = ScreenMode::DIAGNOSTICS;
        } else {
            return false;
        }
        state.ui_dirty = true;
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/theme") == 0) {
        if (strcmp(clean, "cyan") == 0) state.theme = ThemeId::CYBER_CYAN;
        else if (strcmp(clean, "matrix") == 0 || strcmp(clean, "green") == 0) state.theme = ThemeId::MATRIX_GREEN;
        else if (strcmp(clean, "amber") == 0) state.theme = ThemeId::AMBER_CRT;
        else if (strcmp(clean, "red") == 0) state.theme = ThemeId::RED_ALERT;
        else if (strcmp(clean, "mono") == 0 || strcmp(clean, "white") == 0) state.theme = ThemeId::MONO_WHITE;
        else return false;
        state.ui_dirty = true;
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/text") == 0) {
        strncpy(state.headline, clean, sizeof(state.headline) - 1);
        state.headline[sizeof(state.headline) - 1] = '\0';
        state.ui_dirty = true;
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/mode") == 0) {
        if (strcmp(clean, "off") == 0) state.led_mode = LedMode::OFF;
        else if (strcmp(clean, "solid") == 0) state.led_mode = LedMode::SOLID;
        else if (strcmp(clean, "pulse") == 0) state.led_mode = LedMode::PULSE;
        else if (strcmp(clean, "rainbow") == 0) state.led_mode = LedMode::RAINBOW;
        else if (strcmp(clean, "ssh") == 0) state.led_mode = LedMode::SSH_ACTIVITY;
        else return false;
        state.ui_dirty = true;
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/rgb") == 0) {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(clean, "%u %u %u", &r, &g, &b) == 3) {
            state.led_r = static_cast<uint8_t>(r & 0xFF);
            state.led_g = static_cast<uint8_t>(g & 0xFF);
            state.led_b = static_cast<uint8_t>(b & 0xFF);
            state.led_mode = LedMode::SOLID;
            state.ui_dirty = true;
            return true;
        }
        return false;
    }

    if (strncmp(abs_path, "/proc/", 6) == 0 || strncmp(abs_path, "/sys/", 5) == 0) {
        return false;  // Read-only kernel pseudo-files
    }

    if (strcmp(abs_path, "/etc/hostname") == 0 && clean[0] != '\0') {
        strncpy(state.hostname, clean, sizeof(state.hostname) - 1);
        state.hostname[sizeof(state.hostname) - 1] = '\0';
        state.ui_dirty = true;
    }

    // Find existing or free user file entry
    for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
        if (user_files_[i].in_use && strcmp(user_files_[i].path, abs_path) == 0) {
            if (append) {
                size_t cur = strlen(user_files_[i].content);
                snprintf(user_files_[i].content + cur, VFS_MAX_FILE_SIZE - cur, "%s", data);
            } else {
                strncpy(user_files_[i].content, data, VFS_MAX_FILE_SIZE - 1);
                user_files_[i].content[VFS_MAX_FILE_SIZE - 1] = '\0';
            }
            state.fs_sync_requested = true;
            return true;
        }
    }

    for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
        if (!user_files_[i].in_use) {
            user_files_[i].in_use = true;
            strncpy(user_files_[i].path, abs_path, VFS_MAX_PATH_LEN - 1);
            user_files_[i].path[VFS_MAX_PATH_LEN - 1] = '\0';
            strncpy(user_files_[i].content, data, VFS_MAX_FILE_SIZE - 1);
            user_files_[i].content[VFS_MAX_FILE_SIZE - 1] = '\0';
            state.fs_sync_requested = true;
            return true;
        }
    }

    return false;
}

bool VirtualFs::removeFile(SystemState& state, const char* abs_path) {
    if (!abs_path) return false;
    for (size_t i = 0; i < VFS_MAX_USER_FILES; ++i) {
        if (user_files_[i].in_use && strcmp(user_files_[i].path, abs_path) == 0) {
            memset(&user_files_[i], 0, sizeof(user_files_[i]));
            state.fs_sync_requested = true;
            return true;
        }
    }
    return false;
}

}  // namespace c6os
