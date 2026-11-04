#ifndef C6OS_SYSLOG_H
#define C6OS_SYSLOG_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {

// Persistent syslogd: tails the in-RAM dmesg ring and appends new lines to a size-capped,
// generation-rotated log file. Writes are coalesced through a 512 B staging buffer so the
// shared SPI2_HOST bus sees at most one small burst every FLUSH_INTERVAL_SEC.
class SyslogRotator {
public:
    static constexpr size_t   STAGE_BYTES        = 512;
    static constexpr uint32_t FLUSH_INTERVAL_SEC = 10;
    static constexpr uint32_t SD_FREE_FLOOR_MB   = 16;    // pause + purge generations below this
    static constexpr uint16_t SD_MIN_CAP_KB      = 4;
    static constexpr uint16_t SD_MAX_CAP_KB      = 2048;
    static constexpr uint8_t  MAX_FILE_COUNT     = 8;
    static constexpr uint16_t FLASH_CAP_KB       = 16;    // LittleFS fallback ceiling per file
    static constexpr uint8_t  FLASH_FILE_COUNT   = 2;     // LittleFS fallback generations

    SyslogRotator();

    // Copies dmesg lines newer than the last service into the staging buffer, then flushes
    // to the active target when the buffer is full, FLUSH_INTERVAL_SEC has elapsed, `force`
    // is set, or the shell requested a flush/rotation. Call with SystemState locked.
    // Returns true if any bytes reached the filesystem during this call.
    bool service(SystemState& state, VirtualFs& vfs, bool force);

    // Active log path: /mnt/sd/var/log/messages when microSD is mounted, else /var/log/messages
    static const char* targetPath(const SystemState& state);

    // Effective per-file cap and generation count for the current target
    static uint32_t effectiveCapBytes(const SystemState& state);
    static uint8_t effectiveFileCount(const SystemState& state);

    size_t stagedBytes() const { return staged_len_; }

private:
    char staging_[STAGE_BYTES];
    size_t staged_len_;
    uint32_t last_seq_;
    uint32_t last_flush_sec_;
    bool first_service_;

    void collect(SystemState& state);
    bool flush(SystemState& state, VirtualFs& vfs, bool force_rotate);
    void rotate(SystemState& state, VirtualFs& vfs, const char* base, uint8_t count);
    void purgeGenerations(SystemState& state, VirtualFs& vfs, const char* base, uint8_t count);
    bool refreshSdUsage(SystemState& state);
};

}  // namespace c6os

#endif  // C6OS_SYSLOG_H
