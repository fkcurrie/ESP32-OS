#include <string.h>
#include <unity.h>

#include "screen_protocol.h"

using screen::computeBarWidth;
using screen::formatStatusJson;
using screen::nextBacklightPreset;
using screen::nextViewMode;
using screen::parseSerialCommand;
using screen::RgbMode;
using screen::ScreenState;
using screen::ViewMode;
using screen::wheelColor;

void setUp(void) {}
void tearDown(void) {}

void test_ping_command(void) {
    ScreenState state;
    char resp[128] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("  ping \r\n", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_STRING("OK PONG", resp);
}

void test_status_command_json(void) {
    ScreenState state;
    state.wifi_count = 7;
    state.ble_count = 33;
    state.backlight_pct = 85;
    char resp[512] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("STATUS", state, resp, sizeof(resp)));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"board\":\"Waveshare-ESP32-C6-LCD-1.47\""));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"wifi\":7"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"ble\":33"));
    TEST_ASSERT_NOT_NULL(strstr(resp, "\"bl\":85"));
}

void test_text_command(void) {
    ScreenState state;
    state.ui_dirty = false;
    char resp[128] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("TEXT Deploy Complete!", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_STRING("Deploy Complete!", state.custom_text);
    TEST_ASSERT_TRUE(state.ui_dirty);
    TEST_ASSERT_EQUAL_STRING("OK TEXT Deploy Complete!", resp);

    TEST_ASSERT_FALSE(parseSerialCommand("TEXT   ", state, resp, sizeof(resp)));
}

void test_bl_command_and_clamping(void) {
    ScreenState state;
    char resp[128] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("BL 65", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT8(65, state.backlight_pct);

    TEST_ASSERT_TRUE(parseSerialCommand("BL 150", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT8(100, state.backlight_pct);

    TEST_ASSERT_TRUE(parseSerialCommand("BL -10", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT8(0, state.backlight_pct);

    TEST_ASSERT_FALSE(parseSerialCommand("BL abc", state, resp, sizeof(resp)));
}

void test_rgb_and_mode_commands(void) {
    ScreenState state;
    char resp[128] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("RGB 12 128 255", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT8(12, state.solid_color.r);
    TEST_ASSERT_EQUAL_UINT8(128, state.solid_color.g);
    TEST_ASSERT_EQUAL_UINT8(255, state.solid_color.b);
    TEST_ASSERT_EQUAL(RgbMode::SOLID, state.rgb_mode);

    TEST_ASSERT_TRUE(parseSerialCommand("MODE pulse", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL(RgbMode::PULSE, state.rgb_mode);

    TEST_ASSERT_FALSE(parseSerialCommand("MODE invalid", state, resp, sizeof(resp)));
    TEST_ASSERT_FALSE(parseSerialCommand("RGB 10 20", state, resp, sizeof(resp)));
}

void test_host_and_view_commands(void) {
    ScreenState state;
    char resp[128] = {0};
    TEST_ASSERT_TRUE(parseSerialCommand("HOST 42 68 192.168.100.200", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL_UINT8(42, state.host_cpu_pct);
    TEST_ASSERT_EQUAL_UINT8(68, state.host_ram_pct);
    TEST_ASSERT_EQUAL_STRING("192.168.100.200", state.host_ip);

    TEST_ASSERT_TRUE(parseSerialCommand("VIEW host", state, resp, sizeof(resp)));
    TEST_ASSERT_EQUAL(ViewMode::HOST_MONITOR, state.view_mode);

    TEST_ASSERT_FALSE(parseSerialCommand("VIEW bogus", state, resp, sizeof(resp)));
}

void test_scan_and_unknown_commands(void) {
    ScreenState state;
    char resp[128] = {0};
    TEST_ASSERT_FALSE(state.scan_requested);
    TEST_ASSERT_TRUE(parseSerialCommand("SCAN", state, resp, sizeof(resp)));
    TEST_ASSERT_TRUE(state.scan_requested);

    TEST_ASSERT_FALSE(state.landscape_flipped);
    TEST_ASSERT_TRUE(parseSerialCommand("FLIP", state, resp, sizeof(resp)));
    TEST_ASSERT_TRUE(state.landscape_flipped);
    TEST_ASSERT_TRUE(parseSerialCommand("FLIP 0", state, resp, sizeof(resp)));
    TEST_ASSERT_FALSE(state.landscape_flipped);

    TEST_ASSERT_FALSE(parseSerialCommand("FOOBAR 123", state, resp, sizeof(resp)));
    TEST_ASSERT_FALSE(parseSerialCommand("   ", state, resp, sizeof(resp)));
}

void test_ui_helpers(void) {
    TEST_ASSERT_EQUAL(ViewMode::HOST_MONITOR, nextViewMode(ViewMode::DASHBOARD));
    TEST_ASSERT_EQUAL(ViewMode::WIRELESS, nextViewMode(ViewMode::HOST_MONITOR));
    TEST_ASSERT_EQUAL(ViewMode::DASHBOARD, nextViewMode(ViewMode::WIRELESS));

    TEST_ASSERT_EQUAL_UINT8(25, nextBacklightPreset(100));
    TEST_ASSERT_EQUAL_UINT8(50, nextBacklightPreset(25));
    TEST_ASSERT_EQUAL_UINT8(75, nextBacklightPreset(50));
    TEST_ASSERT_EQUAL_UINT8(100, nextBacklightPreset(75));

    TEST_ASSERT_EQUAL_UINT16(0, computeBarWidth(0, 140));
    TEST_ASSERT_EQUAL_UINT16(70, computeBarWidth(50, 140));
    TEST_ASSERT_EQUAL_UINT16(140, computeBarWidth(100, 140));
    TEST_ASSERT_EQUAL_UINT16(140, computeBarWidth(150, 140));

    auto c0 = wheelColor(0, 255);
    auto c85 = wheelColor(85, 255);
    TEST_ASSERT_TRUE(c0.r > 0 || c0.g > 0 || c0.b > 0);
    TEST_ASSERT_TRUE(c85.r > 0 || c85.g > 0 || c85.b > 0);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_ping_command);
    RUN_TEST(test_status_command_json);
    RUN_TEST(test_text_command);
    RUN_TEST(test_bl_command_and_clamping);
    RUN_TEST(test_rgb_and_mode_commands);
    RUN_TEST(test_host_and_view_commands);
    RUN_TEST(test_scan_and_unknown_commands);
    RUN_TEST(test_ui_helpers);
    return UNITY_END();
}
