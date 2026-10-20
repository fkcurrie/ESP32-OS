#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include <stdint.h>

#ifdef PIN_RGB_LED
#undef PIN_RGB_LED
#endif

// Waveshare ESP32-C6-LCD-1.47 Hardware Pin & Display Definitions
// Verified via factory firmware disassembly (0x42000020..0x42001050)
namespace board {

// Shared SPI2 Bus (LCD + MicroSD)
constexpr int PIN_SPI_SCK  = 7;
constexpr int PIN_SPI_MOSI = 6;
constexpr int PIN_SPI_MISO = 5;

// 1.47" IPS TFT LCD (ST7789, 172x320)
constexpr int PIN_LCD_CS   = 14;
constexpr int PIN_LCD_DC   = 15;
constexpr int PIN_LCD_RST  = 21;
constexpr int PIN_LCD_BL   = 22;

constexpr uint16_t LCD_WIDTH    = 320;
constexpr uint16_t LCD_HEIGHT   = 172;
constexpr uint16_t LCD_X_OFFSET = 0;
constexpr uint16_t LCD_Y_OFFSET = 34;
constexpr uint32_t LCD_SPI_HZ   = 80000000UL;  // 80 MHz

// MicroSD Card Slot (SPI mode)
constexpr int PIN_SD_CS         = 4;
constexpr uint32_t SD_SPI_HZ    = 4000000UL;   // 4 MHz

// Onboard Addressable RGB LED (WS2812)
constexpr int PIN_RGB_LED       = 8;

// Onboard BOOT Button (Active-Low)
constexpr int PIN_BTN_BOOT      = 9;

}  // namespace board

#endif  // BOARD_PINS_H
