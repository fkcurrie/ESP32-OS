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

void renderDashboardView(const screen::ScreenState& st, uint32_t now_ms, bool full_redraw) {
    (void)st;
    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(14, 31, "STOPWATCH", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);

        g_lcd.drawCard(163, 26, 151, 102, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(171, 31, "20FPS WAVE", lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 10);
    }

    // Left card: Giant 40px sub-second stopwatch (00.0 .. 99.9s) + 20 FPS sweep bar
    uint32_t sec_mod = (now_ms / 1000UL) % 100UL;
    uint32_t tenths = (now_ms / 100UL) % 10UL;
    char left_buf[12];
    snprintf(left_buf, sizeof(left_buf), "%02lu.%lu", static_cast<unsigned long>(sec_mod), static_cast<unsigned long>(tenths));
    g_lcd.drawString(14, 53, left_buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 5, 4);

    // Smooth 20 FPS triangle wave over a 2-second period (0..100..0)
    uint32_t sweep_ms = now_ms % 2000UL;
    uint8_t sweep_pct = static_cast<uint8_t>(
        (sweep_ms < 1000UL) ? (sweep_ms / 10UL) : ((2000UL - sweep_ms) / 10UL));
    g_lcd.drawProgressBar(14, 102, 135, 18, sweep_pct, lcd::COLOR_WHITE, lcd::COLOR_BG);

    // Right card: Giant 40px live 20 FPS wave percentage (0..100%) + thick bar
    uint32_t wave_ms = (now_ms + 500UL) % 3000UL;
    uint8_t wave_pct = static_cast<uint8_t>(
        (wave_ms < 1500UL) ? (wave_ms / 15UL) : ((3000UL - wave_ms) / 15UL));
    char right_buf[12];
    snprintf(right_buf, sizeof(right_buf), "%3u%%", static_cast<unsigned>(wave_pct));
    g_lcd.drawString(171, 53, right_buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 5, 4);
    g_lcd.drawProgressBar(171, 102, 135, 18, wave_pct, lcd::COLOR_WHITE, lcd::COLOR_BG);
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

    renderHeader(snapshot, now_ms, full_redraw);

    switch (snapshot.view_mode) {
        case screen::ViewMode::HOST_MONITOR:
            renderHostView(snapshot, full_redraw);
            break;
        case screen::ViewMode::WIRELESS:
            renderWirelessView(snapshot, full_redraw);
            break;
        case screen::ViewMode::DASHBOARD:
        default:
            renderDashboardView(snapshot, now_ms, full_redraw);
            break;
    }

    renderFooter(snapshot, now_ms, full_redraw);
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
