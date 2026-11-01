#include "c6os_vfs.h"
#include "ssh_auth.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace c6os {

namespace {

void trimTrailingNewline(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

void removeTreeRecursive(const char* dir_path) {
    if (!dir_path || dir_path[0] == '\0') return;
    DIR* d = opendir(dir_path);
    if (!d) return;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            removeTreeRecursive(child);
            rmdir(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(dir_path);
}

bool parseLoginShellFromPasswd(const char* content, char* out_shell, size_t cap) {
    if (!content || !out_shell || cap == 0) return false;
    const char* line_end = strchr(content, '\n');
    size_t line_len = line_end ? static_cast<size_t>(line_end - content) : strlen(content);
    char line[128];
    if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
    memcpy(line, content, line_len);
    line[line_len] = '\0';
    trimTrailingNewline(line);

    const char* last_colon = strrchr(line, ':');
    if (!last_colon || last_colon[1] != '/') return false;
    strncpy(out_shell, last_colon + 1, cap - 1);
    out_shell[cap - 1] = '\0';
    return true;
}

}  // namespace

VirtualFs::VirtualFs() : owns_temp_root_(false) {
    memset(rootfs_prefix_, 0, sizeof(rootfs_prefix_));
    memset(sd_prefix_, 0, sizeof(sd_prefix_));
    memset(temp_base_, 0, sizeof(temp_base_));

#ifdef ESP_PLATFORM
    strncpy(rootfs_prefix_, "/littlefs", sizeof(rootfs_prefix_) - 1);
    strncpy(sd_prefix_, "/sd", sizeof(sd_prefix_) - 1);
#else
    static unsigned int s_instance_id = 0;
    ++s_instance_id;
    snprintf(temp_base_, sizeof(temp_base_), "/tmp/c6os_vfs_%d_%u",
             static_cast<int>(getpid()), s_instance_id);
    snprintf(rootfs_prefix_, sizeof(rootfs_prefix_), "%.80s/rootfs", temp_base_);
    snprintf(sd_prefix_, sizeof(sd_prefix_), "%.80s/sd", temp_base_);
    mkdir(temp_base_, 0755);
    mkdir(rootfs_prefix_, 0755);
    mkdir(sd_prefix_, 0755);
    owns_temp_root_ = true;
#endif
}

VirtualFs::~VirtualFs() {
#ifndef ESP_PLATFORM
    if (owns_temp_root_ && temp_base_[0] != '\0') {
        removeTreeRecursive(temp_base_);
    }
#endif
}

void VirtualFs::setMountRoots(const char* rootfs_prefix, const char* sd_prefix) {
#ifndef ESP_PLATFORM
    if (owns_temp_root_ && temp_base_[0] != '\0') {
        removeTreeRecursive(temp_base_);
        temp_base_[0] = '\0';
        owns_temp_root_ = false;
    }
#endif
    if (rootfs_prefix) {
        strncpy(rootfs_prefix_, rootfs_prefix, sizeof(rootfs_prefix_) - 1);
        rootfs_prefix_[sizeof(rootfs_prefix_) - 1] = '\0';
    }
    if (sd_prefix) {
        strncpy(sd_prefix_, sd_prefix, sizeof(sd_prefix_) - 1);
        sd_prefix_[sizeof(sd_prefix_) - 1] = '\0';
    }
}

void VirtualFs::ensureParentDirs(const char* real_path) const {
    if (!real_path) return;
    char tmp[224];
    strncpy(tmp, real_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

bool VirtualFs::isVirtualPath(const char* abs_path) const {
    if (!abs_path) return true;
    if (strcmp(abs_path, "/") == 0 ||
        strcmp(abs_path, "/bin") == 0 || strncmp(abs_path, "/bin/", 5) == 0 ||
        strcmp(abs_path, "/proc") == 0 || strncmp(abs_path, "/proc/", 6) == 0 ||
        strcmp(abs_path, "/sys") == 0 || strncmp(abs_path, "/sys/", 5) == 0 ||
        strcmp(abs_path, "/dev") == 0 || strncmp(abs_path, "/dev/", 5) == 0 ||
        strcmp(abs_path, "/var/log/dmesg") == 0 ||
        strcmp(abs_path, "/etc/ssh/ssh_host_ed25519_key.pub") == 0 ||
        strcmp(abs_path, "/etc/ssh/sshd_config") == 0) {
        return true;
    }
    return false;
}

bool VirtualFs::resolveRealPath(const char* abs_path, char* out_real, size_t max_len) const {
    if (!abs_path || !out_real || max_len == 0 || abs_path[0] != '/') return false;
    if (isVirtualPath(abs_path)) return false;

    if (strcmp(abs_path, "/mnt/sd") == 0) {
        if (sd_prefix_[0] == '\0') return false;
        strncpy(out_real, sd_prefix_, max_len - 1);
        out_real[max_len - 1] = '\0';
        return true;
    }
    if (strncmp(abs_path, "/mnt/sd/", 8) == 0) {
        if (sd_prefix_[0] == '\0') return false;
        int w = snprintf(out_real, max_len, "%s/%s", sd_prefix_, abs_path + 8);
        return w > 0 && static_cast<size_t>(w) < max_len;
    }

    if (rootfs_prefix_[0] == '\0') return false;
    int w = snprintf(out_real, max_len, "%s%s", rootfs_prefix_, abs_path);
    return w > 0 && static_cast<size_t>(w) < max_len;
}

void VirtualFs::initDefaultFiles(SystemState& state) {
    mkdirPath("/etc");
    mkdirPath("/etc/ssh");
    mkdirPath("/root");
    mkdirPath("/apps");
    mkdirPath("/var");
    mkdirPath("/var/log");
    mkdirPath("/var/pkg");
    mkdirPath("/mnt");
    mkdirPath("/tmp");

    auto ensureFile = [this](const char* abs_path, const char* content) {
        char real_path[224];
        if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) return;
        struct stat st;
        if (stat(real_path, &st) == 0) return;  // Preserve persisted user edits
        ensureParentDirs(real_path);
        FILE* f = fopen(real_path, "wb");
        if (f) {
            size_t len = strlen(content);
            if (len > 0) fwrite(content, 1, len, f);
            fclose(f);
        }
    };

    ensureFile("/etc/hostname", "c6-linux\n");
    ensureFile("/etc/os-release",
               "NAME=\"C6-MicroLinux\"\n"
               "VERSION=\"6.12.0-riscv32 (Waveshare ESP32-C6-LCD-1.47 QIO)\"\n"
               "ID=c6os\n"
               "PRETTY_NAME=\"C6-MicroLinux 6.12.0-c6os-riscv32\"\n"
               "HOME_URL=\"https://espressif.com/\"\n");
    ensureFile("/etc/motd",
               "Welcome to C6-MicroLinux 6.12.0-c6os-riscv32 (rv32imac)\n"
               " * Board:   Waveshare ESP32-C6-LCD-1.47 (ST7789 320x172 RGB565 DMA)\n"
               " * Apps:    SFTP v3 | epkg | Lua 5.4 (32-bit) | RV32IMAC ELF\n"
               " * Shell:   Type 'menu' for TUI, 'chsh' for default login shell\n");
    ensureFile("/etc/passwd", "root:x:0:0:root:/root:/bin/c6-menu\n");
    ensureFile("/etc/timezone", "America/Toronto\n");

    char wpa_buf[192];
    snprintf(wpa_buf, sizeof(wpa_buf),
             "ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"%s\"\n  psk=\"***\"\n}\n",
             state.wifi_cfg_ssid[0] ? state.wifi_cfg_ssid : "unconfigured");
    ensureFile("/etc/wpa_supplicant.conf", wpa_buf);
    ensureFile("/etc/rc.local",
               "#!/bin/c6-sh\n"
               "# /etc/rc.local - executed by init at boot\n"
               "echo \"rc.local: boot services initialized\" > /dev/tty0\n");
    ensureFile("/etc/crontab",
               "# /etc/crontab: <interval_sec> <command>\n"
               "# Use 'crontab -l' or 'crontab -a <sec> <cmd>'\n");

    char auth_keys_buf[1024];
    crypto::formatDefaultAuthorizedKeys(auth_keys_buf, sizeof(auth_keys_buf));
    ensureFile("/etc/ssh/authorized_keys", auth_keys_buf);

    ensureFile("/root/README.txt",
               "C6-MicroLinux Quick Reference:\n"
               "  menu / chsh                  - Open ANSI TUI or switch login shell\n"
               "  epkg install <pkg.epkg>      - Install Lua/ELF/shell app into /apps\n"
               "  lua -e 'print(sys.heap())'   - Run 32-bit Lua 5.4 with hardware APIs\n"
               "  elf /apps/hello.elf          - Execute RV32IMAC ELF32 binary\n"
               "  ble on | ble scan | i2c scan - On-demand BLE 5, I2C, ADC, PWM, GPIO\n"
               "  fbgrab /root/snap.bmp        - Save pixel-accurate 320x172 screenshot\n"
               "  tz America/Toronto | apitoken- Set POSIX timezone or rotate REST token\n");

    // Synchronize persisted /etc config into SystemState
    char cfg_buf[256];
    if (readFile(state, "/etc/hostname", cfg_buf, sizeof(cfg_buf))) {
        trimTrailingNewline(cfg_buf);
        if (cfg_buf[0] != '\0') {
            strncpy(state.hostname, cfg_buf, sizeof(state.hostname) - 1);
            state.hostname[sizeof(state.hostname) - 1] = '\0';
        }
    }
    if (readFile(state, "/etc/timezone", cfg_buf, sizeof(cfg_buf))) {
        trimTrailingNewline(cfg_buf);
        if (cfg_buf[0] != '\0') {
            resolvePosixTz(cfg_buf, state.tz_name, sizeof(state.tz_name),
                           state.tz_posix, sizeof(state.tz_posix));
        }
    }
    if (readFile(state, "/etc/passwd", cfg_buf, sizeof(cfg_buf))) {
        parseLoginShellFromPasswd(cfg_buf, state.login_shell, sizeof(state.login_shell));
    }
}

void VirtualFs::normalizePath(const char* cwd, const char* input_path, char* out_path, size_t max_len) {
    if (!out_path || max_len == 0) return;
    if (!input_path || input_path[0] == '\0') {
        strncpy(out_path, cwd ? cwd : "/", max_len - 1);
        out_path[max_len - 1] = '\0';
        return;
    }

    char combined[192];
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

    const char* segments[20];
    size_t seg_count = 0;

    char* saveptr = nullptr;
    char* token = strtok_r(combined, "/", &saveptr);
    while (token != nullptr) {
        if (strcmp(token, ".") == 0) {
            // Ignore
        } else if (strcmp(token, "..") == 0) {
            if (seg_count > 0) --seg_count;
        } else if (seg_count < 20) {
            segments[seg_count++] = token;
        }
        token = strtok_r(nullptr, "/", &saveptr);
    }

    if (seg_count == 0) {
        strncpy(out_path, "/", max_len - 1);
        out_path[max_len - 1] = '\0';
        return;
    }

    BufWriter w(out_path, max_len);
    for (size_t i = 0; i < seg_count; ++i) {
        w.fmt("/%s", segments[i]);
    }
}

bool VirtualFs::isDirectory(const char* abs_path) const {
    if (!abs_path) return false;
    static const char* const DIRS[] = {
        "/",
        "/apps",
        "/bin",
        "/dev",
        "/etc",
        "/etc/ssh",
        "/mnt",
        "/mnt/sd",
        "/proc",
        "/proc/net",
        "/root",
        "/sys",
        "/sys/class",
        "/sys/class/backlight",
        "/sys/class/backlight/st7789",
        "/sys/class/gpio",
        "/sys/class/graphics",
        "/sys/class/graphics/fb0",
        "/sys/class/leds",
        "/sys/class/leds/ws2812",
        "/sys/class/thermal",
        "/sys/class/thermal/thermal_zone0",
        "/tmp",
        "/var",
        "/var/log",
        "/var/pkg"
    };
    for (const char* d : DIRS) {
        if (strcmp(abs_path, d) == 0) return true;
    }

    char real_path[224];
    if (resolveRealPath(abs_path, real_path, sizeof(real_path))) {
        struct stat st;
        if (stat(real_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            return true;
        }
    }
    return false;
}

size_t VirtualFs::readDirEntries(const char* abs_path, VfsDirEntry* out_entries, size_t max_entries) const {
    if (!abs_path || !out_entries || max_entries == 0) return 0;
    size_t count = 0;

    auto addEntry = [&](const char* name, bool is_dir, uint64_t sz = 0, uint32_t mtime = 0) {
        if (count >= max_entries) return;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(out_entries[i].name, name) == 0) return;
        }
        strncpy(out_entries[count].name, name, sizeof(out_entries[count].name) - 1);
        out_entries[count].name[sizeof(out_entries[count].name) - 1] = '\0';
        out_entries[count].is_dir = is_dir;
        out_entries[count].size = sz;
        out_entries[count].mode = is_dir ? 0040755U : 0100644U;
        out_entries[count].mtime = mtime;
        ++count;
    };

    if (strcmp(abs_path, "/") == 0) {
        addEntry("apps", true);
        addEntry("bin", true);
        addEntry("dev", true);
        addEntry("etc", true);
        addEntry("mnt", true);
        addEntry("proc", true);
        addEntry("root", true);
        addEntry("sys", true);
        addEntry("tmp", true);
        addEntry("var", true);
        return count;
    }
    if (strcmp(abs_path, "/mnt") == 0) {
        addEntry("sd", true);
    } else if (strcmp(abs_path, "/etc") == 0) {
        addEntry("ssh", true);
    } else if (strcmp(abs_path, "/etc/ssh") == 0) {
        addEntry("ssh_host_ed25519_key.pub", false, 96);
        addEntry("sshd_config", false, 160);
    } else if (strcmp(abs_path, "/var") == 0) {
        addEntry("log", true);
        addEntry("pkg", true);
    } else if (strcmp(abs_path, "/var/log") == 0) {
        addEntry("dmesg", false, 1024);
    }

    char real_path[224];
    if (resolveRealPath(abs_path, real_path, sizeof(real_path))) {
        DIR* d = opendir(real_path);
        if (d) {
            struct dirent* ent = nullptr;
            while ((ent = readdir(d)) != nullptr && count < max_entries) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                char child[512];
                snprintf(child, sizeof(child), "%s/%s", real_path, ent->d_name);
                struct stat st;
                bool is_dir = false;
                uint64_t sz = 0;
                uint32_t mt = 0;
                if (stat(child, &st) == 0) {
                    is_dir = S_ISDIR(st.st_mode);
                    sz = is_dir ? 0ULL : static_cast<uint64_t>(st.st_size);
                    mt = static_cast<uint32_t>(st.st_mtime);
                }
                addEntry(ent->d_name, is_dir, sz, mt);
            }
            closedir(d);
        }
    }

    return count;
}

bool VirtualFs::listDirectory(const char* abs_path, char* out_buf, size_t max_len) const {
    if (!abs_path || !out_buf || max_len == 0) return false;
    BufWriter w(out_buf, max_len);

    if (strcmp(abs_path, "/") == 0) {
        w.putStr("apps/  bin/  dev/  etc/  mnt/  proc/  root/  sys/  tmp/  var/\n");
        return true;
    }
    if (strcmp(abs_path, "/bin") == 0) {
        w.putStr("adc*  apitoken*  ble*  c6-menu*  c6-sh*  cat*  chsh*  crontab*  date*  df*  "
                 "dmesg*  echo*  edit*  elf*  epkg*  fbgrab*  fbi*  free*  gpio*  htop*  i2c*  "
                 "ip*  lua*  ls*  lscpu*  nano*  neofetch*  netstat*  ntpdate*  passwd*  ping*  "
                 "pinout*  ps*  reboot*  rm*  screen*  sensors*  sh*  ss*  top*  tz*  uname*  "
                 "uptime*  vmstat*  watch*  wifi*\n");
        return true;
    }
    if (strcmp(abs_path, "/proc") == 0) {
        w.putStr("cpuinfo  loadavg  meminfo  net/  uptime  version\n");
        return true;
    }
    if (strcmp(abs_path, "/proc/net") == 0) {
        w.putStr("dev  wireless\n");
        return true;
    }
    if (strcmp(abs_path, "/dev") == 0) {
        w.putStr("fb0  i2c-0  input0  led0  null  pts/  tty0  ttyACM0  ttyS0  zero\n");
        return true;
    }
    if (strcmp(abs_path, "/sys") == 0) {
        w.putStr("class/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class") == 0) {
        w.putStr("backlight/  gpio/  graphics/  leds/  thermal/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/backlight") == 0) {
        w.putStr("st7789/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/backlight/st7789") == 0) {
        w.putStr("brightness  max_brightness\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/gpio") == 0) {
        w.putStr("status\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics") == 0) {
        w.putStr("fb0/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0") == 0) {
        w.putStr("alert  mode  name  stride  text  theme  ticker  virtual_size\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds") == 0) {
        w.putStr("ws2812/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812") == 0) {
        w.putStr("mode  rgb\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/thermal") == 0) {
        w.putStr("thermal_zone0/\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/thermal/thermal_zone0") == 0) {
        w.putStr("temp  type\n");
        return true;
    }

    if (!isDirectory(abs_path)) return false;

    VfsDirEntry entries[32];
    size_t count = readDirEntries(abs_path, entries, 32);
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].is_dir) {
            w.fmt("%s/  ", entries[i].name);
        } else {
            w.fmt("%s  ", entries[i].name);
        }
    }
    w.putChar('\n');
    return true;
}

bool VirtualFs::statPath(const char* abs_path, uint64_t* out_size, uint32_t* out_mode, uint32_t* out_mtime) const {
    if (!abs_path) return false;
    if (isDirectory(abs_path)) {
        if (out_size) *out_size = 0;
        if (out_mode) *out_mode = 0040755U;
        if (out_mtime) *out_mtime = 0;
        return true;
    }

    char real_path[224];
    if (resolveRealPath(abs_path, real_path, sizeof(real_path))) {
        struct stat st;
        if (stat(real_path, &st) == 0) {
            if (out_size) *out_size = static_cast<uint64_t>(st.st_size);
            if (out_mode) *out_mode = S_ISDIR(st.st_mode) ? 0040755U : 0100644U;
            if (out_mtime) *out_mtime = static_cast<uint32_t>(st.st_mtime);
            return true;
        }
        return false;
    }

    // Synthetic /proc, /sys, /dev files
    SystemState dummy;
    dummy.initDefaults();
    char probe[256];
    if (readFile(dummy, abs_path, probe, sizeof(probe))) {
        if (out_size) *out_size = strlen(probe);
        if (out_mode) *out_mode = 0100444U;
        if (out_mtime) *out_mtime = 0;
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
                 "flash size\t: %u MB (XMC QIO)\n"
                 "display\t\t: Waveshare 1.47\" ST7789 (320x172 RGB565 @ 80MHz SPI2 DMA)\n",
                 static_cast<unsigned>(state.cpu_freq_mhz),
                 static_cast<unsigned>(state.cpu_freq_mhz),
                 static_cast<unsigned>(state.flash_size_mb));
        return true;
    }
    if (strcmp(abs_path, "/proc/meminfo") == 0) {
        unsigned total_kb = static_cast<unsigned>(state.total_sram_bytes / 1024U);
        unsigned free_kb  = static_cast<unsigned>(state.free_sram_bytes / 1024U);
        unsigned min_kb   = static_cast<unsigned>(state.min_free_sram_bytes / 1024U);
        unsigned used_kb  = (total_kb > free_kb) ? (total_kb - free_kb) : 0U;
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
        snprintf(out_buf, max_len, "%u.00 %u.00\n",
                 static_cast<unsigned>(state.uptime_sec),
                 static_cast<unsigned>(state.uptime_sec));
        return true;
    }
    if (strcmp(abs_path, "/proc/loadavg") == 0) {
        unsigned load_int = state.cpu_load_pct / 100U;
        unsigned load_frac = state.cpu_load_pct % 100U;
        snprintf(out_buf, max_len, "%u.%02u %u.%02u 0.05 1/6 42\n",
                 load_int, load_frac, load_int, load_frac);
        return true;
    }
    if (strcmp(abs_path, "/proc/net/dev") == 0) {
        snprintf(out_buf, max_len,
                 "Inter-|   Receive                                                |  Transmit\n"
                 " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop\n"
                 "    lo:       0       0    0    0    0     0          0         0        0       0    0    0\n"
                 " wlan0:%8u     120    0    0    0     0          0         0 %8u     115    0    0\n"
                 "   ap0:     512       8    0    0    0     0          0         0     1024      10    0    0\n",
                 static_cast<unsigned>(state.net_rx_bytes),
                 static_cast<unsigned>(state.net_tx_bytes));
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
    if (strcmp(abs_path, "/sys/class/graphics/fb0/stride") == 0) {
        snprintf(out_buf, max_len, "640\n");
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
    if (strcmp(abs_path, "/sys/class/graphics/fb0/ticker") == 0) {
        snprintf(out_buf, max_len, "%s\n", state.ticker_enabled ? state.ticker_text : "off");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/alert") == 0) {
        snprintf(out_buf, max_len, "%s\n", state.alert_active ? state.alert_text : "off");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/thermal/thermal_zone0/temp") == 0) {
        snprintf(out_buf, max_len, "%d\n", static_cast<int>(state.cpu_temp_deci_c) * 100);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/thermal/thermal_zone0/type") == 0) {
        snprintf(out_buf, max_len, "esp32c6-tsens\n");
        return true;
    }
    if (strcmp(abs_path, "/sys/class/gpio/status") == 0) {
        snprintf(out_buf, max_len, "out_mask=0x%08x in_mask=0x%08x boot_presses=%u\n",
                 static_cast<unsigned>(state.gpio_out_mask),
                 static_cast<unsigned>(state.gpio_in_mask),
                 static_cast<unsigned>(state.button_press_count));
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
    if (strcmp(abs_path, "/etc/ssh/ssh_host_ed25519_key.pub") == 0) {
        snprintf(out_buf, max_len, "ssh-ed25519 (%s) root@%s\n", state.ssh_hostkey_fp, state.hostname);
        return true;
    }
    if (strcmp(abs_path, "/etc/ssh/sshd_config") == 0) {
        snprintf(out_buf, max_len,
                 "Port %u\n"
                 "HostKey /etc/ssh/ssh_host_ed25519_key\n"
                 "AuthorizedKeysFile /etc/ssh/authorized_keys\n"
                 "KexAlgorithms curve25519-sha256,curve25519-sha256@libssh.org\n"
                 "PubkeyAcceptedAlgorithms ssh-ed25519,rsa-sha2-256,rsa-sha2-512\n"
                 "Ciphers aes128-ctr\n"
                 "MACs hmac-sha2-256\n"
                 "Subsystem sftp internal-sftp\n"
                 "PermitRootLogin yes\n",
                 state.ssh_port);
        return true;
    }
    if (strcmp(abs_path, "/var/log/dmesg") == 0) {
        BufWriter w(out_buf, max_len);
        for (size_t i = 0; i < state.dmesg_count; ++i) {
            w.fmt("%s\n", state.dmesgLineAt(i));
        }
        return true;
    }
    if (strcmp(abs_path, "/dev/tty0") == 0 || strcmp(abs_path, "/dev/fb0") == 0) {
        BufWriter w(out_buf, max_len);
        for (size_t i = 0; i < state.fb_line_count; ++i) {
            w.fmt("%s\n", state.fbLineAt(i));
        }
        return true;
    }
    if (strcmp(abs_path, "/dev/null") == 0) {
        out_buf[0] = '\0';
        return true;
    }

    char real_path[224];
    if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) {
        return false;
    }

    FILE* f = fopen(real_path, "rb");
    if (!f) return false;
    size_t n = fread(out_buf, 1, max_len - 1, f);
    out_buf[n] = '\0';
    fclose(f);
    return true;
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
        state.markDirty(DIRTY_HEADER);
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
        } else if (strcmp(clean, "monitor") == 0 || strcmp(clean, "htop") == 0 || strcmp(clean, "4") == 0) {
            state.screen_mode = ScreenMode::MONITOR;
        } else if (strcmp(clean, "image") == 0 || strcmp(clean, "fbi") == 0 || strcmp(clean, "5") == 0) {
            state.screen_mode = ScreenMode::IMAGE_VIEW;
        } else {
            return false;
        }
        state.markDirty(DIRTY_FULL);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/ticker") == 0) {
        if (strcmp(clean, "off") == 0 || clean[0] == '\0') {
            state.ticker_enabled = false;
        } else {
            state.ticker_enabled = true;
            strncpy(state.ticker_text, clean, sizeof(state.ticker_text) - 1);
            state.ticker_text[sizeof(state.ticker_text) - 1] = '\0';
            state.ticker_offset = 0;
        }
        state.markDirty(DIRTY_TICKER | DIRTY_CARDS);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/alert") == 0) {
        if (strcmp(clean, "off") == 0 || clean[0] == '\0') {
            state.alert_active = false;
        } else {
            state.alert_active = true;
            strncpy(state.alert_text, clean, sizeof(state.alert_text) - 1);
            state.alert_text[sizeof(state.alert_text) - 1] = '\0';
        }
        state.markDirty(DIRTY_FULL);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/theme") == 0) {
        if (strcmp(clean, "cyan") == 0) state.theme = ThemeId::CYBER_CYAN;
        else if (strcmp(clean, "matrix") == 0 || strcmp(clean, "green") == 0) state.theme = ThemeId::MATRIX_GREEN;
        else if (strcmp(clean, "amber") == 0) state.theme = ThemeId::AMBER_CRT;
        else if (strcmp(clean, "red") == 0) state.theme = ThemeId::RED_ALERT;
        else if (strcmp(clean, "mono") == 0 || strcmp(clean, "white") == 0) state.theme = ThemeId::MONO_WHITE;
        else return false;
        state.markDirty(DIRTY_FULL);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/graphics/fb0/text") == 0) {
        strncpy(state.headline, clean, sizeof(state.headline) - 1);
        state.headline[sizeof(state.headline) - 1] = '\0';
        state.markDirty(DIRTY_CARDS | DIRTY_FULL);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/mode") == 0) {
        if (strcmp(clean, "off") == 0) state.led_mode = LedMode::OFF;
        else if (strcmp(clean, "solid") == 0) state.led_mode = LedMode::SOLID;
        else if (strcmp(clean, "pulse") == 0) state.led_mode = LedMode::PULSE;
        else if (strcmp(clean, "rainbow") == 0) state.led_mode = LedMode::RAINBOW;
        else if (strcmp(clean, "ssh") == 0) state.led_mode = LedMode::SSH_ACTIVITY;
        else return false;
        state.markDirty(DIRTY_HEADER);
        return true;
    }
    if (strcmp(abs_path, "/sys/class/leds/ws2812/rgb") == 0) {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(clean, "%u %u %u", &r, &g, &b) == 3) {
            state.led_r = static_cast<uint8_t>(r & 0xFF);
            state.led_g = static_cast<uint8_t>(g & 0xFF);
            state.led_b = static_cast<uint8_t>(b & 0xFF);
            state.led_mode = LedMode::SOLID;
            state.markDirty(DIRTY_HEADER);
            return true;
        }
        return false;
    }

    if (isVirtualPath(abs_path)) {
        return false;
    }

    if (strcmp(abs_path, "/etc/hostname") == 0 && clean[0] != '\0') {
        strncpy(state.hostname, clean, sizeof(state.hostname) - 1);
        state.hostname[sizeof(state.hostname) - 1] = '\0';
        state.markDirty(DIRTY_HEADER);
    } else if (strcmp(abs_path, "/etc/timezone") == 0 && clean[0] != '\0') {
        resolvePosixTz(clean, state.tz_name, sizeof(state.tz_name),
                       state.tz_posix, sizeof(state.tz_posix));
    } else if (strcmp(abs_path, "/etc/passwd") == 0) {
        parseLoginShellFromPasswd(data, state.login_shell, sizeof(state.login_shell));
    }

    return writeBinary(state, abs_path, reinterpret_cast<const uint8_t*>(data), strlen(data), append);
}

FILE* VirtualFs::openPosix(const char* abs_path, const char* mode) const {
    char real_path[224];
    if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) return nullptr;
    if (mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))) {
        ensureParentDirs(real_path);
    }
    return fopen(real_path, mode);
}

bool VirtualFs::fileSize(const char* abs_path, size_t* out_size) const {
    uint64_t sz = 0;
    if (!statPath(abs_path, &sz, nullptr, nullptr)) return false;
    if (out_size) *out_size = static_cast<size_t>(sz);
    return true;
}

bool VirtualFs::readBinary(const char* abs_path, size_t offset, uint8_t* out_buf,
                           size_t max_len, size_t* out_read) const {
    if (out_read) *out_read = 0;
    if (!abs_path || !out_buf) return false;
    FILE* f = openPosix(abs_path, "rb");
    if (!f) return false;
    if (offset > 0 && fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    size_t n = fread(out_buf, 1, max_len, f);
    fclose(f);
    if (out_read) *out_read = n;
    return true;
}

bool VirtualFs::writeBinary(SystemState& state, const char* abs_path,
                            const uint8_t* data, size_t len, bool append) {
    if (!abs_path || (!data && len > 0)) return false;
    FILE* f = openPosix(abs_path, append ? "ab" : "wb");
    if (!f) return false;
    size_t written = (len > 0) ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    if (written != len) return false;
    state.fs_sync_requested = true;
    return true;
}

bool VirtualFs::removeFile(SystemState& state, const char* abs_path) {
    char real_path[224];
    if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) return false;
    if (unlink(real_path) == 0) {
        state.fs_sync_requested = true;
        return true;
    }
    return false;
}

bool VirtualFs::mkdirPath(const char* abs_path) const {
    char real_path[224];
    if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) return false;
    ensureParentDirs(real_path);
    struct stat st;
    if (stat(real_path, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(real_path, 0755) == 0;
}

bool VirtualFs::rmdirPath(const char* abs_path) const {
    char real_path[224];
    if (!resolveRealPath(abs_path, real_path, sizeof(real_path))) return false;
    return rmdir(real_path) == 0;
}

bool VirtualFs::renamePath(const char* old_abs, const char* new_abs) const {
    char old_real[224];
    char new_real[224];
    if (!resolveRealPath(old_abs, old_real, sizeof(old_real))) return false;
    if (!resolveRealPath(new_abs, new_real, sizeof(new_real))) return false;
    ensureParentDirs(new_real);
    unlink(new_real);
    return rename(old_real, new_real) == 0;
}

}  // namespace c6os
