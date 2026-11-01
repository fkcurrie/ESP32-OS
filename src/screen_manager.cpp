#include "screen_manager.h"

#include "c6os_display.h"

namespace c6os {

void ScreenManager::begin() {
    lcd_.begin();
}

void ScreenManager::renderState(const SystemState& state, bool force_full) {
    RenderSnapshot snap;
    snap.captureFrom(state, force_full ? DIRTY_FULL : state.dirty_regions);
    render(snap, force_full);
}

void ScreenManager::render(const RenderSnapshot& snap, bool force_full) {
    if (snap.backlight_pct != last_backlight_) {
        lcd_.setBacklight(snap.backlight_pct);
        last_backlight_ = snap.backlight_pct;
    }

    bool mode_changed = (snap.screen_mode != last_mode_) ||
                        (snap.theme != last_theme_) ||
                        (snap.landscape_flipped != last_flipped_) ||
                        (snap.screen_mode == ScreenMode::IMAGE_VIEW &&
                         snap.fbi_pattern != last_fbi_pattern_);

    if (snap.landscape_flipped != last_flipped_) {
        lcd_.setLandscapeFlip(snap.landscape_flipped);
        last_flipped_ = snap.landscape_flipped;
    }

    uint8_t dirty = snap.dirty_regions;
    if (force_full || mode_changed) {
        dirty = DIRTY_FULL;
    } else {
        if (last_alert_ && !snap.alert_active) {
            dirty |= DIRTY_FULL;
        }
        if (last_ticker_ && !snap.ticker_enabled) {
            dirty |= DIRTY_CARDS | DIRTY_TICKER | DIRTY_CONSOLE;
        }
        if (last_custom_bar_ && !snap.custom_bar_visible) {
            dirty |= DIRTY_CARDS | DIRTY_TICKER;
        }
        if (dirty == DIRTY_NONE) {
            dirty = DIRTY_FULL;
        }
    }

    last_mode_ = snap.screen_mode;
    last_theme_ = snap.theme;
    last_alert_ = snap.alert_active;
    last_ticker_ = snap.ticker_enabled;
    last_custom_bar_ = snap.custom_bar_visible;
    last_fbi_pattern_ = snap.fbi_pattern;

    constexpr int16_t strip_rows = display::DMA_STRIP_ROWS;
    int16_t y = 0;
    while (y < display::FB_HEIGHT) {
        int16_t y_end = static_cast<int16_t>(y + strip_rows);
        if (y_end > display::FB_HEIGHT) y_end = display::FB_HEIGHT;

        if (!display::stripOverlapsDirty(dirty, y, y_end)) {
            y = y_end;
            continue;
        }

        // Find the full contiguous vertical run of dirty strips so CASET/RASET is sent once per run
        int16_t run_end = y_end;
        while (run_end < display::FB_HEIGHT) {
            int16_t next_end = static_cast<int16_t>(run_end + strip_rows);
            if (next_end > display::FB_HEIGHT) next_end = display::FB_HEIGHT;
            if (!display::stripOverlapsDirty(dirty, run_end, next_end)) {
                break;
            }
            run_end = next_end;
        }

        lcd_.beginVerticalWindow(static_cast<uint16_t>(y), static_cast<uint16_t>(run_end - 1));

        bool first_strip = true;
        while (y < run_end) {
            int16_t h = static_cast<int16_t>(run_end - y);
            if (h > strip_rows) h = strip_rows;

            uint8_t slot = 0;
            uint16_t* buf = lcd_.acquireStripBuffer(slot);
            if (!buf) break;

            display::renderStrip(snap, y, h, buf, /*big_endian_out=*/true);
            size_t bytes = static_cast<size_t>(display::FB_WIDTH) *
                           static_cast<size_t>(h) * sizeof(uint16_t);
            lcd_.submitStripBuffer(slot, bytes, first_strip);
            first_strip = false;
            y = static_cast<int16_t>(y + h);
        }
    }

    lcd_.waitIdle();
}

}  // namespace c6os
