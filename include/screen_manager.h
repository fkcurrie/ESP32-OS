#ifndef SCREEN_MANAGER_H
#define SCREEN_MANAGER_H

#include "c6os_types.h"
#include "st7789_lcd.h"

namespace c6os {

class ScreenManager {
public:
    ScreenManager()
        : last_mode_(ScreenMode::COUNT),
          last_theme_(ThemeId::COUNT),
          last_backlight_(255),
          last_flipped_(false),
          last_alert_(false),
          last_ticker_(false),
          last_fbi_pattern_(255) {}

    void begin();
    void render(const SystemState& state, bool force_full_clear = false);

private:
    void renderDashboard(const SystemState& state, const ThemePalette& pal);
    void renderFbConsole(const SystemState& state, const ThemePalette& pal);
    void renderSignboard(const SystemState& state, const ThemePalette& pal);
    void renderDiagnostics(const SystemState& state, const ThemePalette& pal);
    void renderMonitor(const SystemState& state, const ThemePalette& pal);
    void renderImageView(const SystemState& state, const ThemePalette& pal);
    void renderOverlays(const SystemState& state, const ThemePalette& pal);

    lcd::St7789Lcd lcd_;
    ScreenMode last_mode_;
    ThemeId last_theme_;
    uint8_t last_backlight_;
    bool last_flipped_;
    bool last_alert_;
    bool last_ticker_;
    uint8_t last_fbi_pattern_;
};

}  // namespace c6os

#endif  // SCREEN_MANAGER_H
