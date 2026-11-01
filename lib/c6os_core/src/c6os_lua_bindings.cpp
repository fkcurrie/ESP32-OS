#include "c6os_lua.h"

#include <lua.hpp>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <Arduino.h>
#endif

namespace c6os {
namespace luavm {

namespace {

constexpr int HOOK_SLICE_INSNS = 2500;
constexpr int MAX_HOOK_SLICES  = 120;  // 300,000 VM instructions max per invocation

// Prefix each Lua heap block with its exact size so quota accounting stays exact
struct LuaRunCtx {
    SystemState* state;
    VirtualFs* vfs;
    BufWriter* out;
    size_t used_bytes;
    size_t quota_bytes;
    int slices;
};

constexpr char REG_CTX_KEY = 'C';

LuaRunCtx* getCtx(lua_State* L) {
    lua_rawgetp(L, LUA_REGISTRYINDEX, &REG_CTX_KEY);
    auto* ctx = static_cast<LuaRunCtx*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ctx;
}

void* quotaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* ctx = static_cast<LuaRunCtx*>(ud);
    size_t old_bytes = ptr ? osize : 0;
    if (nsize == 0) {
        if (ptr) {
            if (ctx->used_bytes >= old_bytes) ctx->used_bytes -= old_bytes;
            else ctx->used_bytes = 0;
            free(ptr);
        }
        return nullptr;
    }
    if (nsize > old_bytes && (ctx->used_bytes + (nsize - old_bytes) > ctx->quota_bytes)) {
        return nullptr;
    }
    void* nptr = realloc(ptr, nsize);
    if (!nptr) return nullptr;
    ctx->used_bytes = ctx->used_bytes - old_bytes + nsize;
    return nptr;
}

void quotaHook(lua_State* L, lua_Debug* /*ar*/) {
    LuaRunCtx* ctx = getCtx(L);
    if (!ctx) return;
#ifdef ESP_PLATFORM
    delay(0);
#endif
    if (++ctx->slices > MAX_HOOK_SLICES) {
        luaL_error(L, "Lua instruction quota exceeded (300,000 VM ops)");
    }
}

int l_print(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    if (!ctx || !ctx->out) return 0;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t slen = 0;
        const char* s = luaL_tolstring(L, i, &slen);
        if (i > 1) ctx->out->putChar('\t');
        if (s && slen > 0) ctx->out->putRaw(s, slen);
        lua_pop(L, 1);
    }
    ctx->out->putChar('\n');
    return 0;
}

// ---- sys.* ----
int l_sys_uptime_ms(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const HardwareOps* hw = getHardwareOps();
    uint32_t ms = (hw && hw->uptime_ms) ? hw->uptime_ms() : (ctx->state->uptime_sec * 1000U);
    lua_pushinteger(L, static_cast<lua_Integer>(ms));
    return 1;
}

int l_sys_heap(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const HardwareOps* hw = getHardwareOps();
    uint32_t h = (hw && hw->free_heap) ? hw->free_heap() : ctx->state->free_sram_bytes;
    lua_pushinteger(L, static_cast<lua_Integer>(h));
    return 1;
}

int l_sys_min_heap(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->state->min_free_sram_bytes));
    return 1;
}

int l_sys_total_ram(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->state->total_sram_bytes));
    return 1;
}

int l_sys_temp_c(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_pushnumber(L, static_cast<lua_Number>(ctx->state->cpu_temp_deci_c) / 10.0f);
    return 1;
}

int l_sys_hostname(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_pushstring(L, ctx->state->hostname);
    return 1;
}

int l_sys_sleep(lua_State* L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    if (ms > 2000) ms = 2000;
#ifdef ESP_PLATFORM
    delay(static_cast<uint32_t>(ms));
#endif
    return 0;
}

// ---- c6.* (LCD, WS2812, PWM, ADC, I2C) ----
int l_c6_lcd_mode(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* mode = luaL_checkstring(L, 1);
    bool ok = ctx->vfs->writeFile(*ctx->state, "/sys/class/graphics/fb0/mode", mode, false);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_c6_lcd_theme(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* th = luaL_checkstring(L, 1);
    bool ok = ctx->vfs->writeFile(*ctx->state, "/sys/class/graphics/fb0/theme", th, false);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_c6_lcd_text(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* head = luaL_checkstring(L, 1);
    const char* sub  = luaL_optstring(L, 2, nullptr);
    strncpy(ctx->state->headline, head, sizeof(ctx->state->headline) - 1);
    ctx->state->headline[sizeof(ctx->state->headline) - 1] = '\0';
    if (sub) {
        strncpy(ctx->state->subtitle, sub, sizeof(ctx->state->subtitle) - 1);
        ctx->state->subtitle[sizeof(ctx->state->subtitle) - 1] = '\0';
    }
    ctx->state->markDirty(DIRTY_CARDS | DIRTY_FULL);
    lua_pushboolean(L, 1);
    return 1;
}

int l_c6_lcd_ticker(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* txt = luaL_checkstring(L, 1);
    bool ok = ctx->vfs->writeFile(*ctx->state, "/sys/class/graphics/fb0/ticker", txt, false);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_c6_lcd_alert(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* txt = luaL_checkstring(L, 1);
    bool ok = ctx->vfs->writeFile(*ctx->state, "/sys/class/graphics/fb0/alert", txt, false);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_c6_lcd_console(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* txt = luaL_checkstring(L, 1);
    ctx->state->appendFbConsoleLine(txt);
    lua_pushboolean(L, 1);
    return 1;
}

int l_c6_backlight(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_Integer pct = luaL_checkinteger(L, 1);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    ctx->state->backlight_pct = static_cast<uint8_t>(pct);
    ctx->state->markDirty(DIRTY_HEADER);
    lua_pushboolean(L, 1);
    return 1;
}

int l_c6_led(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    int r = static_cast<int>(luaL_checkinteger(L, 1)) & 0xFF;
    int g = static_cast<int>(luaL_checkinteger(L, 2)) & 0xFF;
    int b = static_cast<int>(luaL_checkinteger(L, 3)) & 0xFF;
    ctx->state->led_r = static_cast<uint8_t>(r);
    ctx->state->led_g = static_cast<uint8_t>(g);
    ctx->state->led_b = static_cast<uint8_t>(b);
    ctx->state->led_mode = LedMode::SOLID;
    ctx->state->markDirty(DIRTY_HEADER);
    lua_pushboolean(L, 1);
    return 1;
}

int l_c6_pwm(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    int pin = static_cast<int>(luaL_checkinteger(L, 1));
    int duty = static_cast<int>(luaL_checkinteger(L, 2));
    uint32_t hz = static_cast<uint32_t>(luaL_optinteger(L, 3, 5000));
    if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
        return luaL_error(L, "GPIO %d is invalid or reserved for board peripherals", pin);
    }
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;
    ctx->state->gpio_cmd_pin = static_cast<int8_t>(pin);
    ctx->state->gpio_cmd_action = 3;
    ctx->state->gpio_cmd_val = static_cast<uint8_t>(duty);
    const HardwareOps* hw = getHardwareOps();
    if (hw && hw->gpio_pwm) hw->gpio_pwm(pin, static_cast<uint8_t>(duty), hz);
    lua_pushboolean(L, 1);
    return 1;
}

int l_c6_adc(lua_State* L) {
    int pin = static_cast<int>(luaL_optinteger(L, 1, 0));
    if (pin < 0 || pin > 6 || isReservedBoardPin(pin)) {
        return luaL_error(L, "ADC1 requires GPIO 0..3 (got %d)", pin);
    }
    const HardwareOps* hw = getHardwareOps();
    int mv = (hw && hw->adc_read_mv) ? hw->adc_read_mv(pin) : 1650;
    lua_pushinteger(L, mv);
    return 1;
}

int l_c6_i2c_bus(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    int sda = static_cast<int>(luaL_checkinteger(L, 1));
    int scl = static_cast<int>(luaL_checkinteger(L, 2));
    uint32_t hz = static_cast<uint32_t>(luaL_optinteger(L, 3, 100000));
    if (sda < 0 || sda > 23 || scl < 0 || scl > 23 || sda == scl ||
        isReservedBoardPin(sda) || isReservedBoardPin(scl)) {
        return luaL_error(L, "invalid or reserved I2C pins (%d, %d)", sda, scl);
    }
    ctx->state->i2c_sda = static_cast<uint8_t>(sda);
    ctx->state->i2c_scl = static_cast<uint8_t>(scl);
    ctx->state->i2c_hz  = hz;
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->i2c_init) ? hw->i2c_init(ctx->state->i2c_sda, ctx->state->i2c_scl, hz) : 0;
    lua_pushboolean(L, rc == 0);
    return 1;
}

int l_c6_i2c_scan(lua_State* L) {
    uint8_t addrs[32];
    const HardwareOps* hw = getHardwareOps();
    int count = (hw && hw->i2c_scan) ? hw->i2c_scan(addrs, sizeof(addrs)) : 0;
    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_pushinteger(L, addrs[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_c6_i2c_read(lua_State* L) {
    int addr = static_cast<int>(luaL_checkinteger(L, 1));
    int reg  = static_cast<int>(luaL_checkinteger(L, 2));
    int len  = static_cast<int>(luaL_optinteger(L, 3, 1));
    if (len < 1) len = 1;
    if (len > 32) len = 32;
    uint8_t buf[32] = {0};
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->i2c_read_reg)
        ? hw->i2c_read_reg(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg), buf, static_cast<size_t>(len))
        : 0;
    if (rc < 0) {
        lua_pushnil(L);
        return 1;
    }
    if (len == 1) {
        lua_pushinteger(L, buf[0]);
    } else {
        lua_pushlstring(L, reinterpret_cast<const char*>(buf), static_cast<size_t>(len));
    }
    return 1;
}

int l_c6_i2c_write(lua_State* L) {
    int addr = static_cast<int>(luaL_checkinteger(L, 1));
    int reg  = static_cast<int>(luaL_checkinteger(L, 2));
    uint8_t byte_val = static_cast<uint8_t>(luaL_checkinteger(L, 3) & 0xFF);
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->i2c_write_reg)
        ? hw->i2c_write_reg(static_cast<uint8_t>(addr), static_cast<uint8_t>(reg), &byte_val, 1)
        : 0;
    lua_pushboolean(L, rc == 0);
    return 1;
}

// ---- gpio.* ----
int l_gpio_mode(lua_State* L) {
    int pin = static_cast<int>(luaL_checkinteger(L, 1));
    const char* mstr = luaL_optstring(L, 2, "out");
    if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
        return luaL_error(L, "GPIO %d is invalid or reserved", pin);
    }
    uint8_t mode = 1;
    if (strcmp(mstr, "in") == 0) mode = 2;
    else if (strcmp(mstr, "pullup") == 0) mode = 3;
    else if (strcmp(mstr, "pulldown") == 0) mode = 4;
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->gpio_mode) ? hw->gpio_mode(pin, mode) : 0;
    lua_pushboolean(L, rc == 0);
    return 1;
}

int l_gpio_write(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    int pin = static_cast<int>(luaL_checkinteger(L, 1));
    int val = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : static_cast<int>(luaL_checkinteger(L, 2));
    if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
        return luaL_error(L, "GPIO %d is invalid or reserved", pin);
    }
    if (val) ctx->state->gpio_out_mask |= (1U << pin);
    else ctx->state->gpio_out_mask &= ~(1U << pin);
    const HardwareOps* hw = getHardwareOps();
    if (hw && hw->gpio_write) hw->gpio_write(pin, val ? 1 : 0);
    lua_pushboolean(L, 1);
    return 1;
}

int l_gpio_read(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    int pin = static_cast<int>(luaL_checkinteger(L, 1));
    if (pin < 0 || pin > 23) return luaL_error(L, "GPIO %d out of range", pin);
    const HardwareOps* hw = getHardwareOps();
    int v = (hw && hw->gpio_read) ? hw->gpio_read(pin)
                                  : static_cast<int>((ctx->state->gpio_in_mask >> pin) & 1U);
    lua_pushinteger(L, v);
    return 1;
}

// ---- wifi.* ----
int l_wifi_status(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_newtable(L);
    lua_pushboolean(L, ctx->state->wifi_sta_connected ? 1 : 0); lua_setfield(L, -2, "connected");
    lua_pushstring(L, ctx->state->wifi_ssid);                   lua_setfield(L, -2, "ssid");
    lua_pushstring(L, ctx->state->wifi_ip);                     lua_setfield(L, -2, "ip");
    lua_pushinteger(L, ctx->state->wifi_rssi);                  lua_setfield(L, -2, "rssi");
    lua_pushboolean(L, ctx->state->wifi_ap_enabled ? 1 : 0);    lua_setfield(L, -2, "ap_enabled");
    lua_pushstring(L, ctx->state->wifi_ap_ip);                  lua_setfield(L, -2, "ap_ip");
    return 1;
}

int l_wifi_scan(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    ctx->state->wifi_scan_requested = true;
    lua_newtable(L);
    for (size_t i = 0; i < ctx->state->wifi_scan_count; ++i) {
        lua_newtable(L);
        lua_pushstring(L, ctx->state->wifi_scan[i].ssid);      lua_setfield(L, -2, "ssid");
        lua_pushinteger(L, ctx->state->wifi_scan[i].rssi);     lua_setfield(L, -2, "rssi");
        lua_pushinteger(L, ctx->state->wifi_scan[i].channel);  lua_setfield(L, -2, "channel");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// ---- ble.* ----
int l_ble_on(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->ble_enable) ? hw->ble_enable(true) : 0;
    if (rc == 0) ctx->state->ble_enabled = true;
    lua_pushboolean(L, rc == 0);
    return 1;
}

int l_ble_off(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const HardwareOps* hw = getHardwareOps();
    int rc = (hw && hw->ble_enable) ? hw->ble_enable(false) : 0;
    ctx->state->ble_enabled = false;
    ctx->state->ble_advertising = false;
    lua_pushboolean(L, rc == 0);
    return 1;
}

int l_ble_scan(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    uint32_t ms = static_cast<uint32_t>(luaL_optinteger(L, 1, 2000));
    if (ms > 8000U) ms = 8000U;
    BleScanResult results[12];
    const HardwareOps* hw = getHardwareOps();
    if (!ctx->state->ble_enabled && hw && hw->ble_enable) {
        if (hw->ble_enable(true) == 0) ctx->state->ble_enabled = true;
    }
    int count = (hw && hw->ble_scan) ? hw->ble_scan(ms, results, 12) : 0;
    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_newtable(L);
        lua_pushstring(L, results[i].addr);  lua_setfield(L, -2, "addr");
        lua_pushstring(L, results[i].name);  lua_setfield(L, -2, "name");
        lua_pushinteger(L, results[i].rssi); lua_setfield(L, -2, "rssi");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int l_ble_adv(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    bool en = lua_toboolean(L, 1) != 0;
    const char* name = luaL_optstring(L, 2, ctx->state->ble_adv_name);
    strncpy(ctx->state->ble_adv_name, name, sizeof(ctx->state->ble_adv_name) - 1);
    const HardwareOps* hw = getHardwareOps();
    if (en && !ctx->state->ble_enabled && hw && hw->ble_enable) {
        if (hw->ble_enable(true) == 0) ctx->state->ble_enabled = true;
    }
    int rc = (hw && hw->ble_advertise) ? hw->ble_advertise(en, ctx->state->ble_adv_name) : 0;
    if (rc == 0) ctx->state->ble_advertising = en;
    lua_pushboolean(L, rc == 0);
    return 1;
}

int l_ble_status(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    lua_newtable(L);
    lua_pushboolean(L, ctx->state->ble_enabled ? 1 : 0);      lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, ctx->state->ble_advertising ? 1 : 0);  lua_setfield(L, -2, "advertising");
    lua_pushstring(L, ctx->state->ble_adv_name);              lua_setfield(L, -2, "name");
    return 1;
}

// ---- fs.* ----
int l_fs_read(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* p = luaL_checkstring(L, 1);
    char norm[160];
    VirtualFs::normalizePath("/root", p, norm, sizeof(norm));
    auto* buf = static_cast<char*>(malloc(4096));
    if (!buf) return luaL_error(L, "out of memory");
    bool ok = ctx->vfs->readFile(*ctx->state, norm, buf, 4096);
    if (!ok) {
        free(buf);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    free(buf);
    return 1;
}

int l_fs_write(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* p = luaL_checkstring(L, 1);
    size_t dlen = 0;
    const char* data = luaL_checklstring(L, 2, &dlen);
    bool append = lua_toboolean(L, 3) != 0;
    char norm[160];
    VirtualFs::normalizePath("/root", p, norm, sizeof(norm));
    bool ok = false;
    if (ctx->vfs->isVirtualPath(norm)) {
        ok = ctx->vfs->writeFile(*ctx->state, norm, data, append);
    } else {
        ok = ctx->vfs->writeBinary(*ctx->state, norm, reinterpret_cast<const uint8_t*>(data), dlen, append);
    }
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_fs_ls(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* p = luaL_optstring(L, 1, "/root");
    char norm[160];
    VirtualFs::normalizePath("/root", p, norm, sizeof(norm));
    VfsDirEntry entries[32];
    size_t n = ctx->vfs->readDirEntries(norm, entries, 32);
    lua_newtable(L);
    for (size_t i = 0; i < n; ++i) {
        lua_pushstring(L, entries[i].name);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int l_fs_rm(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* p = luaL_checkstring(L, 1);
    char norm[160];
    VirtualFs::normalizePath("/root", p, norm, sizeof(norm));
    bool ok = ctx->vfs->removeFile(*ctx->state, norm);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int l_fs_exists(lua_State* L) {
    LuaRunCtx* ctx = getCtx(L);
    const char* p = luaL_checkstring(L, 1);
    char norm[160];
    VirtualFs::normalizePath("/root", p, norm, sizeof(norm));
    lua_pushboolean(L, ctx->vfs->statPath(norm, nullptr, nullptr, nullptr) ? 1 : 0);
    return 1;
}

void registerModule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    int count = 0;
    while (funcs[count].name != nullptr) ++count;
    luaL_checkversion(L);
    lua_createtable(L, 0, count);
    luaL_setfuncs(L, funcs, 0);
    lua_setglobal(L, name);
}

void openC6Libraries(lua_State* L) {
    static const luaL_Reg SAFE_STD[] = {
        {LUA_GNAME,       luaopen_base},
        {LUA_COLIBNAME,   luaopen_coroutine},
        {LUA_TABLIBNAME,  luaopen_table},
        {LUA_STRLIBNAME,  luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {nullptr, nullptr}
    };
    for (const luaL_Reg* lib = SAFE_STD; lib->func != nullptr; ++lib) {
        luaL_requiref(L, lib->name, lib->func, 1);
        lua_pop(L, 1);
    }

    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    static const luaL_Reg SYS_LIB[] = {
        {"uptime_ms", l_sys_uptime_ms},
        {"heap",      l_sys_heap},
        {"free_ram",  l_sys_heap},
        {"total_ram", l_sys_total_ram},
        {"min_heap",  l_sys_min_heap},
        {"temp_c",    l_sys_temp_c},
        {"hostname",  l_sys_hostname},
        {"sleep",     l_sys_sleep},
        {nullptr, nullptr}
    };
    registerModule(L, "sys", SYS_LIB);

    static const luaL_Reg C6_LIB[] = {
        {"lcd_mode",    l_c6_lcd_mode},
        {"mode",        l_c6_lcd_mode},
        {"lcd_theme",   l_c6_lcd_theme},
        {"theme",       l_c6_lcd_theme},
        {"lcd_text",    l_c6_lcd_text},
        {"text",        l_c6_lcd_text},
        {"lcd_ticker",  l_c6_lcd_ticker},
        {"ticker",      l_c6_lcd_ticker},
        {"lcd_alert",   l_c6_lcd_alert},
        {"alert",       l_c6_lcd_alert},
        {"lcd_console", l_c6_lcd_console},
        {"console",     l_c6_lcd_console},
        {"backlight",   l_c6_backlight},
        {"led",         l_c6_led},
        {"pwm",         l_c6_pwm},
        {"adc",         l_c6_adc},
        {"i2c_bus",     l_c6_i2c_bus},
        {"i2c_scan",    l_c6_i2c_scan},
        {"i2c_read",    l_c6_i2c_read},
        {"i2c_write",   l_c6_i2c_write},
        {nullptr, nullptr}
    };
    registerModule(L, "c6", C6_LIB);

    static const luaL_Reg GPIO_LIB[] = {
        {"mode",  l_gpio_mode},
        {"write", l_gpio_write},
        {"read",  l_gpio_read},
        {"pwm",   l_c6_pwm},
        {nullptr, nullptr}
    };
    registerModule(L, "gpio", GPIO_LIB);

    static const luaL_Reg WIFI_LIB[] = {
        {"status", l_wifi_status},
        {"scan",   l_wifi_scan},
        {nullptr, nullptr}
    };
    registerModule(L, "wifi", WIFI_LIB);

    static const luaL_Reg BLE_LIB[] = {
        {"on",     l_ble_on},
        {"off",    l_ble_off},
        {"scan",   l_ble_scan},
        {"adv",    l_ble_adv},
        {"status", l_ble_status},
        {nullptr, nullptr}
    };
    registerModule(L, "ble", BLE_LIB);

    static const luaL_Reg FS_LIB[] = {
        {"read",   l_fs_read},
        {"write",  l_fs_write},
        {"ls",     l_fs_ls},
        {"rm",     l_fs_rm},
        {"exists", l_fs_exists},
        {nullptr, nullptr}
    };
    registerModule(L, "fs", FS_LIB);
}

}  // namespace

bool runChunk(SystemState& state, VirtualFs& vfs, const char* chunk_name,
              const char* code, int argc, const char* const* argv,
              BufWriter& out, size_t quota_bytes) {
    if (!code) return false;
    if (quota_bytes < 20480) quota_bytes = 20480;

    LuaRunCtx ctx = {&state, &vfs, &out, 0, quota_bytes, 0};
    lua_State* L = lua_newstate(quotaAlloc, &ctx);
    if (!L) {
        out.putStr("lua: failed to initialize 32-bit Lua 5.4 state\n");
        return false;
    }

    lua_pushlightuserdata(L, &ctx);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &REG_CTX_KEY);
    openC6Libraries(L);

    lua_newtable(L);
    for (int i = 0; i < argc && argv; ++i) {
        lua_pushstring(L, argv[i] ? argv[i] : "");
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");

    lua_sethook(L, quotaHook, LUA_MASKCOUNT, HOOK_SLICE_INSNS);

    // Strip leading `#!...` shebang if present
    const char* src = code;
    if (src[0] == '#' && src[1] == '!') {
        const char* nl = strchr(src, '\n');
        src = nl ? (nl + 1) : "";
    }

    int rc = luaL_loadbuffer(L, src, strlen(src), chunk_name ? chunk_name : "=(chunk)");
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        out.fmt("lua: %s\n", err ? err : "execution error");
        lua_close(L);
        return false;
    }

    lua_close(L);
    return true;
}

bool runFile(SystemState& state, VirtualFs& vfs, const char* script_abs_path,
             int argc, const char* const* argv,
             BufWriter& out, size_t quota_bytes) {
    size_t sz = 0;
    if (!vfs.fileSize(script_abs_path, &sz) || sz > 32768U) {
        out.fmt("lua: cannot open '%s'\n", script_abs_path ? script_abs_path : "");
        return false;
    }
    auto* buf = static_cast<char*>(malloc(sz + 1));
    if (!buf) {
        out.putStr("lua: out of heap loading script\n");
        return false;
    }
    size_t got = 0;
    if (!vfs.readBinary(script_abs_path, 0, reinterpret_cast<uint8_t*>(buf), sz, &got)) {
        free(buf);
        out.fmt("lua: read error on '%s'\n", script_abs_path);
        return false;
    }
    buf[got] = '\0';
    bool ok = runChunk(state, vfs, script_abs_path, buf, argc, argv, out, quota_bytes);
    free(buf);
    return ok;
}

}  // namespace luavm
}  // namespace c6os
