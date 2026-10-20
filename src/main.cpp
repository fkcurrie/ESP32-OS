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

void renderHeader(const screen::ScreenState& st) {
    g_lcd.fillRect(0, 0, board::LCD_WIDTH, 22, lcd::COLOR_NAVY);
    g_lcd.drawString(8, 7, "WAVESHARE ESP32-C6-LCD-1.47 (320x172)", lcd::COLOR_CYAN, lcd::COLOR_NAVY, 1, 38);

    const char* tab_name = "[DASH] ";
    if (st.view_mode == screen::ViewMode::HOST_MONITOR) tab_name = "[HOST] ";
    else if (st.view_mode == screen::ViewMode::WIRELESS) tab_name = "[RADIO]";
    g_lcd.drawString(270, 7, tab_name, lcd::COLOR_AMBER, lcd::COLOR_NAVY, 1, 7);
}

void renderFooter(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 130, 308, 38, lcd::COLOR_CARD, lcd::COLOR_BORDER);
    }
    char msg_line[52];
    snprintf(msg_line, sizeof(msg_line), "MSG: %-43.43s", st.custom_text);
    g_lcd.drawString(12, 137, msg_line, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 48);

    char meta[52];
    snprintf(meta, sizeof(meta), "UP:%lus | BL:%u%% | GPIO9: Short=View Long=Dim",
             static_cast<unsigned long>(st.uptime_sec),
             static_cast<unsigned>(st.backlight_pct));
    g_lcd.drawString(12, 153, meta, lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 48);
}

void renderDashboardView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 33, "SOC & MEMORY", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);

        g_lcd.drawCard(163, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(169, 33, "WIRELESS & IO", lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 20);
    }

    char buf[32];
    uint32_t used_kb = (st.total_heap > st.free_heap) ? ((st.total_heap - st.free_heap) / 1024) : 0;
    uint32_t total_kb = st.total_heap / 1024;
    uint8_t ram_pct = (total_kb > 0) ? static_cast<uint8_t>((used_kb * 100) / total_kb) : 0;

    snprintf(buf, sizeof(buf), "SRAM: %lu/%luKB (%u%%)",
             static_cast<unsigned long>(used_kb),
             static_cast<unsigned long>(total_kb),
             static_cast<unsigned>(ram_pct));
    g_lcd.drawString(12, 49, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawProgressBar(12, 61, 139, 8, ram_pct, lcd::COLOR_CYAN, lcd::COLOR_BG);

    snprintf(buf, sizeof(buf), "Flash: %u MB XMC DIO", static_cast<unsigned>(st.flash_mb));
    g_lcd.drawString(12, 76, buf, lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);

    if (st.sd_mounted) {
        snprintf(buf, sizeof(buf), "SD   : %lu MB OK", static_cast<unsigned long>(st.sd_total_mb));
        g_lcd.drawString(12, 92, buf, lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 23);
    } else {
        g_lcd.drawString(12, 92, "SD   : None (GPIO4)", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    }
    g_lcd.drawString(12, 108, "RGB  : WS2812 (GPIO8)", lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 23);

    snprintf(buf, sizeof(buf), "Wi-Fi 6 : %d APs", static_cast<int>(st.wifi_count));
    g_lcd.drawString(169, 49, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 23);
    snprintf(buf, sizeof(buf), "BLE 5.0 : %d Devs", static_cast<int>(st.ble_count));
    g_lcd.drawString(169, 65, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 81, "802.15.4: Ready", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 97, "USB CDC : /ttyACM0", lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 111, "CPU     : 160MHz RISCV", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
}

void renderHostView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 33, "HOST CPU & RAM", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);

        g_lcd.drawCard(163, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(169, 33, "HOST NETWORK", lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 20);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "CPU LOAD: %u%%", static_cast<unsigned>(st.host_cpu_pct));
    g_lcd.drawString(12, 50, buf, lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawProgressBar(12, 63, 139, 12, st.host_cpu_pct, lcd::COLOR_GREEN, lcd::COLOR_BG);

    snprintf(buf, sizeof(buf), "RAM USED: %u%%", static_cast<unsigned>(st.host_ram_pct));
    g_lcd.drawString(12, 85, buf, lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawProgressBar(12, 98, 139, 12, st.host_ram_pct, lcd::COLOR_AMBER, lcd::COLOR_BG);

    g_lcd.drawString(169, 50, "IPv4 Address:", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 64, st.host_ip, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 86, "Live stream CLI:", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 100, "screen_ctl.py host", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 23);
}

void renderWirelessView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 33, "2.4GHz WI-FI 6", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);

        g_lcd.drawCard(163, 26, 151, 100, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(169, 33, "BLUETOOTH 5 LE", lcd::COLOR_PURPLE, lcd::COLOR_CARD, 1, 20);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d APs", static_cast<int>(st.wifi_count));
    g_lcd.drawString(12, 54, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 11);
    g_lcd.drawString(12, 84, "802.11ax 2.4GHz STA", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(12, 100, "MAC:14:C1:9F:05:7D:10", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);

    snprintf(buf, sizeof(buf), "%d Devs", static_cast<int>(st.ble_count));
    g_lcd.drawString(169, 54, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 11);
    g_lcd.drawString(169, 84, "Active BLE Scan (2s)", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 23);
    g_lcd.drawString(169, 100, "Send 'SCAN' to refresh", lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 23);
}

void renderUiIfNeeded() {
    static uint32_t last_uptime_drawn = 0;
    uint32_t now_sec = millis() / 1000UL;

    screen::ScreenState snapshot;
    bool should_draw = false;

    if (xSemaphoreTake(g_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_state.free_heap = ESP.getFreeHeap();
        g_state.total_heap = ESP.getHeapSize();
        g_state.uptime_sec = now_sec;

        if (g_state.ui_dirty || now_sec != last_uptime_drawn) {
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
        renderHeader(snapshot);
        g_last_rendered_view = snapshot.view_mode;
    }

    switch (snapshot.view_mode) {
        case screen::ViewMode::HOST_MONITOR:
            renderHostView(snapshot, full_redraw);
            break;
        case screen::ViewMode::WIRELESS:
            renderWirelessView(snapshot, full_redraw);
            break;
        case screen::ViewMode::DASHBOARD:
        default:
            renderDashboardView(snapshot, full_redraw);
            break;
    }

    renderFooter(snapshot, full_redraw);
    last_uptime_drawn = now_sec;
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
    delay(10);
}
