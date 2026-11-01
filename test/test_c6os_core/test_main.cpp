#include <string.h>
#include <unity.h>

#include "c6os_shell.h"
#include "c6os_types.h"
#include "c6os_vfs.h"
#include "ssh_packet.h"

using namespace c6os;

void setUp(void) {}
void tearDown(void) {}

void test_vfs_proc_and_sysfs_hooks(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    char buf[512];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/proc/version", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "6.12.0-c6os-riscv32"));

    TEST_ASSERT_TRUE(vfs.readFile(state, "/proc/cpuinfo", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "rv32imac"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "ESP32-C6FH4"));

    // Write to sysfs backlight and verify state updates
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/sys/class/backlight/st7789/brightness", "65\n"));
    TEST_ASSERT_EQUAL_UINT8(65, state.backlight_pct);

    // Write to sysfs fb0 mode
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/sys/class/graphics/fb0/mode", "signboard"));
    TEST_ASSERT_EQUAL(ScreenMode::SIGNBOARD, state.screen_mode);

    // Write to /dev/tty0 and verify framebuffer console buffer
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/dev/tty0", "Hello LCD Console", true));
    TEST_ASSERT_TRUE(vfs.readFile(state, "/dev/tty0", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Hello LCD Console"));
}

void test_vfs_path_normalization_and_user_files(void) {
    char norm[64];
    VirtualFs::normalizePath("/root", "../etc/hostname", norm, sizeof(norm));
    TEST_ASSERT_EQUAL_STRING("/etc/hostname", norm);

    VirtualFs::normalizePath("/etc/ssh", "../../proc/./meminfo", norm, sizeof(norm));
    TEST_ASSERT_EQUAL_STRING("/proc/meminfo", norm);

    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/root/notes.txt", "Test note\n", false));
    char buf[256];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/root/notes.txt", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Test note\n", buf);
}

void test_ssh_mpint_and_packet_framing(void) {
    uint8_t buf[128];
    ssh::SshWriter w(buf, sizeof(buf));

    // High bit set (0x80) requires a leading 0x00 byte in mpint
    const uint8_t secret_msb[4] = {0x85, 0x11, 0x22, 0x33};
    TEST_ASSERT_TRUE(w.putMpint(secret_msb, 4));
    TEST_ASSERT_EQUAL_UINT32(9, w.length());
    ssh::SshReader r(buf, w.length());
    const uint8_t* str_ptr = nullptr;
    size_t str_len = 0;
    TEST_ASSERT_TRUE(r.getString(str_ptr, str_len));
    TEST_ASSERT_EQUAL_UINT32(5, str_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, str_ptr[0]);
    TEST_ASSERT_EQUAL_UINT8(0x85, str_ptr[1]);

    // Packet framing alignment to 16-byte AES block size
    const uint8_t payload[5] = {ssh::SSH_MSG_NEWKEYS, 1, 2, 3, 4};
    uint8_t framed[64];
    size_t total = ssh::framePacket(payload, sizeof(payload), 16, nullptr, framed, sizeof(framed));
    TEST_ASSERT_TRUE(total > 0);
    TEST_ASSERT_EQUAL_UINT32(0, total % 16);
}

void test_interactive_menu_and_shell_commands(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, /*start_in_menu=*/true, out, sizeof(out));
    TEST_ASSERT_EQUAL(SessionMode::MENU_MAIN, session.mode());
    TEST_ASSERT_NOT_NULL(strstr(out, "Interactive Screen Controller"));

    // Press '1' in menu -> cycles screen mode from OS_DASHBOARD to FB_CONSOLE
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "1", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::FB_CONSOLE, state.screen_mode);

    // Press '3' in menu -> cycles theme from CYBER_CYAN to MATRIX_GREEN
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "3", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ThemeId::MATRIX_GREEN, state.theme);

    // Press '-' in menu -> decreases backlight from 100% to 90%
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "-", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(90, state.backlight_pct);

    // Press '2' in menu -> enters headline prompt, then subtitle prompt
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "2", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_PROMPT_HEAD, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "SSH Controlled\n", 15, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_PROMPT_SUB, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "Custom Subtitle\n", 16, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_MAIN, session.mode());
    TEST_ASSERT_EQUAL_STRING("SSH Controlled", state.headline);
    TEST_ASSERT_EQUAL_STRING("Custom Subtitle", state.subtitle);

    // Press 's' in menu -> drops to POSIX shell
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "s", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());

    // Run shell command chain
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "uname -a && echo 45 > /sys/class/backlight/st7789/brightness && screen flip on",
        out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "6.12.0-c6os-riscv32"));
    TEST_ASSERT_EQUAL_UINT8(45, state.backlight_pct);
    TEST_ASSERT_TRUE(state.landscape_flipped);

    // Test batch top / htop and interactive htop mode transitions
    char big_out[4096];
    big_out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "top -b -n 1", big_out, sizeof(big_out)));
    TEST_ASSERT_NOT_NULL(strstr(big_out, "Tasks:"));
    TEST_ASSERT_NOT_NULL(strstr(big_out, "/usr/sbin/sshd"));

    // Launch interactive htop from SHELL_CLI
    big_out[0] = '\0';
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "htop\n", 5, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::HTOP_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.isInteractiveMonitor());
    TEST_ASSERT_NOT_NULL(strstr(big_out, "F10"));

    // Press 'M' in htop to sort by MEM%, then 'q' to return to SHELL_CLI
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "M", 1, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::HTOP_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "q", 1, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());
    TEST_ASSERT_FALSE(session.isInteractiveMonitor());
}

void test_new_ten_os_features(void) {
    SystemState state;
    state.initDefaults();
    state.cpu_temp_deci_c = 425;  // 42.5 C
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[4096];
    session.begin(state, vfs, /*start_in_menu=*/false, out, sizeof(out));

    // 1. On-LCD htop monitor mode
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen mode monitor", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::MONITOR, state.screen_mode);

    // 2. Hardware thermal sensor CLI & sysfs
    out[0] = '\0';
    TEST_ASSERT_TRUE(vfs.readFile(state, "/sys/class/thermal/thermal_zone0/temp", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("42500\n", out);
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "sensors", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "+42.5"));

    // 3. Cron scheduler & watch
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "crontab -a 30 screen text Cron OK && crontab -l", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(1, state.activeCronCount());
    TEST_ASSERT_NOT_NULL(strstr(out, "every   30s"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "watch -b -n 2 sensors", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "Every 2.0s:"));
    TEST_ASSERT_NOT_NULL(strstr(out, "sensors"));
    TEST_ASSERT_NOT_NULL(strstr(out, "+42.5"));

    // 4. Framebuffer screenshot (fbgrab) & image viewer (fbi)
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "fbi tux Linux Mascot", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::IMAGE_VIEW, state.screen_mode);
    TEST_ASSERT_EQUAL_UINT8(1, state.fbi_pattern);
    TEST_ASSERT_EQUAL_STRING("Linux Mascot", state.fbi_caption);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "fbgrab /root/snap.ppm", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "captured /dev/fb0"));

    // 5. Scrolling ticker & flashing alert banner
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "screen ticker Welcome to C6-MicroLinux && screen alert High Load", out, sizeof(out)));
    TEST_ASSERT_TRUE(state.ticker_enabled);
    TEST_ASSERT_EQUAL_STRING("Welcome to C6-MicroLinux", state.ticker_text);
    TEST_ASSERT_TRUE(state.alert_active);
    TEST_ASSERT_EQUAL_STRING("High Load", state.alert_text);

    // 7. Pinout & GPIO / PWM control (protecting SPI2 LCD pins)
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "pinout", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "Waveshare ESP32-C6-LCD-1.47"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio write 14 1", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "reserved for ST7789 SPI2"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio pwm 2 128", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(128, state.gpio_cmd_val);

    // 8. Network diagnostic suite (date, ping, netstat)
    state.wifi_sta_connected = true;
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "date && ping -c 2 192.168.18.1 && netstat", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "0% packet loss"));
    TEST_ASSERT_NOT_NULL(strstr(out, "0.0.0.0:80"));
    TEST_ASSERT_NOT_NULL(strstr(out, "0.0.0.0:514"));

    // 9. Interactive editor & shell script runner (sh)
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "edit /root/hello.sh\n", 20, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::EDIT_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, ":clear\n", 7, out, sizeof(out)));
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "screen text Scripted | Run via sh\n", 34, out, sizeof(out)));
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, ":wq\n", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "sh /root/hello.sh", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Scripted", state.headline);
    TEST_ASSERT_EQUAL_STRING("Run via sh", state.subtitle);

    // 10. Logger / syslogd integration
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "logger Deployment verified", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(1, state.syslog_packets);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_vfs_proc_and_sysfs_hooks);
    RUN_TEST(test_vfs_path_normalization_and_user_files);
    RUN_TEST(test_ssh_mpint_and_packet_framing);
    RUN_TEST(test_interactive_menu_and_shell_commands);
    RUN_TEST(test_new_ten_os_features);
    return UNITY_END();
}
