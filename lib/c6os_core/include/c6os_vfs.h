#ifndef C6OS_VFS_H
#define C6OS_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "c6os_types.h"

namespace c6os {

constexpr size_t VFS_MAX_PATH_LEN = 128;
constexpr size_t VFS_MAX_FILE_SIZE = 2048;  // Max buffer for interactive text edit / script lines

struct VfsDirEntry {
    char name[64];
    bool is_dir;
    uint64_t size;
    uint32_t mode;
    uint32_t mtime;
};

class VirtualFs {
public:
    VirtualFs();
    ~VirtualFs();

    VirtualFs(const VirtualFs&) = delete;
    VirtualFs& operator=(const VirtualFs&) = delete;

    void setMountRoots(const char* rootfs_prefix, const char* sd_prefix);
    const char* rootfsPrefix() const { return rootfs_prefix_; }
    const char* sdPrefix() const { return sd_prefix_; }

    void initDefaultFiles(SystemState& state);

    // Path normalization (resolves relative paths and ~ against cwd)
    static void normalizePath(const char* cwd, const char* input_path, char* out_path, size_t max_len);

    // Maps virtual OS path (e.g. "/root/foo", "/mnt/sd/bar") to underlying POSIX mount path.
    // Returns false for kernel pseudo-filesystems (/proc, /sys, /dev, /bin).
    bool resolveRealPath(const char* abs_path, char* out_real, size_t max_len) const;

    bool isVirtualPath(const char* abs_path) const;
    bool isDirectory(const char* abs_path) const;

    // Directory listing formatted for `ls`
    bool listDirectory(const char* abs_path, char* out_buf, size_t max_len) const;

    // Structured directory enumeration (used by SFTP v3 READDIR and epkg)
    size_t readDirEntries(const char* abs_path, VfsDirEntry* out_entries, size_t max_entries) const;

    // Stat metadata for both virtual and POSIX-backed paths
    bool statPath(const char* abs_path, uint64_t* out_size, uint32_t* out_mode, uint32_t* out_mtime) const;

    // Read text or synthetic file content (cat, scripts, auth keys)
    bool readFile(const SystemState& state, const char* abs_path, char* out_buf, size_t max_len) const;

    // Write text or sysfs/dev file content
    bool writeFile(SystemState& state, const char* abs_path, const char* data, bool append = false);

    // Binary file helpers (unlimited size up to LittleFS / SD capacity)
    FILE* openPosix(const char* abs_path, const char* mode) const;
    bool fileSize(const char* abs_path, size_t* out_size) const;
    bool readBinary(const char* abs_path, size_t offset, uint8_t* out_buf, size_t max_len, size_t* out_read) const;
    bool writeBinary(SystemState& state, const char* abs_path, const uint8_t* data, size_t len, bool append = false);

    // Filesystem mutation helpers
    bool removeFile(SystemState& state, const char* abs_path);
    bool mkdirPath(const char* abs_path) const;
    bool rmdirPath(const char* abs_path) const;
    bool renamePath(const char* old_abs, const char* new_abs) const;

private:
    char rootfs_prefix_[96];
    char sd_prefix_[96];
    char temp_base_[96];
    bool owns_temp_root_;

    void ensureParentDirs(const char* real_path) const;
};

}  // namespace c6os

#endif  // C6OS_VFS_H
