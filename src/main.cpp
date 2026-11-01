#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>

#include "board_pins.h"
#include "screen_protocol.h"
#include "st7789_lcd.h"

namespace {

lcd::St7789Lcd g_lcd;
screen::ScreenState g_state;
SemaphoreHandle_t g_state_mutex = nullptr;

screen::ViewMode g_last_rendered_view = screen::ViewMode::COUNT;
uint8_t g_last_backlight_pct = 100;
bool g_last_landscape_flipped = false;
uint8_t g_rainbow_pos = 0;

// Button debounce & press timing
bool g_btn_pressed = false;
uint32_t g_btn_press_start_ms = 0;
constexpr uint32_t BTN_DEBOUNCE_MS   = 40;
constexpr uint32_t BTN_LONG_PRESS_MS = 700;

void initSdCard() {
    pinMode(board::PIN_SD_CS, OUTPUT);
    digitalWrite(board::PIN_SD_CS, HIGH);
    if (SD.begin(board::PIN_SD_CS, SPI, board::SD_SPI_HZ)) {
        uint8_t card_type = SD.cardType();
        if (card_type != CARD_NONE) {
            g_state.sd_mounted = true;
            g_state.sd_total_mb = static_cast<uint32_t>(SD.cardSize() / (1024ULL * 1024ULL));
            return;
        }
    }
    g_state.sd_mounted = false;
    g_state.sd_total_mb = 0;
}

void wirelessScanTask(void* param) {
    (void)param;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    BLEDevice::init("ESP32C6-LCD");
    BLEScan* ble_scan = BLEDevice::getScan();
    ble_scan->setActiveScan(true);
    ble_scan->setInterval(100);
    ble_scan->setWindow(99);

    bool first_run = true;
    uint32_t last_scan_ms = 0;

    while (true) {
        bool should_scan = first_run || (millis() - last_scan_ms >= 30000);
        if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_state.scan_requested) {
                should_scan = true;
                g_state.scan_requested = false;
            }
            xSemaphoreGive(g_state_mutex);
        }

        if (should_scan) {
            first_run = false;
            int16_t wifi_n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
            WiFi.scanDelete();

            int16_t ble_n = 0;
            BLEScanResults* results = ble_scan->start(2, false);
            if (results) {
                ble_n = static_cast<int16_t>(results->getCount());
                ble_scan->clearResults();
            }

            if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_state.wifi_count = (wifi_n >= 0) ? wifi_n : 0;
                g_state.ble_count = ble_n;
                g_state.ui_dirty = true;
                xSemaphoreGive(g_state_mutex);
            }
            last_scan_ms = millis();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void updateRgbLed() {
    screen::RgbMode mode;
    screen::RgbColor solid;
    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        mode = g_state.rgb_mode;
        solid = g_state.solid_color;
        xSemaphoreGive(g_state_mutex);
    } else {
        return;
    }

    switch (mode) {
        case screen::RgbMode::RAINBOW: {
            auto c = screen::wheelColor(g_rainbow_pos++, 48);
            rgbLedWrite(board::PIN_RGB_LED, c.r, c.g, c.b);
            break;
        }
        case screen::RgbMode::PULSE: {
            uint8_t phase = g_rainbow_pos++;
            uint8_t level = (phase < 128) ? (phase * 2) : ((255 - phase) * 2);
            uint8_t r = static_cast<uint8_t>((static_cast<uint16_t>(solid.r) * level) / 255);
            uint8_t g = static_cast<uint8_t>((static_cast<uint16_t>(solid.g) * level) / 255);
            uint8_t b = static_cast<uint8_t>((static_cast<uint16_t>(solid.b) * level) / 255);
            rgbLedWrite(board::PIN_RGB_LED, r, g, b);
            break;
        }
        case screen::RgbMode::SOLID:
            rgbLedWrite(board::PIN_RGB_LED, solid.r, solid.g, solid.b);
            break;
        case screen::RgbMode::OFF:
        default:
            rgbLedWrite(board::PIN_RGB_LED, 0, 0, 0);
            break;
    }
}

void pollBootButton() {
    bool is_low = (digitalRead(board::PIN_BTN_BOOT) == LOW);
    uint32_t now = millis();

    if (is_low && !g_btn_pressed) {
        g_btn_pressed = true;
        g_btn_press_start_ms = now;
    } else if (!is_low && g_btn_pressed) {
        uint32_t held_ms = now - g_btn_press_start_ms;
        g_btn_pressed = false;
        if (held_ms >= BTN_DEBOUNCE_MS && xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (held_ms >= BTN_LONG_PRESS_MS) {
                g_state.backlight_pct = screen::nextBacklightPreset(g_state.backlight_pct);
            } else {
                g_state.view_mode = screen::nextViewMode(g_state.view_mode);
            }
            g_state.ui_dirty = true;
            xSemaphoreGive(g_state_mutex);
        }
    }
}

void processSerialInput() {
    static char cmd_buf[160];
    static size_t cmd_len = 0;

    while (Serial.available() > 0) {
        char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (cmd_len > 0) {
                cmd_buf[cmd_len] = '\0';
                char response[384] = {0};
                if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_state.free_heap = ESP.getFreeHeap();
                    g_state.total_heap = ESP.getHeapSize();
                    g_state.uptime_sec = millis() / 1000UL;
                    screen::parseSerialCommand(cmd_buf, g_state, response, sizeof(response));
                    xSemaphoreGive(g_state_mutex);
                }
                Serial.println(response);
                cmd_len = 0;
            }
        } else if (cmd_len + 1 < sizeof(cmd_buf)) {
            cmd_buf[cmd_len++] = c;
        }
    }
}

void renderHeader(const screen::ScreenState& st, uint32_t now_ms, bool full_redraw) {
    if (full_redraw) {
        g_lcd.fillRect(0, 0, board::LCD_WIDTH, 24, lcd::COLOR_BG);
        g_lcd.fillRect(6, 22, 308, 1, lcd::COLOR_WHITE);
        g_lcd.drawString(8, 3, "ESP32-C6", lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 8);
    }

    unsigned long sec = static_cast<unsigned long>(now_ms / 1000UL);
    unsigned long tenths = static_cast<unsigned long>((now_ms / 100UL) % 10UL);
    char right_tag[16];
    if (st.view_mode == screen::ViewMode::HOST_MONITOR) {
        snprintf(right_tag, sizeof(right_tag), "HOST %3lu.%lus", sec % 1000UL, tenths);
    } else if (st.view_mode == screen::ViewMode::WIRELESS) {
        snprintf(right_tag, sizeof(right_tag), "RF   %3lu.%lus", sec % 1000UL, tenths);
    } else {
        snprintf(right_tag, sizeof(right_tag), "UP %5lu.%lus", sec % 100000UL, tenths);
    }
    g_lcd.drawString(180, 3, right_tag, lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 11);
}

void renderFooter(const screen::ScreenState& st, uint32_t now_ms, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 131, 308, 37, lcd::COLOR_CARD, lcd::COLOR_BORDER);
    }
    char banner[28];
    if (st.view_mode == screen::ViewMode::HOST_MONITOR && strcmp(st.host_ip, "0.0.0.0") != 0) {
        snprintf(banner, sizeof(banner), "IP: %-20.20s", st.host_ip);
    } else if (st.view_mode == screen::ViewMode::DASHBOARD) {
        uint32_t phase = (now_ms / 2500UL) % 2UL;
        if (phase == 0) {
            snprintf(
                banner,
                sizeof(banner),
                "WIFI:%-2d AP   BLE:%-3d DV",
                static_cast<int>(st.wifi_count),
                static_cast<int>(st.ble_count));
        } else {
            snprintf(
                banner,
                sizeof(banner),
                "HEAP:%3luKB  FLASH:%2uMB",
                static_cast<unsigned long>(st.free_heap / 1024UL),
                static_cast<unsigned>(st.flash_mb));
        }
    } else {
        snprintf(banner, sizeof(banner), "%-24.24s", st.custom_text);
    }
    g_lcd.drawString(14, 142, banner, lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 24);
}

uint8_t stepSmoothBar(uint8_t current, uint8_t target) {
    if (current == target) {
        return current;
    }
    int diff = static_cast<int>(target) - static_cast<int>(current);
    int step = diff / 2;
    if (step == 0) {
        step = (diff > 0) ? 1 : -1;
    }
    return static_cast<uint8_t>(static_cast<int>(current) + step);
}

uint32_t g_clock_sync_millis = 0;
uint32_t g_clock_base_ds = (22UL * 3600UL + 18UL * 60UL) * 10UL;

void initClockFromCompileTime() {
    int hh = 22, mm = 18, ss = 0;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) == 3) {
        g_clock_base_ds = static_cast<uint32_t>((hh * 3600L + mm * 60L + ss) * 10L);
        g_state.clock_base_ds = g_clock_base_ds;
    }
    g_clock_sync_millis = millis();
}

void drawCloudShape(int16_t cx, int16_t cy, uint16_t top_color, uint16_t mid_color) {
    // Shadow/base layer
    g_lcd.fillCircle(cx - 18, cy + 4, 14, mid_color);
    g_lcd.fillCircle(cx + 18, cy + 5, 13, mid_color);
    g_lcd.fillRect(cx - 18, cy + 4, 36, 15, mid_color);
    // Highlight puffs
    g_lcd.fillCircle(cx - 6, cy - 5, 16, top_color);
    g_lcd.fillCircle(cx + 10, cy - 2, 14, top_color);
    g_lcd.fillRect(cx - 16, cy + 2, 32, 13, top_color);
}

void drawSnowflake(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
    g_lcd.fillRect(cx - r, cy - 1, 2 * r + 1, 3, color);
    g_lcd.fillRect(cx - 1, cy - r, 3, 2 * r + 1, color);
    int16_t d = (r * 7) / 10;
    g_lcd.drawThickLine(cx - d, cy - d, cx + d, cy + d, 1, color);
    g_lcd.drawThickLine(cx - d, cy + d, cx + d, cy - d, 1, color);
}

void drawWeatherIcon(screen::WeatherKind kind) {
    // Clear 112x136 icon region on the left
    g_lcd.fillRect(4, 30, 116, 138, lcd::COLOR_BG);

    switch (kind) {
        case screen::WeatherKind::SUNNY: {
            const int16_t cx = 62;
            const int16_t cy = 98;
            // 8 radiating sunbeams
            g_lcd.drawThickLine(cx - 42, cy, cx + 42, cy, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(cx, cy - 42, cx, cy + 42, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(cx - 30, cy - 30, cx + 30, cy + 30, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(cx - 30, cy + 30, cx + 30, cy - 30, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.fillCircle(cx, cy, 29, lcd::COLOR_BG);
            g_lcd.fillCircle(cx, cy, 24, lcd::ICON_SUN_YELLOW);
            g_lcd.fillCircle(cx, cy, 15, lcd::ICON_SUN_CORE);
            break;
        }
        case screen::WeatherKind::PARTLY_CLOUDY: {
            // Sun in upper-left
            const int16_t sx = 42;
            const int16_t sy = 72;
            g_lcd.drawThickLine(sx - 28, sy, sx + 28, sy, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(sx, sy - 28, sx, sy + 28, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(sx - 20, sy - 20, sx + 20, sy + 20, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.drawThickLine(sx - 20, sy + 20, sx + 20, sy - 20, 2, lcd::ICON_SUN_YELLOW);
            g_lcd.fillCircle(sx, sy, 21, lcd::COLOR_BG);
            g_lcd.fillCircle(sx, sy, 17, lcd::ICON_SUN_YELLOW);
            g_lcd.fillCircle(sx, sy, 10, lcd::ICON_SUN_CORE);
            // Cloud in foreground
            drawCloudShape(68, 104, lcd::ICON_CLOUD_TOP, lcd::ICON_CLOUD_MID);
            break;
        }
        case screen::WeatherKind::CLOUDY: {
            // Darker cloud behind + bright white cloud in front
            drawCloudShape(48, 78, lcd::ICON_CLOUD_MID, lcd::ICON_CLOUD_DARK);
            drawCloudShape(68, 104, lcd::ICON_CLOUD_TOP, lcd::ICON_CLOUD_MID);
            break;
        }
        case screen::WeatherKind::RAIN: {
            drawCloudShape(62, 74, lcd::ICON_CLOUD_TOP, lcd::ICON_CLOUD_MID);
            // Vibrant blue falling rain streaks
            g_lcd.drawThickLine(38, 106, 30, 130, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(56, 106, 48, 130, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(74, 106, 66, 130, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(92, 106, 84, 130, 2, lcd::ICON_RAIN_BLUE);

            g_lcd.drawThickLine(44, 136, 38, 154, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(62, 136, 56, 154, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(80, 136, 74, 154, 2, lcd::ICON_RAIN_BLUE);
            break;
        }
        case screen::WeatherKind::LIGHTNING: {
            drawCloudShape(62, 70, lcd::ICON_CLOUD_MID, lcd::ICON_CLOUD_DARK);
            // Flanking raindrops
            g_lcd.drawThickLine(34, 104, 26, 128, 2, lcd::ICON_RAIN_BLUE);
            g_lcd.drawThickLine(92, 104, 84, 128, 2, lcd::ICON_RAIN_BLUE);
            // Vivid yellow jagged lightning bolt
            g_lcd.drawThickLine(66, 94, 50, 122, 3, lcd::ICON_BOLT_GOLD);
            g_lcd.drawThickLine(50, 122, 68, 122, 2, lcd::ICON_BOLT_GOLD);
            g_lcd.drawThickLine(68, 122, 50, 156, 3, lcd::ICON_BOLT_GOLD);
            break;
        }
        case screen::WeatherKind::LIGHT_SNOW: {
            drawCloudShape(62, 74, lcd::ICON_CLOUD_TOP, lcd::ICON_CLOUD_MID);
            drawSnowflake(40, 118, 7, lcd::ICON_SNOW_ICE);
            drawSnowflake(64, 138, 8, lcd::COLOR_WHITE);
            drawSnowflake(86, 118, 7, lcd::ICON_SNOW_ICE);
            break;
        }
        case screen::WeatherKind::HEAVY_SNOW:
        default: {
            drawCloudShape(62, 68, lcd::ICON_CLOUD_TOP, lcd::ICON_CLOUD_MID);
            drawSnowflake(36, 112, 7, lcd::COLOR_WHITE);
            drawSnowflake(62, 110, 7, lcd::ICON_SNOW_ICE);
            drawSnowflake(88, 112, 7, lcd::COLOR_WHITE);
            drawSnowflake(48, 136, 7, lcd::ICON_SNOW_ICE);
            drawSnowflake(76, 136, 7, lcd::COLOR_WHITE);
            // Snowdrift accumulation bar at bottom
            g_lcd.fillRect(24, 154, 76, 6, lcd::ICON_SNOW_ICE);
            g_lcd.fillRect(30, 151, 64, 4, lcd::COLOR_WHITE);
            break;
        }
    }
}

void renderFullClockPage(uint32_t now_ms, bool full_redraw) {
    static char s_last_clock[12] = "";

    if (full_redraw) {
        g_lcd.fillScreen(lcd::COLOR_BG);
        g_lcd.drawString(10, 6, "TORONTO", lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 8);
        g_lcd.drawString(212, 6, "LIVE 0.1S", lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 9);
        g_lcd.fillRect(8, 25, 304, 1, lcd::COLOR_WHITE);
        memset(s_last_clock, 0, sizeof(s_last_clock));
    }

    uint32_t elapsed_ds = (now_ms - g_clock_sync_millis) / 100UL;
    uint32_t total_ds = (g_clock_base_ds + elapsed_ds) % 864000UL;
    uint32_t hh = (total_ds / 36000UL) % 24UL;
    uint32_t mm = (total_ds / 600UL) % 60UL;
    uint32_t ss = (total_ds / 10UL) % 60UL;
    uint32_t tenths = total_ds % 10UL;

    char clock_buf[12];
    snprintf(
        clock_buf,
        sizeof(clock_buf),
        "%02lu:%02lu:%02lu.%lu",
        static_cast<unsigned long>(hh),
        static_cast<unsigned long>(mm),
        static_cast<unsigned long>(ss),
        static_cast<unsigned long>(tenths));

    // Draw 10 characters at scale_x=5 (30px/char -> 300px wide), scale_y=12 (96px tall!)
    const int16_t start_x = 12;
    const int16_t start_y = 36;
    for (int i = 0; i < 10 && clock_buf[i] != '\0'; ++i) {
        if (full_redraw || clock_buf[i] != s_last_clock[i]) {
            g_lcd.drawCharScaled(
                start_x + i * 30,
                start_y,
                clock_buf[i],
                lcd::COLOR_WHITE,
                lcd::COLOR_BG,
                5,
                12);
            s_last_clock[i] = clock_buf[i];
        }
    }

    // Smooth 20 FPS sub-second sweep bar across bottom
    uint8_t sub_pct = static_cast<uint8_t>(((now_ms - g_clock_sync_millis) % 1000UL) / 10UL);
    g_lcd.drawProgressBar(12, 144, 296, 18, sub_pct, lcd::COLOR_WHITE, lcd::COLOR_BG);
}

void renderTorontoWeatherPage(const screen::ScreenState& st, bool full_redraw) {
    static screen::WeatherKind s_last_kind = screen::WeatherKind::COUNT;
    static int16_t s_last_temp = -999;
    static int16_t s_last_high = -999;
    static int16_t s_last_low = -999;

    if (full_redraw) {
        g_lcd.fillScreen(lcd::COLOR_BG);
        g_lcd.drawString(10, 6, "TORONTO", lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 8);
        g_lcd.fillRect(8, 25, 304, 1, lcd::COLOR_WHITE);
        s_last_kind = screen::WeatherKind::COUNT;
        s_last_temp = -999;
        s_last_high = -999;
        s_last_low = -999;
    }

    if (full_redraw || st.weather_kind != s_last_kind) {
        char cond_buf[16];
        snprintf(cond_buf, sizeof(cond_buf), "%11s", screen::weatherKindLabel(st.weather_kind));
        g_lcd.drawString(180, 6, cond_buf, lcd::COLOR_WHITE, lcd::COLOR_BG, 2, 11);
        drawWeatherIcon(st.weather_kind);
        s_last_kind = st.weather_kind;
    }

    if (full_redraw || st.weather_temp_c != s_last_temp) {
        char temp_buf[10];
        snprintf(temp_buf, sizeof(temp_buf), "%3d~C", static_cast<int>(st.weather_temp_c));
        g_lcd.drawStringScaled(128, 34, temp_buf, lcd::COLOR_WHITE, lcd::COLOR_BG, 6, 7, 5);
        s_last_temp = st.weather_temp_c;
    }

    if (full_redraw || st.weather_high_c != s_last_high) {
        char hi_buf[12];
        snprintf(hi_buf, sizeof(hi_buf), "HI:%3d~C", static_cast<int>(st.weather_high_c));
        g_lcd.drawString(132, 102, hi_buf, lcd::COLOR_WHITE, lcd::COLOR_BG, 3, 8);
        s_last_high = st.weather_high_c;
    }

    if (full_redraw || st.weather_low_c != s_last_low) {
        char lo_buf[12];
        snprintf(lo_buf, sizeof(lo_buf), "LO:%3d~C", static_cast<int>(st.weather_low_c));
        g_lcd.drawString(132, 136, lo_buf, lcd::COLOR_WHITE, lcd::COLOR_BG, 3, 8);
        s_last_low = st.weather_low_c;
    }
}

void renderDashboardView(const screen::ScreenState& st, uint32_t now_ms, bool full_redraw) {
    static uint8_t s_last_subpage = 0xFF;
    // Alternate every 5.0 seconds between Page 0 (Full Clock) and Page 1 (Toronto Weather)
    uint8_t subpage = static_cast<uint8_t>((now_ms / 5000UL) % 2UL);
    bool page_redraw = full_redraw || (subpage != s_last_subpage);
    s_last_subpage = subpage;

    if (subpage == 0) {
        renderFullClockPage(now_ms, page_redraw);
    } else {
        renderTorontoWeatherPage(st, page_redraw);
    }
}

void renderHostView(const screen::ScreenState& st, bool full_redraw) {
    static uint8_t s_cpu_bar = 0;
    static uint8_t s_ram_bar = 0;

    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(14, 31, "CPU LOAD", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);

        g_lcd.drawCard(163, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(171, 31, "RAM USED", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);
    }

    char buf[12];
    snprintf(buf, sizeof(buf), "%3u%%", static_cast<unsigned>(st.host_cpu_pct));
    g_lcd.drawString(14, 53, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 5, 4);
    s_cpu_bar = stepSmoothBar(s_cpu_bar, st.host_cpu_pct);
    g_lcd.drawProgressBar(14, 102, 135, 18, s_cpu_bar, lcd::COLOR_WHITE, lcd::COLOR_BG);

    snprintf(buf, sizeof(buf), "%3u%%", static_cast<unsigned>(st.host_ram_pct));
    g_lcd.drawString(171, 53, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 5, 4);
    s_ram_bar = stepSmoothBar(s_ram_bar, st.host_ram_pct);
    g_lcd.drawProgressBar(171, 102, 135, 18, s_ram_bar, lcd::COLOR_WHITE, lcd::COLOR_BG);
}

void renderWirelessView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(14, 31, "WIFI APs", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);

        g_lcd.drawCard(163, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(171, 31, "BLE DEVS", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);
    }

    char buf[12];
    snprintf(buf, sizeof(buf), "%-3d", static_cast<int>(st.wifi_count));
    g_lcd.drawString(20, 60, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 6, 3);

    snprintf(buf, sizeof(buf), "%-3d", static_cast<int>(st.ble_count));
    g_lcd.drawString(177, 60, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 6, 3);
}

void renderTransitView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.fillScreen(lcd::COLOR_BG);
        
        // Horizontal dividing line between Subway and Bus
        g_lcd.fillRect(0, 72, 320, 2, lcd::COLOR_CYAN);
    }

    // -------------------------------------------------------------------------
    // TOP HALF: TTC LINE 1 SUBWAY (Eglinton Southbound)
    // -------------------------------------------------------------------------
    // Section Header
    g_lcd.drawString(8, 4, "TTC LINE 1 SUBWAY", lcd::COLOR_AMBER, lcd::COLOR_BG, 2, 17);
    
    // Top Right: Update Time
    if (st.custom_text[0] != '\0') {
        const char* ts = strchr(st.custom_text, '@');
        if (ts) ts += 2; else ts = st.custom_text;
        g_lcd.drawString(220, 8, ts, lcd::COLOR_CYAN, lcd::COLOR_BG, 1, 8);
    }

    // Main Subway Arrival (Scale 5 - Huge 30x40 font)
    char m1_buf[12];
    if (st.host_cpu_pct == 0xFF || st.host_cpu_pct == 0) {
        snprintf(m1_buf, sizeof(m1_buf), "-- MIN");
    } else {
        snprintf(m1_buf, sizeof(m1_buf), "%2u MIN", static_cast<unsigned>(st.host_cpu_pct));
    }
    g_lcd.drawString(8, 26, m1_buf, lcd::COLOR_WHITE, lcd::COLOR_BG, 5, 6);

    // Next Subway Arrival (Scale 3 - Large 18x24 font)
    char m2_buf[16];
    if (st.host_ram_pct == 0xFF || st.host_ram_pct == 0) {
        snprintf(m2_buf, sizeof(m2_buf), "NXT: --m");
    } else {
        snprintf(m2_buf, sizeof(m2_buf), "NXT:%2um", static_cast<unsigned>(st.host_ram_pct));
    }
    g_lcd.drawString(190, 38, m2_buf, lcd::COLOR_AMBER, lcd::COLOR_BG, 3, 8);

    // -------------------------------------------------------------------------
    // BOTTOM HALF: DUNFIELD AVE BUSES (Route 34 & 103/74)
    // -------------------------------------------------------------------------
    // Section Header
    g_lcd.drawString(8, 80, "DUNFIELD AVE BUSES", lcd::COLOR_CYAN, lcd::COLOR_BG, 2, 18);

    // Parse incoming bus string: "34:1m,15m | 103:5m,24m" or similar
    char line1[24] = "34: No Data";
    char line2[24] = "";
    
    if (strcmp(st.host_ip, "0.0.0.0") != 0 && st.host_ip[0] != '\0') {
        const char* sep = strchr(st.host_ip, '|');
        if (sep) {
            size_t len1 = sep - st.host_ip;
            if (len1 >= sizeof(line1)) len1 = sizeof(line1) - 1;
            strncpy(line1, st.host_ip, len1);
            line1[len1] = '\0';
            
            const char* p2 = sep + 1;
            while (*p2 == ' ') p2++;
            strncpy(line2, p2, sizeof(line2) - 1);
            line2[sizeof(line2) - 1] = '\0';
        } else {
            strncpy(line1, st.host_ip, sizeof(line1) - 1);
            line1[sizeof(line1) - 1] = '\0';
        }
    }

    // Format Route 34 display string
    char b1_fmt[28];
    if (strncmp(line1, "34:", 3) == 0) {
        snprintf(b1_fmt, sizeof(b1_fmt), "34 EGL: %s", line1 + 3);
    } else {
        snprintf(b1_fmt, sizeof(b1_fmt), "%s", line1);
    }

    // Clear and draw Bus Row 1 (Scale 3 - 18x24 font)
    g_lcd.fillRect(8, 104, 304, 26, lcd::COLOR_BG);
    g_lcd.drawString(8, 104, b1_fmt, lcd::COLOR_WHITE, lcd::COLOR_BG, 3, 16);

    // Format Route 103/74 display string
    char b2_fmt[28] = "";
    if (line2[0] != '\0') {
        if (strncmp(line2, "103:", 4) == 0) {
            snprintf(b2_fmt, sizeof(b2_fmt), "103 MT: %s", line2 + 4);
        } else if (strncmp(line2, "74:", 3) == 0) {
            snprintf(b2_fmt, sizeof(b2_fmt), "74 MT : %s", line2 + 3);
        } else {
            snprintf(b2_fmt, sizeof(b2_fmt), "%s", line2);
        }
    }

    // Clear and draw Bus Row 2 (Scale 3 - 18x24 font)
    g_lcd.fillRect(8, 136, 304, 26, lcd::COLOR_BG);
    if (b2_fmt[0] != '\0') {
        g_lcd.drawString(8, 136, b2_fmt, lcd::COLOR_GREEN, lcd::COLOR_BG, 3, 16);
    } else {
        g_lcd.drawString(8, 136, "103 MT: No Data", lcd::COLOR_GREEN, lcd::COLOR_BG, 3, 16);
    }
}

void renderUiIfNeeded() {
    constexpr uint32_t FRAME_INTERVAL_MS = 50;  // 20 FPS smooth refresh
    static uint32_t last_frame_ms = 0;
    uint32_t now_ms = millis();

    screen::ScreenState snapshot;
    bool should_draw = false;

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_state.free_heap = ESP.getFreeHeap();
        g_state.total_heap = ESP.getHeapSize();
        g_state.uptime_sec = now_ms / 1000UL;
        if (g_state.clock_sync_requested) {
            g_clock_base_ds = g_state.clock_base_ds;
            g_clock_sync_millis = now_ms;
            g_state.clock_sync_requested = false;
        }

        if (g_state.ui_dirty || (now_ms - last_frame_ms >= FRAME_INTERVAL_MS)) {
            should_draw = true;
            g_state.ui_dirty = false;
            snapshot = g_state;
        }
        xSemaphoreGive(g_state_mutex);
    }

    if (!should_draw) {
        return;
    }

    if (snapshot.backlight_pct != g_last_backlight_pct) {
        g_lcd.setBacklight(snapshot.backlight_pct);
        g_last_backlight_pct = snapshot.backlight_pct;
    }

    bool full_redraw = (snapshot.view_mode != g_last_rendered_view);
    if (snapshot.landscape_flipped != g_last_landscape_flipped) {
        g_lcd.setLandscapeFlip(snapshot.landscape_flipped);
        g_last_landscape_flipped = snapshot.landscape_flipped;
        full_redraw = true;
    }

    if (full_redraw) {
        g_lcd.fillScreen(lcd::COLOR_BG);
        g_last_rendered_view = snapshot.view_mode;
    }

    switch (snapshot.view_mode) {
        case screen::ViewMode::HOST_MONITOR:
            renderHeader(snapshot, now_ms, full_redraw);
            renderHostView(snapshot, full_redraw);
            renderFooter(snapshot, now_ms, full_redraw);
            break;
        case screen::ViewMode::WIRELESS:
            renderHeader(snapshot, now_ms, full_redraw);
            renderWirelessView(snapshot, full_redraw);
            renderFooter(snapshot, now_ms, full_redraw);
            break;
        case screen::ViewMode::TRANSIT:
            renderTransitView(snapshot, full_redraw);
            break;
        case screen::ViewMode::DASHBOARD:
        default:
            renderDashboardView(snapshot, now_ms, full_redraw);
            break;
    }

    last_frame_ms = now_ms;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    g_state_mutex = xSemaphoreCreateMutex();

    pinMode(board::PIN_BTN_BOOT, INPUT_PULLUP);
    pinMode(board::PIN_RGB_LED, OUTPUT);

    g_state.flash_mb = static_cast<uint16_t>(ESP.getFlashChipSize() / (1024UL * 1024UL));
    g_state.free_heap = ESP.getFreeHeap();
    g_state.total_heap = ESP.getHeapSize();
    initClockFromCompileTime();

    g_lcd.begin();
    initSdCard();
    renderUiIfNeeded();

    xTaskCreate(
        wirelessScanTask,
        "WirelessScanTask",
        4096,
        nullptr,
        1,
        nullptr);
}

void loop() {
    pollBootButton();
    processSerialInput();
    renderUiIfNeeded();

    static uint32_t last_rgb_ms = 0;
    if (millis() - last_rgb_ms >= 35) {
        last_rgb_ms = millis();
        updateRgbLed();
    }
    delay(2);
}
