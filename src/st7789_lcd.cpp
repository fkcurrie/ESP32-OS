#include "st7789_lcd.h"

namespace lcd {

namespace {

// Standard 5x7 ASCII bitmap font (0x20..0x7E)
const uint8_t FONT_5X7[95][5] PROGMEM = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // 0x20 ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // 0x21 '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // 0x22 '"'
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // 0x23 '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // 0x24 '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // 0x25 '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // 0x26 '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // 0x27 '''
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // 0x28 '('
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // 0x29 ')'
    {0x14, 0x08, 0x3E, 0x08, 0x14},  // 0x2A '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // 0x2B '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // 0x2C ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // 0x2D '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // 0x2E '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // 0x2F '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0x30 '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 0x31 '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 0x32 '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 0x33 '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 0x34 '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 0x35 '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 0x36 '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 0x37 '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 0x38 '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 0x39 '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // 0x3A ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // 0x3B ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // 0x3C '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // 0x3D '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // 0x3E '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // 0x3F '?'
    {0x32, 0x49, 0x79, 0x41, 0x3E},  // 0x40 '@'
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // 0x41 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // 0x42 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // 0x43 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // 0x44 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // 0x45 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // 0x46 'F'
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // 0x47 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // 0x48 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // 0x49 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // 0x4A 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // 0x4B 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // 0x4C 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // 0x4D 'M'
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // 0x4E 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // 0x4F 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // 0x50 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // 0x51 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // 0x52 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 0x53 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // 0x54 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // 0x55 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // 0x56 'V'
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // 0x57 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 0x58 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07},  // 0x59 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 0x5A 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00},  // 0x5B '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // 0x5C '\'
    {0x00, 0x41, 0x41, 0x7F, 0x00},  // 0x5D ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // 0x5E '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // 0x5F '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // 0x60 '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 0x61 'a'
    {0x7F, 0x48, 0x44, 0x44, 0x38},  // 0x62 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 0x63 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7F},  // 0x64 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 0x65 'e'
    {0x08, 0x7E, 0x09, 0x01, 0x02},  // 0x66 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E},  // 0x67 'g'
    {0x7F, 0x08, 0x04, 0x04, 0x78},  // 0x68 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00},  // 0x69 'i'
    {0x20, 0x40, 0x44, 0x3D, 0x00},  // 0x6A 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00},  // 0x6B 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00},  // 0x6C 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78},  // 0x6D 'm'
    {0x7C, 0x08, 0x04, 0x04, 0x78},  // 0x6E 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 0x6F 'o'
    {0x7C, 0x14, 0x14, 0x14, 0x08},  // 0x70 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7C},  // 0x71 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x08},  // 0x72 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 0x73 's'
    {0x04, 0x3F, 0x44, 0x40, 0x20},  // 0x74 't'
    {0x3C, 0x40, 0x40, 0x20, 0x7C},  // 0x75 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C},  // 0x76 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C},  // 0x77 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 0x78 'x'
    {0x0C, 0x50, 0x50, 0x50, 0x3C},  // 0x79 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44},  // 0x7A 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // 0x7B '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00},  // 0x7C '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // 0x7D '}'
    {0x06, 0x09, 0x09, 0x06, 0x00}   // 0x7E '~' (rendered as degree symbol °)
};

}  // namespace

void St7789Lcd::writeCommand(uint8_t cmd) {
    SPI.beginTransaction(SPISettings(board::LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(board::PIN_LCD_CS, LOW);
    digitalWrite(board::PIN_LCD_DC, LOW);
    SPI.transfer(cmd);
    digitalWrite(board::PIN_LCD_CS, HIGH);
    SPI.endTransaction();
}

void St7789Lcd::writeData(uint8_t data) {
    SPI.beginTransaction(SPISettings(board::LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(board::PIN_LCD_CS, LOW);
    digitalWrite(board::PIN_LCD_DC, HIGH);
    SPI.transfer(data);
    digitalWrite(board::PIN_LCD_CS, HIGH);
    SPI.endTransaction();
}

void St7789Lcd::setAddrWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    const uint16_t col_start = x1 + board::LCD_X_OFFSET;
    const uint16_t col_end   = x2 + board::LCD_X_OFFSET;
    const uint16_t row_start = y1 + board::LCD_Y_OFFSET;
    const uint16_t row_end   = y2 + board::LCD_Y_OFFSET;

    writeCommand(0x2A);  // CASET
    writeData(col_start >> 8);
    writeData(col_start & 0xFF);
    writeData(col_end >> 8);
    writeData(col_end & 0xFF);

    writeCommand(0x2B);  // RASET
    writeData(row_start >> 8);
    writeData(row_start & 0xFF);
    writeData(row_end >> 8);
    writeData(row_end & 0xFF);

    writeCommand(0x2C);  // RAMWR
}

void St7789Lcd::begin() {
    pinMode(board::PIN_LCD_CS, OUTPUT);
    pinMode(board::PIN_LCD_DC, OUTPUT);
    pinMode(board::PIN_LCD_RST, OUTPUT);

    SPI.begin(board::PIN_SPI_SCK, board::PIN_SPI_MISO, board::PIN_SPI_MOSI, -1);

    // Hardware reset sequence (matches factory firmware @ 0x42000340)
    digitalWrite(board::PIN_LCD_CS, LOW);
    delay(50);
    digitalWrite(board::PIN_LCD_RST, LOW);
    delay(50);
    digitalWrite(board::PIN_LCD_RST, HIGH);
    delay(50);

    // ST7789 register initialization (matches factory firmware @ 0x42000494..0x420005fc)
    writeCommand(0x11);  // SLPOUT
    delay(120);

    writeCommand(0x36);  // MADCTL (Landscape 320x172: MV=1, MY=1)
    writeData(0xA0);

    writeCommand(0x3A);  // COLMOD (16-bit RGB565)
    writeData(0x05);

    writeCommand(0xB0);  // RAMCTRL
    writeData(0x00);
    writeData(0xE8);

    writeCommand(0xB2);  // PORCTRL
    writeData(0x0C);
    writeData(0x0C);
    writeData(0x00);
    writeData(0x33);
    writeData(0x33);

    writeCommand(0xB7);  // GCTRL
    writeData(0x35);

    writeCommand(0xBB);  // VCOMS
    writeData(0x35);

    writeCommand(0xC0);  // LCMCTRL
    writeData(0x2C);

    writeCommand(0xC2);  // VDVVRHEN
    writeData(0x01);

    writeCommand(0xC3);  // VRHS
    writeData(0x13);

    writeCommand(0xC4);  // VDVS
    writeData(0x20);

    writeCommand(0xC6);  // FRCTRL2
    writeData(0x0F);

    writeCommand(0xD0);  // PWCTRL1
    writeData(0xA4);
    writeData(0xA1);

    writeCommand(0xD6);
    writeData(0xA1);

    writeCommand(0xE0);  // PVGAMCTRL
    const uint8_t pvgam[] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
    for (uint8_t v : pvgam) writeData(v);

    writeCommand(0xE1);  // NVGAMCTRL
    const uint8_t nvgam[] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};
    for (uint8_t v : nvgam) writeData(v);

    writeCommand(0x21);  // INVON (IPS panel inversion)
    writeCommand(0x11);  // SLPOUT
    delay(120);
    writeCommand(0x29);  // DISPON

    // Configure Backlight PWM (1000 Hz, 10-bit resolution on GPIO 22)
    ledcAttach(board::PIN_LCD_BL, 1000, 10);
    setBacklight(100);
}

void St7789Lcd::setLandscapeFlip(bool flipped) {
    writeCommand(0x36);  // MADCTL
    writeData(flipped ? 0x60 : 0xA0);
}

void St7789Lcd::setBacklight(uint8_t pct) {
    if (pct > 100) pct = 100;
    // 10-bit resolution (0..1023); factory firmware uses scale * 10
    uint32_t duty = (static_cast<uint32_t>(pct) * 1023U) / 100U;
    ledcWrite(board::PIN_LCD_BL, duty);
}

void St7789Lcd::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0 || x >= board::LCD_WIDTH || y >= board::LCD_HEIGHT) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > board::LCD_WIDTH)  w = board::LCD_WIDTH - x;
    if (y + h > board::LCD_HEIGHT) h = board::LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    setAddrWindow(x, y, x + w - 1, y + h - 1);

    uint16_t line_buf[board::LCD_WIDTH];
    const uint16_t swapped = (color >> 8) | (color << 8);
    for (int16_t i = 0; i < w; ++i) {
        line_buf[i] = swapped;
    }

    SPI.beginTransaction(SPISettings(board::LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(board::PIN_LCD_CS, LOW);
    digitalWrite(board::PIN_LCD_DC, HIGH);
    for (int16_t row = 0; row < h; ++row) {
        SPI.writeBytes(reinterpret_cast<const uint8_t*>(line_buf), w * 2);
    }
    digitalWrite(board::PIN_LCD_CS, HIGH);
    SPI.endTransaction();
}

void St7789Lcd::fillScreen(uint16_t color) {
    fillRect(0, 0, board::LCD_WIDTH, board::LCD_HEIGHT, color);
}

void St7789Lcd::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    fillRect(x, y, w, 1, color);
    fillRect(x, y + h - 1, w, 1, color);
    fillRect(x, y, 1, h, color);
    fillRect(x + w - 1, y, 1, h, color);
}

void St7789Lcd::fillCircle(int16_t cx, int16_t cy, int16_t r, uint16_t color) {
    if (r <= 0) return;
    for (int16_t dy = -r; dy <= r; ++dy) {
        int32_t dx_sq = static_cast<int32_t>(r) * r - static_cast<int32_t>(dy) * dy;
        int16_t dx = 0;
        while (static_cast<int32_t>(dx + 1) * (dx + 1) <= dx_sq) {
            ++dx;
        }
        fillRect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

void St7789Lcd::drawThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t r, uint16_t color) {
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t steps = (dx > dy) ? dx : dy;
    if (steps == 0) {
        fillCircle(x0, y0, r, color);
        return;
    }
    for (int16_t i = 0; i <= steps; ++i) {
        int16_t x = x0 + static_cast<int16_t>((static_cast<int32_t>(x1 - x0) * i) / steps);
        int16_t y = y0 + static_cast<int16_t>((static_cast<int32_t>(y1 - y0) * i) / steps);
        fillRect(x - r, y - r, 2 * r + 1, 2 * r + 1, color);
    }
}

void St7789Lcd::drawCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t bg_color, uint16_t border_color) {
    fillRect(x + 1, y + 1, w - 2, h - 2, bg_color);
    drawRect(x, y, w, h, border_color);
}

void St7789Lcd::drawProgressBar(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t pct, uint16_t fill_color, uint16_t track_color) {
    if (pct > 100) pct = 100;
    drawRect(x, y, w, h, COLOR_WHITE);
    const int16_t inner_w = w - 4;
    const int16_t inner_h = h - 4;
    if (inner_w <= 0 || inner_h <= 0) return;

    int16_t fill_w = static_cast<int16_t>((static_cast<int32_t>(inner_w) * pct) / 100);
    if (fill_w > 0) {
        fillRect(x + 2, y + 2, fill_w, inner_h, fill_color);
    }
    if (fill_w < inner_w) {
        fillRect(x + 2 + fill_w, y + 2, inner_w - fill_w, inner_h, track_color);
    }
}

void St7789Lcd::drawCharScaled(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale_x, uint8_t scale_y) {
    if (scale_x == 0) scale_x = 1;
    if (scale_x > 10) scale_x = 10;
    if (scale_y == 0) scale_y = 1;
    if (scale_y > 16) scale_y = 16;
    if (c < 0x20 || c > 0x7E) c = '?';

    const int16_t char_w = 6 * scale_x;
    const int16_t char_h = 8 * scale_y;
    if (x < 0 || y < 0 || x + char_w > board::LCD_WIDTH || y + char_h > board::LCD_HEIGHT) {
        return;
    }

    const uint8_t* glyph = FONT_5X7[c - 0x20];
    const uint16_t fg_be = (fg >> 8) | (fg << 8);
    const uint16_t bg_be = (bg >> 8) | (bg << 8);

    uint16_t row_buf[60];

    setAddrWindow(x, y, x + char_w - 1, y + char_h - 1);
    SPI.beginTransaction(SPISettings(board::LCD_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(board::PIN_LCD_CS, LOW);
    digitalWrite(board::PIN_LCD_DC, HIGH);

    for (uint8_t row = 0; row < 8; ++row) {
        int idx = 0;
        for (uint8_t col = 0; col < 6; ++col) {
            bool on = false;
            if (col < 5 && row < 7) {
                on = (pgm_read_byte(&glyph[col]) >> row) & 0x01;
            }
            uint16_t px = on ? fg_be : bg_be;
            for (uint8_t sx = 0; sx < scale_x; ++sx) {
                row_buf[idx++] = px;
            }
        }
        for (uint8_t sy = 0; sy < scale_y; ++sy) {
            SPI.writeBytes(reinterpret_cast<const uint8_t*>(row_buf), char_w * 2);
        }
    }

    digitalWrite(board::PIN_LCD_CS, HIGH);
    SPI.endTransaction();
}

void St7789Lcd::drawStringScaled(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale_x, uint8_t scale_y, uint8_t max_chars) {
    if (!str) return;
    const int16_t step = 6 * scale_x;
    uint8_t count = 0;
    while (*str && (max_chars == 0 || count < max_chars)) {
        if (x + step > board::LCD_WIDTH) break;
        drawCharScaled(x, y, *str++, fg, bg, scale_x, scale_y);
        x += step;
        ++count;
    }
    while (max_chars > 0 && count < max_chars && x + step <= board::LCD_WIDTH) {
        drawCharScaled(x, y, ' ', fg, bg, scale_x, scale_y);
        x += step;
        ++count;
    }
}

void St7789Lcd::drawChar(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale) {
    drawCharScaled(x, y, c, fg, bg, scale, scale);
}

void St7789Lcd::drawString(int16_t x, int16_t y, const char* str, uint16_t fg, uint16_t bg, uint8_t scale, uint8_t max_chars) {
    drawStringScaled(x, y, str, fg, bg, scale, scale, max_chars);
}

}  // namespace lcd
