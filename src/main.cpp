#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/sdspi_host.h>
#include <esp_bt.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <fcntl.h>
#include <host/ble_gap.h>
#include <host/ble_hs.h>
#include <host/util/util.h>
#include <lwip/ip_addr.h>
#include <lwip/sockets.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <ping/ping_sock.h>
#include <sdmmc_cmd.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <strings.h>
#include <time.h>

#include "board_pins.h"
#include "c6os_shell.h"
#include "c6os_types.h"
#include "c6os_vfs.h"
#include "screen_manager.h"
#include "ssh_auth.h"
#include "ssh_server.h"

extern "C" void ble_store_config_init(void);

SET_LOOP_TASK_STACK_SIZE(8 * 1024);

namespace {

constexpr const char* SOFTAP_SSID = "C6-Linux-OS";
constexpr const char* SOFTAP_PASS = "esp32linux";

c6os::SystemState g_state;
c6os::VirtualFs g_vfs;
c6os::ScreenManager g_screen;
c6os::SshServer g_sshd(22);
c6os::ShellSession g_bg_shell;
int g_httpd_fd = -1;
int g_syslog_fd = -1;
SemaphoreHandle_t g_mutex = nullptr;

bool g_littlefs_mounted = false;
sdmmc_card_t* g_sd_card = nullptr;

// Button debounce & press timing
volatile bool g_btn_irq = false;
bool g_btn_pressed = false;
uint32_t g_btn_press_start_ms = 0;
constexpr uint32_t BTN_DEBOUNCE_MS   = 40;
constexpr uint32_t BTN_LONG_PRESS_MS = 700;

void IRAM_ATTR onBootButtonIsr() {
    g_btn_irq = true;
}

uint8_t g_led_phase = 0;

// Peripheral runtime state (minimal scalar footprint; 0 B BLE heap when off)
uint32_t g_pwm_attached_mask = 0;
uint32_t g_pwm_freq_hz[24] = {0};
bool g_i2c_ready = false;

volatile bool g_ble_ready = false;
volatile bool g_ble_synced = false;
uint8_t g_ble_own_addr_type = BLE_OWN_ADDR_PUBLIC;

// Event-driven Wi-Fi flags
volatile bool g_wifi_got_ip_evt = false;
volatile bool g_wifi_lost_evt = false;

// FreeRTOS runtime delta counters for real CPU load %
uint32_t g_prev_total_runtime = 0;
uint32_t g_prev_idle_runtime = 0;

void formatRandomHex32(char out_hex[33]) {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out_hex[i * 2]     = HEX_DIGITS[(raw[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = HEX_DIGITS[raw[i] & 0x0F];
    }
    out_hex[32] = '\0';
}

// ============================================================================
// HardwareOps Implementation (GPIO, PWM, ADC, I2C, Lazy BLE 5, Ping, Tasks)
// ============================================================================

int hwGpioMode(int pin, uint8_t mode) {
    if (pin < 0 || pin > 23 || c6os::isReservedBoardPin(pin)) return -1;
    if (g_pwm_attached_mask & (1UL << pin)) {
        ledcDetach(pin);
        g_pwm_attached_mask &= ~(1UL << pin);
    }
    switch (mode) {
        case 1: pinMode(pin, OUTPUT); break;
        case 2: pinMode(pin, INPUT); break;
        case 3: pinMode(pin, INPUT_PULLUP); break;
        case 4: pinMode(pin, INPUT_PULLDOWN); break;
        default: return -1;
    }
    return 0;
}

int hwGpioWrite(int pin, int val) {
    if (pin < 0 || pin > 23 || c6os::isReservedBoardPin(pin)) return -1;
    if (g_pwm_attached_mask & (1UL << pin)) {
        ledcDetach(pin);
        g_pwm_attached_mask &= ~(1UL << pin);
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val ? HIGH : LOW);
    return 0;
}

int hwGpioRead(int pin) {
    if (pin < 0 || pin > 23) return -1;
    return (digitalRead(pin) == HIGH) ? 1 : 0;
}

int hwGpioPwm(int pin, uint8_t duty, uint32_t freq_hz) {
    if (pin < 0 || pin > 23 || c6os::isReservedBoardPin(pin)) return -1;
    if (freq_hz == 0) freq_hz = 1000U;
    uint32_t bit = (1UL << pin);
    if ((g_pwm_attached_mask & bit) && g_pwm_freq_hz[pin] != freq_hz) {
        ledcDetach(pin);
        g_pwm_attached_mask &= ~bit;
    }
    if (!(g_pwm_attached_mask & bit)) {
        if (!ledcAttach(pin, freq_hz, 8)) return -1;
        g_pwm_attached_mask |= bit;
        g_pwm_freq_hz[pin] = freq_hz;
    }
    ledcWrite(pin, duty);
    return 0;
}

int hwAdcReadMv(int pin) {
    if (pin < 0 || pin > 3) return -1;
    return static_cast<int>(analogReadMilliVolts(pin));
}

int hwI2cInit(uint8_t sda, uint8_t scl, uint32_t hz) {
    if (sda > 23 || scl > 23 || sda == scl ||
        c6os::isReservedBoardPin(sda) || c6os::isReservedBoardPin(scl)) {
        return -1;
    }
    if (g_i2c_ready) {
        Wire.end();
        g_i2c_ready = false;
    }
    if (!Wire.begin(static_cast<int>(sda), static_cast<int>(scl), hz ? hz : 100000U)) {
        return -1;
    }
    g_i2c_ready = true;
    return 0;
}

int hwI2cScan(uint8_t* out_addrs, size_t max_addrs) {
    if (!g_i2c_ready) {
        if (hwI2cInit(g_state.i2c_sda, g_state.i2c_scl, g_state.i2c_hz) != 0) {
            return -1;
        }
    }
    size_t found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (out_addrs && found < max_addrs) {
                out_addrs[found] = addr;
            }
            ++found;
        }
    }
    return static_cast<int>(found);
}

int hwI2cWriteReg(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len) {
    if (!g_i2c_ready && hwI2cInit(g_state.i2c_sda, g_state.i2c_scl, g_state.i2c_hz) != 0) {
        return -1;
    }
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (data && len > 0) {
        Wire.write(data, len);
    }
    return (Wire.endTransmission() == 0) ? 0 : -1;
}

int hwI2cReadReg(uint8_t addr, uint8_t reg, uint8_t* data, size_t len) {
    if (!data || len == 0) return -1;
    if (!g_i2c_ready && hwI2cInit(g_state.i2c_sda, g_state.i2c_scl, g_state.i2c_hz) != 0) {
        return -1;
    }
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        return -1;
    }
    size_t got = Wire.requestFrom(static_cast<int>(addr), static_cast<int>(len));
    if (got < len) return -1;
    for (size_t i = 0; i < len; ++i) {
        int b = Wire.read();
        data[i] = (b >= 0) ? static_cast<uint8_t>(b) : 0;
    }
    return 0;
}

void onBleReset(int) {
    g_ble_synced = false;
}

void onBleSync() {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ble_hs_util_ensure_addr(1);
    }
    if (ble_hs_id_copy_addr(BLE_OWN_ADDR_PUBLIC, nullptr, nullptr) == 0) {
        g_ble_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    } else {
        g_ble_own_addr_type = BLE_OWN_ADDR_RANDOM;
    }
    g_ble_synced = true;
}

void bleHostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int hwBleEnable(bool enable) {
    if (enable) {
        if (g_ble_ready) return 0;
        g_ble_synced = false;
        if (nimble_port_init() != ESP_OK) {
            return -1;
        }
        ble_hs_cfg.reset_cb = onBleReset;
        ble_hs_cfg.sync_cb = onBleSync;
        ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
        ble_svc_gap_init();
        ble_svc_gatt_init();
        ble_store_config_init();
        nimble_port_freertos_init(bleHostTask);

        uint32_t t0 = millis();
        while (!g_ble_synced && (millis() - t0 < 1500U)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!g_ble_synced) {
            nimble_port_stop();
            nimble_port_deinit();
            return -1;
        }
        ble_svc_gap_device_name_set("c6-linux");
        g_ble_ready = true;
        return 0;
    }

    if (!g_ble_ready) return 0;
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        esp_bt_controller_deinit();
    }
    g_ble_ready = false;
    g_ble_synced = false;
    return 0;
}

struct BleScanCtx {
    c6os::BleScanResult* results;
    size_t max_results;
    size_t count;
    SemaphoreHandle_t done_sem;
};

void extractBleAdvName(const uint8_t* data, uint8_t data_len, char* out_name, size_t cap) {
    if (!data || !out_name || cap == 0) return;
    size_t pos = 0;
    while (pos + 1 < data_len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + 1 + field_len > data_len) break;
        uint8_t ad_type = data[pos + 1];
        if (ad_type == 0x09 || (ad_type == 0x08 && out_name[0] == '\0')) {
            size_t nlen = field_len - 1U;
            if (nlen >= cap) nlen = cap - 1U;
            memcpy(out_name, data + pos + 2, nlen);
            out_name[nlen] = '\0';
            if (ad_type == 0x09) return;
        }
        pos += static_cast<size_t>(field_len) + 1U;
    }
}

int bleScanGapCb(struct ble_gap_event* event, void* arg) {
    BleScanCtx* ctx = static_cast<BleScanCtx*>(arg);
    if (!ctx || !event) return 0;

    if (event->type == BLE_GAP_EVENT_DISC) {
        char addr_str[18];
        const uint8_t* v = event->disc.addr.val;
        snprintf(addr_str, sizeof(addr_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 v[5], v[4], v[3], v[2], v[1], v[0]);

        for (size_t i = 0; i < ctx->count; ++i) {
            if (strcmp(ctx->results[i].addr, addr_str) == 0) {
                ctx->results[i].rssi = event->disc.rssi;
                extractBleAdvName(event->disc.data, event->disc.length_data,
                                  ctx->results[i].name, sizeof(ctx->results[i].name));
                return 0;
            }
        }

        if (ctx->results && ctx->count < ctx->max_results) {
            auto& entry = ctx->results[ctx->count++];
            memcpy(entry.addr, addr_str, sizeof(entry.addr));
            entry.name[0] = '\0';
            entry.rssi = event->disc.rssi;
            extractBleAdvName(event->disc.data, event->disc.length_data,
                              entry.name, sizeof(entry.name));
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (ctx->done_sem) {
            xSemaphoreGive(ctx->done_sem);
        }
    }
    return 0;
}

int hwBleScan(uint32_t duration_ms, c6os::BleScanResult* out_results, size_t max_results) {
    if (!g_ble_ready && hwBleEnable(true) != 0) {
        return -1;
    }
    ble_gap_disc_cancel();

    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) return -1;

    BleScanCtx ctx{out_results, max_results, 0, done_sem};
    struct ble_gap_disc_params params = {};
    params.itvl = 0x30;
    params.window = 0x30;
    params.filter_policy = 0;
    params.limited = 0;
    params.passive = 0;
    params.filter_duplicates = 1;

    int rc = ble_gap_disc(g_ble_own_addr_type, static_cast<int32_t>(duration_ms),
                          &params, bleScanGapCb, &ctx);
    if (rc == 0) {
        xSemaphoreTake(done_sem, pdMS_TO_TICKS(duration_ms + 500U));
        ble_gap_disc_cancel();
    }
    vSemaphoreDelete(done_sem);
    return (rc == 0) ? static_cast<int>(ctx.count) : -1;
}

int hwBleAdvertise(bool enable, const char* name) {
    if (!enable) {
        if (g_ble_ready) {
            ble_gap_adv_stop();
        }
        return 0;
    }
    if (!g_ble_ready && hwBleEnable(true) != 0) {
        return -1;
    }
    ble_gap_adv_stop();

    const char* adv_name = (name && name[0]) ? name : "c6-linux";
    ble_svc_gap_device_name_set(adv_name);

    struct ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = reinterpret_cast<const uint8_t*>(adv_name);
    fields.name_len = static_cast<uint8_t>(strnlen(adv_name, 20));
    fields.name_is_complete = 1;

    if (ble_gap_adv_set_fields(&fields) != 0) {
        return -1;
    }

    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    return (ble_gap_adv_start(g_ble_own_addr_type, nullptr, BLE_HS_FOREVER,
                              &adv_params, nullptr, nullptr) == 0)
               ? 0
               : -1;
}

struct PingRunCtx {
    c6os::PingStats* stats;
    uint64_t rtt_sum_us;
    SemaphoreHandle_t done_sem;
};

void onPingSuccess(esp_ping_handle_t hdl, void* args) {
    PingRunCtx* ctx = static_cast<PingRunCtx*>(args);
    if (!ctx || !ctx->stats) return;
    uint32_t elapsed_ms = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    uint32_t rtt_us = (elapsed_ms > 0) ? (elapsed_ms * 1000U) : 450U;

    uint16_t idx = ctx->stats->received;
    if (idx < 5) {
        ctx->stats->rtts_us[idx] = rtt_us;
    }
    ctx->stats->sent++;
    ctx->stats->received++;
    ctx->stats->reachable = true;
    if (ctx->stats->received == 1 || rtt_us < ctx->stats->min_us) {
        ctx->stats->min_us = rtt_us;
    }
    if (rtt_us > ctx->stats->max_us) {
        ctx->stats->max_us = rtt_us;
    }
    ctx->rtt_sum_us += rtt_us;
    ctx->stats->avg_us = static_cast<uint32_t>(ctx->rtt_sum_us / ctx->stats->received);
}

void onPingTimeout(esp_ping_handle_t, void* args) {
    PingRunCtx* ctx = static_cast<PingRunCtx*>(args);
    if (ctx && ctx->stats) {
        ctx->stats->sent++;
    }
}

void onPingEnd(esp_ping_handle_t, void* args) {
    PingRunCtx* ctx = static_cast<PingRunCtx*>(args);
    if (ctx && ctx->done_sem) {
        xSemaphoreGive(ctx->done_sem);
    }
}

int hwIcmpPing(const char* host, uint16_t count, uint32_t timeout_ms, c6os::PingStats* out_stats) {
    if (!host || !out_stats) return -1;
    memset(out_stats, 0, sizeof(*out_stats));

    IPAddress resolved;
    if (!WiFi.hostByName(host, resolved)) {
        return -1;
    }

    ip_addr_t target_addr = {};
    String ip_str = resolved.toString();
    if (!ipaddr_aton(ip_str.c_str(), &target_addr)) {
        return -1;
    }

    if (count == 0) count = 3;
    if (count > 5) count = 5;
    if (timeout_ms < 200U) timeout_ms = 800U;

    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) return -1;

    PingRunCtx ctx{out_stats, 0, done_sem};
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target_addr;
    cfg.count = count;
    cfg.interval_ms = 200;
    cfg.timeout_ms = timeout_ms;
    cfg.task_stack_size = 3072;

    esp_ping_callbacks_t cbs = {};
    cbs.cb_args = &ctx;
    cbs.on_ping_success = onPingSuccess;
    cbs.on_ping_timeout = onPingTimeout;
    cbs.on_ping_end = onPingEnd;

    esp_ping_handle_t ping_hdl = nullptr;
    if (esp_ping_new_session(&cfg, &cbs, &ping_hdl) != ESP_OK || !ping_hdl) {
        vSemaphoreDelete(done_sem);
        return -1;
    }

    esp_ping_start(ping_hdl);
    xSemaphoreTake(done_sem, pdMS_TO_TICKS(count * (timeout_ms + 250U) + 500U));
    esp_ping_stop(ping_hdl);
    esp_ping_delete_session(ping_hdl);
    vSemaphoreDelete(done_sem);
    return 0;
}

size_t hwGetTasks(c6os::TaskInfoEntry* out_tasks, size_t max_tasks, uint32_t* out_total_cpu_permille) {
    if (!out_tasks || max_tasks == 0) {
        // Fast O(1) path for 1 Hz dashboard CPU sampling: avoids heap allocation
        // and scheduler-wide vTaskSuspendAll() inside uxTaskGetSystemState().
        uint32_t total_runtime = static_cast<uint32_t>(esp_timer_get_time());
        uint32_t idle_runtime  = ulTaskGetIdleRunTimeCounter();
        uint32_t dt_total = total_runtime - g_prev_total_runtime;
        uint32_t dt_idle  = idle_runtime - g_prev_idle_runtime;
        if (dt_total > 0) {
            g_prev_total_runtime = total_runtime;
            g_prev_idle_runtime  = idle_runtime;
        }
        uint32_t active_permille = 10U;
        if (dt_total > 0 && dt_idle <= dt_total) {
            active_permille = static_cast<uint32_t>(
                (static_cast<uint64_t>(dt_total - dt_idle) * 1000ULL) / dt_total);
        } else if (dt_total > 0) {
            active_permille = 0U;
        }
        if (out_total_cpu_permille) {
            *out_total_cpu_permille = active_permille;
        }
        return static_cast<size_t>(uxTaskGetNumberOfTasks());
    }

    UBaseType_t num = uxTaskGetNumberOfTasks();
    size_t alloc_n = static_cast<size_t>(num) + 4U;
    c6os::HeapArray<TaskStatus_t> sys_tasks(alloc_n);
    if (!sys_tasks) {
        if (out_total_cpu_permille) *out_total_cpu_permille = 0;
        return 0;
    }

    uint32_t total_runtime = 0;
    UBaseType_t got = uxTaskGetSystemState(sys_tasks.get(), alloc_n, &total_runtime);

    uint32_t idle_runtime = 0;
    for (UBaseType_t i = 0; i < got; ++i) {
        if (strncmp(sys_tasks[i].pcTaskName, "IDLE", 4) == 0) {
            idle_runtime += sys_tasks[i].ulRunTimeCounter;
        }
    }

    uint32_t dt_total = total_runtime - g_prev_total_runtime;
    uint32_t dt_idle  = idle_runtime - g_prev_idle_runtime;
    if (dt_total > 0) {
        g_prev_total_runtime = total_runtime;
        g_prev_idle_runtime = idle_runtime;
    }

    uint32_t active_permille = 10U;
    if (dt_total > 0 && dt_idle <= dt_total) {
        active_permille = static_cast<uint32_t>(
            (static_cast<uint64_t>(dt_total - dt_idle) * 1000ULL) / dt_total);
    }
    if (out_total_cpu_permille) {
        *out_total_cpu_permille = active_permille;
    }

    size_t count = 0;
    uint32_t denom = (total_runtime > 0) ? total_runtime : 1U;
    for (UBaseType_t i = 0; i < got && count < max_tasks; ++i) {
        auto& dst = out_tasks[count++];
        dst.pid = static_cast<uint32_t>(sys_tasks[i].xTaskNumber);
        strncpy(dst.name, sys_tasks[i].pcTaskName, sizeof(dst.name) - 1);
        dst.name[sizeof(dst.name) - 1] = '\0';
        switch (sys_tasks[i].eCurrentState) {
            case eRunning:
            case eReady:     dst.state_ch = 'R'; break;
            case eBlocked:   dst.state_ch = 'S'; break;
            case eSuspended: dst.state_ch = 'T'; break;
            case eDeleted:   dst.state_ch = 'Z'; break;
            default:         dst.state_ch = 'S'; break;
        }
        dst.prio = static_cast<int>(sys_tasks[i].uxCurrentPriority);
        dst.stack_free_bytes =
            static_cast<uint32_t>(sys_tasks[i].usStackHighWaterMark) * sizeof(StackType_t);
        dst.cpu_permille =
            static_cast<uint32_t>((static_cast<uint64_t>(sys_tasks[i].ulRunTimeCounter) * 1000ULL) / denom);
        dst.runtime_cs = sys_tasks[i].ulRunTimeCounter / 10000U;
    }

    return count;
}

void hwRotateApiToken(char out_token[33]) {
    formatRandomHex32(out_token);
}

uint32_t hwUptimeMs() {
    return millis();
}

uint32_t hwFreeHeap() {
    return ESP.getFreeHeap();
}

const c6os::HardwareOps HW_OPS = {
    hwGpioMode,
    hwGpioWrite,
    hwGpioRead,
    hwGpioPwm,
    hwAdcReadMv,
    hwI2cInit,
    hwI2cScan,
    hwI2cWriteReg,
    hwI2cReadReg,
    hwBleEnable,
    hwBleScan,
    hwBleAdvertise,
    hwIcmpPing,
    hwGetTasks,
    hwRotateApiToken,
    hwUptimeMs,
    hwFreeHeap,
};

// ============================================================================
// Persistent NVS Configuration & Storage Initialization
// ============================================================================

void loadPersistentConfig() {
    Preferences prefs;
    prefs.begin("c6os_cfg", false);

    String host = prefs.getString("hostname", "c6-linux");
    String shell_path = prefs.getString("login_shell", "/bin/c6-menu");
    String ssid = prefs.getString("wifi_ssid", "");
    String wpass = prefs.getString("wifi_pass", "");
    bool ap_pinned = prefs.getBool("ap_pinned", false);

    strncpy(g_state.hostname, host.c_str(), sizeof(g_state.hostname) - 1);
    g_state.hostname[sizeof(g_state.hostname) - 1] = '\0';

    strncpy(g_state.login_shell, shell_path.c_str(), sizeof(g_state.login_shell) - 1);
    g_state.login_shell[sizeof(g_state.login_shell) - 1] = '\0';

    strncpy(g_state.wifi_cfg_ssid, ssid.c_str(), sizeof(g_state.wifi_cfg_ssid) - 1);
    g_state.wifi_cfg_ssid[sizeof(g_state.wifi_cfg_ssid) - 1] = '\0';

    strncpy(g_state.wifi_cfg_pass, wpass.c_str(), sizeof(g_state.wifi_cfg_pass) - 1);
    g_state.wifi_cfg_pass[sizeof(g_state.wifi_cfg_pass) - 1] = '\0';

    g_state.wifi_ap_pinned = ap_pinned;
    if (ap_pinned) {
        g_state.wifi_ap_enabled = true;
    }

    // Salted SHA-256 root password (migrating any legacy plaintext NVS entry once)
    if (prefs.getBytesLength("pass_salt") == 16 && prefs.getBytesLength("pass_hash") == 32) {
        prefs.getBytes("pass_salt", g_state.pass_salt, 16);
        prefs.getBytes("pass_hash", g_state.pass_hash, 32);
        g_state.pass_configured = true;
    } else {
        String init_pass = prefs.getString("root_pass", "esp32c6");
        esp_fill_random(g_state.pass_salt, sizeof(g_state.pass_salt));
        c6os::crypto::hashPassword(init_pass.c_str(), g_state.pass_salt, g_state.pass_hash);
        g_state.pass_configured = true;
        prefs.putBytes("pass_salt", g_state.pass_salt, 16);
        prefs.putBytes("pass_hash", g_state.pass_hash, 32);
        if (prefs.isKey("root_pass")) {
            prefs.remove("root_pass");
        }
    }

    // Random 32-hex-char HTTP bearer token
    String tok = prefs.getString("api_token", "");
    if (tok.length() == 32) {
        memcpy(g_state.api_token, tok.c_str(), 33);
    } else {
        formatRandomHex32(g_state.api_token);
        prefs.putString("api_token", g_state.api_token);
    }

    prefs.end();
}

void savePersistentConfig() {
    Preferences prefs;
    prefs.begin("c6os_cfg", false);
    prefs.putString("hostname", g_state.hostname);
    prefs.putString("login_shell", g_state.login_shell);
    prefs.putBytes("pass_salt", g_state.pass_salt, 16);
    prefs.putBytes("pass_hash", g_state.pass_hash, 32);
    prefs.putString("api_token", g_state.api_token);
    prefs.putString("wifi_ssid", g_state.wifi_cfg_ssid);
    prefs.putString("wifi_pass", g_state.wifi_cfg_pass);
    prefs.putBool("ap_pinned", g_state.wifi_ap_pinned);
    prefs.end();
}

void initStorage() {
    if (LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
        g_littlefs_mounted = true;
        g_state.rootfs_total_kb = static_cast<uint32_t>(LittleFS.totalBytes() / 1024ULL);
        g_state.rootfs_used_kb  = static_cast<uint32_t>(LittleFS.usedBytes() / 1024ULL);
        g_state.appendDmesg(millis(), "VFS: Mounted LittleFS rootfs at /littlefs");
    } else {
        g_state.appendDmesg(millis(), "VFS: LittleFS mount failed; rootfs unavailable");
    }

    // Attach MicroSD slot (GPIO 4 CS, 4 MHz) onto shared SPI2_HOST bus initialized by St7789Lcd
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = static_cast<int>(board::SD_SPI_HZ / 1000UL);

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SPI2_HOST;
    slot_cfg.gpio_cs = static_cast<gpio_num_t>(board::PIN_SD_CS);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    if (esp_vfs_fat_sdspi_mount("/sd", &host, &slot_cfg, &mount_cfg, &g_sd_card) == ESP_OK &&
        g_sd_card != nullptr) {
        g_state.sd_mounted = true;
        uint64_t cap_bytes = static_cast<uint64_t>(g_sd_card->csd.capacity) *
                             static_cast<uint64_t>(g_sd_card->csd.sector_size);
        g_state.sd_total_mb = static_cast<uint32_t>(cap_bytes / (1024ULL * 1024ULL));
        char msg[64];
        snprintf(msg, sizeof(msg), "mmcblk0: MicroSD mounted on /sd (%lu MB)",
                 static_cast<unsigned long>(g_state.sd_total_mb));
        g_state.appendDmesg(millis(), msg);
    }

    g_vfs.initDefaultFiles(g_state);
    if (g_littlefs_mounted) {
        g_state.rootfs_total_kb = static_cast<uint32_t>(LittleFS.totalBytes() / 1024ULL);
        g_state.rootfs_used_kb  = static_cast<uint32_t>(LittleFS.usedBytes() / 1024ULL);
        g_vfs.consumeUsageDirty();
    }
    setenv("TZ", g_state.tz_posix, 1);
    tzset();
}

// ============================================================================
// Event-Driven Wi-Fi, WPA2 SoftAP Policy, and Non-Blocking Scan
// ============================================================================

void onWifiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            g_wifi_got_ip_evt = true;
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            g_wifi_lost_evt = true;
            break;
        default:
            break;
    }
}

void applyWifiConfig() {
    char ssid[33] = {0};
    char pass[65] = {0};
    bool ap_en = true;
    char host[32] = {0};

    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(ssid, g_state.wifi_cfg_ssid, sizeof(ssid) - 1);
        strncpy(pass, g_state.wifi_cfg_pass, sizeof(pass) - 1);
        ap_en = g_state.wifi_ap_enabled;
        strncpy(host, g_state.hostname, sizeof(host) - 1);
        xSemaphoreGive(g_mutex);
    }

    WiFi.setHostname(host);
    if (ap_en) {
        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(SOFTAP_SSID, SOFTAP_PASS);
        String ap_ip = WiFi.softAPIP().toString();
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            strncpy(g_state.wifi_ap_ssid, SOFTAP_SSID, sizeof(g_state.wifi_ap_ssid) - 1);
            strncpy(g_state.wifi_ap_ip, ap_ip.c_str(), sizeof(g_state.wifi_ap_ip) - 1);
            xSemaphoreGive(g_mutex);
        }
    } else {
        WiFi.enableAP(false);
        WiFi.mode(WIFI_STA);
    }

    String mac = WiFi.macAddress();
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(g_state.wifi_mac, mac.c_str(), sizeof(g_state.wifi_mac) - 1);
        xSemaphoreGive(g_mutex);
    }

    if (ssid[0] != '\0') {
        WiFi.disconnect(false);
        vTaskDelay(pdMS_TO_TICKS(40));
        if (pass[0] != '\0') {
            WiFi.begin(ssid, pass);
        } else {
            WiFi.begin(ssid);
        }
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            char msg[72];
            snprintf(msg, sizeof(msg), "wlan0: Associating with '%s'...", ssid);
            g_state.appendDmesg(millis(), msg);
            g_state.markDirty(c6os::DIRTY_HEADER | c6os::DIRTY_CARDS);
            xSemaphoreGive(g_mutex);
        }
    }
}

void harvestAsyncWifiScan(int16_t found_count) {
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        g_state.wifi_scan_count = 0;
        for (int16_t i = 0; i < found_count && g_state.wifi_scan_count < c6os::MAX_WIFI_SCAN_RESULTS; ++i) {
            String s = WiFi.SSID(i);
            if (s.length() == 0) continue;
            auto& entry = g_state.wifi_scan[g_state.wifi_scan_count++];
            strncpy(entry.ssid, s.c_str(), sizeof(entry.ssid) - 1);
            entry.ssid[sizeof(entry.ssid) - 1] = '\0';
            int32_t r = WiFi.RSSI(i);
            if (r < -127) r = -127;
            if (r > 0) r = 0;
            entry.rssi = static_cast<int8_t>(r);
            entry.channel = static_cast<uint8_t>(WiFi.channel(i));
            entry.open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "wlan0: Scan completed (%u networks found)",
                 static_cast<unsigned>(g_state.wifi_scan_count));
        g_state.appendDmesg(millis(), msg);
        xSemaphoreGive(g_mutex);
    }
    WiFi.scanDelete();
}

// ============================================================================
// Event-Driven lwIP HTTP Port 80 REST API & UDP 514 Syslog Listener
// ============================================================================

void initNetworkListeners() {
    if (g_httpd_fd < 0) {
        int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd >= 0) {
            int reuse = 1;
            lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(80);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (lwip_bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0 &&
                lwip_listen(fd, 2) == 0) {
                int flags = lwip_fcntl(fd, F_GETFL, 0);
                lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                g_httpd_fd = fd;
            } else {
                lwip_close(fd);
            }
        }
    }

    if (g_syslog_fd < 0) {
        int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd >= 0) {
            int reuse = 1;
            lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            struct sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(514);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            if (lwip_bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                int flags = lwip_fcntl(fd, F_GETFL, 0);
                lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                g_syslog_fd = fd;
            } else {
                lwip_close(fd);
            }
        }
    }
}

void waitNetworkSocketsOrTimeout(uint32_t timeout_ms) {
    if (g_httpd_fd < 0 && g_syslog_fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    int max_fd = -1;
    if (g_httpd_fd >= 0) {
        FD_SET(g_httpd_fd, &rfds);
        if (g_httpd_fd > max_fd) max_fd = g_httpd_fd;
    }
    if (g_syslog_fd >= 0) {
        FD_SET(g_syslog_fd, &rfds);
        if (g_syslog_fd > max_fd) max_fd = g_syslog_fd;
    }
    struct timeval tv = {};
    tv.tv_sec = static_cast<long>(timeout_ms / 1000U);
    tv.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
    lwip_select(max_fd + 1, &rfds, nullptr, nullptr, &tv);
}

void sendHttpStr(int cfd, const char* text) {
    if (!text) return;
    size_t len = strlen(text);
    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < len && (millis() - t0 < 500U)) {
        int n = lwip_send(cfd, text + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(cfd, &wfds);
            struct timeval tv = {0, 50000};
            lwip_select(cfd + 1, nullptr, &wfds, nullptr, &tv);
            continue;
        }
        break;
    }
}

void urlDecodeInPlace(char* str) {
    char* src = str;
    char* dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            ++src;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = static_cast<char>(strtol(hex, nullptr, 16));
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void parseQueryParams(const char* query, char* out_cmd, size_t cmd_cap,
                      char* out_tok, size_t tok_cap) {
    if (out_cmd && cmd_cap > 0) out_cmd[0] = '\0';
    if (out_tok && tok_cap > 0) out_tok[0] = '\0';
    if (!query) return;

    const char* p = query;
    while (*p) {
        const char* amp = strchr(p, '&');
        size_t seg_len = amp ? static_cast<size_t>(amp - p) : strlen(p);
        if ((seg_len > 2 && (strncmp(p, "q=", 2) == 0 || strncmp(p, "c=", 2) == 0)) &&
            out_cmd && cmd_cap > 0) {
            size_t vlen = seg_len - 2U;
            if (vlen >= cmd_cap) vlen = cmd_cap - 1U;
            memcpy(out_cmd, p + 2, vlen);
            out_cmd[vlen] = '\0';
            urlDecodeInPlace(out_cmd);
        } else if (seg_len > 6 && strncmp(p, "token=", 6) == 0 && out_tok && tok_cap > 0) {
            size_t vlen = seg_len - 6U;
            if (vlen >= tok_cap) vlen = tok_cap - 1U;
            memcpy(out_tok, p + 6, vlen);
            out_tok[vlen] = '\0';
            urlDecodeInPlace(out_tok);
        }
        if (!amp) break;
        p = amp + 1;
    }
}

void pollHttpAndSyslog() {
    // 1. Drain UDP 514 Syslog socket with zero heap allocations
    if (g_syslog_fd >= 0) {
        for (int burst = 0; burst < 4; ++burst) {
            char sbuf[96] = {0};
            int len = lwip_recvfrom(g_syslog_fd, sbuf, sizeof(sbuf) - 1U, MSG_DONTWAIT, nullptr, nullptr);
            if (len <= 0) break;
            sbuf[len] = '\0';
            for (int i = 0; i < len; ++i) {
                if (sbuf[i] == '\r' || sbuf[i] == '\n') sbuf[i] = ' ';
            }
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.syslog_packets++;
                g_state.net_rx_bytes += static_cast<uint32_t>(len);
                char msg[112];
                snprintf(msg, sizeof(msg), "syslogd: %.80s", sbuf);
                g_state.appendDmesg(millis(), msg);
                xSemaphoreGive(g_mutex);
            }
        }
    }

    // 2. Poll TCP 80 HTTP REST API socket
    if (g_httpd_fd < 0) return;
    struct sockaddr_in peer = {};
    socklen_t peer_len = sizeof(peer);
    int cfd = lwip_accept(g_httpd_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
    if (cfd < 0) return;

    int nodelay = 1;
    lwip_setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    char req_buf[640] = {0};
    size_t used = 0;
    uint32_t t0 = millis();
    while (used + 1U < sizeof(req_buf) && (millis() - t0 < 250U)) {
        int n = lwip_recv(cfd, req_buf + used, sizeof(req_buf) - 1U - used, MSG_DONTWAIT);
        if (n > 0) {
            used += static_cast<size_t>(n);
            req_buf[used] = '\0';
            if (strstr(req_buf, "\r\n\r\n") != nullptr || strstr(req_buf, "\n\n") != nullptr) {
                break;
            }
            continue;
        }
        if (n == 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        struct timeval tv = {0, 50000};
        lwip_select(cfd + 1, &rfds, nullptr, nullptr, &tv);
    }

    if (used == 0) {
        lwip_close(cfd);
        return;
    }

    char* line_end = strpbrk(req_buf, "\r\n");
    const char* hdr_cursor = nullptr;
    if (line_end) {
        hdr_cursor = line_end + 1;
        if (*line_end == '\r' && *hdr_cursor == '\n') {
            ++hdr_cursor;
        }
        *line_end = '\0';
    }

    char hdr_token[48] = {0};
    while (hdr_cursor && *hdr_cursor) {
        const char* next_eol = strpbrk(hdr_cursor, "\r\n");
        size_t hlen = next_eol ? static_cast<size_t>(next_eol - hdr_cursor) : strlen(hdr_cursor);
        if (hlen == 0) break;
        if (hlen > 14U && strncasecmp(hdr_cursor, "Authorization:", 14) == 0) {
            const char* val = hdr_cursor + 14;
            const char* vend = hdr_cursor + hlen;
            while (val < vend && (*val == ' ' || *val == '\t')) ++val;
            if (static_cast<size_t>(vend - val) > 7U && strncasecmp(val, "Bearer ", 7) == 0) {
                val += 7;
                while (val < vend && *val == ' ') ++val;
                while (vend > val && (vend[-1] == ' ' || vend[-1] == '\t')) --vend;
                size_t tlen = static_cast<size_t>(vend - val);
                if (tlen >= sizeof(hdr_token)) tlen = sizeof(hdr_token) - 1U;
                memcpy(hdr_token, val, tlen);
                hdr_token[tlen] = '\0';
            }
        }
        if (!next_eol) break;
        hdr_cursor = next_eol + 1;
        if (*next_eol == '\r' && *hdr_cursor == '\n') {
            ++hdr_cursor;
        }
    }

    if (strncmp(req_buf, "GET /api/status", 15) == 0 &&
        (req_buf[15] == ' ' || req_buf[15] == '?' || req_buf[15] == '\0')) {
        c6os::HeapArray<char> json(1024);
        if (json) {
            json[0] = '\0';
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_state.http_requests++;
                c6os::ShellSession::formatStatusJson(g_state, json.get(), json.bytes());
                xSemaphoreGive(g_mutex);
            }
            sendHttpStr(cfd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
            sendHttpStr(cfd, json.get());
        }
    } else if (strncmp(req_buf, "GET /api/cmd", 12) == 0 &&
               (req_buf[12] == ' ' || req_buf[12] == '?' || req_buf[12] == '/' || req_buf[12] == '\0')) {
        char* uri = req_buf + 4;
        char* sp2 = strchr(uri, ' ');
        if (sp2) *sp2 = '\0';
        const char* qmark = strchr(uri, '?');

        char cmd_buf[192] = {0};
        char query_token[48] = {0};
        if (qmark) {
            parseQueryParams(qmark + 1, cmd_buf, sizeof(cmd_buf),
                             query_token, sizeof(query_token));
        }

        const char* candidate_tok = hdr_token[0] ? hdr_token : query_token;
        char expected_tok[33] = {0};
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.http_requests++;
            memcpy(expected_tok, g_state.api_token, sizeof(expected_tok));
            xSemaphoreGive(g_mutex);
        }

        bool authorized = (strlen(candidate_tok) == 32 &&
                           expected_tok[0] != '\0' &&
                           c6os::crypto::constantTimeEqual(candidate_tok, expected_tok, 32));

        if (!authorized) {
            sendHttpStr(cfd,
                        "HTTP/1.1 401 Unauthorized\r\n"
                        "WWW-Authenticate: Bearer realm=\"c6os\"\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"unauthorized\",\"hint\":\"run 'apitoken' over SSH\"}\n");
            lwip_close(cfd);
            return;
        }

        if (cmd_buf[0] == '\0') {
            sendHttpStr(cfd,
                        "HTTP/1.1 400 Bad Request\r\n"
                        "Content-Type: application/json\r\n"
                        "Connection: close\r\n\r\n"
                        "{\"error\":\"missing_command_param_c_or_q\"}\n");
            lwip_close(cfd);
            return;
        }

        c6os::HeapArray<char> out(2048);
        if (out) {
            out[0] = '\0';
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
                g_bg_shell.executeCommandLine(g_state, g_vfs, cmd_buf, out.get(), out.bytes());
                xSemaphoreGive(g_mutex);
            }
            sendHttpStr(cfd,
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n");
            sendHttpStr(cfd, out.get());
        }
    } else {
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.http_requests++;
            xSemaphoreGive(g_mutex);
        }
        sendHttpStr(cfd,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                    "C6-MicroLinux HTTP REST API (port 80)\n"
                    "Endpoints:\n"
                    "  GET /api/status  (read-only telemetry JSON)\n"
                    "  GET /api/cmd?c=<url-encoded-command>  (requires Authorization: Bearer <token>)\n");
    }
    lwip_close(cfd);
}

void networkTask(void* param) {
    (void)param;
    bool scan_in_progress = false;
    uint32_t lost_since_ms = 0;
    uint32_t last_rssi_poll_ms = 0;
    int8_t active_wifi_ps = -1;  // -1 = unknown, 0 = WIFI_PS_NONE, 1 = WIFI_PS_MIN_MODEM

    while (true) {
        bool do_reconnect = false;
        bool do_scan = false;
        bool do_sync = false;
        bool do_reboot = false;
        bool do_ntp = false;
        bool ap_enabled = false;
        uint8_t ssh_sessions = 0;
        char tz_posix[48] = {0};

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (g_state.wifi_reconnect_requested) {
                do_reconnect = true;
                g_state.wifi_reconnect_requested = false;
            }
            if (g_state.wifi_scan_requested) {
                do_scan = true;
                g_state.wifi_scan_requested = false;
            }
            if (g_state.fs_sync_requested) {
                do_sync = true;
                g_state.fs_sync_requested = false;
            }
            if (g_state.reboot_requested) {
                do_reboot = true;
                g_state.reboot_requested = false;
            }
            if (g_state.ntp_sync_requested) {
                do_ntp = true;
                g_state.ntp_sync_requested = false;
            }
            ap_enabled = g_state.wifi_ap_enabled;
            ssh_sessions = g_state.ssh_active_sessions;
            if (do_ntp || g_wifi_got_ip_evt) {
                strncpy(tz_posix, g_state.tz_posix, sizeof(tz_posix) - 1);
            }
            xSemaphoreGive(g_mutex);
        }

        if (do_sync) {
            savePersistentConfig();
        }
        if (do_reboot) {
            vTaskDelay(pdMS_TO_TICKS(400));
            ESP.restart();
        }
        if (do_reconnect) {
            active_wifi_ps = -1;
            applyWifiConfig();
        }
        if (do_scan && !scan_in_progress) {
            int16_t rc = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
            if (rc == WIFI_SCAN_RUNNING) {
                scan_in_progress = true;
            } else if (rc >= 0) {
                harvestAsyncWifiScan(rc);
            }
        }
        if (scan_in_progress) {
            int16_t status = WiFi.scanComplete();
            if (status >= 0) {
                harvestAsyncWifiScan(status);
                scan_in_progress = false;
            } else if (status == WIFI_SCAN_FAILED) {
                WiFi.scanDelete();
                scan_in_progress = false;
            }
        }

        if (g_wifi_got_ip_evt) {
            g_wifi_got_ip_evt = false;
            lost_since_ms = 0;
            active_wifi_ps = -1;

            String ip = WiFi.localIP().toString();
            String gw = WiFi.gatewayIP().toString();
            String ssid = WiFi.SSID();
            int32_t rssi = WiFi.RSSI();
            bool disable_ap_now = false;

            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_state.wifi_sta_connected = true;
                strncpy(g_state.wifi_ip, ip.c_str(), sizeof(g_state.wifi_ip) - 1);
                strncpy(g_state.wifi_gateway, gw.c_str(), sizeof(g_state.wifi_gateway) - 1);
                strncpy(g_state.wifi_ssid, ssid.c_str(), sizeof(g_state.wifi_ssid) - 1);
                g_state.wifi_rssi = rssi;
                if (tz_posix[0] == '\0') {
                    strncpy(tz_posix, g_state.tz_posix, sizeof(tz_posix) - 1);
                }

                char msg[96];
                snprintf(msg, sizeof(msg), "wlan0: Bound to %s (SSID: %s)",
                         g_state.wifi_ip, g_state.wifi_ssid);
                g_state.appendDmesg(millis(), msg);

                if (g_state.wifi_ap_enabled && !g_state.wifi_ap_pinned) {
                    g_state.wifi_ap_enabled = false;
                    ap_enabled = false;
                    disable_ap_now = true;
                    g_state.appendDmesg(millis(), "wlan0: Auto-disabled SoftAP on LAN join");
                }
                g_state.markDirty(c6os::DIRTY_HEADER | c6os::DIRTY_CARDS);
                xSemaphoreGive(g_mutex);
            }

            if (disable_ap_now) {
                WiFi.enableAP(false);
            }
            configTzTime(tz_posix[0] ? tz_posix : "EST5EDT,M3.2.0,M11.1.0",
                         "pool.ntp.org", "time.nist.gov");
        }

        if (g_wifi_lost_evt) {
            g_wifi_lost_evt = false;
            if (WiFi.status() != WL_CONNECTED) {
                lost_since_ms = millis();
                active_wifi_ps = -1;
                if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (g_state.wifi_sta_connected) {
                        g_state.wifi_sta_connected = false;
                        strncpy(g_state.wifi_ip, "0.0.0.0", sizeof(g_state.wifi_ip) - 1);
                        g_state.wifi_rssi = -100;
                        g_state.appendDmesg(millis(), "wlan0: Link lost");
                        g_state.markDirty(c6os::DIRTY_HEADER | c6os::DIRTY_CARDS);
                    }
                    xSemaphoreGive(g_mutex);
                }
            }
        }

        // Re-enable WPA2 fallback SoftAP if LAN connection remains down for > 8 seconds
        if (lost_since_ms != 0 && WiFi.status() != WL_CONNECTED &&
            (millis() - lost_since_ms > 8000U)) {
            lost_since_ms = 0;
            bool need_ap = false;
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                if (!g_state.wifi_ap_enabled) {
                    g_state.wifi_ap_enabled = true;
                    ap_enabled = true;
                    need_ap = true;
                }
                xSemaphoreGive(g_mutex);
            }
            if (need_ap) {
                active_wifi_ps = -1;
                applyWifiConfig();
            }
        }

        if (do_ntp && WiFi.status() == WL_CONNECTED) {
            configTzTime(tz_posix[0] ? tz_posix : "EST5EDT,M3.2.0,M11.1.0",
                         "pool.ntp.org", "time.nist.gov");
        }

        // Dynamic Wi-Fi modem sleep: disable sleep for zero keystroke latency while
        // SSH/SFTP or SoftAP is active; enable MIN_MODEM sleep when idle on STA.
        if (WiFi.status() == WL_CONNECTED) {
            bool want_modem_sleep = (!ap_enabled && ssh_sessions == 0 && !scan_in_progress);
            int8_t target_ps = want_modem_sleep ? 1 : 0;
            if (target_ps != active_wifi_ps) {
                WiFi.setSleep(want_modem_sleep ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
                active_wifi_ps = target_ps;
            }
        }

        // Refresh RSSI & NTP epoch once every 2 seconds while connected
        uint32_t now_ms = millis();
        if (WiFi.status() == WL_CONNECTED && (now_ms - last_rssi_poll_ms >= 2000U)) {
            last_rssi_poll_ms = now_ms;
            int32_t rssi = WiFi.RSSI();
            time_t now_epoch = time(nullptr);
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.wifi_rssi = rssi;
                if (now_epoch > 1700000000) {
                    g_state.epoch_offset_sec = static_cast<uint32_t>(now_epoch) - (now_ms / 1000U);
                    g_state.ntp_synced = true;
                }
                xSemaphoreGive(g_mutex);
            }
        }

        pollHttpAndSyslog();
        waitNetworkSocketsOrTimeout(scan_in_progress ? 80U : 250U);
    }
}

void sshdTask(void* param) {
    (void)param;
    while (true) {
        // pollAcceptAndServe blocks up to 1000 ms in lwip_select() when idle
        g_sshd.pollAcceptAndServe(g_state, g_vfs, g_mutex);
    }
}

void serialConsoleTask(void* param) {
    (void)param;
    c6os::ShellSession serial_shell;

    vTaskDelay(pdMS_TO_TICKS(500));
    {
        c6os::HeapArray<char> boot_buf(4096);
        if (boot_buf) {
            boot_buf[0] = '\0';
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                serial_shell.begin(g_state, g_vfs, /*start_in_menu=*/false,
                                   boot_buf.get(), boot_buf.bytes());
                xSemaphoreGive(g_mutex);
            }
            if (boot_buf[0] != '\0') {
                Serial.print(boot_buf.get());
            }
        }
    }

    uint32_t last_mon_tick = millis();
    uint32_t last_rx_ms = 0;

    while (true) {
        int avail = Serial.available();
        if (avail > 0) {
            char rx[64];
            size_t to_read = (static_cast<size_t>(avail) < sizeof(rx))
                                 ? static_cast<size_t>(avail)
                                 : sizeof(rx);
            size_t got = Serial.readBytes(rx, to_read);
            if (got > 0) {
                uint32_t now = millis();
                last_mon_tick = now;
                last_rx_ms = now;
                c6os::HeapArray<char> out_buf(4096);
                if (out_buf) {
                    out_buf[0] = '\0';
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        bool keep = serial_shell.consumeBytes(g_state, g_vfs, rx, got,
                                                              out_buf.get(), out_buf.bytes());
                        if (!keep) {
                            serial_shell.begin(g_state, g_vfs, /*start_in_menu=*/false,
                                               out_buf.get(), out_buf.bytes());
                        }
                        xSemaphoreGive(g_mutex);
                    }
                    if (out_buf[0] != '\0') {
                        Serial.print(out_buf.get());
                    }
                }
            }
        } else if (serial_shell.isInteractiveMonitor()) {
            uint32_t now = millis();
            if (now - last_mon_tick >= serial_shell.monitorIntervalMs()) {
                last_mon_tick = now;
                c6os::HeapArray<char> out_buf(4096);
                if (out_buf) {
                    out_buf[0] = '\0';
                    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        serial_shell.renderMonitorTick(g_state, g_vfs,
                                                       out_buf.get(), out_buf.bytes());
                        xSemaphoreGive(g_mutex);
                    }
                    if (out_buf[0] != '\0') {
                        Serial.print(out_buf.get());
                    }
                }
            }
        }

        uint32_t sleep_ms = (serial_shell.isInteractiveMonitor() ||
                             (last_rx_ms != 0 && (millis() - last_rx_ms < 5000U)))
                                ? 15U
                                : 50U;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

void updateRgbLed(const c6os::RenderSnapshot& snap) {
    static uint32_t s_last_rgb = 0xFFFFFFFFU;
    ++g_led_phase;

    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    switch (snap.led_mode) {
        case c6os::LedMode::OFF:
            break;
        case c6os::LedMode::SOLID:
            r = snap.led_r;
            g = snap.led_g;
            b = snap.led_b;
            break;
        case c6os::LedMode::PULSE: {
            uint8_t level = (g_led_phase < 128) ? (g_led_phase * 2) : ((255 - g_led_phase) * 2);
            r = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_r) * level) / 255U);
            g = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_g) * level) / 255U);
            b = static_cast<uint8_t>((static_cast<uint16_t>(snap.led_b) * level) / 255U);
            break;
        }
        case c6os::LedMode::RAINBOW: {
            uint8_t p = g_led_phase;
            if (p < 85) {
                r = static_cast<uint8_t>(255 - p * 3);
                g = static_cast<uint8_t>(p * 3);
            } else if (p < 170) {
                p -= 85;
                g = static_cast<uint8_t>(255 - p * 3);
                b = static_cast<uint8_t>(p * 3);
            } else {
                p -= 170;
                b = static_cast<uint8_t>(255 - p * 3);
                r = static_cast<uint8_t>(p * 3);
            }
            r /= 4U;
            g /= 4U;
            b /= 4U;
            break;
        }
        case c6os::LedMode::SSH_ACTIVITY:
        default: {
            if (snap.ssh_active_sessions > 0) {
                g = 64;
                b = 16;
            } else {
                uint8_t level = (g_led_phase < 128) ? (g_led_phase / 4) : ((255 - g_led_phase) / 4);
                g = level / 2;
                b = level;
            }
            break;
        }
    }

    uint32_t packed = (static_cast<uint32_t>(r) << 16) |
                      (static_cast<uint32_t>(g) << 8) |
                      static_cast<uint32_t>(b);
    if (packed != s_last_rgb) {
        s_last_rgb = packed;
        rgbLedWrite(board::PIN_RGB_LED, r, g, b);
    }
}

void pollBootButton() {
    if (!g_btn_irq && !g_btn_pressed) {
        return;
    }
    g_btn_irq = false;

    bool is_down = (digitalRead(board::PIN_BTN_BOOT) == LOW);
    uint32_t now = millis();

    if (is_down && !g_btn_pressed) {
        g_btn_pressed = true;
        g_btn_press_start_ms = now;
    } else if (!is_down && g_btn_pressed) {
        uint32_t held_ms = now - g_btn_press_start_ms;
        g_btn_pressed = false;
        if (held_ms >= BTN_DEBOUNCE_MS) {
            if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                g_state.button_press_count++;
                if (held_ms >= BTN_LONG_PRESS_MS) {
                    if (g_state.backlight_pct >= 90) g_state.backlight_pct = 50;
                    else if (g_state.backlight_pct >= 40) g_state.backlight_pct = 15;
                    else g_state.backlight_pct = 100;
                    char msg[64];
                    snprintf(msg, sizeof(msg), "buttond: GPIO9 long press -> BL=%u%%",
                             static_cast<unsigned>(g_state.backlight_pct));
                    g_state.appendDmesg(now, msg);
                    g_state.markDirty(c6os::DIRTY_HEADER);
                } else {
                    if (g_state.alert_active) {
                        g_state.alert_active = false;
                        g_state.appendDmesg(now, "buttond: GPIO9 short press -> cleared alert");
                        g_state.markDirty(c6os::DIRTY_FULL);
                    } else {
                        uint8_t next = (static_cast<uint8_t>(g_state.screen_mode) + 1) %
                                       static_cast<uint8_t>(c6os::ScreenMode::COUNT);
                        g_state.screen_mode = static_cast<c6os::ScreenMode>(next);
                        char msg[64];
                        snprintf(msg, sizeof(msg), "buttond: GPIO9 short press -> mode=%s",
                                 c6os::screenModeName(g_state.screen_mode));
                        g_state.appendDmesg(now, msg);
                        g_state.markDirty(c6os::DIRTY_FULL);
                    }
                }
                xSemaphoreGive(g_mutex);
            }
        }
    }
}

void refreshUserGpioInputMask() {
    static const int USER_PINS[] = {0, 1, 2, 3, 9, 10, 11, 18, 19, 20, 23};
    uint32_t in_mask = 0;
    for (size_t i = 0; i < sizeof(USER_PINS) / sizeof(USER_PINS[0]); ++i) {
        int p = USER_PINS[i];
        if (digitalRead(p) == HIGH) {
            in_mask |= (1UL << p);
        }
    }
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        g_state.gpio_in_mask = in_mask;
        xSemaphoreGive(g_mutex);
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    pinMode(board::PIN_SD_CS, OUTPUT);
    digitalWrite(board::PIN_SD_CS, HIGH);
    pinMode(board::PIN_BTN_BOOT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(board::PIN_BTN_BOOT), onBootButtonIsr, CHANGE);
    rgbLedWrite(board::PIN_RGB_LED, 0, 16, 32);

    g_mutex = xSemaphoreCreateMutex();
    c6os::setHardwareOps(&HW_OPS);
    g_state.initDefaults();
    g_state.total_sram_bytes = ESP.getHeapSize();
    g_state.free_sram_bytes  = ESP.getFreeHeap();
    g_state.min_free_sram_bytes = ESP.getMinFreeHeap();
    g_state.flash_size_mb = static_cast<uint32_t>(ESP.getFlashChipSize() / (1024ULL * 1024ULL));
    g_state.cpu_temp_deci_c = static_cast<int16_t>(temperatureRead() * 10.0f);

    loadPersistentConfig();

    // Initialize shared SPI2_HOST bus and ST7789 80 MHz DMA panel first
    g_screen.begin();
    g_state.screen_mode = c6os::ScreenMode::FB_CONSOLE;
    g_state.appendDmesg(millis(), "Linux version 6.12.0-c6os-riscv32 (gcc 13.2.0)");
    g_state.appendDmesg(millis(), "Machine: Waveshare ESP32-C6-LCD-1.47 (RV32IMAC)");
    g_state.appendDmesg(millis(), "fb0: ST7789 320x172 RGB565 @ 80MHz SPI2 DMA ready");
    g_screen.renderState(g_state, true);

    initStorage();
    g_screen.renderState(g_state, false);

    WiFi.onEvent(onWifiEvent);
    applyWifiConfig();
    WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
    g_screen.renderState(g_state, false);

    g_sshd.begin(g_state, g_mutex);
    initNetworkListeners();
    g_state.appendDmesg(millis(), "httpd: Bearer-gated REST API on 0.0.0.0:80");
    g_state.appendDmesg(millis(), "syslogd: UDP listener ready on 0.0.0.0:514");
    g_state.appendDmesg(millis(), "init: Executing /etc/rc.local & starting services");
    {
        c6os::HeapArray<char> rc_out(512);
        if (rc_out) {
            g_bg_shell.executeCommandLine(g_state, g_vfs, "sh /etc/rc.local", rc_out.get(), rc_out.bytes());
        }
    }
    g_screen.renderState(g_state, false);

    delay(500);
    g_state.screen_mode = c6os::ScreenMode::OS_DASHBOARD;
    g_state.markDirty(c6os::DIRTY_FULL);
    g_screen.renderState(g_state, true);
    g_state.ui_dirty = false;
    g_state.dirty_regions = c6os::DIRTY_NONE;

    xTaskCreate(networkTask, "net_mgr", 5120, nullptr, 1, nullptr);
    xTaskCreate(sshdTask, "sshd", 3072, nullptr, 2, nullptr);
    xTaskCreate(serialConsoleTask, "ttyACM0", 6144, nullptr, 1, nullptr);
}

void loop() {
    static uint32_t last_sec_tick = 0;
    static uint32_t last_led_tick = 0;
    static uint32_t last_ticker_tick = 0;

    c6os::RenderSnapshot& render_snap = g_screen.snapshot();

    pollBootButton();

    uint32_t now = millis();
    if (now - last_ticker_tick >= 220U) {
        last_ticker_tick = now;
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (g_state.ticker_enabled) {
                g_state.ticker_offset++;
                g_state.markDirty(c6os::DIRTY_TICKER);
            }
            xSemaphoreGive(g_mutex);
        }
    }

    if (now - last_sec_tick >= 1000U) {
        last_sec_tick = now;
        refreshUserGpioInputMask();
        int16_t raw_deci_c = static_cast<int16_t>(temperatureRead() * 10.0f);
        uint32_t active_permille = 0;
        hwGetTasks(nullptr, 0, &active_permille);

        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            g_state.uptime_sec = now / 1000U;
            g_state.free_sram_bytes = ESP.getFreeHeap();
            g_state.min_free_sram_bytes = ESP.getMinFreeHeap();
            if (g_littlefs_mounted && g_vfs.consumeUsageDirty()) {
                g_state.rootfs_used_kb = static_cast<uint32_t>(LittleFS.usedBytes() / 1024ULL);
            }
            if (raw_deci_c > -200 && raw_deci_c < 1200) {
                g_state.cpu_temp_deci_c = raw_deci_c;
            }

            uint8_t load = static_cast<uint8_t>((active_permille + 5U) / 10U);
            if (load < 1U) load = 1U;
            if (load > 99U) load = 99U;
            g_state.cpu_load_pct = load;
            g_state.cpu_history[g_state.cpu_history_idx % 32] = load;
            g_state.cpu_history_idx = static_cast<uint8_t>((g_state.cpu_history_idx + 1) % 32);

            for (size_t i = 0; i < c6os::MAX_CRON_JOBS; ++i) {
                if (g_state.cron_jobs[i].active && g_state.cron_jobs[i].enabled &&
                    g_state.cron_jobs[i].interval_sec > 0) {
                    if (g_state.uptime_sec - g_state.cron_jobs[i].last_run_sec >=
                        g_state.cron_jobs[i].interval_sec) {
                        g_state.cron_jobs[i].last_run_sec = g_state.uptime_sec;
                        g_state.cron_jobs[i].run_count++;
                        c6os::HeapArray<char> cron_out(512);
                        if (cron_out) {
                            g_bg_shell.executeCommandLine(
                                g_state, g_vfs, g_state.cron_jobs[i].cmd, cron_out.get(), cron_out.bytes());
                        }
                    }
                }
            }

            if (g_state.screen_mode == c6os::ScreenMode::OS_DASHBOARD ||
                g_state.screen_mode == c6os::ScreenMode::MONITOR) {
                g_state.markDirty(c6os::DIRTY_HEADER | c6os::DIRTY_CARDS);
            } else if (g_state.screen_mode == c6os::ScreenMode::DIAGNOSTICS) {
                g_state.markDirty(c6os::DIRTY_CARDS);
            }
            xSemaphoreGive(g_mutex);
        }
    }

    bool should_render = false;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(30)) == pdTRUE) {
        if (g_state.ui_dirty) {
            uint8_t dmask = g_state.dirty_regions ? g_state.dirty_regions : c6os::DIRTY_FULL;
            render_snap.captureFrom(g_state, dmask);
            g_state.ui_dirty = false;
            g_state.dirty_regions = c6os::DIRTY_NONE;
            should_render = true;
        } else {
            render_snap.ssh_active_sessions = g_state.ssh_active_sessions;
            render_snap.led_mode = g_state.led_mode;
            render_snap.led_r = g_state.led_r;
            render_snap.led_g = g_state.led_g;
            render_snap.led_b = g_state.led_b;
        }
        xSemaphoreGive(g_mutex);
    }

    if (should_render) {
        g_screen.render(render_snap, false);
    }

    if (now - last_led_tick >= 30U) {
        last_led_tick = now;
        updateRgbLed(render_snap);
    }

    bool fast_poll = g_btn_pressed ||
                     render_snap.led_mode == c6os::LedMode::PULSE ||
                     render_snap.led_mode == c6os::LedMode::RAINBOW;
    delay(fast_poll ? 20 : 30);
}
