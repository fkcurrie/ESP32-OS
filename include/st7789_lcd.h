#ifndef ST7789_LCD_H
#define ST7789_LCD_H

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>

#include "board_pins.h"

namespace lcd {

// RGB565 High-Contrast Palette (Pure Black Background + Bright White Foreground)
constexpr uint16_t COLOR_BLACK     = 0x0000;
constexpr uint16_t COLOR_BG        = 0x0000;  // Pure black (#000000)
constexpr uint16_t COLOR_CARD      = 0x0000;  // Pure black (#000000)
constexpr uint16_t COLOR_NAVY      = 0x0000;  // Pure black (#000000)
constexpr uint16_t COLOR_WHITE     = 0xFFFF;  // Pure bright white (#FFFFFF)
constexpr uint16_t COLOR_BORDER    = 0xFFFF;  // Crisp white card border
constexpr uint16_t COLOR_MUTED     = 0xFFFF;  // Bright white
constexpr uint16_t COLOR_CYAN      = 0xFFFF;  // Bright white
constexpr uint16_t COLOR_GREEN     = 0xFFFF;  // Bright white
constexpr uint16_t COLOR_AMBER     = 0xFFFF;  // Bright white
constexpr uint16_t COLOR_RED       = 0xFFFF;  // Bright white
constexpr uint16_t COLOR_PURPLE    = 0xFFFF;  // Bright white

// Weather Icon Vibrant RGB565 Palette
constexpr uint16_t ICON_SUN_YELLOW = 0xFFE0;  // Bright sun yellow (#FFFF00)
constexpr uint16_t ICON_SUN_CORE   = 0xFD20;  // Warm solar orange (#FFA500)
constexpr uint16_t ICON_CLOUD_TOP  = 0xFFFF;  // Bright white cloud (#FFFFFF)
constexpr uint16_t ICON_CLOUD_MID  = 0xC618;  // Silver cloud (#C0C0C0)
constexpr uint16_t ICON_CLOUD_DARK = 0x7BEF;  // Storm slate cloud (#787C78)
constexpr uint16_t ICON_RAIN_BLUE  = 0x05FF;  // Vibrant rain blue (#00BFFF)
constexpr uint16_t ICON_BOLT_GOLD  = 0xFFE0;  // Electric lightning yellow (#FFFF00)
constexpr uint16_t ICON_SNOW_ICE   = 0xAFFF;  // Icy crystal cyan (#A8FFFF)

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((static_cast<uint16_t>(r & 0xF8) << 8) |
            (static_cast<uint16_t>(g & 0xFC) << 3) |
            (static_cast<uint16_t>(b) >> 3));
}

class St7789Lcd {
public:
    void begin();
    void setBacklight(uint8_t pct);
    void setLandscapeFlip(bool flipped);

    void fillScreen(uint16_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color);
    void drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t r, uint16_t color);
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg_color, uint16_t border_color);
    void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fill_color, uint16_t track_color);

    void drawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale = 1);
    void drawString(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale = 1, uint8_t max_chars = 0);
    void drawCharScaled(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale_x, uint8_t scale_y);
    void drawStringScaled(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale_x, uint8_t scale_y, uint8_t max_chars = 0);

private:
    void writeCommand(uint8_t cmd);
    void writeData(uint8_t data);
    void setAddrWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
};

}  // namespace lcd

#endif  // ST7789_LCD_H
