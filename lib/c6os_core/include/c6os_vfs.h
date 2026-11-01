#ifndef C6OS_VFS_H
#define C6OS_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"

namespace c6os {

constexpr size_t VFS_MAX_USER_FILES = 16;
constexpr size_t VFS_MAX_PATH_LEN   = 64;
constexpr size_t VFS_MAX_FILE_SIZE  = 512;

struct VfsFileEntry {
    bool in_use;
    char path[VFS_MAX_PATH_LEN];
    char content[VFS_MAX_FILE_SIZE];
};

class VirtualFs {
public:
    VirtualFs();

    void initDefaultFiles(const SystemState& state);

    // Path normalization (resolves relative paths against cwd)
    static void normalizePath(const char* cwd, const char* input_path, char* out_path, size_t max_len);

    bool isDirectory(const char* abs_path) const;

    // Directory listing (ls)
    bool listDirectory(const char* abs_path, char* out_buf, size_t max_len) const;

    // Read file content (cat)
    bool readFile(const SystemState& state, const char* abs_path, char* out_buf, size_t max_len) const;

    // Write file content (echo ... > path)
    bool writeFile(SystemState& state, const char* abs_path, const char* data, bool append = false);

    // Delete user file (rm)
    bool removeFile(SystemState& state, const char* abs_path);

    const VfsFileEntry* getUserFiles() const { return user_files_; }
    size_t getMaxUserFiles() const { return VFS_MAX_USER_FILES; }

private:
    VfsFileEntry user_files_[VFS_MAX_USER_FILES];
};

}  // namespace c6os

#endif  // C6OS_VFS_H
