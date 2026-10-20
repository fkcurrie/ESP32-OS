#include "screen_protocol.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace screen {

namespace {

const char* viewModeToString(ViewMode mode) {
    switch (mode) {
        case ViewMode::DASHBOARD:
            return "dashboard";
        case ViewMode::HOST_MONITOR:
            return "host";
        case ViewMode::WIRELESS:
            return "wireless";
        default:
            return "dashboard";
    }
}

const char* rgbModeToString(RgbMode mode) {
    switch (mode) {
        case RgbMode::RAINBOW:
            return "rainbow";
        case RgbMode::PULSE:
            return "pulse";
        case RgbMode::SOLID:
            return "solid";
        case RgbMode::OFF:
            return "off";
        default:
            return "rainbow";
    }
}

const char* weatherKindToString(WeatherKind kind) {
    switch (kind) {
        case WeatherKind::SUNNY:
            return "sunny";
        case WeatherKind::PARTLY_CLOUDY:
            return "partly";
        case WeatherKind::CLOUDY:
            return "cloudy";
        case WeatherKind::RAIN:
            return "rain";
        case WeatherKind::LIGHTNING:
            return "lightning";
        case WeatherKind::LIGHT_SNOW:
            return "light_snow";
        case WeatherKind::HEAVY_SNOW:
            return "heavy_snow";
        default:
            return "sunny";
    }
}

bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool parseWeatherKind(const char* str, WeatherKind* out) {
    if (!str || !out) return false;
    if (equalsIgnoreCase(str, "sunny") || equalsIgnoreCase(str, "clear")) {
        *out = WeatherKind::SUNNY;
        return true;
    }
    if (equalsIgnoreCase(str, "partly") || equalsIgnoreCase(str, "partly_cloudy")) {
        *out = WeatherKind::PARTLY_CLOUDY;
        return true;
    }
    if (equalsIgnoreCase(str, "cloudy") || equalsIgnoreCase(str, "overcast")) {
        *out = WeatherKind::CLOUDY;
        return true;
    }
    if (equalsIgnoreCase(str, "rain") || equalsIgnoreCase(str, "drizzle")) {
        *out = WeatherKind::RAIN;
        return true;
    }
    if (equalsIgnoreCase(str, "lightning") || equalsIgnoreCase(str, "storm") || equalsIgnoreCase(str, "thunder")) {
        *out = WeatherKind::LIGHTNING;
        return true;
    }
    if (equalsIgnoreCase(str, "light_snow") || equalsIgnoreCase(str, "snow")) {
        *out = WeatherKind::LIGHT_SNOW;
        return true;
    }
    if (equalsIgnoreCase(str, "heavy_snow") || equalsIgnoreCase(str, "blizzard")) {
        *out = WeatherKind::HEAVY_SNOW;
        return true;
    }
    return false;
}

bool parseIntInRange(const char* str, int min_val, int max_val, int* out) {
    if (!str || *str == '\0') {
        return false;
    }
    char* endptr = nullptr;
    long val = strtol(str, &endptr, 10);
    if (*endptr != '\0') {
        return false;
    }
    if (val < min_val) {
        val = min_val;
    } else if (val > max_val) {
        val = max_val;
    }
    *out = static_cast<int>(val);
    return true;
}

void copySanitizedJsonString(const char* src, char* dst, size_t dst_len) {
    if (dst_len == 0) {
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_len; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            c = '\'';
        } else if (static_cast<unsigned char>(c) < 0x20) {
            c = ' ';
        }
        dst[j++] = c;
    }
    dst[j] = '\0';
}

}  // namespace

const char* weatherKindLabel(WeatherKind kind) {
    switch (kind) {
        case WeatherKind::SUNNY:
            return "SUNNY";
        case WeatherKind::PARTLY_CLOUDY:
            return "PART CLOUD";
        case WeatherKind::CLOUDY:
            return "CLOUDY";
        case WeatherKind::RAIN:
            return "RAIN";
        case WeatherKind::LIGHTNING:
            return "LIGHTNING";
        case WeatherKind::LIGHT_SNOW:
            return "LIGHT SNOW";
        case WeatherKind::HEAVY_SNOW:
            return "HEAVY SNOW";
        default:
            return "SUNNY";
    }
}

void formatStatusJson(const ScreenState& state, char* out, size_t out_len) {
    char safe_text[64];
    char safe_ip[24];
    copySanitizedJsonString(state.custom_text, safe_text, sizeof(safe_text));
    copySanitizedJsonString(state.host_ip, safe_ip, sizeof(safe_ip));

    snprintf(
        out, out_len,
        "{\"board\":\"Waveshare-ESP32-C6-LCD-1.47\",\"view\":\"%s\",\"rgb_mode\":\"%s\","
        "\"rgb\":[%u,%u,%u],\"bl\":%u,\"flipped\":%s,\"flash_mb\":%u,\"free_heap\":%lu,\"total_heap\":%lu,"
        "\"sd_mounted\":%s,\"sd_mb\":%lu,\"wifi\":%d,\"ble\":%d,\"uptime_s\":%lu,"
        "\"weather\":\"%s\",\"temp_c\":%d,\"high_c\":%d,\"low_c\":%d,"
        "\"host_cpu\":%u,\"host_ram\":%u,\"host_ip\":\"%s\",\"text\":\"%s\"}",
        viewModeToString(state.view_mode),
        rgbModeToString(state.rgb_mode),
        static_cast<unsigned>(state.solid_color.r),
        static_cast<unsigned>(state.solid_color.g),
        static_cast<unsigned>(state.solid_color.b),
        static_cast<unsigned>(state.backlight_pct),
        state.landscape_flipped ? "true" : "false",
        static_cast<unsigned>(state.flash_mb),
        static_cast<unsigned long>(state.free_heap),
        static_cast<unsigned long>(state.total_heap),
        state.sd_mounted ? "true" : "false",
        static_cast<unsigned long>(state.sd_total_mb),
        static_cast<int>(state.wifi_count),
        static_cast<int>(state.ble_count),
        static_cast<unsigned long>(state.uptime_sec),
        weatherKindToString(state.weather_kind),
        static_cast<int>(state.weather_temp_c),
        static_cast<int>(state.weather_high_c),
        static_cast<int>(state.weather_low_c),
        static_cast<unsigned>(state.host_cpu_pct),
        static_cast<unsigned>(state.host_ram_pct),
        safe_ip,
        safe_text);
}

bool parseSerialCommand(const char* line, ScreenState& state, char* response, size_t resp_len) {
    if (!line || !response || resp_len == 0) {
        return false;
    }

    // Skip leading whitespace
    while (*line && isspace(static_cast<unsigned char>(*line))) {
        ++line;
    }

    // Copy into mutable buffer and strip trailing whitespace/CR/LF
    char buf[160];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    while (len > 0 && isspace(static_cast<unsigned char>(buf[len - 1]))) {
        buf[--len] = '\0';
    }

    if (len == 0) {
        snprintf(response, resp_len, "ERR empty command");
        return false;
    }

    // Extract verb
    char* space = strchr(buf, ' ');
    const char* args = "";
    if (space) {
        *space = '\0';
        args = space + 1;
        while (*args && isspace(static_cast<unsigned char>(*args))) {
            ++args;
        }
    }

    if (equalsIgnoreCase(buf, "PING")) {
        snprintf(response, resp_len, "OK PONG");
        return true;
    }

    if (equalsIgnoreCase(buf, "STATUS")) {
        formatStatusJson(state, response, resp_len);
        return true;
    }

    if (equalsIgnoreCase(buf, "SCAN")) {
        state.scan_requested = true;
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK SCAN_QUEUED");
        return true;
    }

    if (equalsIgnoreCase(buf, "FLIP")) {
        if (*args == '\0') {
            state.landscape_flipped = !state.landscape_flipped;
        } else if (equalsIgnoreCase(args, "1") || equalsIgnoreCase(args, "true")) {
            state.landscape_flipped = true;
        } else if (equalsIgnoreCase(args, "0") || equalsIgnoreCase(args, "false")) {
            state.landscape_flipped = false;
        } else {
            snprintf(response, resp_len, "ERR FLIP accepts empty, 0, or 1");
            return false;
        }
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK FLIP %u", state.landscape_flipped ? 1U : 0U);
        return true;
    }

    if (equalsIgnoreCase(buf, "TEXT")) {
        if (*args == '\0') {
            snprintf(response, resp_len, "ERR TEXT requires message");
            return false;
        }
        strncpy(state.custom_text, args, sizeof(state.custom_text) - 1);
        state.custom_text[sizeof(state.custom_text) - 1] = '\0';
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK TEXT %s", state.custom_text);
        return true;
    }

    if (equalsIgnoreCase(buf, "BL")) {
        int val = 0;
        if (!parseIntInRange(args, 0, 100, &val)) {
            snprintf(response, resp_len, "ERR BL requires integer 0..100");
            return false;
        }
        state.backlight_pct = static_cast<uint8_t>(val);
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK BL %u", static_cast<unsigned>(state.backlight_pct));
        return true;
    }

    if (equalsIgnoreCase(buf, "VIEW")) {
        if (equalsIgnoreCase(args, "dashboard")) {
            state.view_mode = ViewMode::DASHBOARD;
        } else if (equalsIgnoreCase(args, "host")) {
            state.view_mode = ViewMode::HOST_MONITOR;
        } else if (equalsIgnoreCase(args, "wireless")) {
            state.view_mode = ViewMode::WIRELESS;
        } else {
            snprintf(response, resp_len, "ERR VIEW must be dashboard|host|wireless");
            return false;
        }
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK VIEW %s", viewModeToString(state.view_mode));
        return true;
    }

    if (equalsIgnoreCase(buf, "MODE")) {
        if (equalsIgnoreCase(args, "rainbow")) {
            state.rgb_mode = RgbMode::RAINBOW;
        } else if (equalsIgnoreCase(args, "pulse")) {
            state.rgb_mode = RgbMode::PULSE;
        } else if (equalsIgnoreCase(args, "solid")) {
            state.rgb_mode = RgbMode::SOLID;
        } else if (equalsIgnoreCase(args, "off")) {
            state.rgb_mode = RgbMode::OFF;
        } else {
            snprintf(response, resp_len, "ERR MODE must be rainbow|pulse|solid|off");
            return false;
        }
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK MODE %s", rgbModeToString(state.rgb_mode));
        return true;
    }

    if (equalsIgnoreCase(buf, "RGB")) {
        char arg_buf[64];
        strncpy(arg_buf, args, sizeof(arg_buf) - 1);
        arg_buf[sizeof(arg_buf) - 1] = '\0';

        char* saveptr = nullptr;
        char* tok_r = strtok_r(arg_buf, " ", &saveptr);
        char* tok_g = strtok_r(nullptr, " ", &saveptr);
        char* tok_b = strtok_r(nullptr, " ", &saveptr);
        char* tok_extra = strtok_r(nullptr, " ", &saveptr);

        int r = 0, g = 0, b = 0;
        if (!tok_r || !tok_g || !tok_b || tok_extra ||
            !parseIntInRange(tok_r, 0, 255, &r) ||
            !parseIntInRange(tok_g, 0, 255, &g) ||
            !parseIntInRange(tok_b, 0, 255, &b)) {
            snprintf(response, resp_len, "ERR RGB requires 3 integers (r g b)");
            return false;
        }

        state.solid_color = {
            static_cast<uint8_t>(r),
            static_cast<uint8_t>(g),
            static_cast<uint8_t>(b)};
        state.rgb_mode = RgbMode::SOLID;
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK RGB %d %d %d", r, g, b);
        return true;
    }

    if (equalsIgnoreCase(buf, "HOST")) {
        char arg_buf[96];
        strncpy(arg_buf, args, sizeof(arg_buf) - 1);
        arg_buf[sizeof(arg_buf) - 1] = '\0';

        char* saveptr = nullptr;
        char* tok_cpu = strtok_r(arg_buf, " ", &saveptr);
        char* tok_ram = strtok_r(nullptr, " ", &saveptr);
        char* tok_ip = strtok_r(nullptr, " ", &saveptr);

        int cpu = 0, ram = 0;
        if (!tok_cpu || !tok_ram || !tok_ip ||
            !parseIntInRange(tok_cpu, 0, 100, &cpu) ||
            !parseIntInRange(tok_ram, 0, 100, &ram)) {
            snprintf(response, resp_len, "ERR HOST requires <cpu_pct> <ram_pct> <ip>");
            return false;
        }

        state.host_cpu_pct = static_cast<uint8_t>(cpu);
        state.host_ram_pct = static_cast<uint8_t>(ram);
        strncpy(state.host_ip, tok_ip, sizeof(state.host_ip) - 1);
        state.host_ip[sizeof(state.host_ip) - 1] = '\0';
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK HOST %d %d %s", cpu, ram, state.host_ip);
        return true;
    }

    if (equalsIgnoreCase(buf, "TIME")) {
        int hh = 0, mm = 0, ss = 0, ds = 0;
        int matched = sscanf(args, "%d:%d:%d.%d", &hh, &mm, &ss, &ds);
        if (matched < 3 || hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 59 || ds < 0 || ds > 9) {
            snprintf(response, resp_len, "ERR TIME requires HH:MM:SS[.d]");
            return false;
        }
        state.clock_base_ds = static_cast<uint32_t>((hh * 3600L + mm * 60L + ss) * 10L + ds);
        state.clock_sync_requested = true;
        state.ui_dirty = true;
        snprintf(response, resp_len, "OK TIME %02d:%02d:%02d.%d", hh, mm, ss, ds);
        return true;
    }

    if (equalsIgnoreCase(buf, "WEATHER")) {
        char arg_buf[96];
        strncpy(arg_buf, args, sizeof(arg_buf) - 1);
        arg_buf[sizeof(arg_buf) - 1] = '\0';

        char* saveptr = nullptr;
        char* tok_kind = strtok_r(arg_buf, " ", &saveptr);
        char* tok_temp = strtok_r(nullptr, " ", &saveptr);
        char* tok_high = strtok_r(nullptr, " ", &saveptr);
        char* tok_low = strtok_r(nullptr, " ", &saveptr);

        WeatherKind kind = WeatherKind::SUNNY;
        int temp_c = 0, high_c = 0, low_c = 0;
        if (!tok_kind || !tok_temp || !tok_high || !tok_low ||
            !parseWeatherKind(tok_kind, &kind) ||
            !parseIntInRange(tok_temp, -60, 60, &temp_c) ||
            !parseIntInRange(tok_high, -60, 60, &high_c) ||
            !parseIntInRange(tok_low, -60, 60, &low_c)) {
            snprintf(response, resp_len, "ERR WEATHER requires <kind> <temp_c> <high_c> <low_c>");
            return false;
        }

        state.weather_kind = kind;
        state.weather_temp_c = static_cast<int16_t>(temp_c);
        state.weather_high_c = static_cast<int16_t>(high_c);
        state.weather_low_c = static_cast<int16_t>(low_c);
        state.ui_dirty = true;
        snprintf(
            response,
            resp_len,
            "OK WEATHER %s %d %d %d",
            weatherKindToString(state.weather_kind),
            temp_c,
            high_c,
            low_c);
        return true;
    }

    snprintf(response, resp_len, "ERR unknown command: %s", buf);
    return false;
}

ViewMode nextViewMode(ViewMode current) {
    uint8_t next = (static_cast<uint8_t>(current) + 1) % static_cast<uint8_t>(ViewMode::COUNT);
    return static_cast<ViewMode>(next);
}

uint8_t nextBacklightPreset(uint8_t current_pct) {
    if (current_pct < 25) return 25;
    if (current_pct < 50) return 50;
    if (current_pct < 75) return 75;
    if (current_pct < 100) return 100;
    return 25;
}

RgbColor wheelColor(uint8_t wheel_pos, uint8_t brightness_scale) {
    uint8_t r = 0, g = 0, b = 0;
    uint8_t pos = 255 - wheel_pos;
    if (pos < 85) {
        r = static_cast<uint8_t>(255 - pos * 3);
        g = 0;
        b = static_cast<uint8_t>(pos * 3);
    } else if (pos < 170) {
        pos -= 85;
        r = 0;
        g = static_cast<uint8_t>(pos * 3);
        b = static_cast<uint8_t>(255 - pos * 3);
    } else {
        pos -= 170;
        r = static_cast<uint8_t>(pos * 3);
        g = static_cast<uint8_t>(255 - pos * 3);
        b = 0;
    }
    return {
        static_cast<uint8_t>((static_cast<uint16_t>(r) * brightness_scale) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(g) * brightness_scale) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(b) * brightness_scale) / 255)};
}

uint16_t computeBarWidth(uint8_t pct, uint16_t max_width) {
    if (pct >= 100) {
        return max_width;
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(pct) * max_width) / 100);
}

}  // namespace screen
