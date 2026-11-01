#ifndef ST7789_LCD_H
#define ST7789_LCD_H

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "board_pins.h"
#include "c6os_display.h"

namespace lcd {

class St7789Lcd {
public:
    St7789Lcd();
    ~St7789Lcd();

    St7789Lcd(const St7789Lcd&) = delete;
    St7789Lcd& operator=(const St7789Lcd&) = delete;

    // Initializes SPI2_HOST bus, esp_lcd_panel_io_spi @ 80 MHz, ST7789 registers,
    // LEDC backlight PWM, and 2 x (320 x 16 x 2B) = 20,480 B dual scanline DMA buffers.
    void begin();

    void setBacklight(uint8_t pct);
    void setLandscapeFlip(bool flipped);

    // Opens a contiguous vertical scanline window [y0 .. y1] across full width (0..319).
    void beginVerticalWindow(uint16_t y0, uint16_t y1);

    // Acquires the next available DMA scanline buffer (0 or 1), blocking if still in-flight.
    uint16_t* acquireStripBuffer(uint8_t& out_slot);

    // Enqueues a rendered scanline strip for asynchronous 80 MHz SPI2 DMA transmission.
    // Pass first_in_window=true on the first strip after beginVerticalWindow() (sends 0x2C RAMWR),
    // and first_in_window=false on contiguous follow-up strips (continues RAMWR with cmd=-1).
    void submitStripBuffer(uint8_t slot, size_t byte_len, bool first_in_window);

    // Waits until all queued DMA color transfers have completed.
    void waitIdle();

    static bool onColorTransDoneStatic(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t* edata,
                                       void* user_ctx);

private:
    void writeCmdParam(int cmd, const void* param = nullptr, size_t param_size = 0);

    esp_lcd_panel_io_handle_t io_;
    uint16_t* dma_buf_[2];
    SemaphoreHandle_t buf_sem_[2];
    portMUX_TYPE fifo_mux_;
    uint8_t in_flight_fifo_[4];
    volatile uint8_t fifo_head_;
    volatile uint8_t fifo_tail_;
    uint8_t next_slot_;
    bool bus_ready_;
};

}  // namespace lcd

#endif  // ST7789_LCD_H
