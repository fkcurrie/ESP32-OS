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
    g_lcd.fillRect(0, 0, board::LCD_WIDTH, 28, lcd::COLOR_NAVY);
    g_lcd.drawString(8, 6, "ESP32-C6 LCD 1.47", lcd::COLOR_CYAN, lcd::COLOR_NAVY, 1, 18);

    const char* tab_name = "DASH";
    if (st.view_mode == screen::ViewMode::HOST_MONITOR) tab_name = "HOST";
    else if (st.view_mode == screen::ViewMode::WIRELESS) tab_name = "RF";
    g_lcd.drawString(134, 6, tab_name, lcd::COLOR_AMBER, lcd::COLOR_NAVY, 1, 5);
    g_lcd.drawString(8, 17, "172x320 ST7789 | 8MB DIO", lcd::COLOR_MUTED, lcd::COLOR_NAVY, 1, 26);
}

void renderFooter(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 250, 160, 64, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 256, "MESSAGE / SERIAL", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);
    }
    char line1[25] = {0};
    char line2[25] = {0};
    strncpy(line1, st.custom_text, 24);
    if (strlen(st.custom_text) > 24) {
        strncpy(line2, st.custom_text + 24, 24);
    }
    g_lcd.drawString(12, 270, line1, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 282, line2, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 24);

    char meta[28];
    snprintf(meta, sizeof(meta), "UP:%lus BL:%u%% BTN:GPIO9",
             static_cast<unsigned long>(st.uptime_sec),
             static_cast<unsigned>(st.backlight_pct));
    g_lcd.drawString(12, 298, meta, lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
}

void renderDashboardView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 34, 160, 68, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 40, "SOC & MEMORY", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 16);

        g_lcd.drawCard(6, 108, 160, 66, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 114, "WIRELESS SCAN", lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 16);

        g_lcd.drawCard(6, 180, 160, 64, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 186, "STORAGE & PERIPHERALS", lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 22);
    }

    char buf[32];
    uint32_t used_kb = (st.total_heap > st.free_heap) ? ((st.total_heap - st.free_heap) / 1024) : 0;
    uint32_t total_kb = st.total_heap / 1024;
    uint8_t ram_pct = (total_kb > 0) ? static_cast<uint8_t>((used_kb * 100) / total_kb) : 0;

    snprintf(buf, sizeof(buf), "SRAM: %lu/%lu KB (%u%%)",
             static_cast<unsigned long>(used_kb),
             static_cast<unsigned long>(total_kb),
             static_cast<unsigned>(ram_pct));
    g_lcd.drawString(12, 54, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawProgressBar(12, 66, 148, 8, ram_pct, lcd::COLOR_CYAN, lcd::COLOR_BG);

    snprintf(buf, sizeof(buf), "FLASH: %u MB XMC DIO", static_cast<unsigned>(st.flash_mb));
    g_lcd.drawString(12, 82, buf, lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);

    snprintf(buf, sizeof(buf), "Wi-Fi 6 APs : %d", static_cast<int>(st.wifi_count));
    g_lcd.drawString(12, 130, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 22);
    snprintf(buf, sizeof(buf), "BLE 5 Devs  : %d", static_cast<int>(st.ble_count));
    g_lcd.drawString(12, 144, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 22);
    g_lcd.drawString(12, 158, "802.15.4    : Ready", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 22);

    if (st.sd_mounted) {
        snprintf(buf, sizeof(buf), "SD Card: %lu MB OK", static_cast<unsigned long>(st.sd_total_mb));
        g_lcd.drawString(12, 202, buf, lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 24);
    } else {
        g_lcd.drawString(12, 202, "SD Card: None (GPIO4)", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
    }
    g_lcd.drawString(12, 216, "RGB LED: WS2812 (GPIO8)", lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 230, "USB JTAG/CDC: /ttyACM0", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
}

void renderHostView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 34, 160, 210, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 42, "HOST TELEMETRY", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "IP: %s", st.host_ip);
    g_lcd.drawString(12, 62, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 1, 24);

    snprintf(buf, sizeof(buf), "CPU LOAD: %u%%", static_cast<unsigned>(st.host_cpu_pct));
    g_lcd.drawString(12, 88, buf, lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawProgressBar(12, 102, 148, 14, st.host_cpu_pct, lcd::COLOR_GREEN, lcd::COLOR_BG);

    snprintf(buf, sizeof(buf), "RAM USED: %u%%", static_cast<unsigned>(st.host_ram_pct));
    g_lcd.drawString(12, 132, buf, lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawProgressBar(12, 146, 148, 14, st.host_ram_pct, lcd::COLOR_AMBER, lcd::COLOR_BG);

    g_lcd.drawString(12, 180, "Update via host CLI:", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 194, "screen_ctl.py host", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 212, "Short press BOOT: View", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 224, "Long press BOOT : Dim", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
}

void renderWirelessView(const screen::ScreenState& st, bool full_redraw) {
    if (full_redraw) {
        g_lcd.drawCard(6, 34, 160, 210, lcd::COLOR_CARD, lcd::COLOR_BORDER);
        g_lcd.drawString(12, 42, "RADIO ENVIRONMENT", lcd::COLOR_GREEN, lcd::COLOR_CARD, 1, 20);
    }

    char buf[32];
    g_lcd.drawString(12, 64, "2.4GHz Wi-Fi 6", lcd::COLOR_CYAN, lcd::COLOR_CARD, 1, 20);
    snprintf(buf, sizeof(buf), "%d APs", static_cast<int>(st.wifi_count));
    g_lcd.drawString(12, 78, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 11);

    g_lcd.drawString(12, 112, "Bluetooth 5 LE", lcd::COLOR_PURPLE, lcd::COLOR_CARD, 1, 20);
    snprintf(buf, sizeof(buf), "%d Devs", static_cast<int>(st.ble_count));
    g_lcd.drawString(12, 126, buf, lcd::COLOR_WHITE, lcd::COLOR_CARD, 2, 11);

    g_lcd.drawString(12, 164, "MAC: 14:C1:9F:05:7D:10", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 180, "Chip: ESP32-C6FH4 v0.2", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 196, "Send 'SCAN' on USB", lcd::COLOR_AMBER, lcd::COLOR_CARD, 1, 24);
    g_lcd.drawString(12, 210, "for instant refresh.", lcd::COLOR_MUTED, lcd::COLOR_CARD, 1, 24);
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
