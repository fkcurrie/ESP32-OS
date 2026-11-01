#ifndef C6OS_LUA_H
#define C6OS_LUA_H

#include <stddef.h>
#include <stdint.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {
namespace luavm {

constexpr size_t LUA_DEFAULT_QUOTA_BYTES = 65536;  // 64 KB cap per Lua VM invocation

// Evaluates an inline Lua 5.4 chunk (e.g. `lua -e "print(sys.heap())"`) with 0 B idle SRAM
bool runChunk(SystemState& state, VirtualFs& vfs, const char* chunk_name,
              const char* code, int argc, const char* const* argv,
              BufWriter& out, size_t quota_bytes = LUA_DEFAULT_QUOTA_BYTES);

// Reads a .lua file via VirtualFs (stripping an optional `#!...` shebang line) and executes it
bool runFile(SystemState& state, VirtualFs& vfs, const char* script_abs_path,
             int argc, const char* const* argv,
             BufWriter& out, size_t quota_bytes = LUA_DEFAULT_QUOTA_BYTES);

}  // namespace luavm
}  // namespace c6os

#endif  // C6OS_LUA_H
