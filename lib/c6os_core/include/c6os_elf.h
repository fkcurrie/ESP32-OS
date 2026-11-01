#ifndef C6OS_ELF_H
#define C6OS_ELF_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {
namespace elf {

// Returns true if the first 4 bytes match "\x7fELF"
bool hasElfMagic(const uint8_t* data, size_t len);

// Loads and executes an RV32IMAC ELF32 binary (ET_REL or ET_EXEC) inside a bounds-checked
// 48 KB sandboxed address space with C6-MicroLinux C ABI imports and Linux RISC-V ecalls.
bool runElfFile(SystemState& state, VirtualFs& vfs,
                const char* elf_abs_path, int argc, const char* const* argv,
                BufWriter& out, int* out_exit_code = nullptr);

}  // namespace elf
}  // namespace c6os

#endif  // C6OS_ELF_H
