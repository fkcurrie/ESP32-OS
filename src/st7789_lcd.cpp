#include "st7789_lcd.h"

namespace lcd {

St7789Lcd::St7789Lcd()
    : io_(nullptr),
      dma_buf_{nullptr, nullptr},
      buf_sem_{nullptr, nullptr},
      fifo_mux_(portMUX_INITIALIZER_UNLOCKED),
      in_flight_fifo_{0, 0, 0, 0},
      fifo_head_(0),
      fifo_tail_(0),
      next_slot_(0),
      bus_ready_(false) {}

St7789Lcd::~St7789Lcd() {
    waitIdle();
    if (io_) {
        esp_lcd_panel_io_del(io_);
        io_ = nullptr;
    }
    for (int i = 0; i < 2; ++i) {
        if (dma_buf_[i]) {
            heap_caps_free(dma_buf_[i]);
            dma_buf_[i] = nullptr;
        }
        if (buf_sem_[i]) {
            vSemaphoreDelete(buf_sem_[i]);
            buf_sem_[i] = nullptr;
        }
    }
}

bool IRAM_ATTR St7789Lcd::onColorTransDoneStatic(esp_lcd_panel_io_handle_t,
                                                 esp_lcd_panel_io_event_data_t*,
                                                 void* user_ctx) {
    St7789Lcd* self = static_cast<St7789Lcd*>(user_ctx);
    if (!self) return false;

    uint8_t slot = 0;
    bool valid = false;
    portENTER_CRITICAL_ISR(&self->fifo_mux_);
    if (self->fifo_tail_ != self->fifo_head_) {
        slot = self->in_flight_fifo_[self->fifo_tail_ & 3U];
        self->fifo_tail_ = static_cast<uint8_t>(self->fifo_tail_ + 1U);
        valid = true;
    }
    portEXIT_CRITICAL_ISR(&self->fifo_mux_);

    BaseType_t hp_woken = pdFALSE;
    if (valid && slot < 2 && self->buf_sem_[slot]) {
        xSemaphoreGiveFromISR(self->buf_sem_[slot], &hp_woken);
    }
    return hp_woken == pdTRUE;
}

void St7789Lcd::writeCmdParam(int cmd, const void* param, size_t param_size) {
    if (!io_) return;
    esp_lcd_panel_io_tx_param(io_, cmd, param, param_size);
}

void St7789Lcd::begin() {
    if (bus_ready_) return;

    // Ensure MicroSD CS (GPIO 4) is deasserted high before initializing shared SPI2_HOST bus
    pinMode(board::PIN_SD_CS, OUTPUT);
    digitalWrite(board::PIN_SD_CS, HIGH);

    pinMode(board::PIN_LCD_RST, OUTPUT);
    digitalWrite(board::PIN_LCD_RST, LOW);
    delay(40);
    digitalWrite(board::PIN_LCD_RST, HIGH);
    delay(40);

    constexpr size_t strip_bytes =
        static_cast<size_t>(c6os::display::FB_WIDTH) *
        static_cast<size_t>(c6os::display::DMA_STRIP_ROWS) * sizeof(uint16_t);

    for (int i = 0; i < 2; ++i) {
        dma_buf_[i] = static_cast<uint16_t*>(
            heap_caps_malloc(strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        buf_sem_[i] = xSemaphoreCreateBinary();
        if (buf_sem_[i]) {
            xSemaphoreGive(buf_sem_[i]);
        }
    }

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = board::PIN_SPI_MOSI;
    buscfg.miso_io_num = board::PIN_SPI_MISO;
    buscfg.sclk_io_num = board::PIN_SPI_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = static_cast<int>(strip_bytes + 64);

    esp_err_t bus_err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        return;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = board::PIN_LCD_CS;
    io_cfg.dc_gpio_num = board::PIN_LCD_DC;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = board::LCD_SPI_HZ;
    io_cfg.trans_queue_depth = 2;
    io_cfg.on_color_trans_done = &St7789Lcd::onColorTransDoneStatic;
    io_cfg.user_ctx = this;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;

    if (esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST),
                                 &io_cfg, &io_) != ESP_OK) {
        return;
    }

    // ST7789 initialization sequence matching Waveshare ESP32-C6-LCD-1.47 panel
    writeCmdParam(0x11);  // SLPOUT
    delay(120);

    const uint8_t madctl = 0xA0;  // Landscape 320x172 (MV=1, MY=1)
    writeCmdParam(0x36, &madctl, 1);

    const uint8_t colmod = 0x05;  // 16-bit RGB565
    writeCmdParam(0x3A, &colmod, 1);

    const uint8_t ramctrl[] = {0x00, 0xE8};
    writeCmdParam(0xB0, ramctrl, sizeof(ramctrl));

    const uint8_t porctrl[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    writeCmdParam(0xB2, porctrl, sizeof(porctrl));

    const uint8_t gctrl = 0x35;
    writeCmdParam(0xB7, &gctrl, 1);

    const uint8_t vcoms = 0x35;
    writeCmdParam(0xBB, &vcoms, 1);

    const uint8_t lcmctrl = 0x2C;
    writeCmdParam(0xC0, &lcmctrl, 1);

    const uint8_t vdvvrhen = 0x01;
    writeCmdParam(0xC2, &vdvvrhen, 1);

    const uint8_t vrhs = 0x13;
    writeCmdParam(0xC3, &vrhs, 1);

    const uint8_t vdvs = 0x20;
    writeCmdParam(0xC4, &vdvs, 1);

    const uint8_t frctrl2 = 0x0F;
    writeCmdParam(0xC6, &frctrl2, 1);

    const uint8_t pwctrl1[] = {0xA4, 0xA1};
    writeCmdParam(0xD0, pwctrl1, sizeof(pwctrl1));

    const uint8_t d6_val = 0xA1;
    writeCmdParam(0xD6, &d6_val, 1);

    const uint8_t pvgam[] = {
        0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30
    };
    writeCmdParam(0xE0, pvgam, sizeof(pvgam));

    const uint8_t nvgam[] = {
        0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32
    };
    writeCmdParam(0xE1, nvgam, sizeof(nvgam));

    writeCmdParam(0x21);  // INVON
    writeCmdParam(0x11);  // SLPOUT
    delay(120);
    writeCmdParam(0x29);  // DISPON

    ledcAttach(board::PIN_LCD_BL, 1000, 10);
    setBacklight(100);
    bus_ready_ = true;
}

void St7789Lcd::setLandscapeFlip(bool flipped) {
    if (!io_) return;
    waitIdle();
    const uint8_t madctl = flipped ? 0x60 : 0xA0;
    writeCmdParam(0x36, &madctl, 1);
}

void St7789Lcd::setBacklight(uint8_t pct) {
    if (pct > 100) pct = 100;
    uint32_t duty = (static_cast<uint32_t>(pct) * 1023U) / 100U;
    ledcWrite(board::PIN_LCD_BL, duty);
}

void St7789Lcd::beginVerticalWindow(uint16_t y0, uint16_t y1) {
    if (!io_) return;
    const uint16_t col_start = board::LCD_X_OFFSET;
    const uint16_t col_end   = static_cast<uint16_t>(board::LCD_WIDTH - 1U + board::LCD_X_OFFSET);
    const uint16_t row_start = static_cast<uint16_t>(y0 + board::LCD_Y_OFFSET);
    const uint16_t row_end   = static_cast<uint16_t>(y1 + board::LCD_Y_OFFSET);

    const uint8_t caset[4] = {
        static_cast<uint8_t>(col_start >> 8),
        static_cast<uint8_t>(col_start & 0xFF),
        static_cast<uint8_t>(col_end >> 8),
        static_cast<uint8_t>(col_end & 0xFF),
    };
    const uint8_t raset[4] = {
        static_cast<uint8_t>(row_start >> 8),
        static_cast<uint8_t>(row_start & 0xFF),
        static_cast<uint8_t>(row_end >> 8),
        static_cast<uint8_t>(row_end & 0xFF),
    };

    writeCmdParam(0x2A, caset, sizeof(caset));
    writeCmdParam(0x2B, raset, sizeof(raset));
}

uint16_t* St7789Lcd::acquireStripBuffer(uint8_t& out_slot) {
    uint8_t slot = next_slot_ & 1U;
    next_slot_ = static_cast<uint8_t>((slot + 1U) & 1U);
    out_slot = slot;
    if (!dma_buf_[slot] || !buf_sem_[slot]) {
        return nullptr;
    }
    if (xSemaphoreTake(buf_sem_[slot], pdMS_TO_TICKS(500)) != pdTRUE) {
        return nullptr;
    }
    return dma_buf_[slot];
}

void St7789Lcd::submitStripBuffer(uint8_t slot, size_t byte_len, bool first_in_window) {
    if (!io_ || slot >= 2 || !dma_buf_[slot] || byte_len == 0) {
        if (slot < 2 && buf_sem_[slot]) {
            xSemaphoreGive(buf_sem_[slot]);
        }
        return;
    }

    portENTER_CRITICAL(&fifo_mux_);
    in_flight_fifo_[fifo_head_ & 3U] = slot;
    fifo_head_ = static_cast<uint8_t>(fifo_head_ + 1U);
    portEXIT_CRITICAL(&fifo_mux_);

    int lcd_cmd = first_in_window ? 0x2C : -1;
    if (esp_lcd_panel_io_tx_color(io_, lcd_cmd, dma_buf_[slot], byte_len) != ESP_OK) {
        portENTER_CRITICAL(&fifo_mux_);
        if (fifo_head_ != fifo_tail_) {
            fifo_head_ = static_cast<uint8_t>(fifo_head_ - 1U);
        }
        portEXIT_CRITICAL(&fifo_mux_);
        xSemaphoreGive(buf_sem_[slot]);
    }
}

void St7789Lcd::waitIdle() {
    for (int i = 0; i < 2; ++i) {
        if (buf_sem_[i]) {
            if (xSemaphoreTake(buf_sem_[i], pdMS_TO_TICKS(500)) == pdTRUE) {
                xSemaphoreGive(buf_sem_[i]);
            }
        }
    }
}

}  // namespace lcd
