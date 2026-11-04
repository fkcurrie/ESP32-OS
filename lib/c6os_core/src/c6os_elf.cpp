#include "c6os_elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include <Arduino.h>
#endif

namespace c6os {
namespace elf {

namespace {

constexpr uint16_t ET_REL   = 1;
constexpr uint16_t ET_EXEC  = 2;
constexpr uint16_t EM_RISCV = 243;

constexpr uint32_t PT_LOAD       = 1;
constexpr uint32_t PF_X          = 0x1U;
constexpr uint32_t PF_W          = 0x2U;
constexpr uint32_t PF_R          = 0x4U;

constexpr uint32_t SHT_PROGBITS  = 1;
constexpr uint32_t SHT_SYMTAB    = 2;
constexpr uint32_t SHT_RELA      = 4;
constexpr uint32_t SHT_NOBITS    = 8;
constexpr uint32_t SHF_WRITE     = 0x1U;
constexpr uint32_t SHF_ALLOC     = 0x2U;
constexpr uint32_t SHF_EXECINSTR = 0x4U;

constexpr uint32_t R_RISCV_NONE         = 0;
constexpr uint32_t R_RISCV_32           = 1;
constexpr uint32_t R_RISCV_BRANCH       = 16;
constexpr uint32_t R_RISCV_JAL          = 17;
constexpr uint32_t R_RISCV_CALL         = 18;
constexpr uint32_t R_RISCV_CALL_PLT     = 19;
constexpr uint32_t R_RISCV_PCREL_HI20   = 23;
constexpr uint32_t R_RISCV_PCREL_LO12_I = 24;
constexpr uint32_t R_RISCV_PCREL_LO12_S = 25;
constexpr uint32_t R_RISCV_HI20         = 26;
constexpr uint32_t R_RISCV_LO12_I       = 27;
constexpr uint32_t R_RISCV_LO12_S       = 28;
constexpr uint32_t R_RISCV_ADD32        = 35;
constexpr uint32_t R_RISCV_SUB32        = 39;
constexpr uint32_t R_RISCV_ALIGN        = 43;
constexpr uint32_t R_RISCV_RVC_BRANCH   = 44;
constexpr uint32_t R_RISCV_RVC_JUMP     = 45;
constexpr uint32_t R_RISCV_RELAX        = 51;

constexpr uint32_t VM_BASE        = 0x00010000U;
constexpr uint32_t VM_MIN_SIZE    = 16384U;   // 16 KB minimum dynamic sandbox
constexpr uint32_t VM_MAX_SIZE    = 49152U;   // 48 KB maximum dynamic sandbox
constexpr uint32_t VM_STEP_SIZE   = 4096U;    // 4 KB allocation quantum
constexpr uint32_t VM_STACK_ARENA = 4096U;    // Top 4 KB reserved for argv + stack
constexpr uint32_t TRAMP_BASE     = 0xF0000000U;
constexpr uint32_t EXIT_TRAMP     = 0xF000FFFCU;
constexpr uint32_t MAX_STEPS      = 2000000U;

constexpr uint8_t PERM_R = 0x1U;
constexpr uint8_t PERM_W = 0x2U;
constexpr uint8_t PERM_X = 0x4U;
constexpr size_t  MAX_VM_REGIONS = 24U;

#pragma pack(push, 1)
struct Elf32Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct Elf32Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct Elf32Sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

struct Elf32Rela {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
};
#pragma pack(pop)

enum ImportId : uint32_t {
    IMP_NONE = 0,
    IMP_PRINTF,
    IMP_PUTS,
    IMP_PUTCHAR,
    IMP_SNPRINTF,
    IMP_MALLOC,
    IMP_FREE,
    IMP_MEMCPY,
    IMP_MEMSET,
    IMP_STRLEN,
    IMP_STRCMP,
    IMP_C6_LCD_TEXT,
    IMP_C6_LCD_MODE,
    IMP_C6_LCD_THEME,
    IMP_C6_BACKLIGHT,
    IMP_C6_LED_RGB,
    IMP_C6_GPIO_MODE,
    IMP_C6_GPIO_WRITE,
    IMP_C6_GPIO_READ,
    IMP_C6_PWM_WRITE,
    IMP_C6_ADC_READ_MV,
    IMP_C6_I2C_SCAN,
    IMP_C6_UPTIME_MS,
    IMP_C6_FREE_RAM,
    IMP_C6_DELAY_MS,
    IMP_COUNT
};

struct ImportEntry {
    const char* name;
    ImportId id;
};

constexpr ImportEntry IMPORT_TABLE[] = {
    {"printf",         IMP_PRINTF},
    {"puts",           IMP_PUTS},
    {"putchar",        IMP_PUTCHAR},
    {"snprintf",       IMP_SNPRINTF},
    {"malloc",         IMP_MALLOC},
    {"free",           IMP_FREE},
    {"memcpy",         IMP_MEMCPY},
    {"memset",         IMP_MEMSET},
    {"strlen",         IMP_STRLEN},
    {"strcmp",         IMP_STRCMP},
    {"c6_lcd_text",    IMP_C6_LCD_TEXT},
    {"c6_lcd_mode",    IMP_C6_LCD_MODE},
    {"c6_lcd_theme",   IMP_C6_LCD_THEME},
    {"c6_backlight",   IMP_C6_BACKLIGHT},
    {"c6_led_rgb",     IMP_C6_LED_RGB},
    {"c6_gpio_mode",   IMP_C6_GPIO_MODE},
    {"c6_gpio_write",  IMP_C6_GPIO_WRITE},
    {"c6_gpio_read",   IMP_C6_GPIO_READ},
    {"c6_pwm_write",   IMP_C6_PWM_WRITE},
    {"c6_adc_read_mv", IMP_C6_ADC_READ_MV},
    {"c6_i2c_scan",    IMP_C6_I2C_SCAN},
    {"c6_uptime_ms",   IMP_C6_UPTIME_MS},
    {"c6_free_ram",    IMP_C6_FREE_RAM},
    {"c6_delay_ms",    IMP_C6_DELAY_MS},
};

uint32_t lookupImportAddr(const char* name) {
    if (!name) return 0;
    for (const auto& e : IMPORT_TABLE) {
        if (strcmp(name, e.name) == 0) {
            return TRAMP_BASE + static_cast<uint32_t>(e.id) * 4U;
        }
    }
    return 0;
}

uint16_t rd16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void wr16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

void wr32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

int32_t signExtend(uint32_t val, unsigned bits) {
    uint32_t shift = 32U - bits;
    return static_cast<int32_t>(val << shift) >> shift;
}

uint32_t encodeUType(uint32_t inst, uint32_t imm20) {
    return (inst & 0x00000FFFU) | (imm20 << 12);
}

uint32_t encodeIType(uint32_t inst, uint32_t imm12) {
    return (inst & 0x000FFFFFU) | ((imm12 & 0xFFFU) << 20);
}

uint32_t encodeSType(uint32_t inst, uint32_t imm12) {
    uint32_t lo5 = imm12 & 0x1FU;
    uint32_t hi7 = (imm12 >> 5) & 0x7FU;
    return (inst & 0x01FFF07FU) | (lo5 << 7) | (hi7 << 25);
}

uint32_t encodeJType(uint32_t inst, int32_t off) {
    uint32_t u = static_cast<uint32_t>(off);
    uint32_t b20    = (u >> 20) & 0x1U;
    uint32_t b10_1  = (u >> 1) & 0x3FFU;
    uint32_t b11    = (u >> 11) & 0x1U;
    uint32_t b19_12 = (u >> 12) & 0xFFU;
    return (inst & 0x00000FFFU) | (b20 << 31) | (b10_1 << 21) | (b11 << 20) | (b19_12 << 12);
}

uint32_t encodeBType(uint32_t inst, int32_t off) {
    uint32_t u = static_cast<uint32_t>(off);
    uint32_t b12   = (u >> 12) & 0x1U;
    uint32_t b10_5 = (u >> 5) & 0x3FU;
    uint32_t b4_1  = (u >> 1) & 0xFU;
    uint32_t b11   = (u >> 11) & 0x1U;
    return (inst & 0x01FFF07FU) | (b12 << 31) | (b10_5 << 25) | (b4_1 << 8) | (b11 << 7);
}

bool computeDynamicVmSize(uint32_t image_span_bytes, uint32_t* out_vm_size) {
    if (image_span_bytes + VM_STACK_ARENA + 1024U > VM_MAX_SIZE) {
        return false;
    }
    uint32_t needed = image_span_bytes + 8192U;  // >= 4 KB heap + 4 KB argv/stack
    if (needed > VM_MAX_SIZE) {
        needed = VM_MAX_SIZE;
    }
    uint32_t sz = (needed + VM_STEP_SIZE - 1U) & ~(VM_STEP_SIZE - 1U);
    if (sz < VM_MIN_SIZE) sz = VM_MIN_SIZE;
    if (sz > VM_MAX_SIZE) sz = VM_MAX_SIZE;
    *out_vm_size = sz;
    return true;
}

struct HiRelocRecord {
    uint32_t pc_addr;
    int32_t full_off;
};

struct MemRegion {
    uint32_t lo;
    uint32_t hi;
    uint8_t perm;
};

struct RvVm {
    uint8_t* mem;
    uint32_t mem_size;
    uint32_t heap_base;
    uint32_t heap_brk;
    uint32_t heap_end;
    uint32_t argv_base;
    uint32_t sp_init;
    MemRegion regions[MAX_VM_REGIONS];
    size_t region_count;
    uint32_t regs[32];
    uint32_t pc;
    uint32_t lr_addr;
    bool lr_valid;
    bool exited;
    int exit_code;
    SystemState* state;
    VirtualFs* vfs;
    BufWriter* out;

    bool validRange(uint32_t vaddr, size_t len) const {
        if (vaddr < VM_BASE || len > mem_size) return false;
        uint32_t off = vaddr - VM_BASE;
        return off + len <= mem_size;
    }

    bool addRegion(uint32_t lo, uint32_t hi, uint8_t perm) {
        if (lo >= hi) return true;
        if ((perm & (PERM_W | PERM_X)) == (PERM_W | PERM_X)) return false;
        if (lo < VM_BASE || hi > VM_BASE + mem_size) return false;
        if (region_count >= MAX_VM_REGIONS) return false;
        for (size_t i = 0; i < region_count; ++i) {
            const MemRegion& r = regions[i];
            if (lo < r.hi && hi > r.lo) {
                uint8_t combined = static_cast<uint8_t>(perm | r.perm);
                if ((combined & (PERM_W | PERM_X)) == (PERM_W | PERM_X)) {
                    return false;
                }
            }
        }
        regions[region_count++] = {lo, hi, perm};
        return true;
    }

    bool hasPerm(uint32_t vaddr, size_t len, uint8_t req_perm) const {
        if (!validRange(vaddr, len)) return false;
        if (len == 0) return true;
        uint32_t cur = vaddr;
        uint32_t end = vaddr + static_cast<uint32_t>(len);
        while (cur < end) {
            bool matched = false;
            for (size_t i = 0; i < region_count; ++i) {
                const MemRegion& r = regions[i];
                if (cur >= r.lo && cur < r.hi &&
                    (r.perm & req_perm) == req_perm &&
                    (r.perm & (PERM_W | PERM_X)) != (PERM_W | PERM_X)) {
                    cur = (end < r.hi) ? end : r.hi;
                    matched = true;
                    break;
                }
            }
            if (!matched) return false;
        }
        return true;
    }

    // Loader-only raw pointer (used before execution begins)
    uint8_t* rawPtr(uint32_t vaddr, size_t len) const {
        return validRange(vaddr, len) ? (mem + (vaddr - VM_BASE)) : nullptr;
    }

    const uint8_t* readPtr(uint32_t vaddr, size_t len) const {
        return hasPerm(vaddr, len, PERM_R) ? (mem + (vaddr - VM_BASE)) : nullptr;
    }

    uint8_t* writePtr(uint32_t vaddr, size_t len) const {
        return hasPerm(vaddr, len, PERM_W) ? (mem + (vaddr - VM_BASE)) : nullptr;
    }

    const uint8_t* execPtr(uint32_t vaddr, size_t len) const {
        return (len > 0 && hasPerm(vaddr, len, PERM_X)) ? (mem + (vaddr - VM_BASE)) : nullptr;
    }

    bool readCString(uint32_t vaddr, char* dst, size_t cap) const {
        if (!dst || cap == 0) return false;
        if (vaddr == 0) {
            dst[0] = '\0';
            return true;
        }
        if (vaddr < VM_BASE || vaddr >= VM_BASE + mem_size) return false;
        size_t max_avail = (VM_BASE + mem_size) - vaddr;
        const char* src = reinterpret_cast<const char*>(mem + (vaddr - VM_BASE));
        size_t i = 0;
        while (i < cap - 1 && i < max_avail) {
            if (!hasPerm(vaddr + static_cast<uint32_t>(i), 1, PERM_R)) return false;
            if (src[i] == '\0') break;
            dst[i] = src[i];
            ++i;
        }
        dst[i] = '\0';
        return true;
    }
};

// Minimal safe formatter for VM printf/snprintf reading args from a1..a7 and VM stack
size_t vmFormat(RvVm& vm, uint32_t fmt_vaddr, int first_arg_reg, char* dst, size_t dst_cap) {
    char fmt[256];
    if (!vm.readCString(fmt_vaddr, fmt, sizeof(fmt))) return 0;

    BufWriter w(dst, dst_cap);
    int arg_idx = 0;
    auto nextArg = [&]() -> uint32_t {
        int reg = first_arg_reg + arg_idx;
        ++arg_idx;
        if (reg <= 17) {
            return vm.regs[reg];
        }
        uint32_t sp_off = vm.regs[2] + static_cast<uint32_t>(reg - 18) * 4U;
        const uint8_t* p = vm.readPtr(sp_off, 4);
        return p ? rd32(p) : 0U;
    };

    for (size_t i = 0; fmt[i] != '\0'; ++i) {
        if (fmt[i] != '%') {
            w.putChar(fmt[i]);
            continue;
        }
        ++i;
        if (fmt[i] == '%') {
            w.putChar('%');
            continue;
        }
        // Skip optional width/zero flags
        while (fmt[i] == '-' || fmt[i] == '0' || (fmt[i] >= '1' && fmt[i] <= '9') || fmt[i] == '.') {
            ++i;
        }
        if (fmt[i] == 'l') ++i;
        char spec = fmt[i];
        if (spec == '\0') break;
        uint32_t val = nextArg();
        if (spec == 's') {
            char sbuf[192];
            if (vm.readCString(val, sbuf, sizeof(sbuf))) w.putStr(sbuf);
            else w.putStr("(null)");
        } else if (spec == 'd' || spec == 'i') {
            w.fmt("%d", static_cast<int>(static_cast<int32_t>(val)));
        } else if (spec == 'u') {
            w.fmt("%u", static_cast<unsigned>(val));
        } else if (spec == 'x') {
            w.fmt("%x", static_cast<unsigned>(val));
        } else if (spec == 'X' || spec == 'p') {
            w.fmt("0x%08x", static_cast<unsigned>(val));
        } else if (spec == 'c') {
            w.putChar(static_cast<char>(val & 0xFFU));
        } else {
            w.putChar(spec);
        }
    }
    return w.length();
}

bool handleImportCall(RvVm& vm, ImportId id) {
    const HardwareOps* hw = getHardwareOps();
    uint32_t a0 = vm.regs[10];
    uint32_t a1 = vm.regs[11];
    uint32_t a2 = vm.regs[12];

    switch (id) {
        case IMP_PRINTF: {
            char buf[384];
            size_t n = vmFormat(vm, a0, 11, buf, sizeof(buf));
            vm.out->putStr(buf);
            vm.regs[10] = static_cast<uint32_t>(n);
            return true;
        }
        case IMP_PUTS: {
            char buf[256];
            if (!vm.readCString(a0, buf, sizeof(buf))) return false;
            vm.out->putStr(buf);
            vm.out->putChar('\n');
            vm.regs[10] = 0;
            return true;
        }
        case IMP_PUTCHAR:
            vm.out->putChar(static_cast<char>(a0 & 0xFFU));
            vm.regs[10] = a0 & 0xFFU;
            return true;
        case IMP_SNPRINTF: {
            if (a1 == 0) {
                vm.regs[10] = 0;
                return true;
            }
            uint8_t* dst = vm.writePtr(a0, a1);
            if (!dst) return false;
            size_t n = vmFormat(vm, a2, 13, reinterpret_cast<char*>(dst), a1);
            vm.regs[10] = static_cast<uint32_t>(n);
            return true;
        }
        case IMP_MALLOC: {
            if (a0 > vm.heap_end - vm.heap_brk) {
                vm.regs[10] = 0;
                return true;
            }
            uint32_t sz = (a0 + 7U) & ~7U;
            if (sz > vm.heap_end - vm.heap_brk) {
                vm.regs[10] = 0;
            } else {
                vm.regs[10] = vm.heap_brk;
                vm.heap_brk += sz;
            }
            return true;
        }
        case IMP_FREE:
            vm.regs[10] = 0;
            return true;
        case IMP_MEMCPY: {
            uint8_t* dst = vm.writePtr(a0, a2);
            const uint8_t* src = vm.readPtr(a1, a2);
            if (!dst || !src) return false;
            if (a2 > 0) memmove(dst, src, a2);
            vm.regs[10] = a0;
            return true;
        }
        case IMP_MEMSET: {
            uint8_t* dst = vm.writePtr(a0, a2);
            if (!dst) return false;
            if (a2 > 0) memset(dst, static_cast<int>(a1 & 0xFFU), a2);
            vm.regs[10] = a0;
            return true;
        }
        case IMP_STRLEN: {
            char buf[256];
            if (!vm.readCString(a0, buf, sizeof(buf))) return false;
            vm.regs[10] = static_cast<uint32_t>(strlen(buf));
            return true;
        }
        case IMP_STRCMP: {
            char s1[192], s2[192];
            if (!vm.readCString(a0, s1, sizeof(s1)) || !vm.readCString(a1, s2, sizeof(s2))) return false;
            vm.regs[10] = static_cast<uint32_t>(static_cast<int32_t>(strcmp(s1, s2)));
            return true;
        }
        case IMP_C6_LCD_TEXT: {
            char h[48], s[64];
            if (!vm.readCString(a0, h, sizeof(h)) || !vm.readCString(a1, s, sizeof(s))) return false;
            if (h[0] != '\0') strncpy(vm.state->headline, h, sizeof(vm.state->headline) - 1);
            if (a1 != 0) strncpy(vm.state->subtitle, s, sizeof(vm.state->subtitle) - 1);
            vm.state->markDirty(DIRTY_CARDS | DIRTY_FULL);
            vm.regs[10] = 0;
            return true;
        }
        case IMP_C6_LCD_MODE: {
            char m[32];
            if (!vm.readCString(a0, m, sizeof(m))) return false;
            bool ok = vm.vfs->writeFile(*vm.state, "/sys/class/graphics/fb0/mode", m, false);
            vm.regs[10] = ok ? 0U : static_cast<uint32_t>(-1);
            return true;
        }
        case IMP_C6_LCD_THEME: {
            char t[32];
            if (!vm.readCString(a0, t, sizeof(t))) return false;
            bool ok = vm.vfs->writeFile(*vm.state, "/sys/class/graphics/fb0/theme", t, false);
            vm.regs[10] = ok ? 0U : static_cast<uint32_t>(-1);
            return true;
        }
        case IMP_C6_BACKLIGHT: {
            int pct = static_cast<int>(a0);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            vm.state->backlight_pct = static_cast<uint8_t>(pct);
            vm.state->markDirty(DIRTY_HEADER);
            vm.regs[10] = 0;
            return true;
        }
        case IMP_C6_LED_RGB:
            vm.state->led_r = static_cast<uint8_t>(a0 & 0xFFU);
            vm.state->led_g = static_cast<uint8_t>(a1 & 0xFFU);
            vm.state->led_b = static_cast<uint8_t>(a2 & 0xFFU);
            vm.state->led_mode = LedMode::SOLID;
            vm.state->markDirty(DIRTY_HEADER);
            vm.regs[10] = 0;
            return true;
        case IMP_C6_GPIO_MODE: {
            int pin = static_cast<int>(a0);
            if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
                vm.regs[10] = static_cast<uint32_t>(-1);
                return true;
            }
            int rc = (hw && hw->gpio_mode) ? hw->gpio_mode(pin, static_cast<uint8_t>(a1)) : 0;
            vm.regs[10] = static_cast<uint32_t>(rc);
            return true;
        }
        case IMP_C6_GPIO_WRITE: {
            int pin = static_cast<int>(a0);
            if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
                vm.regs[10] = static_cast<uint32_t>(-1);
                return true;
            }
            if (a1) vm.state->gpio_out_mask |= (1U << pin);
            else vm.state->gpio_out_mask &= ~(1U << pin);
            int rc = (hw && hw->gpio_write) ? hw->gpio_write(pin, a1 ? 1 : 0) : 0;
            vm.regs[10] = static_cast<uint32_t>(rc);
            return true;
        }
        case IMP_C6_GPIO_READ: {
            int pin = static_cast<int>(a0);
            if (pin < 0 || pin > 23) {
                vm.regs[10] = static_cast<uint32_t>(-1);
                return true;
            }
            int val = (hw && hw->gpio_read) ? hw->gpio_read(pin)
                                            : ((vm.state->gpio_in_mask >> pin) & 1U);
            vm.regs[10] = static_cast<uint32_t>(val);
            return true;
        }
        case IMP_C6_PWM_WRITE: {
            int pin = static_cast<int>(a0);
            if (pin < 0 || pin > 23 || isReservedBoardPin(pin)) {
                vm.regs[10] = static_cast<uint32_t>(-1);
                return true;
            }
            uint8_t duty = static_cast<uint8_t>(a1 & 0xFFU);
            vm.state->gpio_cmd_pin = static_cast<int8_t>(pin);
            vm.state->gpio_cmd_action = 3;
            vm.state->gpio_cmd_val = duty;
            int rc = (hw && hw->gpio_pwm) ? hw->gpio_pwm(pin, duty, 5000U) : 0;
            vm.regs[10] = static_cast<uint32_t>(rc);
            return true;
        }
        case IMP_C6_ADC_READ_MV: {
            int pin = static_cast<int>(a0);
            int mv = (hw && hw->adc_read_mv) ? hw->adc_read_mv(pin) : 1650;
            vm.regs[10] = static_cast<uint32_t>(mv);
            return true;
        }
        case IMP_C6_I2C_SCAN: {
            int max_n = static_cast<int>(a1);
            if (max_n <= 0) {
                vm.regs[10] = 0;
                return true;
            }
            if (max_n > 32) max_n = 32;
            uint8_t* dst = vm.writePtr(a0, static_cast<size_t>(max_n));
            if (!dst) return false;
            int n = (hw && hw->i2c_scan) ? hw->i2c_scan(dst, static_cast<size_t>(max_n)) : 0;
            vm.regs[10] = static_cast<uint32_t>(n);
            return true;
        }
        case IMP_C6_UPTIME_MS:
            vm.regs[10] = (hw && hw->uptime_ms) ? hw->uptime_ms() : (vm.state->uptime_sec * 1000U);
            return true;
        case IMP_C6_FREE_RAM:
            vm.regs[10] = (hw && hw->free_heap) ? hw->free_heap() : vm.state->free_sram_bytes;
            return true;
        case IMP_C6_DELAY_MS: {
            uint32_t ms = a0 > 2000U ? 2000U : a0;
#ifdef ESP_PLATFORM
            delay(ms);
#else
            (void)ms;
#endif
            vm.regs[10] = 0;
            return true;
        }
        default:
            return false;
    }
}

// Expands a 16-bit RV32C compressed instruction into an equivalent 32-bit RV32I instruction
uint32_t expandRvc(uint16_t c) {
    uint8_t op = c & 0x3U;
    uint8_t funct3 = (c >> 13) & 0x7U;

    if (op == 0x0U) {
        uint32_t rd_p = 8U + ((c >> 2) & 0x7U);
        uint32_t rs1_p = 8U + ((c >> 7) & 0x7U);
        if (funct3 == 0x0U) {  // C.ADDI4SPN
            uint32_t nzuimm = ((c >> 1) & 0x3C0U) | ((c >> 7) & 0x30U) |
                              ((c >> 2) & 0x8U) | ((c >> 4) & 0x4U);
            if (nzuimm == 0) return 0;
            return (nzuimm << 20) | (2U << 15) | (0U << 12) | (rd_p << 7) | 0x13U;
        }
        if (funct3 == 0x2U) {  // C.LW
            uint32_t uimm = ((c << 1) & 0x40U) | ((c >> 7) & 0x38U) | ((c >> 4) & 0x4U);
            return (uimm << 20) | (rs1_p << 15) | (2U << 12) | (rd_p << 7) | 0x03U;
        }
        if (funct3 == 0x6U) {  // C.SW
            uint32_t uimm = ((c << 1) & 0x40U) | ((c >> 7) & 0x38U) | ((c >> 4) & 0x4U);
            uint32_t lo5 = uimm & 0x1FU;
            uint32_t hi7 = (uimm >> 5) & 0x7FU;
            return (hi7 << 25) | (rd_p << 20) | (rs1_p << 15) | (2U << 12) | (lo5 << 7) | 0x23U;
        }
    } else if (op == 0x1U) {
        uint32_t rd = (c >> 7) & 0x1FU;
        if (funct3 == 0x0U) {  // C.ADDI / C.NOP
            int32_t imm = signExtend(((c >> 7) & 0x20U) | ((c >> 2) & 0x1FU), 6);
            return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (rd << 15) | (0U << 12) | (rd << 7) | 0x13U;
        }
        if (funct3 == 0x1U || funct3 == 0x5U) {  // C.JAL (rd=1) / C.J (rd=0)
            uint32_t u = ((c >> 1) & 0x800U) | ((c << 2) & 0x400U) | ((c >> 1) & 0x300U) |
                         ((c << 1) & 0x80U) | ((c >> 1) & 0x40U) | ((c << 3) & 0x20U) |
                         ((c >> 7) & 0x10U) | ((c >> 2) & 0xEU);
            int32_t off = signExtend(u, 12);
            uint32_t link = (funct3 == 0x1U) ? 1U : 0U;
            return encodeJType((link << 7) | 0x6FU, off);
        }
        if (funct3 == 0x2U) {  // C.LI
            int32_t imm = signExtend(((c >> 7) & 0x20U) | ((c >> 2) & 0x1FU), 6);
            return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (0U << 15) | (0U << 12) | (rd << 7) | 0x13U;
        }
        if (funct3 == 0x3U) {
            if (rd == 2U) {  // C.ADDI16SP
                uint32_t u = ((c >> 3) & 0x200U) | ((c << 4) & 0x180U) |
                             ((c << 1) & 0x40U) | ((c << 3) & 0x20U) | ((c >> 2) & 0x10U);
                int32_t imm = signExtend(u, 10);
                return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (2U << 15) | (0U << 12) | (2U << 7) | 0x13U;
            } else {  // C.LUI
                uint32_t u = ((c << 5) & 0x20000U) | ((c << 10) & 0x1F000U);
                int32_t imm = signExtend(u >> 12, 6);
                return (static_cast<uint32_t>(imm) << 12) | (rd << 7) | 0x37U;
            }
        }
        if (funct3 == 0x4U) {
            uint32_t rd_p = 8U + ((c >> 7) & 0x7U);
            uint32_t rs2_p = 8U + ((c >> 2) & 0x7U);
            uint8_t sub2 = (c >> 10) & 0x3U;
            uint32_t shamt = ((c >> 7) & 0x20U) | ((c >> 2) & 0x1FU);
            if (sub2 == 0x0U) {  // C.SRLI
                return (shamt << 20) | (rd_p << 15) | (5U << 12) | (rd_p << 7) | 0x13U;
            }
            if (sub2 == 0x1U) {  // C.SRAI
                return (0x20U << 25) | (shamt << 20) | (rd_p << 15) | (5U << 12) | (rd_p << 7) | 0x13U;
            }
            if (sub2 == 0x2U) {  // C.ANDI
                int32_t imm = signExtend(shamt, 6);
                return (static_cast<uint32_t>(imm & 0xFFF) << 20) | (rd_p << 15) | (7U << 12) | (rd_p << 7) | 0x13U;
            }
            uint8_t sub_op = (c >> 5) & 0x3U;
            if (sub_op == 0x0U) return (0x20U << 25) | (rs2_p << 20) | (rd_p << 15) | (0U << 12) | (rd_p << 7) | 0x33U; // SUB
            if (sub_op == 0x1U) return (rs2_p << 20) | (rd_p << 15) | (4U << 12) | (rd_p << 7) | 0x33U; // XOR
            if (sub_op == 0x2U) return (rs2_p << 20) | (rd_p << 15) | (6U << 12) | (rd_p << 7) | 0x33U; // OR
            if (sub_op == 0x3U) return (rs2_p << 20) | (rd_p << 15) | (7U << 12) | (rd_p << 7) | 0x33U; // AND
        }
        if (funct3 == 0x6U || funct3 == 0x7U) {  // C.BEQZ / C.BNEZ
            uint32_t rs1_p = 8U + ((c >> 7) & 0x7U);
            uint32_t u = ((c >> 4) & 0x100U) | ((c << 1) & 0xC0U) |
                         ((c << 3) & 0x20U) | ((c >> 7) & 0x18U) | ((c >> 2) & 0x6U);
            int32_t off = signExtend(u, 9);
            uint32_t bf3 = (funct3 == 0x6U) ? 0U : 1U;
            return encodeBType((rs1_p << 15) | (bf3 << 12) | 0x63U, off);
        }
    } else if (op == 0x2U) {
        uint32_t rd = (c >> 7) & 0x1FU;
        uint32_t rs2 = (c >> 2) & 0x1FU;
        if (funct3 == 0x0U) {  // C.SLLI
            uint32_t shamt = ((c >> 7) & 0x20U) | rs2;
            return (shamt << 20) | (rd << 15) | (1U << 12) | (rd << 7) | 0x13U;
        }
        if (funct3 == 0x2U) {  // C.LWSP
            uint32_t uimm = ((c << 4) & 0xC0U) | ((c >> 7) & 0x20U) | ((c >> 2) & 0x1CU);
            return (uimm << 20) | (2U << 15) | (2U << 12) | (rd << 7) | 0x03U;
        }
        if (funct3 == 0x4U) {
            bool bit12 = ((c >> 12) & 1U) != 0;
            if (!bit12 && rs2 == 0) {  // C.JR
                return (rd << 15) | 0x67U;
            }
            if (!bit12 && rs2 != 0) {  // C.MV
                return (rs2 << 20) | (0U << 15) | (0U << 12) | (rd << 7) | 0x33U;
            }
            if (bit12 && rd == 0 && rs2 == 0) {  // C.EBREAK
                return 0x00100073U;
            }
            if (bit12 && rs2 == 0) {  // C.JALR
                return (rd << 15) | (1U << 7) | 0x67U;
            }
            if (bit12 && rs2 != 0) {  // C.ADD
                return (rs2 << 20) | (rd << 15) | (0U << 12) | (rd << 7) | 0x33U;
            }
        }
        if (funct3 == 0x6U) {  // C.SWSP
            uint32_t uimm = ((c >> 1) & 0xC0U) | ((c >> 7) & 0x3CU);
            uint32_t lo5 = uimm & 0x1FU;
            uint32_t hi7 = (uimm >> 5) & 0x7FU;
            return (hi7 << 25) | (rs2 << 20) | (2U << 15) | (2U << 12) | (lo5 << 7) | 0x23U;
        }
    }
    return 0;
}

bool executeVm(RvVm& vm) {
    for (uint32_t step = 0; step < MAX_STEPS && !vm.exited; ++step) {
        vm.regs[0] = 0;
#ifdef ESP_PLATFORM
        if ((step & 0x3FFFU) == 0x3FFFU) {
            delay(0);
        }
#endif
        if (vm.pc == EXIT_TRAMP) {
            vm.exited = true;
            vm.exit_code = static_cast<int>(vm.regs[10]);
            return true;
        }
        if (vm.pc >= TRAMP_BASE && vm.pc < TRAMP_BASE + IMP_COUNT * 4U) {
            ImportId id = static_cast<ImportId>((vm.pc - TRAMP_BASE) / 4U);
            if (!handleImportCall(vm, id)) {
                vm.out->fmt("elf: W^X or bounds fault in C ABI call #%u\n", static_cast<unsigned>(id));
                return false;
            }
            vm.pc = vm.regs[1];
            continue;
        }

        const uint8_t* pc_ptr = vm.execPtr(vm.pc, 2);
        if (!pc_ptr) {
            if (vm.validRange(vm.pc, 2)) {
                vm.out->fmt("elf: W^X execute fault at non-executable PC=0x%08x\n",
                            static_cast<unsigned>(vm.pc));
            } else {
                vm.out->fmt("elf: fault fetching PC=0x%08x\n", static_cast<unsigned>(vm.pc));
            }
            return false;
        }

        uint16_t lo16 = rd16(pc_ptr);
        uint32_t inst = 0;
        uint32_t inst_len = 4;
        if ((lo16 & 0x3U) != 0x3U) {
            inst_len = 2;
            inst = expandRvc(lo16);
            if (inst == 0) {
                vm.out->fmt("elf: illegal RVC instruction 0x%04x at PC=0x%08x\n",
                            static_cast<unsigned>(lo16), static_cast<unsigned>(vm.pc));
                return false;
            }
        } else {
            const uint8_t* p32 = vm.execPtr(vm.pc, 4);
            if (!p32) {
                vm.out->fmt("elf: W^X execute fault spanning PC=0x%08x\n",
                            static_cast<unsigned>(vm.pc));
                return false;
            }
            inst = rd32(p32);
        }

        uint32_t opcode = inst & 0x7FU;
        uint32_t rd     = (inst >> 7) & 0x1FU;
        uint32_t funct3 = (inst >> 12) & 0x7U;
        uint32_t rs1    = (inst >> 15) & 0x1FU;
        uint32_t rs2    = (inst >> 20) & 0x1FU;
        uint32_t funct7 = (inst >> 25) & 0x7FU;
        uint32_t next_pc = vm.pc + inst_len;

        switch (opcode) {
            case 0x37U:  // LUI
                if (rd) vm.regs[rd] = inst & 0xFFFFF000U;
                break;
            case 0x17U:  // AUIPC
                if (rd) vm.regs[rd] = vm.pc + (inst & 0xFFFFF000U);
                break;
            case 0x6FU: {  // JAL
                uint32_t u = ((inst >> 11) & 0x100000U) | (inst & 0xFF000U) |
                             ((inst >> 9) & 0x800U) | ((inst >> 20) & 0x7FEU);
                int32_t off = signExtend(u, 21);
                if (rd) vm.regs[rd] = next_pc;
                next_pc = vm.pc + static_cast<uint32_t>(off);
                break;
            }
            case 0x67U: {  // JALR
                int32_t off = signExtend(inst >> 20, 12);
                uint32_t target = (vm.regs[rs1] + static_cast<uint32_t>(off)) & ~1U;
                if (rd) vm.regs[rd] = next_pc;
                next_pc = target;
                break;
            }
            case 0x63U: {  // BRANCH
                uint32_t u = ((inst >> 19) & 0x1000U) | ((inst << 4) & 0x800U) |
                             ((inst >> 20) & 0x7E0U) | ((inst >> 7) & 0x1EU);
                int32_t off = signExtend(u, 13);
                uint32_t a = vm.regs[rs1], b = vm.regs[rs2];
                bool take = false;
                switch (funct3) {
                    case 0: take = (a == b); break;
                    case 1: take = (a != b); break;
                    case 4: take = (static_cast<int32_t>(a) < static_cast<int32_t>(b)); break;
                    case 5: take = (static_cast<int32_t>(a) >= static_cast<int32_t>(b)); break;
                    case 6: take = (a < b); break;
                    case 7: take = (a >= b); break;
                }
                if (take) next_pc = vm.pc + static_cast<uint32_t>(off);
                break;
            }
            case 0x03U: {  // LOAD
                int32_t off = signExtend(inst >> 20, 12);
                uint32_t addr = vm.regs[rs1] + static_cast<uint32_t>(off);
                size_t sz = (funct3 == 0 || funct3 == 4) ? 1 : ((funct3 == 1 || funct3 == 5) ? 2 : 4);
                const uint8_t* p = vm.readPtr(addr, sz);
                if (!p) {
                    vm.out->fmt("elf: load fault at 0x%08x (PC=0x%08x)\n",
                                static_cast<unsigned>(addr), static_cast<unsigned>(vm.pc));
                    return false;
                }
                uint32_t val = 0;
                if (funct3 == 0) val = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(p[0])));
                else if (funct3 == 1) val = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rd16(p))));
                else if (funct3 == 2) val = rd32(p);
                else if (funct3 == 4) val = p[0];
                else if (funct3 == 5) val = rd16(p);
                if (rd) vm.regs[rd] = val;
                break;
            }
            case 0x23U: {  // STORE
                uint32_t u = ((inst >> 20) & 0xFE0U) | ((inst >> 7) & 0x1FU);
                int32_t off = signExtend(u, 12);
                uint32_t addr = vm.regs[rs1] + static_cast<uint32_t>(off);
                size_t sz = (funct3 == 0) ? 1 : ((funct3 == 1) ? 2 : 4);
                uint8_t* p = vm.writePtr(addr, sz);
                if (!p) {
                    if (vm.validRange(addr, sz)) {
                        vm.out->fmt("elf: W^X write fault at non-writable 0x%08x (PC=0x%08x)\n",
                                    static_cast<unsigned>(addr), static_cast<unsigned>(vm.pc));
                    } else {
                        vm.out->fmt("elf: store fault at 0x%08x (PC=0x%08x)\n",
                                    static_cast<unsigned>(addr), static_cast<unsigned>(vm.pc));
                    }
                    return false;
                }
                uint32_t val = vm.regs[rs2];
                if (funct3 == 0) p[0] = static_cast<uint8_t>(val & 0xFFU);
                else if (funct3 == 1) wr16(p, static_cast<uint16_t>(val & 0xFFFFU));
                else wr32(p, val);
                break;
            }
            case 0x13U: {  // OP-IMM
                int32_t imm = signExtend(inst >> 20, 12);
                uint32_t a = vm.regs[rs1];
                uint32_t res = 0;
                switch (funct3) {
                    case 0: res = a + static_cast<uint32_t>(imm); break;
                    case 2: res = (static_cast<int32_t>(a) < imm) ? 1U : 0U; break;
                    case 3: res = (a < static_cast<uint32_t>(imm)) ? 1U : 0U; break;
                    case 4: res = a ^ static_cast<uint32_t>(imm); break;
                    case 6: res = a | static_cast<uint32_t>(imm); break;
                    case 7: res = a & static_cast<uint32_t>(imm); break;
                    case 1: res = a << (rs2 & 0x1FU); break;
                    case 5:
                        if ((funct7 & 0x20U) != 0) res = static_cast<uint32_t>(static_cast<int32_t>(a) >> (rs2 & 0x1FU));
                        else res = a >> (rs2 & 0x1FU);
                        break;
                }
                if (rd) vm.regs[rd] = res;
                break;
            }
            case 0x33U: {  // OP + RV32M
                uint32_t a = vm.regs[rs1], b = vm.regs[rs2];
                uint32_t res = 0;
                if (funct7 == 0x01U) {
                    switch (funct3) {
                        case 0: res = a * b; break;
                        case 1: res = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) *
                                                             static_cast<int64_t>(static_cast<int32_t>(b))) >> 32); break;
                        case 2: res = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(a)) *
                                                             static_cast<uint64_t>(b)) >> 32); break;
                        case 3: res = static_cast<uint32_t>((static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32); break;
                        case 4:
                            if (b == 0) res = 0xFFFFFFFFU;
                            else if (a == 0x80000000U && b == 0xFFFFFFFFU) res = 0x80000000U;
                            else res = static_cast<uint32_t>(static_cast<int32_t>(a) / static_cast<int32_t>(b));
                            break;
                        case 5:
                            res = (b == 0) ? 0xFFFFFFFFU : (a / b);
                            break;
                        case 6:
                            if (b == 0) res = a;
                            else if (a == 0x80000000U && b == 0xFFFFFFFFU) res = 0;
                            else res = static_cast<uint32_t>(static_cast<int32_t>(a) % static_cast<int32_t>(b));
                            break;
                        case 7:
                            res = (b == 0) ? a : (a % b);
                            break;
                    }
                } else {
                    switch (funct3) {
                        case 0: res = (funct7 & 0x20U) ? (a - b) : (a + b); break;
                        case 1: res = a << (b & 0x1FU); break;
                        case 2: res = (static_cast<int32_t>(a) < static_cast<int32_t>(b)) ? 1U : 0U; break;
                        case 3: res = (a < b) ? 1U : 0U; break;
                        case 4: res = a ^ b; break;
                        case 5: res = (funct7 & 0x20U) ? static_cast<uint32_t>(static_cast<int32_t>(a) >> (b & 0x1FU))
                                                       : (a >> (b & 0x1FU)); break;
                        case 6: res = a | b; break;
                        case 7: res = a & b; break;
                    }
                }
                if (rd) vm.regs[rd] = res;
                break;
            }
            case 0x2FU: {  // RV32A Atomics
                uint32_t addr = vm.regs[rs1];
                uint8_t* p = vm.writePtr(addr, 4);
                if (!p) {
                    vm.out->fmt("elf: W^X or bounds atomic fault at 0x%08x (PC=0x%08x)\n",
                                static_cast<unsigned>(addr), static_cast<unsigned>(vm.pc));
                    return false;
                }
                uint32_t funct5 = funct7 >> 2;
                uint32_t old = rd32(p);
                uint32_t rhs = vm.regs[rs2];
                if (funct5 == 0x02U) {  // LR.W
                    vm.lr_addr = addr;
                    vm.lr_valid = true;
                    if (rd) vm.regs[rd] = old;
                } else if (funct5 == 0x03U) {  // SC.W
                    if (vm.lr_valid && vm.lr_addr == addr) {
                        wr32(p, rhs);
                        if (rd) vm.regs[rd] = 0;
                    } else if (rd) {
                        vm.regs[rd] = 1;
                    }
                    vm.lr_valid = false;
                } else {
                    uint32_t nv = old;
                    if (funct5 == 0x01U) nv = rhs;
                    else if (funct5 == 0x00U) nv = old + rhs;
                    else if (funct5 == 0x04U) nv = old ^ rhs;
                    else if (funct5 == 0x0CU) nv = old & rhs;
                    else if (funct5 == 0x08U) nv = old | rhs;
                    wr32(p, nv);
                    if (rd) vm.regs[rd] = old;
                }
                break;
            }
            case 0x0FU:  // FENCE / FENCE.I
                break;
            case 0x73U: {  // SYSTEM (ECALL / EBREAK / CSR)
                if (funct3 == 0) {
                    if ((inst >> 20) == 0) {  // ECALL (Linux RISC-V syscall ABI)
                        uint32_t sysno = vm.regs[17];  // a7
                        if (sysno == 93U || sysno == 94U) {  // exit / exit_group
                            vm.exited = true;
                            vm.exit_code = static_cast<int>(vm.regs[10]);
                            return true;
                        } else if (sysno == 64U) {  // write(fd, buf, count)
                            uint32_t buf_va = vm.regs[11];
                            uint32_t count  = vm.regs[12];
                            const uint8_t* src = vm.readPtr(buf_va, count);
                            if (!src) return false;
                            vm.out->putRaw(reinterpret_cast<const char*>(src), count);
                            vm.regs[10] = count;
                        } else if (sysno == 214U) {  // brk
                            if (vm.regs[10] >= vm.heap_base && vm.regs[10] <= vm.heap_end) {
                                vm.heap_brk = vm.regs[10];
                            }
                            vm.regs[10] = vm.heap_brk;
                        } else {
                            vm.regs[10] = static_cast<uint32_t>(-38);  // -ENOSYS
                        }
                    } else {  // EBREAK
                        vm.exited = true;
                        return true;
                    }
                } else if (rd) {
                    vm.regs[rd] = step;  // Safe CSR cycle read
                }
                break;
            }
            default:
                vm.out->fmt("elf: unsupported opcode 0x%02x at PC=0x%08x\n",
                            static_cast<unsigned>(opcode), static_cast<unsigned>(vm.pc));
                return false;
        }
        vm.pc = next_pc;
    }

    if (!vm.exited) {
        vm.out->putStr("elf: execution quota exceeded (2,000,000 instructions)\n");
        return false;
    }
    return true;
}

bool loadRelocatableElf(const uint8_t* file_buf, size_t file_len, RvVm& vm, uint32_t* out_entry, BufWriter& out) {
    const auto* eh = reinterpret_cast<const Elf32Ehdr*>(file_buf);
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf32Shdr) ||
        eh->e_shnum == 0 || eh->e_shnum > 48 ||
        eh->e_shoff + static_cast<size_t>(eh->e_shnum) * sizeof(Elf32Shdr) > file_len) {
        out.putStr("elf: invalid section header table\n");
        return false;
    }

    const auto* shdrs = reinterpret_cast<const Elf32Shdr*>(file_buf + eh->e_shoff);
    uint32_t sec_vaddr[48] = {0};

    // 3-pass W^X section layout: [0] RX code, [1] R-only rodata, [2] RW data/bss
    uint32_t cursor = VM_BASE;
    uint32_t exec_end = VM_BASE;
    uint32_t ro_end = VM_BASE;

    for (int pass = 0; pass < 3; ++pass) {
        for (uint16_t i = 0; i < eh->e_shnum; ++i) {
            const Elf32Shdr& sh = shdrs[i];
            if ((sh.sh_flags & SHF_ALLOC) == 0) continue;
            bool is_x = (sh.sh_flags & SHF_EXECINSTR) != 0;
            bool is_w = (sh.sh_flags & SHF_WRITE) != 0;
            if (is_x && is_w) {
                out.putStr("elf: W^X violation (section is both writable and executable)\n");
                return false;
            }
            int sec_pass = is_x ? 0 : (!is_w ? 1 : 2);
            if (sec_pass != pass) continue;

            uint32_t align = (sh.sh_addralign > 4U) ? sh.sh_addralign : 4U;
            cursor = (cursor + align - 1U) & ~(align - 1U);
            sec_vaddr[i] = cursor;
            if (sh.sh_size > VM_MAX_SIZE || (cursor - VM_BASE) + sh.sh_size > VM_MAX_SIZE) {
                out.putStr("elf: sections exceed sandbox capacity\n");
                return false;
            }
            if (sh.sh_type == SHT_PROGBITS && sh.sh_size > 0 && sh.sh_offset + sh.sh_size > file_len) {
                out.putStr("elf: truncated PROGBITS section\n");
                return false;
            }
            cursor += sh.sh_size;
        }
        cursor = (cursor + 3U) & ~3U;
        if (pass == 0) exec_end = cursor;
        else if (pass == 1) ro_end = cursor;
    }

    uint32_t rw_end = cursor;
    uint32_t vm_size = 0;
    if (!computeDynamicVmSize(rw_end - VM_BASE, &vm_size)) {
        out.putStr("elf: sections exceed 48 KB sandbox capacity\n");
        return false;
    }

    vm.mem = static_cast<uint8_t*>(malloc(vm_size));
    if (!vm.mem) {
        out.fmt("elf: insufficient heap for %u KB ELF sandbox\n", static_cast<unsigned>(vm_size / 1024U));
        return false;
    }
    memset(vm.mem, 0, vm_size);
    vm.mem_size  = vm_size;
    vm.heap_base = (rw_end + 15U) & ~15U;
    vm.heap_brk  = vm.heap_base;
    vm.argv_base = VM_BASE + vm_size - VM_STACK_ARENA;
    vm.heap_end  = vm.argv_base;
    vm.sp_init   = VM_BASE + vm_size - 16U;

    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32Shdr& sh = shdrs[i];
        if ((sh.sh_flags & SHF_ALLOC) == 0 || sh.sh_size == 0) continue;
        uint8_t* dst = vm.rawPtr(sec_vaddr[i], sh.sh_size);
        if (!dst) return false;
        if (sh.sh_type == SHT_PROGBITS) {
            memcpy(dst, file_buf + sh.sh_offset, sh.sh_size);
        } else if (sh.sh_type == SHT_NOBITS) {
            memset(dst, 0, sh.sh_size);
        }
    }

    // Locate symbol table & string table
    int symtab_idx = -1;
    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        if (shdrs[i].sh_type == SHT_SYMTAB) {
            symtab_idx = i;
            break;
        }
    }
    if (symtab_idx < 0) {
        out.putStr("elf: missing symbol table in ET_REL object\n");
        return false;
    }

    const Elf32Shdr& sym_sh = shdrs[symtab_idx];
    if (sym_sh.sh_link >= eh->e_shnum || sym_sh.sh_entsize != sizeof(Elf32Sym)) return false;
    const Elf32Shdr& str_sh = shdrs[sym_sh.sh_link];
    if (sym_sh.sh_offset + sym_sh.sh_size > file_len ||
        str_sh.sh_offset + str_sh.sh_size > file_len) {
        return false;
    }

    const auto* syms = reinterpret_cast<const Elf32Sym*>(file_buf + sym_sh.sh_offset);
    size_t sym_count = sym_sh.sh_size / sizeof(Elf32Sym);
    const char* strtab = reinterpret_cast<const char*>(file_buf + str_sh.sh_offset);

    auto resolveSym = [&](uint32_t sym_i, uint32_t* out_val) -> bool {
        if (sym_i >= sym_count) return false;
        const Elf32Sym& sym = syms[sym_i];
        const char* name = (sym.st_name < str_sh.sh_size) ? (strtab + sym.st_name) : "";
        if (sym.st_shndx == 0) {  // SHN_UNDEF import
            uint32_t imp = lookupImportAddr(name);
            if (imp == 0) {
                out.fmt("elf: unresolved symbol '%s'\n", name);
                return false;
            }
            *out_val = imp;
            return true;
        }
        if (sym.st_shndx < eh->e_shnum) {
            *out_val = sec_vaddr[sym.st_shndx] + sym.st_value;
            return true;
        }
        *out_val = sym.st_value;
        return true;
    };

    // Apply relocations on raw loader memory prior to locking W^X permissions
    HeapArray<HiRelocRecord> hi_records(64);
    if (!hi_records) return false;
    size_t hi_count = 0;

    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        const Elf32Shdr& rsh = shdrs[i];
        if (rsh.sh_type != SHT_RELA || rsh.sh_info >= eh->e_shnum) continue;
        uint32_t target_base = sec_vaddr[rsh.sh_info];
        if (target_base == 0) continue;
        if (rsh.sh_offset + rsh.sh_size > file_len) return false;

        const auto* relas = reinterpret_cast<const Elf32Rela*>(file_buf + rsh.sh_offset);
        size_t rcount = rsh.sh_size / sizeof(Elf32Rela);

        for (size_t pass = 0; pass < 2; ++pass) {
            for (size_t r = 0; r < rcount; ++r) {
                uint32_t type = relas[r].r_info & 0xFFU;
                if (type == R_RISCV_NONE || type == R_RISCV_RELAX || type == R_RISCV_ALIGN) {
                    continue;
                }
                uint32_t sym_i = relas[r].r_info >> 8;
                bool is_lo_pcrel = (type == R_RISCV_PCREL_LO12_I || type == R_RISCV_PCREL_LO12_S);
                if ((pass == 0 && is_lo_pcrel) || (pass == 1 && !is_lo_pcrel)) continue;

                uint32_t P = target_base + relas[r].r_offset;
                uint32_t S = 0;
                if (!resolveSym(sym_i, &S)) return false;
                int32_t A = relas[r].r_addend;

                if (type == R_RISCV_32) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    wr32(p, S + static_cast<uint32_t>(A));
                } else if (type == R_RISCV_ADD32) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    wr32(p, rd32(p) + S + static_cast<uint32_t>(A));
                } else if (type == R_RISCV_SUB32) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    wr32(p, rd32(p) - (S + static_cast<uint32_t>(A)));
                } else if (type == R_RISCV_CALL || type == R_RISCV_CALL_PLT) {
                    uint8_t* p = vm.rawPtr(P, 8);
                    if (!p) return false;
                    int32_t off = static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P);
                    uint32_t hi20 = static_cast<uint32_t>((off + 0x800) >> 12);
                    uint32_t lo12 = static_cast<uint32_t>(off) & 0xFFFU;
                    wr32(p, encodeUType(rd32(p), hi20));
                    wr32(p + 4, encodeIType(rd32(p + 4), lo12));
                } else if (type == R_RISCV_PCREL_HI20) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    int32_t off = static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P);
                    uint32_t hi20 = static_cast<uint32_t>((off + 0x800) >> 12);
                    wr32(p, encodeUType(rd32(p), hi20));
                    if (hi_count < 64) {
                        hi_records[hi_count++] = {P, off};
                    }
                } else if (type == R_RISCV_PCREL_LO12_I || type == R_RISCV_PCREL_LO12_S) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    int32_t full_off = 0;
                    for (size_t h = 0; h < hi_count; ++h) {
                        if (hi_records[h].pc_addr == S) {
                            full_off = hi_records[h].full_off + A;
                            break;
                        }
                    }
                    uint32_t lo12 = static_cast<uint32_t>(full_off) & 0xFFFU;
                    if (type == R_RISCV_PCREL_LO12_I) wr32(p, encodeIType(rd32(p), lo12));
                    else wr32(p, encodeSType(rd32(p), lo12));
                } else if (type == R_RISCV_HI20) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    int32_t val = static_cast<int32_t>(S) + A;
                    uint32_t hi20 = static_cast<uint32_t>((val + 0x800) >> 12);
                    wr32(p, encodeUType(rd32(p), hi20));
                } else if (type == R_RISCV_LO12_I || type == R_RISCV_LO12_S) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    uint32_t lo12 = static_cast<uint32_t>(static_cast<int32_t>(S) + A) & 0xFFFU;
                    if (type == R_RISCV_LO12_I) wr32(p, encodeIType(rd32(p), lo12));
                    else wr32(p, encodeSType(rd32(p), lo12));
                } else if (type == R_RISCV_JAL) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    int32_t off = static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P);
                    wr32(p, encodeJType(rd32(p), off));
                } else if (type == R_RISCV_BRANCH) {
                    uint8_t* p = vm.rawPtr(P, 4);
                    if (!p) return false;
                    int32_t off = static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P);
                    wr32(p, encodeBType(rd32(p), off));
                } else if (type == R_RISCV_RVC_JUMP) {
                    uint8_t* p = vm.rawPtr(P, 2);
                    if (!p) return false;
                    uint32_t u = static_cast<uint32_t>(static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P));
                    uint16_t cj = static_cast<uint16_t>((rd16(p) & 0xE003U) |
                        (((u >> 11) & 1U) << 12) | (((u >> 4) & 1U) << 11) |
                        (((u >> 8) & 3U) << 9) | (((u >> 10) & 1U) << 8) |
                        (((u >> 6) & 1U) << 7) | (((u >> 7) & 1U) << 6) |
                        (((u >> 1) & 7U) << 3) | (((u >> 5) & 1U) << 2));
                    wr16(p, cj);
                } else if (type == R_RISCV_RVC_BRANCH) {
                    uint8_t* p = vm.rawPtr(P, 2);
                    if (!p) return false;
                    uint32_t u = static_cast<uint32_t>(static_cast<int32_t>((S + static_cast<uint32_t>(A)) - P));
                    uint16_t cb = static_cast<uint16_t>((rd16(p) & 0xE383U) |
                        (((u >> 8) & 1U) << 12) | (((u >> 3) & 3U) << 10) |
                        (((u >> 6) & 3U) << 5) | (((u >> 1) & 3U) << 3) |
                        (((u >> 5) & 1U) << 2));
                    wr16(p, cb);
                }
            }
        }
    }

    // Lock strict W^X permission regions: [RX code][R rodata][RW data+bss+heap+argv+stack]
    if (!vm.addRegion(VM_BASE, exec_end, PERM_R | PERM_X) ||
        !vm.addRegion(exec_end, ro_end, PERM_R) ||
        !vm.addRegion(ro_end, VM_BASE + vm_size, PERM_R | PERM_W)) {
        out.putStr("elf: failed configuring W^X regions\n");
        return false;
    }

    // Find entry symbol (`main`, `app_main`, or `_start`) inside executable region
    for (size_t i = 0; i < sym_count; ++i) {
        if (syms[i].st_name >= str_sh.sh_size || syms[i].st_shndx == 0) continue;
        const char* name = strtab + syms[i].st_name;
        if (strcmp(name, "main") == 0 || strcmp(name, "app_main") == 0 || strcmp(name, "_start") == 0) {
            uint32_t candidate = sec_vaddr[syms[i].st_shndx] + syms[i].st_value;
            if (!vm.execPtr(candidate, 2)) {
                out.fmt("elf: W^X violation (entry '%s' at 0x%08x is not executable)\n",
                        name, static_cast<unsigned>(candidate));
                return false;
            }
            *out_entry = candidate;
            return true;
        }
    }

    out.putStr("elf: entry symbol 'main' not found\n");
    return false;
}

bool loadExecElf(const uint8_t* file_buf, size_t file_len, RvVm& vm, uint32_t* out_entry, BufWriter& out) {
    const auto* eh = reinterpret_cast<const Elf32Ehdr*>(file_buf);
    if (eh->e_phoff == 0 || eh->e_phentsize != sizeof(Elf32Phdr) ||
        eh->e_phnum == 0 || eh->e_phnum > 16 ||
        eh->e_phoff + static_cast<size_t>(eh->e_phnum) * sizeof(Elf32Phdr) > file_len) {
        out.putStr("elf: invalid program header table\n");
        return false;
    }

    const auto* phdrs = reinterpret_cast<const Elf32Phdr*>(file_buf + eh->e_phoff);
    uint32_t max_end = VM_BASE;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const Elf32Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        if ((ph.p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) {
            out.putStr("elf: W^X violation (PT_LOAD segment is both writable and executable)\n");
            return false;
        }
        if (ph.p_vaddr < VM_BASE || ph.p_memsz > VM_MAX_SIZE ||
            (ph.p_vaddr - VM_BASE) + ph.p_memsz > VM_MAX_SIZE ||
            ph.p_offset + ph.p_filesz > file_len || ph.p_filesz > ph.p_memsz) {
            out.fmt("elf: PT_LOAD segment 0x%08x..0x%08x outside sandbox [0x%08x..0x%08x)\n",
                    static_cast<unsigned>(ph.p_vaddr),
                    static_cast<unsigned>(ph.p_vaddr + ph.p_memsz),
                    static_cast<unsigned>(VM_BASE),
                    static_cast<unsigned>(VM_BASE + VM_MAX_SIZE));
            return false;
        }
        uint32_t seg_end = ph.p_vaddr + ph.p_memsz;
        if (seg_end > max_end) max_end = seg_end;
    }

    if (max_end <= VM_BASE) {
        out.putStr("elf: no loadable PT_LOAD segments\n");
        return false;
    }

    uint32_t vm_size = 0;
    if (!computeDynamicVmSize(max_end - VM_BASE, &vm_size)) {
        out.putStr("elf: PT_LOAD segments exceed 48 KB sandbox capacity\n");
        return false;
    }

    vm.mem = static_cast<uint8_t*>(malloc(vm_size));
    if (!vm.mem) {
        out.fmt("elf: insufficient heap for %u KB ELF sandbox\n", static_cast<unsigned>(vm_size / 1024U));
        return false;
    }
    memset(vm.mem, 0, vm_size);
    vm.mem_size  = vm_size;
    vm.heap_base = (max_end + 15U) & ~15U;
    vm.heap_brk  = vm.heap_base;
    vm.argv_base = VM_BASE + vm_size - VM_STACK_ARENA;
    vm.heap_end  = vm.argv_base;
    vm.sp_init   = VM_BASE + vm_size - 16U;

    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        const Elf32Phdr& ph = phdrs[i];
        if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
        uint8_t* dst = vm.rawPtr(ph.p_vaddr, ph.p_memsz);
        if (!dst) return false;
        memset(dst, 0, ph.p_memsz);
        if (ph.p_filesz > 0) {
            memcpy(dst, file_buf + ph.p_offset, ph.p_filesz);
        }
        uint8_t perm = PERM_R;
        if ((ph.p_flags & PF_X) != 0) perm |= PERM_X;
        else if ((ph.p_flags & PF_W) != 0) perm |= PERM_W;
        if (!vm.addRegion(ph.p_vaddr, ph.p_vaddr + ph.p_memsz, perm)) {
            out.putStr("elf: W^X violation across overlapping PT_LOAD segments\n");
            return false;
        }
    }

    if (!vm.addRegion(max_end, VM_BASE + vm_size, PERM_R | PERM_W)) {
        out.putStr("elf: failed configuring RW heap/stack region\n");
        return false;
    }

    if (!vm.execPtr(eh->e_entry, 2)) {
        out.fmt("elf: W^X violation or invalid entry point 0x%08x\n", static_cast<unsigned>(eh->e_entry));
        return false;
    }
    *out_entry = eh->e_entry;
    return true;
}

}  // namespace

bool hasElfMagic(const uint8_t* data, size_t len) {
    return data && len >= 4 &&
           data[0] == 0x7FU && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
}

bool runElfFile(SystemState& state, VirtualFs& vfs,
                const char* elf_abs_path, int argc, const char* const* argv,
                BufWriter& out, int* out_exit_code,
                size_t* out_vm_bytes) {
    if (out_exit_code) *out_exit_code = -1;
    if (out_vm_bytes) *out_vm_bytes = 0;

    size_t file_len = 0;
    if (!vfs.fileSize(elf_abs_path, &file_len) || file_len < sizeof(Elf32Ehdr) || file_len > 65536U) {
        out.fmt("elf: cannot open valid RV32IMAC ELF '%s'\n", elf_abs_path ? elf_abs_path : "");
        return false;
    }

    auto* file_buf = static_cast<uint8_t*>(malloc(file_len));
    if (!file_buf) {
        out.putStr("elf: insufficient heap to read ELF header\n");
        return false;
    }

    size_t got = 0;
    if (!vfs.readBinary(elf_abs_path, 0, file_buf, file_len, &got) || got != file_len) {
        free(file_buf);
        out.putStr("elf: read error\n");
        return false;
    }

    const auto* eh = reinterpret_cast<const Elf32Ehdr*>(file_buf);
    if (!hasElfMagic(eh->e_ident, 4) || eh->e_ident[4] != 1 /* ELFCLASS32 */ ||
        eh->e_ident[5] != 1 /* ELFDATA2LSB */ || eh->e_machine != EM_RISCV) {
        free(file_buf);
        out.putStr("elf: expected 32-bit little-endian RISC-V (EM_RISCV=243) binary\n");
        return false;
    }

    RvVm vm = {};
    vm.state = &state;
    vm.vfs = &vfs;
    vm.out = &out;

    uint32_t entry_va = 0;
    bool loaded = false;
    if (eh->e_type == ET_REL) {
        loaded = loadRelocatableElf(file_buf, file_len, vm, &entry_va, out);
    } else if (eh->e_type == ET_EXEC) {
        loaded = loadExecElf(file_buf, file_len, vm, &entry_va, out);
    } else {
        out.fmt("elf: unsupported ELF type %u\n", eh->e_type);
    }
    // Release raw ELF buffer immediately so it never coexists with VM execution
    free(file_buf);

    if (!loaded) {
        free(vm.mem);
        return false;
    }

    if (out_vm_bytes) {
        *out_vm_bytes = vm.mem_size;
    }

    // Populate argc / argv inside vm.argv_base (RW non-executable arena)
    int safe_argc = (argc > 0 && argc <= 8) ? argc : 1;
    uint32_t ptr_table = vm.argv_base;
    uint32_t str_cursor = vm.argv_base + 40U;
    for (int i = 0; i < safe_argc; ++i) {
        const char* s = (argv && i < argc && argv[i]) ? argv[i] : elf_abs_path;
        size_t slen = strlen(s) + 1;
        if (str_cursor + slen >= vm.sp_init - 256U) {
            safe_argc = i;
            break;
        }
        uint8_t* sdst = vm.writePtr(str_cursor, slen);
        uint8_t* pdst = vm.writePtr(ptr_table + static_cast<uint32_t>(i) * 4U, 4);
        if (!sdst || !pdst) {
            free(vm.mem);
            return false;
        }
        memcpy(sdst, s, slen);
        wr32(pdst, str_cursor);
        str_cursor += static_cast<uint32_t>(slen);
    }

    vm.pc = entry_va;
    vm.regs[1]  = EXIT_TRAMP;  // ra -> clean return from main()
    vm.regs[2]  = vm.sp_init;  // sp
    vm.regs[10] = static_cast<uint32_t>(safe_argc);  // a0 = argc
    vm.regs[11] = ptr_table;                         // a1 = argv

    bool ok = executeVm(vm);
    if (ok && out_exit_code) {
        *out_exit_code = vm.exit_code;
    }
    free(vm.mem);
    return ok;
}

}  // namespace elf
}  // namespace c6os
