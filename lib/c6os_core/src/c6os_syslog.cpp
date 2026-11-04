#include "c6os_syslog.h"

#include <stdio.h>
#include <string.h>

namespace c6os {

namespace {

constexpr const char* SD_LOG_PATH    = "/mnt/sd/var/log/messages";
constexpr const char* FLASH_LOG_PATH = "/var/log/messages";

void formatGeneration(const char* base, unsigned gen, char* out, size_t cap) {
    if (gen == 0) {
        snprintf(out, cap, "%s", base);
    } else {
        snprintf(out, cap, "%s.%u", base, gen);
    }
}

}  // namespace

SyslogRotator::SyslogRotator()
    : staged_len_(0), last_seq_(0), last_flush_sec_(0), first_service_(true) {
    memset(staging_, 0, sizeof(staging_));
}

const char* SyslogRotator::targetPath(const SystemState& state) {
    return state.sd_mounted ? SD_LOG_PATH : FLASH_LOG_PATH;
}

uint32_t SyslogRotator::effectiveCapBytes(const SystemState& state) {
    uint32_t cap_kb = state.log_file_cap_kb;
    if (cap_kb < SD_MIN_CAP_KB) cap_kb = SD_MIN_CAP_KB;
    if (cap_kb > SD_MAX_CAP_KB) cap_kb = SD_MAX_CAP_KB;
    if (!state.sd_mounted && cap_kb > FLASH_CAP_KB) cap_kb = FLASH_CAP_KB;
    return cap_kb * 1024U;
}

uint8_t SyslogRotator::effectiveFileCount(const SystemState& state) {
    uint8_t count = state.log_file_count;
    if (count < 1) count = 1;
    if (count > MAX_FILE_COUNT) count = MAX_FILE_COUNT;
    if (!state.sd_mounted && count > FLASH_FILE_COUNT) count = FLASH_FILE_COUNT;
    return count;
}

void SyslogRotator::collect(SystemState& state) {
    if (first_service_) {
        // Pick up every boot line still resident in the ring on the first pass
        first_service_ = false;
        uint32_t resident = state.dmesg_count;
        last_seq_ = (state.dmesg_seq > resident) ? (state.dmesg_seq - resident) : 0U;
    }
    if (state.dmesg_seq == last_seq_) return;

    uint32_t pending = state.dmesg_seq - last_seq_;
    if (pending > state.dmesg_count) {
        // Ring overran between services: lines older than the ring are gone
        state.log_lines_dropped += pending - state.dmesg_count;
        pending = state.dmesg_count;
    }

    size_t first_idx = static_cast<size_t>(state.dmesg_count - pending);
    for (size_t i = first_idx; i < state.dmesg_count; ++i) {
        const char* line = state.dmesgLineAt(i);
        size_t len = strlen(line);
        if (len + 1U > STAGE_BYTES) len = STAGE_BYTES - 1U;
        if (staged_len_ + len + 1U > STAGE_BYTES) {
            // Staging full: remaining lines stay in the ring for the next service pass
            return;
        }
        memcpy(staging_ + staged_len_, line, len);
        staged_len_ += len;
        staging_[staged_len_++] = '\n';
        ++last_seq_;
    }
}

bool SyslogRotator::refreshSdUsage(SystemState& state) {
    const HardwareOps* hw = getHardwareOps();
    if (!state.sd_mounted || !hw || !hw->sd_usage) return false;
    uint64_t total = 0;
    uint64_t free_b = 0;
    if (hw->sd_usage(&total, &free_b) != 0 || total == 0) return false;
    uint64_t used = (total > free_b) ? (total - free_b) : 0U;
    state.sd_total_mb = static_cast<uint32_t>(total / (1024ULL * 1024ULL));
    state.sd_used_mb  = static_cast<uint32_t>(used / (1024ULL * 1024ULL));
    state.sd_free_mb  = static_cast<uint32_t>(free_b / (1024ULL * 1024ULL));
    return true;
}

void SyslogRotator::purgeGenerations(SystemState& state, VirtualFs& vfs, const char* base, uint8_t count) {
    for (unsigned gen = 1; gen < count; ++gen) {
        char path[64];
        formatGeneration(base, gen, path, sizeof(path));
        size_t sz = 0;
        if (vfs.fileSize(path, &sz)) {
            vfs.removeFile(state, path);
        }
    }
}

void SyslogRotator::rotate(SystemState& state, VirtualFs& vfs, const char* base, uint8_t count) {
    // messages.(count-2) -> messages.(count-1) ... messages -> messages.1 (rename drops the oldest)
    for (int gen = static_cast<int>(count) - 2; gen >= 0; --gen) {
        char from[64];
        char to[64];
        formatGeneration(base, static_cast<unsigned>(gen), from, sizeof(from));
        formatGeneration(base, static_cast<unsigned>(gen) + 1U, to, sizeof(to));
        size_t sz = 0;
        if (vfs.fileSize(from, &sz)) {
            vfs.renamePath(from, to);
        }
    }
    if (count == 1) {
        vfs.removeFile(state, base);
    }
    state.log_rotations++;
}

bool SyslogRotator::flush(SystemState& state, VirtualFs& vfs, bool force_rotate) {
    const char* base = targetPath(state);
    uint32_t cap_bytes = effectiveCapBytes(state);
    uint8_t count = effectiveFileCount(state);

    if (state.sd_mounted && refreshSdUsage(state)) {
        if (state.sd_free_mb < SD_FREE_FLOOR_MB) {
            if (!state.log_sd_paused) {
                purgeGenerations(state, vfs, base, count);
                refreshSdUsage(state);
            }
            if (state.sd_free_mb < SD_FREE_FLOOR_MB) {
                // Still below floor: drop staged lines rather than fill the card
                if (!state.log_sd_paused) {
                    state.log_sd_paused = true;
                }
                unsigned dropped_lines = 0;
                for (size_t i = 0; i < staged_len_; ++i) {
                    if (staging_[i] == '\n') ++dropped_lines;
                }
                state.log_lines_dropped += dropped_lines;
                staged_len_ = 0;
                return false;
            }
        }
        state.log_sd_paused = false;
    }

    size_t cur_size = 0;
    bool exists = vfs.fileSize(base, &cur_size);
    if (force_rotate || (exists && cur_size + staged_len_ > cap_bytes)) {
        rotate(state, vfs, base, count);
    }

    if (staged_len_ == 0) return false;

    FILE* fp = vfs.openPosix(base, "ab");
    if (!fp) {
        return false;
    }
    size_t wrote = fwrite(staging_, 1, staged_len_, fp);
    fclose(fp);
    if (wrote != staged_len_) {
        staged_len_ = 0;
        return false;
    }

    unsigned lines = 0;
    for (size_t i = 0; i < staged_len_; ++i) {
        if (staging_[i] == '\n') ++lines;
    }
    state.log_lines_written += lines;
    staged_len_ = 0;
    return true;
}

bool SyslogRotator::service(SystemState& state, VirtualFs& vfs, bool force) {
    collect(state);

    bool rotate_now = state.log_rotate_requested;
    bool flush_now = force || state.log_flush_requested || rotate_now;
    state.log_flush_requested = false;
    state.log_rotate_requested = false;

    if (!state.log_persist_enabled) {
        staged_len_ = 0;
        return false;
    }

    bool interval_due = (state.uptime_sec - last_flush_sec_) >= FLUSH_INTERVAL_SEC;
    bool stage_full = staged_len_ >= (STAGE_BYTES - DMESG_LINE_LEN);
    if (!flush_now && !stage_full && !(interval_due && staged_len_ > 0)) {
        return false;
    }

    last_flush_sec_ = state.uptime_sec;
    return flush(state, vfs, rotate_now);
}

}  // namespace c6os
