#ifndef C6OS_EPKG_H
#define C6OS_EPKG_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {
namespace epkg {

bool installPackage(SystemState& state, VirtualFs& vfs, const char* pkg_abs_path,
                    BufWriter& out, bool allow_unsigned = false);
bool removePackage(SystemState& state, VirtualFs& vfs, const char* pkg_name, BufWriter& out);
bool listPackages(const VirtualFs& vfs, BufWriter& out);
bool packageInfo(const SystemState& state, const VirtualFs& vfs, const char* pkg_name, BufWriter& out);

// Builds an unsigned EPK1 binary package in memory (178 B header + payload)
size_t buildPackageBlob(const char* name, const char* version, const char* arch,
                        const char* description, const char* target_vpath,
                        uint16_t mode, const uint8_t* file_data, size_t file_len,
                        uint8_t* out_blob, size_t out_cap);

// Builds an Ed25519-signed EPK2 binary package in memory (242 B header + payload)
size_t buildSignedPackageBlob(const char* name, const char* version, const char* arch,
                              const char* description, const char* target_vpath,
                              uint16_t mode, const uint8_t* file_data, size_t file_len,
                              const uint8_t ed25519_sk[64],
                              uint8_t* out_blob, size_t out_cap);

}  // namespace epkg
}  // namespace c6os

#endif  // C6OS_EPKG_H
