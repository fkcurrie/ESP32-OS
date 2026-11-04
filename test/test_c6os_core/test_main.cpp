#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "c6os_display.h"
#include "c6os_elf.h"
#include "c6os_epkg.h"
#include "c6os_lua.h"
#include "c6os_shell.h"
#include "c6os_syslog.h"
#include "c6os_types.h"
#include "c6os_vfs.h"
#include "ssh_auth.h"
#include "ssh_packet.h"
#include "ssh_sftp.h"

using namespace c6os;

void setUp(void) {
    setHardwareOps(nullptr);
}
void tearDown(void) {
    setHardwareOps(nullptr);
}

namespace {

// Real RV32IMAC ELF32 relocatable binary compiled with riscv32-esp-elf-gcc -march=rv32imac -mabi=ilp32 -Os
const uint8_t TEST_RV32_ELF[1696] = {
    0x7f, 0x45, 0x4c, 0x46, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0xf3, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x98, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00,
    0x0d, 0x00, 0x0c, 0x00, 0x52, 0x56, 0x33, 0x32, 0x49, 0x4d, 0x41, 0x43, 0x00, 0x00, 0x00, 0x00,
    0x45, 0x4c, 0x46, 0x20, 0x4e, 0x61, 0x74, 0x69, 0x76, 0x65, 0x20, 0x41, 0x70, 0x70, 0x00, 0x00,
    0x65, 0x6c, 0x66, 0x5f, 0x6f, 0x6b, 0x20, 0x61, 0x72, 0x67, 0x63, 0x3d, 0x25, 0x64, 0x20, 0x72,
    0x61, 0x6d, 0x3d, 0x25, 0x75, 0x0a, 0x00, 0x00, 0x41, 0x11, 0x22, 0xc4, 0x06, 0xc6, 0x85, 0x47,
    0x2a, 0x84, 0x63, 0xd4, 0xa7, 0x04, 0xcc, 0x41, 0x37, 0x05, 0x00, 0x00, 0x13, 0x05, 0x05, 0x00,
    0x97, 0x00, 0x00, 0x00, 0xe7, 0x80, 0x00, 0x00, 0x13, 0x05, 0x80, 0x05, 0x97, 0x00, 0x00, 0x00,
    0xe7, 0x80, 0x00, 0x00, 0x97, 0x00, 0x00, 0x00, 0xe7, 0x80, 0x00, 0x00, 0x2a, 0x86, 0x37, 0x05,
    0x00, 0x00, 0xa2, 0x85, 0x13, 0x05, 0x05, 0x00, 0x97, 0x00, 0x00, 0x00, 0xe7, 0x80, 0x00, 0x00,
    0xb2, 0x40, 0x22, 0x44, 0x01, 0x45, 0x41, 0x01, 0x82, 0x80, 0xb7, 0x05, 0x00, 0x00, 0x93, 0x85,
    0x05, 0x00, 0x5d, 0xbf, 0x00, 0x47, 0x43, 0x43, 0x3a, 0x20, 0x28, 0x63, 0x72, 0x6f, 0x73, 0x73,
    0x74, 0x6f, 0x6f, 0x6c, 0x2d, 0x4e, 0x47, 0x20, 0x65, 0x73, 0x70, 0x2d, 0x31, 0x34, 0x2e, 0x32,
    0x2e, 0x30, 0x5f, 0x32, 0x30, 0x32, 0x35, 0x31, 0x31, 0x30, 0x37, 0x29, 0x20, 0x31, 0x34, 0x2e,
    0x32, 0x2e, 0x30, 0x00, 0x41, 0x46, 0x00, 0x00, 0x00, 0x72, 0x69, 0x73, 0x63, 0x76, 0x00, 0x01,
    0x3c, 0x00, 0x00, 0x00, 0x04, 0x10, 0x05, 0x72, 0x76, 0x33, 0x32, 0x69, 0x32, 0x70, 0x31, 0x5f,
    0x6d, 0x32, 0x70, 0x30, 0x5f, 0x61, 0x32, 0x70, 0x31, 0x5f, 0x63, 0x32, 0x70, 0x30, 0x5f, 0x7a,
    0x6d, 0x6d, 0x75, 0x6c, 0x31, 0x70, 0x30, 0x5f, 0x7a, 0x61, 0x61, 0x6d, 0x6f, 0x31, 0x70, 0x30,
    0x5f, 0x7a, 0x61, 0x6c, 0x72, 0x73, 0x63, 0x31, 0x70, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0xf1, 0xff, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x05, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x08, 0x00, 0x3d, 0x00, 0x00, 0x00,
    0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x42, 0x00, 0x00, 0x00,
    0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x47, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x4c, 0x00, 0x00, 0x00,
    0x52, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x50, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x09, 0x00, 0x54, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00, 0x12, 0x00, 0x05, 0x00, 0x59, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x61, 0x70, 0x70,
    0x2e, 0x63, 0x00, 0x24, 0x78, 0x72, 0x76, 0x33, 0x32, 0x69, 0x32, 0x70, 0x31, 0x5f, 0x6d, 0x32,
    0x70, 0x30, 0x5f, 0x61, 0x32, 0x70, 0x31, 0x5f, 0x63, 0x32, 0x70, 0x30, 0x5f, 0x7a, 0x6d, 0x6d,
    0x75, 0x6c, 0x31, 0x70, 0x30, 0x5f, 0x7a, 0x61, 0x61, 0x6d, 0x6f, 0x31, 0x70, 0x30, 0x5f, 0x7a,
    0x61, 0x6c, 0x72, 0x73, 0x63, 0x31, 0x70, 0x30, 0x00, 0x2e, 0x4c, 0x43, 0x31, 0x00, 0x2e, 0x4c,
    0x43, 0x32, 0x00, 0x2e, 0x4c, 0x43, 0x30, 0x00, 0x2e, 0x4c, 0x33, 0x00, 0x2e, 0x4c, 0x32, 0x00,
    0x6d, 0x61, 0x69, 0x6e, 0x00, 0x63, 0x36, 0x5f, 0x6c, 0x63, 0x64, 0x5f, 0x74, 0x65, 0x78, 0x74,
    0x00, 0x63, 0x36, 0x5f, 0x62, 0x61, 0x63, 0x6b, 0x6c, 0x69, 0x67, 0x68, 0x74, 0x00, 0x63, 0x36,
    0x5f, 0x66, 0x72, 0x65, 0x65, 0x5f, 0x72, 0x61, 0x6d, 0x00, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x66,
    0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x10, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x1a, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x1b, 0x09, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x13, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x13, 0x12, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2c, 0x00, 0x00, 0x00, 0x13, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x1a, 0x0a, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3c, 0x00, 0x00, 0x00, 0x1b, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x13, 0x14, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x52, 0x00, 0x00, 0x00, 0x1a, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x52, 0x00, 0x00, 0x00,
    0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00, 0x1b, 0x0b, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5a, 0x00, 0x00, 0x00, 0x2d, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2e, 0x73, 0x79,
    0x6d, 0x74, 0x61, 0x62, 0x00, 0x2e, 0x73, 0x74, 0x72, 0x74, 0x61, 0x62, 0x00, 0x2e, 0x73, 0x68,
    0x73, 0x74, 0x72, 0x74, 0x61, 0x62, 0x00, 0x2e, 0x74, 0x65, 0x78, 0x74, 0x00, 0x2e, 0x64, 0x61,
    0x74, 0x61, 0x00, 0x2e, 0x62, 0x73, 0x73, 0x00, 0x2e, 0x72, 0x6f, 0x64, 0x61, 0x74, 0x61, 0x2e,
    0x73, 0x74, 0x72, 0x31, 0x2e, 0x34, 0x00, 0x2e, 0x72, 0x65, 0x6c, 0x61, 0x2e, 0x74, 0x65, 0x78,
    0x74, 0x2e, 0x73, 0x74, 0x61, 0x72, 0x74, 0x75, 0x70, 0x00, 0x2e, 0x63, 0x6f, 0x6d, 0x6d, 0x65,
    0x6e, 0x74, 0x00, 0x2e, 0x6e, 0x6f, 0x74, 0x65, 0x2e, 0x47, 0x4e, 0x55, 0x2d, 0x73, 0x74, 0x61,
    0x63, 0x6b, 0x00, 0x2e, 0x72, 0x69, 0x73, 0x63, 0x76, 0x2e, 0x61, 0x74, 0x74, 0x72, 0x69, 0x62,
    0x75, 0x74, 0x65, 0x73, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x27, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x33, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x68, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3b, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x03, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00,
    0x0a, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x4e, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc4, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x57, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x67, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf4, 0x00, 0x00, 0x00, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x01, 0x00, 0x00, 0x50, 0x01, 0x00, 0x00,
    0x0b, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x8c, 0x02, 0x00, 0x00, 0x85, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x04, 0x00, 0x00, 0x79, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// RFC 4252 + RFC 8332 1024-bit RSA-SHA256 userauth publickey vector
const uint8_t TEST_RSA_PK_BLOB[151] = {
    0x00, 0x00, 0x00, 0x07, 0x73, 0x73, 0x68, 0x2d, 0x72, 0x73, 0x61, 0x00, 0x00, 0x00, 0x03, 0x01,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x81, 0x00, 0x95, 0xb0, 0xe2, 0x8f, 0xa6, 0xa1, 0xc2, 0x54, 0x27,
    0x88, 0xac, 0x8a, 0x13, 0x44, 0xfd, 0x08, 0x18, 0x5d, 0x74, 0x01, 0x0b, 0xc3, 0x72, 0x7d, 0xf1,
    0xc9, 0x88, 0xe3, 0x3a, 0x54, 0x95, 0xb5, 0x2f, 0xc2, 0xfa, 0x38, 0xfd, 0xbd, 0x24, 0x1e, 0x6c,
    0x73, 0x57, 0x41, 0xc8, 0xc6, 0x3b, 0x15, 0x11, 0xee, 0xc5, 0x9d, 0x3a, 0xa7, 0x4b, 0xc5, 0xbd,
    0xbb, 0x1a, 0x4c, 0xbf, 0xdd, 0x14, 0x25, 0x35, 0x47, 0xb8, 0x61, 0xda, 0xcb, 0x81, 0x8e, 0xd8,
    0xa3, 0x3e, 0x78, 0x6e, 0x11, 0x51, 0xa5, 0xe1, 0x3f, 0x92, 0x25, 0xe5, 0x96, 0xf5, 0x9b, 0x5f,
    0xcc, 0x6a, 0x50, 0xed, 0x19, 0xa8, 0x1a, 0xb0, 0xef, 0xbb, 0xe9, 0xb3, 0xca, 0xcc, 0x0b, 0xc0,
    0xe1, 0xb8, 0x8c, 0x97, 0xcb, 0xec, 0x59, 0x10, 0xd1, 0x85, 0x30, 0x4d, 0x3c, 0xec, 0x51, 0xda,
    0x2e, 0xe0, 0x54, 0x8b, 0x4d, 0x4f, 0xeb,
};

const uint8_t TEST_RSA_SIG_BLOB[148] = {
    0x00, 0x00, 0x00, 0x0c, 0x72, 0x73, 0x61, 0x2d, 0x73, 0x68, 0x61, 0x32, 0x2d, 0x32, 0x35, 0x36,
    0x00, 0x00, 0x00, 0x80, 0x22, 0x3c, 0xc5, 0x0b, 0x54, 0xb9, 0xcd, 0xb6, 0xbf, 0xdb, 0x6c, 0xe3,
    0xe9, 0x0e, 0xef, 0xb7, 0xf0, 0xb0, 0x8a, 0xfd, 0x21, 0x80, 0x78, 0x73, 0x15, 0x53, 0x03, 0xcc,
    0x1c, 0x2c, 0x58, 0xed, 0xf3, 0x96, 0xe5, 0x57, 0x25, 0x08, 0x1e, 0x62, 0xe2, 0xb3, 0x0f, 0x29,
    0x61, 0xb0, 0x3b, 0xd8, 0xde, 0x55, 0x44, 0xcf, 0x9d, 0x40, 0xbc, 0xa3, 0x1d, 0xa0, 0xb3, 0xbf,
    0x94, 0x3a, 0x73, 0xc6, 0x77, 0xc5, 0x23, 0x75, 0xfc, 0x5b, 0x1e, 0x0a, 0x4a, 0x0e, 0x31, 0xfe,
    0x4b, 0x98, 0x6b, 0x5e, 0x2b, 0x1f, 0xe7, 0x87, 0xd1, 0x31, 0x96, 0x21, 0x44, 0xee, 0x01, 0x47,
    0x9d, 0xf6, 0x62, 0xef, 0xaa, 0xb5, 0x92, 0x48, 0x3a, 0x7a, 0xf2, 0xa5, 0xa6, 0xb7, 0x0f, 0x09,
    0xeb, 0x5b, 0x59, 0xc4, 0xb9, 0x1a, 0x39, 0x2b, 0x52, 0x09, 0x1f, 0x1e, 0x1f, 0x7a, 0x8f, 0xa5,
    0x5c, 0x16, 0xef, 0x51,
};

struct SftpCapture {
    uint8_t buf[2048];
    size_t len;
};

int captureSftpSend(void* ctx, const uint8_t* data, size_t len) {
    SftpCapture* cap = static_cast<SftpCapture*>(ctx);
    if (cap->len + len <= sizeof(cap->buf)) {
        memcpy(cap->buf + cap->len, data, len);
        cap->len += len;
        return 0;
    }
    return -1;
}

void sendSftpPkt(ssh::SftpSession& sftp, SftpCapture& cap, const uint8_t* body, size_t body_len) {
    cap.len = 0;
    uint8_t hdr[4] = {
        static_cast<uint8_t>((body_len >> 24) & 0xFF),
        static_cast<uint8_t>((body_len >> 16) & 0xFF),
        static_cast<uint8_t>((body_len >> 8) & 0xFF),
        static_cast<uint8_t>(body_len & 0xFF),
    };
    TEST_ASSERT_TRUE(sftp.feed(hdr, 4));
    TEST_ASSERT_TRUE(sftp.feed(body, body_len));
}

}  // namespace

void test_vfs_proc_and_sysfs_hooks(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    char buf[512];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/proc/version", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "6.12.0-c6os-riscv32"));

    TEST_ASSERT_TRUE(vfs.readFile(state, "/proc/cpuinfo", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "rv32imac"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "ESP32-C6FH4"));

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/sys/class/backlight/st7789/brightness", "65\n"));
    TEST_ASSERT_EQUAL_UINT8(65, state.backlight_pct);

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/sys/class/graphics/fb0/mode", "signboard"));
    TEST_ASSERT_EQUAL(ScreenMode::SIGNBOARD, state.screen_mode);

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/dev/tty0", "Hello LCD Console", true));
    TEST_ASSERT_TRUE(vfs.readFile(state, "/dev/tty0", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Hello LCD Console"));
}

void test_vfs_path_normalization_and_user_files(void) {
    char norm[64];
    VirtualFs::normalizePath("/root", "../etc/hostname", norm, sizeof(norm));
    TEST_ASSERT_EQUAL_STRING("/etc/hostname", norm);

    VirtualFs::normalizePath("/etc/ssh", "../../proc/./meminfo", norm, sizeof(norm));
    TEST_ASSERT_EQUAL_STRING("/proc/meminfo", norm);

    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/root/notes.txt", "Test note\n", false));
    char buf[256];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/root/notes.txt", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("Test note\n", buf);
}

void test_ssh_mpint_and_packet_framing(void) {
    uint8_t buf[128];
    ssh::SshWriter w(buf, sizeof(buf));

    const uint8_t secret_msb[4] = {0x85, 0x11, 0x22, 0x33};
    TEST_ASSERT_TRUE(w.putMpint(secret_msb, 4));
    TEST_ASSERT_EQUAL_UINT32(9, w.length());
    ssh::SshReader r(buf, w.length());
    const uint8_t* str_ptr = nullptr;
    size_t str_len = 0;
    TEST_ASSERT_TRUE(r.getString(str_ptr, str_len));
    TEST_ASSERT_EQUAL_UINT32(5, str_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, str_ptr[0]);
    TEST_ASSERT_EQUAL_UINT8(0x85, str_ptr[1]);

    const uint8_t payload[5] = {ssh::SSH_MSG_NEWKEYS, 1, 2, 3, 4};
    uint8_t framed[64];
    size_t total = ssh::framePacket(payload, sizeof(payload), 16, nullptr, framed, sizeof(framed));
    TEST_ASSERT_TRUE(total > 0);
    TEST_ASSERT_EQUAL_UINT32(0, total % 16);
}

void test_interactive_menu_and_shell_commands(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, /*start_in_menu=*/true, out, sizeof(out));
    TEST_ASSERT_EQUAL(SessionMode::MENU_MAIN, session.mode());
    TEST_ASSERT_NOT_NULL(strstr(out, "Interactive Screen Controller"));

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "1", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::FB_CONSOLE, state.screen_mode);

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "3", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(ThemeId::MATRIX_GREEN, state.theme);

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "-", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(90, state.backlight_pct);

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "2", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_PROMPT_HEAD, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "SSH Controlled\n", 15, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_PROMPT_SUB, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "Custom Subtitle\n", 16, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::MENU_MAIN, session.mode());
    TEST_ASSERT_EQUAL_STRING("SSH Controlled", state.headline);
    TEST_ASSERT_EQUAL_STRING("Custom Subtitle", state.subtitle);

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "s", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "uname -a && echo 45 > /sys/class/backlight/st7789/brightness && screen flip on",
        out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "6.12.0-c6os-riscv32"));
    TEST_ASSERT_EQUAL_UINT8(45, state.backlight_pct);
    TEST_ASSERT_TRUE(state.landscape_flipped);

    char big_out[4096];
    big_out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "top -b -n 1", big_out, sizeof(big_out)));
    TEST_ASSERT_NOT_NULL(strstr(big_out, "Tasks:"));
    TEST_ASSERT_NOT_NULL(strstr(big_out, "/usr/sbin/sshd"));

    big_out[0] = '\0';
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "htop\n", 5, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::HTOP_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.isInteractiveMonitor());
    TEST_ASSERT_NOT_NULL(strstr(big_out, "F10"));

    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "M", 1, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::HTOP_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "q", 1, big_out, sizeof(big_out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());
    TEST_ASSERT_FALSE(session.isInteractiveMonitor());
}

void test_new_ten_os_features(void) {
    SystemState state;
    state.initDefaults();
    state.cpu_temp_deci_c = 425;
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[4096];
    session.begin(state, vfs, /*start_in_menu=*/false, out, sizeof(out));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen mode monitor", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::MONITOR, state.screen_mode);

    out[0] = '\0';
    TEST_ASSERT_TRUE(vfs.readFile(state, "/sys/class/thermal/thermal_zone0/temp", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("42500\n", out);
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "sensors", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "+42.5"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "crontab -a 30 screen text Cron OK && crontab -l", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(1, state.activeCronCount());
    TEST_ASSERT_NOT_NULL(strstr(out, "every   30s"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "watch -b -n 2 sensors", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "Every 2.0s:"));
    TEST_ASSERT_NOT_NULL(strstr(out, "sensors"));
    TEST_ASSERT_NOT_NULL(strstr(out, "+42.5"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "fbi tux Linux Mascot", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ScreenMode::IMAGE_VIEW, state.screen_mode);
    TEST_ASSERT_EQUAL_UINT8(1, state.fbi_pattern);
    TEST_ASSERT_EQUAL_STRING("Linux Mascot", state.fbi_caption);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "fbgrab /root/snap.ppm", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "captured /dev/fb0"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "screen ticker Welcome to C6-MicroLinux && screen alert High Load", out, sizeof(out)));
    TEST_ASSERT_TRUE(state.ticker_enabled);
    TEST_ASSERT_EQUAL_STRING("Welcome to C6-MicroLinux", state.ticker_text);
    TEST_ASSERT_TRUE(state.alert_active);
    TEST_ASSERT_EQUAL_STRING("High Load", state.alert_text);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "pinout", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "Waveshare ESP32-C6-LCD-1.47"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio write 14 1", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "reserved for ST7789 SPI2"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio pwm 2 128", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(128, state.gpio_cmd_val);

    state.wifi_sta_connected = true;
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "date && ping -c 2 192.168.18.1 && netstat", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "0% packet loss"));
    TEST_ASSERT_NOT_NULL(strstr(out, "0.0.0.0:80"));
    TEST_ASSERT_NOT_NULL(strstr(out, "0.0.0.0:514"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "edit /root/hello.sh\n", 20, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::EDIT_VIEW, session.mode());
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, ":clear\n", 7, out, sizeof(out)));
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, "screen text Scripted | Run via sh\n", 34, out, sizeof(out)));
    TEST_ASSERT_TRUE(session.consumeBytes(state, vfs, ":wq\n", 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL(SessionMode::SHELL_CLI, session.mode());

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "sh /root/hello.sh", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Scripted", state.headline);
    TEST_ASSERT_EQUAL_STRING("Run via sh", state.subtitle);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "logger Deployment verified", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT32(1, state.syslog_packets);

    // 11. O(1) Circular Ring Buffer wrap-around verification for dmesg & fb_lines
    state.clearFbConsole();
    for (int i = 0; i < 30; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "fb-%02d", i);
        state.appendFbConsoleLine(line);
    }
    TEST_ASSERT_EQUAL_UINT32(FB_CONSOLE_ROWS, state.fb_line_count);
    TEST_ASSERT_EQUAL_STRING("fb-12", state.getFbLine(0));
    TEST_ASSERT_EQUAL_STRING("fb-29", state.getFbLine(FB_CONSOLE_ROWS - 1));

    for (int i = 0; i < 40; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "klog-%02d", i);
        state.appendDmesg(static_cast<uint32_t>(i * 1000), line);
    }
    TEST_ASSERT_EQUAL_UINT32(DMESG_MAX_LINES, state.dmesg_count);
    TEST_ASSERT_NOT_NULL(strstr(state.getDmesgLine(0), "klog-08"));
    TEST_ASSERT_NOT_NULL(strstr(state.getDmesgLine(DMESG_MAX_LINES - 1), "klog-39"));
}

void test_ssh_ed25519_rsa_and_salted_password_auth(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    // 1. Default salted password ("esp32c6") passes; backdoor strings ("c6linux", "root") fail
    TEST_ASSERT_TRUE(crypto::verifyPassword("esp32c6", state.pass_salt, state.pass_hash));
    TEST_ASSERT_FALSE(crypto::verifyPassword("c6linux", state.pass_salt, state.pass_hash));
    TEST_ASSERT_FALSE(crypto::verifyPassword("root", state.pass_salt, state.pass_hash));

    // Update password via shell `passwd` and verify re-salting
    ShellSession session;
    char out[1024];
    session.begin(state, vfs, false, out, sizeof(out));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "passwd Strong#Pass99", out, sizeof(out)));
    TEST_ASSERT_FALSE(crypto::verifyPassword("esp32c6", state.pass_salt, state.pass_hash));
    TEST_ASSERT_TRUE(crypto::verifyPassword("Strong#Pass99", state.pass_salt, state.pass_hash));

    // 2. Ed25519 userauth signature & authorized_keys matching
    uint8_t seed[32];
    for (size_t i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(0x41 + i);
    uint8_t pk[32], sk[64];
    crypto::ed25519KeypairFromSeed(pk, sk, seed);

    uint8_t pk_blob[64];
    ssh::SshWriter pkw(pk_blob, sizeof(pk_blob));
    pkw.putCString("ssh-ed25519");
    pkw.putString(pk, 32);

    // Not authorized before adding to /etc/ssh/authorized_keys
    char auth_keys[1536];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/etc/ssh/authorized_keys", auth_keys, sizeof(auth_keys)));
    TEST_ASSERT_FALSE(crypto::isPubkeyAuthorized(auth_keys, pk_blob, pkw.length()));

    // Append OpenSSH ssh-ed25519 line to /etc/ssh/authorized_keys
    char b64_pk[128];
    crypto::base64Encode(pk_blob, pkw.length(), b64_pk, sizeof(b64_pk), false);
    char new_line[192];
    snprintf(new_line, sizeof(new_line), "ssh-ed25519 %s root@test-workstation\n", b64_pk);
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/etc/ssh/authorized_keys", new_line, /*append=*/true));
    TEST_ASSERT_TRUE(vfs.readFile(state, "/etc/ssh/authorized_keys", auth_keys, sizeof(auth_keys)));
    TEST_ASSERT_TRUE(crypto::isPubkeyAuthorized(auth_keys, pk_blob, pkw.length()));

    uint8_t session_id[32];
    for (uint8_t i = 0; i < 32; ++i) session_id[i] = static_cast<uint8_t>(i + 1);

    uint8_t signed_msg[512];
    size_t signed_len = crypto::buildUserauthSignedData(
        session_id, "root", "ssh-connection", "ssh-ed25519",
        pk_blob, pkw.length(), signed_msg, sizeof(signed_msg));
    TEST_ASSERT_TRUE(signed_len > 0);

    uint8_t raw_sig[64];
    crypto::ed25519Sign(raw_sig, signed_msg, signed_len, sk);

    uint8_t sig_blob[128];
    ssh::SshWriter sigw(sig_blob, sizeof(sig_blob));
    sigw.putCString("ssh-ed25519");
    sigw.putString(raw_sig, 64);

    TEST_ASSERT_TRUE(crypto::verifyUserauthSignature(
        session_id, "root", "ssh-connection", "ssh-ed25519",
        pk_blob, pkw.length(), sig_blob, sigw.length()));

    // Flip 1 bit in Ed25519 signature -> must reject
    sig_blob[sigw.length() - 1] ^= 0x01;
    TEST_ASSERT_FALSE(crypto::verifyUserauthSignature(
        session_id, "root", "ssh-connection", "ssh-ed25519",
        pk_blob, pkw.length(), sig_blob, sigw.length()));

    // 3. RSA-SHA256 PKCS#1 v1.5 userauth verification & tamper rejection
    TEST_ASSERT_TRUE(crypto::verifyUserauthSignature(
        session_id, "root", "ssh-connection", "rsa-sha2-256",
        TEST_RSA_PK_BLOB, sizeof(TEST_RSA_PK_BLOB),
        TEST_RSA_SIG_BLOB, sizeof(TEST_RSA_SIG_BLOB)));

    uint8_t tampered_rsa[sizeof(TEST_RSA_SIG_BLOB)];
    memcpy(tampered_rsa, TEST_RSA_SIG_BLOB, sizeof(TEST_RSA_SIG_BLOB));
    tampered_rsa[sizeof(tampered_rsa) - 3] ^= 0x04;
    TEST_ASSERT_FALSE(crypto::verifyUserauthSignature(
        session_id, "root", "ssh-connection", "rsa-sha2-256",
        TEST_RSA_PK_BLOB, sizeof(TEST_RSA_PK_BLOB),
        tampered_rsa, sizeof(tampered_rsa)));
}

void test_sftp_v3_binary_upload_stat_readdir_and_download(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    SftpCapture cap;
    memset(&cap, 0, sizeof(cap));
    ssh::SftpSession sftp;
    TEST_ASSERT_TRUE(sftp.init(&state, &vfs, captureSftpSend, &cap));

    // 1. SSH_FXP_INIT (type=1, version=3) -> SSH_FXP_VERSION (type=2, version=3)
    const uint8_t init_body[5] = {1, 0, 0, 0, 3};
    sendSftpPkt(sftp, cap, init_body, sizeof(init_body));
    TEST_ASSERT_EQUAL_UINT32(9, cap.len);
    TEST_ASSERT_EQUAL_UINT8(2, cap.buf[4]);
    TEST_ASSERT_EQUAL_UINT8(3, cap.buf[8]);

    // 2. SSH_FXP_OPEN("/apps/bin_test.dat", WRITE|CREAT|TRUNC = 0x1A)
    uint8_t pkt[256];
    ssh::SshWriter ow(pkt, sizeof(pkt));
    ow.putByte(3);  // SSH_FXP_OPEN
    ow.putUint32(101);
    ow.putCString("/apps/bin_test.dat");
    ow.putUint32(0x0000001AU);
    ow.putUint32(0);  // attrs flags = 0
    sendSftpPkt(sftp, cap, pkt, ow.length());
    TEST_ASSERT_TRUE(cap.len > 9);
    TEST_ASSERT_EQUAL_UINT8(102, cap.buf[4]);  // SSH_FXP_HANDLE
    ssh::SshReader hr(cap.buf + 5, cap.len - 5);
    uint32_t rid = 0;
    const uint8_t* hptr = nullptr;
    size_t hlen = 0;
    TEST_ASSERT_TRUE(hr.getUint32(rid));
    TEST_ASSERT_TRUE(hr.getString(hptr, hlen));
    char handle[16] = {0};
    memcpy(handle, hptr, hlen);

    // 3. SSH_FXP_WRITE binary payload containing embedded NULs
    const uint8_t sample_data[12] = {0x00, 0x7F, 0x45, 0x4C, 0x46, 0xFF, 0x10, 0x20, 0x00, 0xAA, 0x55, 0xEF};
    ssh::SshWriter ww(pkt, sizeof(pkt));
    ww.putByte(6);  // SSH_FXP_WRITE
    ww.putUint32(102);
    ww.putString(reinterpret_cast<const uint8_t*>(handle), hlen);
    ww.putUint32(0);  // offset hi
    ww.putUint32(0);  // offset lo
    ww.putString(sample_data, sizeof(sample_data));
    sendSftpPkt(sftp, cap, pkt, ww.length());
    TEST_ASSERT_EQUAL_UINT8(101, cap.buf[4]);  // SSH_FXP_STATUS
    TEST_ASSERT_EQUAL_UINT8(0, cap.buf[12]);   // SSH_FX_OK

    // 4. SSH_FXP_CLOSE
    ssh::SshWriter cw(pkt, sizeof(pkt));
    cw.putByte(4);
    cw.putUint32(103);
    cw.putString(reinterpret_cast<const uint8_t*>(handle), hlen);
    sendSftpPkt(sftp, cap, pkt, cw.length());
    TEST_ASSERT_EQUAL_UINT8(101, cap.buf[4]);

    // 5. Verify stat & binary readback via VirtualFs
    size_t fsz = 0;
    TEST_ASSERT_TRUE(vfs.fileSize("/apps/bin_test.dat", &fsz));
    TEST_ASSERT_EQUAL_UINT32(sizeof(sample_data), fsz);
    uint8_t readback[16] = {0};
    size_t nread = 0;
    TEST_ASSERT_TRUE(vfs.readBinary("/apps/bin_test.dat", 0, readback, sizeof(readback), &nread));
    TEST_ASSERT_EQUAL_UINT32(sizeof(sample_data), nread);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sample_data, readback, sizeof(sample_data));
}

void test_epkg_install_sha256_tamper_reject_and_lua_runtime(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    const char* lua_src =
        "c6.text('Lua 5.4 Live', arg[1] or 'DefaultSub')\n"
        "c6.backlight(73)\n"
        "print('lua_ram=' .. tostring(sys.total_ram()))\n";

    uint8_t pkg_blob[1024];
    size_t pkg_len = epkg::buildPackageBlob(
        "demo-lua", "1.0.2", "rv32imac", "Lua LCD controller",
        "/apps/demo.lua", 0755,
        reinterpret_cast<const uint8_t*>(lua_src), strlen(lua_src),
        pkg_blob, sizeof(pkg_blob));
    TEST_ASSERT_TRUE(pkg_len > 178);

    // 1. Tampered payload byte must fail SHA-256 verification
    uint8_t bad_blob[1024];
    memcpy(bad_blob, pkg_blob, pkg_len);
    bad_blob[pkg_len - 2] ^= 0x80;
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/root/bad.epkg", bad_blob, pkg_len, false));

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, false, out, sizeof(out));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "epkg install /root/bad.epkg", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "SHA-256"));

    // 2. Valid package installs cleanly and executes via direct /apps resolution
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/root/good.epkg", pkg_blob, pkg_len, false));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "epkg install /root/good.epkg && epkg list", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "demo-lua"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "demo CustomArg", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Lua 5.4 Live", state.headline);
    TEST_ASSERT_EQUAL_STRING("CustomArg", state.subtitle);
    TEST_ASSERT_EQUAL_UINT8(73, state.backlight_pct);
    TEST_ASSERT_NOT_NULL(strstr(out, "lua_ram=327680"));

    // 3. Runaway Lua loop hits instruction quota hook instead of hanging
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "lua -e \"while true do end\"", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "quota"));

    // 4. Remove package cleanly
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "epkg remove demo-lua", out, sizeof(out)));
    size_t dummy_sz = 0;
    TEST_ASSERT_FALSE(vfs.fileSize("/apps/demo.lua", &dummy_sz));
}

void test_rv32imac_elf_sandbox_execution(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/native_demo.elf", TEST_RV32_ELF, sizeof(TEST_RV32_ELF), false));

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, false, out, sizeof(out));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "native_demo FastDMA", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ELF Native App", state.headline);
    TEST_ASSERT_EQUAL_STRING("FastDMA", state.subtitle);
    TEST_ASSERT_EQUAL_UINT8(88, state.backlight_pct);
    TEST_ASSERT_NOT_NULL(strstr(out, "elf_ok argc=2 ram=256000"));
}

void test_scanline_fbgrab_bmp_ppm_tz_chsh_apitoken_and_reserved_pins(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, false, out, sizeof(out));

    // 1. Pixel-accurate 320x172 BMP and PPM screenshots
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "fbgrab /root/shot.bmp && fbgrab /root/shot.ppm", out, sizeof(out)));

    size_t bmp_sz = 0, ppm_sz = 0;
    TEST_ASSERT_TRUE(vfs.fileSize("/root/shot.bmp", &bmp_sz));
    TEST_ASSERT_EQUAL_UINT32(54U + 320U * 172U * 3U, bmp_sz);
    uint8_t bmp_hdr[26] = {0};
    size_t nread = 0;
    TEST_ASSERT_TRUE(vfs.readBinary("/root/shot.bmp", 0, bmp_hdr, sizeof(bmp_hdr), &nread));
    TEST_ASSERT_EQUAL_UINT8('B', bmp_hdr[0]);
    TEST_ASSERT_EQUAL_UINT8('M', bmp_hdr[1]);

    TEST_ASSERT_TRUE(vfs.fileSize("/root/shot.ppm", &ppm_sz));
    TEST_ASSERT_EQUAL_UINT32(15U + 320U * 172U * 3U, ppm_sz);
    uint8_t ppm_hdr[16] = {0};
    TEST_ASSERT_TRUE(vfs.readBinary("/root/shot.ppm", 0, ppm_hdr, 15, &nread));
    TEST_ASSERT_EQUAL_MEMORY("P6\n320 172\n255\n", ppm_hdr, 15);

    // 2. Timezone (/etc/timezone), login shell (/etc/passwd), and bearer token rotation
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "tz UTC && chsh /bin/c6-sh && apitoken rotate", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("UTC", state.tz_name);
    TEST_ASSERT_EQUAL_STRING("/bin/c6-sh", state.login_shell);
    TEST_ASSERT_EQUAL_UINT32(32, strlen(state.api_token));
    TEST_ASSERT_TRUE(strcmp(state.api_token, "00000000000000000000000000000000") != 0);

    char login_shell_entry[128];
    TEST_ASSERT_TRUE(vfs.readFile(state, "/etc/passwd", login_shell_entry, sizeof(login_shell_entry)));
    TEST_ASSERT_NOT_NULL(strstr(login_shell_entry, "/bin/c6-sh"));

    // 3. Reserved board pins (SD_CS=4, WS2812=8, BOOT=9) are protected from write, while GPIO 23 is writable
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio write 4 1", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "reserved for ST7789 SPI2"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio write 8 1", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "reserved for ST7789 SPI2"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "gpio write 23 1 && gpio read 23", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "OK GPIO 23 WRITE 1"));
    TEST_ASSERT_NOT_NULL(strstr(out, "GPIO 23 = 1 (HIGH)"));

    // 4. Shell script recursion depth cap prevents stack overflow
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/root/loop.sh", "sh /root/loop.sh\n", false));
    out[0] = '\0';
    session.executeCommandLine(state, vfs, "sh /root/loop.sh", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "max execution depth"));
}

void test_vfs_usage_dirty_selective_snapshot_and_8row_strip_parity(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    // 1. VFS usage_dirty_ lifecycle avoids redundant flash metadata scans on read-only ticks
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_FALSE(vfs.consumeUsageDirty());

    char buf[128] = {0};
    TEST_ASSERT_TRUE(vfs.readFile(state, "/etc/hostname", buf, sizeof(buf)));
    size_t sz = 0;
    TEST_ASSERT_TRUE(vfs.fileSize("/etc/rc.local", &sz));
    TEST_ASSERT_FALSE(vfs.consumeUsageDirty());

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/tmp/dirty_check.txt", "hello\n", false));
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_FALSE(vfs.consumeUsageDirty());

    TEST_ASSERT_TRUE(vfs.mkdirPath("/tmp/subd"));
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_TRUE(vfs.renamePath("/tmp/dirty_check.txt", "/tmp/subd/moved.txt"));
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_TRUE(vfs.removeFile(state, "/tmp/subd/moved.txt"));
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_TRUE(vfs.rmdirPath("/tmp/subd"));
    TEST_ASSERT_TRUE(vfs.consumeUsageDirty());
    TEST_ASSERT_FALSE(vfs.consumeUsageDirty());

    // 2. Mode-selective RenderSnapshot::captureFrom preserves active mode & overlay fields
    state.appendDmesg(1234, "kernel: selective snapshot check");
    strncpy(state.ssh_hostkey_fp, "SHA256:TestKeyFingerprint12345", sizeof(state.ssh_hostkey_fp) - 1);
    state.ticker_enabled = true;
    strncpy(state.ticker_text, "Rolling telemetry ticker", sizeof(state.ticker_text) - 1);
    state.custom_bar_visible = true;
    state.custom_bar_pct = 64;
    strncpy(state.custom_bar_label, "OTA Sync", sizeof(state.custom_bar_label) - 1);

    RenderSnapshot snap = {};
    state.screen_mode = ScreenMode::OS_DASHBOARD;
    snap.captureFrom(state, DIRTY_FULL);
    TEST_ASSERT_EQUAL_STRING(state.headline, snap.headline);
    TEST_ASSERT_EQUAL_STRING(state.ticker_text, snap.ticker_text);
    TEST_ASSERT_EQUAL_STRING("OTA Sync", snap.custom_bar_label);

    state.screen_mode = ScreenMode::DIAGNOSTICS;
    snap.captureFrom(state, DIRTY_FULL);
    TEST_ASSERT_EQUAL_STRING("SHA256:TestKeyFingerprint12345", snap.ssh_hostkey_fp);

    state.screen_mode = ScreenMode::FB_CONSOLE;
    snap.captureFrom(state, DIRTY_FULL);
    TEST_ASSERT_TRUE(snap.fb_line_count > 0);

    // 3. 8-row strip rendering (DMA_STRIP_ROWS == 8) matches full-frame rasterization bit-for-bit
    TEST_ASSERT_EQUAL_INT16(8, display::DMA_STRIP_ROWS);
    HeapArray<uint16_t> full_fb(static_cast<size_t>(display::FB_WIDTH) * display::FB_HEIGHT);
    HeapArray<uint16_t> tiled_fb(static_cast<size_t>(display::FB_WIDTH) * display::FB_HEIGHT);
    TEST_ASSERT_TRUE(static_cast<bool>(full_fb));
    TEST_ASSERT_TRUE(static_cast<bool>(tiled_fb));

    for (uint8_t m = 0; m < static_cast<uint8_t>(ScreenMode::COUNT); ++m) {
        state.screen_mode = static_cast<ScreenMode>(m);
        state.alert_active = (m == static_cast<uint8_t>(ScreenMode::MONITOR));
        strncpy(state.alert_text, "HIGH TEMP TEST", sizeof(state.alert_text) - 1);
        snap.captureFrom(state, DIRTY_FULL);

        display::renderStrip(snap, 0, display::FB_HEIGHT, full_fb.get(), false);
        memset(tiled_fb.get(), 0xA5, tiled_fb.bytes());
        for (int16_t y = 0; y < display::FB_HEIGHT; y = static_cast<int16_t>(y + display::DMA_STRIP_ROWS)) {
            int16_t h = display::DMA_STRIP_ROWS;
            if (y + h > display::FB_HEIGHT) {
                h = static_cast<int16_t>(display::FB_HEIGHT - y);
            }
            display::renderStrip(
                snap, y, h,
                tiled_fb.get() + static_cast<size_t>(y) * static_cast<size_t>(display::FB_WIDTH),
                false);
        }
        TEST_ASSERT_EQUAL_MEMORY(full_fb.get(), tiled_fb.get(), full_fb.bytes());
    }
}

void test_screen_idle_dim_panel_sleep_and_wake(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    TEST_ASSERT_EQUAL_UINT16(120, state.screen_dim_sec);
    TEST_ASSERT_EQUAL_UINT16(600, state.screen_sleep_sec);
    TEST_ASSERT_EQUAL_UINT8(100, state.effectiveBacklightPct());
    TEST_ASSERT_FALSE(state.shouldPanelSleep());

    // Stage 1: After 120s idle, backlight dims to 15% without panel sleep
    state.uptime_sec = 125;
    TEST_ASSERT_EQUAL_UINT8(15, state.effectiveBacklightPct());
    TEST_ASSERT_FALSE(state.shouldPanelSleep());
    RenderSnapshot snap = {};
    snap.captureFrom(state, DIRTY_HEADER);
    TEST_ASSERT_EQUAL_UINT8(15, snap.backlight_pct);

    // Stage 2: After 600s idle, backlight drops to 0% and ST7789 enters panel sleep
    state.uptime_sec = 605;
    TEST_ASSERT_EQUAL_UINT8(0, state.effectiveBacklightPct());
    TEST_ASSERT_TRUE(state.shouldPanelSleep());

    // Critical alert wakes display policy immediately even while idle
    ShellSession session;
    char out[1024];
    session.begin(state, vfs, false, out, sizeof(out));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen alert ThermalTrip", out, sizeof(out)));
    TEST_ASSERT_TRUE(state.alert_active);
    TEST_ASSERT_FALSE(state.shouldPanelSleep());
    TEST_ASSERT_EQUAL_UINT8(100, state.effectiveBacklightPct());

    // Clear alert, enter sleep, then wake via interactive shell input
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen clear", out, sizeof(out)));
    state.screen_sleeping = true;
    state.last_activity_sec = 0;
    state.dirty_regions = 0;
    state.ui_dirty = false;
    out[0] = '\0';
    session.consumeBytes(state, vfs, "\n", 1, out, sizeof(out));
    TEST_ASSERT_FALSE(state.screen_sleeping);
    TEST_ASSERT_EQUAL_UINT32(605, state.last_activity_sec);
    TEST_ASSERT_EQUAL_UINT8(100, state.effectiveBacklightPct());
    TEST_ASSERT_TRUE((state.dirty_regions & DIRTY_FULL) != 0);

    // CLI and sysfs timeout configuration
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen timeout 30 90", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT16(30, state.screen_dim_sec);
    TEST_ASSERT_EQUAL_UINT16(90, state.screen_sleep_sec);

    TEST_ASSERT_TRUE(vfs.writeFile(state, "/sys/class/graphics/fb0/timeout", "45 180\n", false));
    TEST_ASSERT_EQUAL_UINT16(45, state.screen_dim_sec);
    TEST_ASSERT_EQUAL_UINT16(180, state.screen_sleep_sec);
    char sys_buf[64] = {0};
    TEST_ASSERT_TRUE(vfs.readFile(state, "/sys/class/graphics/fb0/timeout", sys_buf, sizeof(sys_buf)));
    TEST_ASSERT_EQUAL_STRING("45 180\n", sys_buf);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen timeout off", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT16(0, state.screen_dim_sec);
    TEST_ASSERT_EQUAL_UINT16(0, state.screen_sleep_sec);
}

void test_epkg_ed25519_trust_on_populate_and_streamed_install(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, false, out, sizeof(out));

    // 1. Default /etc/epkg/trusted_keys has 0 keys -> unsigned multi-chunk (> 4 KB) EPK1 streams cleanly
    const size_t big_len = 5400;  // Spans 3 x 2,048 B streaming chunks
    HeapArray<uint8_t> big_src(big_len);
    HeapArray<uint8_t> pkg_blob(big_len + 512);
    TEST_ASSERT_TRUE(static_cast<bool>(big_src) && static_cast<bool>(pkg_blob));
    for (size_t i = 0; i < big_len; ++i) {
        big_src[i] = static_cast<uint8_t>('A' + (i % 26));
    }
    size_t u_len = epkg::buildPackageBlob(
        "stream-blob", "1.0.0", "noarch", "Multi-chunk streamed asset",
        "/apps/streamed.dat", 0644, big_src.get(), big_len,
        pkg_blob.get(), pkg_blob.bytes());
    TEST_ASSERT_TRUE(u_len > 178 + big_len);
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/tmp/unsigned.epkg", pkg_blob.get(), u_len, false));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "epkg install /tmp/unsigned.epkg", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "installed stream-blob 1.0.0"));

    HeapArray<uint8_t> read_back(big_len);
    size_t got = 0;
    TEST_ASSERT_TRUE(vfs.readBinary("/apps/streamed.dat", 0, read_back.get(), big_len, &got));
    TEST_ASSERT_EQUAL_UINT32(big_len, got);
    TEST_ASSERT_EQUAL_MEMORY(big_src.get(), read_back.get(), big_len);

    // 2. Populate /etc/epkg/trusted_keys with an Ed25519 key -> unsigned EPK1 rejected unless --allow-unsigned
    uint8_t trusted_seed[32], rogue_seed[32];
    for (size_t i = 0; i < 32; ++i) {
        trusted_seed[i] = static_cast<uint8_t>(0x11U + i);
        rogue_seed[i]   = static_cast<uint8_t>(0x91U + i);
    }
    uint8_t trusted_pk[32], trusted_sk[64], rogue_pk[32], rogue_sk[64];
    crypto::ed25519KeypairFromSeed(trusted_pk, trusted_sk, trusted_seed);
    crypto::ed25519KeypairFromSeed(rogue_pk, rogue_sk, rogue_seed);

    char pub_line[160] = {0};
    TEST_ASSERT_TRUE(crypto::formatOpenSshEd25519Pubkey(trusted_pk, "release@c6-linux", pub_line, sizeof(pub_line)) > 0);
    strncat(pub_line, "\n", sizeof(pub_line) - strlen(pub_line) - 1);
    TEST_ASSERT_TRUE(vfs.writeFile(state, "/etc/epkg/trusted_keys", pub_line, false));

    out[0] = '\0';
    session.executeCommandLine(state, vfs, "epkg install /tmp/unsigned.epkg", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "unsigned EPK1 rejected"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(
        state, vfs, "epkg install --allow-unsigned /tmp/unsigned.epkg", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "installed stream-blob 1.0.0"));

    // 3. Valid Ed25519-signed EPK2 installs cleanly and records signed=ed25519
    const char* lua_code = "print('signed_epk2_ok')\n";
    size_t s_len = epkg::buildSignedPackageBlob(
        "signed-tool", "2.0.0", "noarch", "Signed EPK2 package",
        "/apps/signed_tool.lua", 0755,
        reinterpret_cast<const uint8_t*>(lua_code), strlen(lua_code),
        trusted_sk, pkg_blob.get(), pkg_blob.bytes());
    TEST_ASSERT_TRUE(s_len > 242);
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/tmp/signed.epkg", pkg_blob.get(), s_len, false));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "epkg install /tmp/signed.epkg", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "Ed25519 + SHA-256 verified"));

    char manifest[512] = {0};
    TEST_ASSERT_TRUE(vfs.readFile(state, "/var/pkg/signed-tool.manifest", manifest, sizeof(manifest)));
    TEST_ASSERT_NOT_NULL(strstr(manifest, "signed=ed25519"));

    // 4. Tampered EPK2 signature & untrusted-signer EPK2 are rejected even with --allow-unsigned
    pkg_blob[200] ^= 0x01U;
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/tmp/bad_sig.epkg", pkg_blob.get(), s_len, false));
    out[0] = '\0';
    session.executeCommandLine(state, vfs, "epkg install --allow-unsigned /tmp/bad_sig.epkg", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "Ed25519 signature verification FAILED"));

    size_t r_len = epkg::buildSignedPackageBlob(
        "rogue-tool", "1.0.0", "noarch", "Untrusted signer",
        "/apps/rogue.lua", 0755,
        reinterpret_cast<const uint8_t*>(lua_code), strlen(lua_code),
        rogue_sk, pkg_blob.get(), pkg_blob.bytes());
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/tmp/rogue.epkg", pkg_blob.get(), r_len, false));
    out[0] = '\0';
    session.executeCommandLine(state, vfs, "epkg install /tmp/rogue.epkg", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "Ed25519 signature verification FAILED"));
}

namespace {

size_t buildMinimalExecElf(const uint32_t* insts, size_t inst_count,
                           uint32_t p_flags, uint32_t p_memsz,
                           uint8_t* out_buf, size_t out_cap) {
    const size_t code_off = 0x80U;
    const size_t code_bytes = inst_count * 4U;
    if (code_off + code_bytes > out_cap) return 0;
    memset(out_buf, 0, code_off + code_bytes);

    auto wr16le = [](uint8_t* p, uint16_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFFU);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    };
    auto wr32le = [](uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFFU);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
    };

    // Elf32_Ehdr (52 bytes)
    out_buf[0] = 0x7FU; out_buf[1] = 'E'; out_buf[2] = 'L'; out_buf[3] = 'F';
    out_buf[4] = 1;  // ELFCLASS32
    out_buf[5] = 1;  // ELFDATA2LSB
    out_buf[6] = 1;  // EV_CURRENT
    wr16le(out_buf + 16, 2);      // ET_EXEC
    wr16le(out_buf + 18, 243);    // EM_RISCV
    wr32le(out_buf + 20, 1);
    wr32le(out_buf + 24, 0x00010000U);  // e_entry
    wr32le(out_buf + 28, 52U);          // e_phoff
    wr16le(out_buf + 40, 52U);          // e_ehsize
    wr16le(out_buf + 42, 32U);          // e_phentsize
    wr16le(out_buf + 44, 1U);           // e_phnum

    // Elf32_Phdr at offset 52 (32 bytes)
    uint8_t* ph = out_buf + 52U;
    wr32le(ph + 0, 1U);                 // PT_LOAD
    wr32le(ph + 4, code_off);           // p_offset
    wr32le(ph + 8, 0x00010000U);        // p_vaddr
    wr32le(ph + 12, 0x00010000U);       // p_paddr
    wr32le(ph + 16, static_cast<uint32_t>(code_bytes));
    wr32le(ph + 20, p_memsz);
    wr32le(ph + 24, p_flags);
    wr32le(ph + 28, 4U);

    for (size_t i = 0; i < inst_count; ++i) {
        wr32le(out_buf + code_off + i * 4U, insts[i]);
    }
    return code_off + code_bytes;
}

}  // namespace

void test_rv32imac_elf_dynamic_sizing_and_wx_fencing(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);

    // 1. Small ET_REL utility dynamically allocates 16 KB (16,384 B) instead of fixed 48 KB
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/small.elf", TEST_RV32_ELF, sizeof(TEST_RV32_ELF), false));
    char log_buf[512];
    BufWriter w(log_buf, sizeof(log_buf));
    int exit_code = -1;
    size_t vm_bytes = 0;
    const char* argv_small[] = {"small", "Check16K"};
    TEST_ASSERT_TRUE(elf::runElfFile(state, vfs, "/apps/small.elf", 2, argv_small, w, &exit_code, &vm_bytes));
    TEST_ASSERT_EQUAL_INT(0, exit_code);
    TEST_ASSERT_EQUAL_UINT32(16384U, static_cast<uint32_t>(vm_bytes));

    // 2. Medium 16 KB PT_LOAD image scales sandbox dynamically to 24 KB (24,576 B)
    uint8_t elf_buf[256];
    const uint32_t ok_prog[] = {
        0x02A00513U,  // addi a0, zero, 42
        0x05D00893U,  // addi a7, zero, 93 (SYS_exit)
        0x00000073U,  // ecall
    };
    size_t elen = buildMinimalExecElf(ok_prog, 3, 0x5U /* PF_R|PF_X */, 16384U, elf_buf, sizeof(elf_buf));
    TEST_ASSERT_TRUE(elen > 0);
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/med24k.elf", elf_buf, elen, false));
    BufWriter w2(log_buf, sizeof(log_buf));
    TEST_ASSERT_TRUE(elf::runElfFile(state, vfs, "/apps/med24k.elf", 1, nullptr, w2, &exit_code, &vm_bytes));
    TEST_ASSERT_EQUAL_INT(42, exit_code);
    TEST_ASSERT_EQUAL_UINT32(24576U, static_cast<uint32_t>(vm_bytes));

    // 3. Loader rejects PT_LOAD segment marked both Writable and Executable (PF_W | PF_X)
    elen = buildMinimalExecElf(ok_prog, 3, 0x7U /* PF_R|PF_W|PF_X */, 64U, elf_buf, sizeof(elf_buf));
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/rwx_bad.elf", elf_buf, elen, false));
    BufWriter w3(log_buf, sizeof(log_buf));
    TEST_ASSERT_FALSE(elf::runElfFile(state, vfs, "/apps/rwx_bad.elf", 1, nullptr, w3, &exit_code, &vm_bytes));
    TEST_ASSERT_NOT_NULL(strstr(log_buf, "W^X violation"));

    // 4. Runtime traps STORE into executable .text (W^X write fault)
    const uint32_t write_text_prog[] = {
        0x00000297U,  // auipc t0, 0
        0x0002A023U,  // sw zero, 0(t0)
    };
    elen = buildMinimalExecElf(write_text_prog, 2, 0x5U /* PF_R|PF_X */, 64U, elf_buf, sizeof(elf_buf));
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/wx_write.elf", elf_buf, elen, false));
    BufWriter w4(log_buf, sizeof(log_buf));
    TEST_ASSERT_FALSE(elf::runElfFile(state, vfs, "/apps/wx_write.elf", 1, nullptr, w4, &exit_code, &vm_bytes));
    TEST_ASSERT_NOT_NULL(strstr(log_buf, "W^X write fault"));

    // 5. Runtime traps JALR onto writable stack (W^X execute fault)
    const uint32_t exec_stack_prog[] = {
        0x000082B7U,  // lui t0, 8
        0x06728293U,  // addi t0, t0, 0x67  (0x00008067 = ret)
        0x00512023U,  // sw t0, 0(sp)
        0x000100E7U,  // jalr ra, 0(sp)
    };
    elen = buildMinimalExecElf(exec_stack_prog, 4, 0x5U /* PF_R|PF_X */, 64U, elf_buf, sizeof(elf_buf));
    TEST_ASSERT_TRUE(vfs.writeBinary(state, "/apps/wx_exec.elf", elf_buf, elen, false));
    BufWriter w5(log_buf, sizeof(log_buf));
    TEST_ASSERT_FALSE(elf::runElfFile(state, vfs, "/apps/wx_exec.elf", 1, nullptr, w5, &exit_code, &vm_bytes));
    TEST_ASSERT_NOT_NULL(strstr(log_buf, "W^X execute fault"));
}

namespace {

uint64_t g_mock_sd_total = 0;
uint64_t g_mock_sd_free = 0;

int mockSdUsage(uint64_t* out_total, uint64_t* out_free) {
    *out_total = g_mock_sd_total;
    *out_free = g_mock_sd_free;
    return 0;
}

size_t sumLogBytes(VirtualFs& vfs, const char* base, unsigned gens, unsigned* out_files) {
    size_t total = 0;
    unsigned files = 0;
    for (unsigned g = 0; g < gens; ++g) {
        char path[64];
        if (g == 0) snprintf(path, sizeof(path), "%s", base);
        else snprintf(path, sizeof(path), "%s.%u", base, g);
        size_t sz = 0;
        if (vfs.fileSize(path, &sz)) {
            total += sz;
            ++files;
        }
    }
    if (out_files) *out_files = files;
    return total;
}

}  // namespace

void test_syslogd_sd_rotation_cap_and_free_space_floor(void) {
    SystemState state;
    state.initDefaults();
    VirtualFs vfs;
    vfs.initDefaultFiles(state);
    SyslogRotator rotator;

    // 1. LittleFS fallback (no SD): caps clamp to 16 KB x 2 and target is /var/log/messages
    TEST_ASSERT_EQUAL_STRING("/var/log/messages", SyslogRotator::targetPath(state));
    TEST_ASSERT_EQUAL_UINT32(16U * 1024U, SyslogRotator::effectiveCapBytes(state));
    TEST_ASSERT_EQUAL_UINT8(2, SyslogRotator::effectiveFileCount(state));

    // 2. First service persists the boot lines resident in the dmesg ring
    state.appendDmesg(1000, "kernel: boot line A");
    state.appendDmesg(1100, "kernel: boot line B");
    state.uptime_sec = 1;
    TEST_ASSERT_TRUE(rotator.service(state, vfs, /*force=*/true));
    TEST_ASSERT_EQUAL_UINT32(2, state.log_lines_written);
    char buf[512] = {0};
    TEST_ASSERT_TRUE(vfs.readFile(state, "/var/log/messages", buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "kernel: boot line A"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "kernel: boot line B"));

    // 3. Interval gating: new lines stay staged until 10 s elapse or force
    state.appendDmesg(2000, "sshd: session opened");
    state.uptime_sec = 3;
    TEST_ASSERT_FALSE(rotator.service(state, vfs, false));
    TEST_ASSERT_TRUE(rotator.stagedBytes() > 0);
    state.uptime_sec = 12;
    TEST_ASSERT_TRUE(rotator.service(state, vfs, false));
    TEST_ASSERT_EQUAL_UINT32(0, rotator.stagedBytes());
    TEST_ASSERT_EQUAL_UINT32(3, state.log_lines_written);

    // 4. Ring overrun between services counts dropped lines instead of corrupting output
    for (int i = 0; i < 40; ++i) {
        state.appendDmesg(3000U + static_cast<uint32_t>(i), "burst: filler line for overrun test");
    }
    state.uptime_sec = 30;
    rotator.service(state, vfs, true);
    TEST_ASSERT_EQUAL_UINT32(8, state.log_lines_dropped);

    // 5. microSD target with 4 KB x 3 rotation never exceeds the 12 KB ceiling
    state.sd_mounted = true;
    state.sd_total_mb = 1024;
    state.log_file_cap_kb = 4;
    state.log_file_count = 3;
    g_mock_sd_total = 1024ULL * 1024ULL * 1024ULL;
    g_mock_sd_free = 512ULL * 1024ULL * 1024ULL;
    HardwareOps ops = {};
    ops.sd_usage = mockSdUsage;
    setHardwareOps(&ops);

    TEST_ASSERT_EQUAL_STRING("/mnt/sd/var/log/messages", SyslogRotator::targetPath(state));
    TEST_ASSERT_EQUAL_UINT32(4096U, SyslogRotator::effectiveCapBytes(state));
    TEST_ASSERT_EQUAL_UINT8(3, SyslogRotator::effectiveFileCount(state));

    uint32_t rotations_before = state.log_rotations;
    for (int round = 0; round < 60; ++round) {
        for (int i = 0; i < 6; ++i) {
            char msg[64];
            snprintf(msg, sizeof(msg), "sdlog: round %d line %d payload-xxxxxxxxxxxxxxxx", round, i);
            state.appendDmesg(40000U + static_cast<uint32_t>(round * 6 + i), msg);
        }
        state.uptime_sec = 100U + static_cast<uint32_t>(round);
        rotator.service(state, vfs, true);
    }
    unsigned files = 0;
    size_t total_bytes = sumLogBytes(vfs, "/mnt/sd/var/log/messages", 6, &files);
    TEST_ASSERT_TRUE(state.log_rotations > rotations_before);
    TEST_ASSERT_EQUAL_UINT32(3, files);
    TEST_ASSERT_TRUE(total_bytes <= 3U * 4096U);
    size_t gen3 = 0;
    TEST_ASSERT_FALSE(vfs.fileSize("/mnt/sd/var/log/messages.3", &gen3));
    TEST_ASSERT_EQUAL_UINT32(512, state.sd_total_mb - state.sd_used_mb);
    TEST_ASSERT_FALSE(state.log_sd_paused);

    // 6. Free-space floor: below 16 MB free, rotated generations purge and new lines pause
    g_mock_sd_free = 8ULL * 1024ULL * 1024ULL;
    uint32_t dropped_before = state.log_lines_dropped;
    state.appendDmesg(90000, "sdlog: should be dropped while paused");
    state.uptime_sec = 200;
    TEST_ASSERT_FALSE(rotator.service(state, vfs, true));
    TEST_ASSERT_TRUE(state.log_sd_paused);
    TEST_ASSERT_TRUE(state.log_lines_dropped > dropped_before);
    sumLogBytes(vfs, "/mnt/sd/var/log/messages", 6, &files);
    TEST_ASSERT_EQUAL_UINT32(1, files);

    // 7. Recovery once free space returns, plus df / logrotate CLI surfaces
    g_mock_sd_free = 256ULL * 1024ULL * 1024ULL;
    state.appendDmesg(91000, "sdlog: resumed after free space recovered");
    state.uptime_sec = 220;
    TEST_ASSERT_TRUE(rotator.service(state, vfs, true));
    TEST_ASSERT_FALSE(state.log_sd_paused);

    ShellSession session;
    char out[2048];
    session.begin(state, vfs, false, out, sizeof(out));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "df", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "/mnt/sd"));
    TEST_ASSERT_NOT_NULL(strstr(out, " 75% /mnt/sd"));

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "logrotate 512 2", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT16(512, state.log_file_cap_kb);
    TEST_ASSERT_EQUAL_UINT8(2, state.log_file_count);
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "logrotate status", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "512 KB x 2 files (ceiling 1024 KB)"));
    out[0] = '\0';
    session.executeCommandLine(state, vfs, "logrotate 3 9", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "ERR usage"));
    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "logrotate force", out, sizeof(out)));
    TEST_ASSERT_TRUE(state.log_rotate_requested);
    uint32_t rot_before = state.log_rotations;
    state.uptime_sec = 240;
    rotator.service(state, vfs, false);
    TEST_ASSERT_EQUAL_UINT32(rot_before + 1, state.log_rotations);
    TEST_ASSERT_FALSE(state.log_rotate_requested);

    out[0] = '\0';
    TEST_ASSERT_TRUE(session.executeCommandLine(state, vfs, "screen status", out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"log_target\":\"/mnt/sd/var/log/messages\""));
    TEST_ASSERT_NOT_NULL(strstr(out, "\"sd_free_mb\":256"));
    setHardwareOps(nullptr);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_vfs_proc_and_sysfs_hooks);
    RUN_TEST(test_vfs_path_normalization_and_user_files);
    RUN_TEST(test_ssh_mpint_and_packet_framing);
    RUN_TEST(test_interactive_menu_and_shell_commands);
    RUN_TEST(test_new_ten_os_features);
    RUN_TEST(test_ssh_ed25519_rsa_and_salted_password_auth);
    RUN_TEST(test_sftp_v3_binary_upload_stat_readdir_and_download);
    RUN_TEST(test_epkg_install_sha256_tamper_reject_and_lua_runtime);
    RUN_TEST(test_rv32imac_elf_sandbox_execution);
    RUN_TEST(test_scanline_fbgrab_bmp_ppm_tz_chsh_apitoken_and_reserved_pins);
    RUN_TEST(test_vfs_usage_dirty_selective_snapshot_and_8row_strip_parity);
    RUN_TEST(test_screen_idle_dim_panel_sleep_and_wake);
    RUN_TEST(test_epkg_ed25519_trust_on_populate_and_streamed_install);
    RUN_TEST(test_rv32imac_elf_dynamic_sizing_and_wx_fencing);
    RUN_TEST(test_syslogd_sd_rotation_cap_and_free_space_floor);
    return UNITY_END();
}
