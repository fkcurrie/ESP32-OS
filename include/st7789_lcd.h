#ifndef ST7789_LCD_H
#define ST7789_LCD_H

#include <Arduino.h>
#include <SPI.h>
#include <stdint.h>

#include "board_pins.h"

namespace lcd {

// RGB565 Color Palette (Dark UI Theme)
constexpr uint16_t COLOR_BG        = 0x0861;  // Deep slate (#080C08)
constexpr uint16_t COLOR_CARD      = 0x10E3;  // Dark card surface (#101C18)
constexpr uint16_t COLOR_BORDER    = 0x2965;  // Subtle border
constexpr uint16_t COLOR_WHITE     = 0xFFFF;
constexpr uint16_t COLOR_MUTED     = 0x9CF3;  // Light gray text
constexpr uint16_t COLOR_CYAN      = 0x07FF;
constexpr uint16_t COLOR_GREEN     = 0x2668;
constexpr uint16_t COLOR_AMBER     = 0xFD20;
constexpr uint16_t COLOR_RED       = 0xF986;
constexpr uint16_t COLOR_PURPLE    = 0xB39F;
constexpr uint16_t COLOR_NAVY      = 0x094A;

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
    void drawCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg_color, uint16_t border_color);
    void drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fill_color, uint16_t track_color);

    void drawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale = 1);
    void drawString(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale = 1, uint8_t max_chars = 0);

private:
    void writeCommand(uint8_t cmd);
    void writeData(uint8_t data);
    void setAddrWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
};

}  // namespace lcd

#endif  // ST7789_LCD_H
