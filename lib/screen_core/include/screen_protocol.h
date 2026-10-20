#ifndef SCREEN_PROTOCOL_H
#define SCREEN_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

namespace screen {

enum class ViewMode : uint8_t {
    DASHBOARD = 0,
    HOST_MONITOR = 1,
    WIRELESS = 2,
    COUNT = 3
};

enum class RgbMode : uint8_t {
    RAINBOW = 0,
    PULSE = 1,
    SOLID = 2,
    OFF = 3,
    COUNT = 4
};

struct RgbColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct ScreenState {
    ViewMode view_mode = ViewMode::DASHBOARD;
    RgbMode rgb_mode = RgbMode::RAINBOW;
    RgbColor solid_color = {0, 180, 255};
    uint8_t backlight_pct = 100;

    // Hardware & wireless metrics
    uint16_t flash_mb = 8;
    uint32_t free_heap = 0;
    uint32_t total_heap = 0;
    bool sd_mounted = false;
    uint32_t sd_total_mb = 0;
    int16_t wifi_count = 0;
    int16_t ble_count = 0;
    uint32_t uptime_sec = 0;
    bool scan_requested = false;
    bool ui_dirty = true;

    // Host telemetry & custom banner
    uint8_t host_cpu_pct = 0;
    uint8_t host_ram_pct = 0;
    char host_ip[24] = "0.0.0.0";
    char custom_text[64] = "Waveshare ESP32-C6 Ready";
};

// Parse a single line command from USB Serial and update state.
// Writes a single-line response into `response` (null-terminated).
// Returns true if the command was valid, false on syntax/unknown command error.
bool parseSerialCommand(const char* line, ScreenState& state, char* response, size_t resp_len);

// Format current state as a compact single-line JSON string.
void formatStatusJson(const ScreenState& state, char* out, size_t out_len);

// Cycle to the next view mode (used by short press on BOOT button).
ViewMode nextViewMode(ViewMode current);

// Cycle backlight brightness across preset steps: 25 -> 50 -> 75 -> 100 -> 25 (used by long press).
uint8_t nextBacklightPreset(uint8_t current_pct);

// Compute a 24-bit RGB color from a 0..255 rainbow wheel position with brightness scaling (0..255).
RgbColor wheelColor(uint8_t wheel_pos, uint8_t brightness_scale = 64);

// Compute pixel bar width clamped to [0, max_width] for a percentage [0..100].
uint16_t computeBarWidth(uint8_t pct, uint16_t max_width);

}  // namespace screen

#endif  // SCREEN_PROTOCOL_H
