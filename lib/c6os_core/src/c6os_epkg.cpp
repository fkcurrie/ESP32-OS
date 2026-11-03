#include "c6os_epkg.h"
#include "ssh_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {
namespace epkg {

namespace {

constexpr size_t EPKG_HEADER_LEN = 178;
constexpr size_t EPKG_MAX_PAYLOAD = 98304;  // 96 KB max package payload in 320 KB SRAM

uint16_t getBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t getBe32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

void putBe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[1] = static_cast<uint8_t>(v & 0xFFU);
}

void putBe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<uint8_t>(v & 0xFFU);
}

bool isValidPkgName(const char* name) {
    if (!name || name[0] == '\0') return false;
    for (size_t i = 0; name[i] != '\0'; ++i) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

bool isSupportedArch(const char* arch) {
    return strcmp(arch, "rv32imac") == 0 ||
           strcmp(arch, "riscv32") == 0 ||
           strcmp(arch, "noarch") == 0;
}

}  // namespace

bool installPackage(SystemState& state, VirtualFs& vfs, const char* pkg_abs_path, BufWriter& out) {
    size_t total_sz = 0;
    if (!vfs.fileSize(pkg_abs_path, &total_sz) || total_sz < EPKG_HEADER_LEN) {
        out.fmt("epkg: cannot open valid package '%s'\n", pkg_abs_path ? pkg_abs_path : "");
        return false;
    }

    size_t payload_len = total_sz - EPKG_HEADER_LEN;
    if (payload_len == 0 || payload_len > EPKG_MAX_PAYLOAD) {
        out.fmt("epkg: invalid payload size (%u bytes)\n", static_cast<unsigned>(payload_len));
        return false;
    }

    uint8_t hdr[EPKG_HEADER_LEN];
    size_t got_hdr = 0;
    if (!vfs.readBinary(pkg_abs_path, 0, hdr, EPKG_HEADER_LEN, &got_hdr) || got_hdr != EPKG_HEADER_LEN) {
        out.putStr("epkg: failed reading EPK1 header\n");
        return false;
    }

    if (memcmp(hdr, "EPK1", 4) != 0) {
        out.putStr("epkg: bad magic (expected EPK1)\n");
        return false;
    }

    uint16_t fmt_ver = getBe16(hdr + 4);
    uint16_t abi_ver = getBe16(hdr + 6);
    if (fmt_ver != 1 || abi_ver != 1) {
        out.fmt("epkg: unsupported version %u.%u\n", fmt_ver, abi_ver);
        return false;
    }

    char name[33], version[17], arch[25], desc[65];
    memcpy(name, hdr + 8, 32);     name[32] = '\0';
    memcpy(version, hdr + 40, 16); version[16] = '\0';
    memcpy(arch, hdr + 56, 24);    arch[24] = '\0';
    memcpy(desc, hdr + 80, 64);    desc[64] = '\0';
    const uint8_t* expected_sha = hdr + 144;
    uint16_t file_count = getBe16(hdr + 176);

    if (!isValidPkgName(name)) {
        out.putStr("epkg: invalid package name\n");
        return false;
    }
    if (!isSupportedArch(arch)) {
        out.fmt("epkg: incompatible architecture '%s' (expected rv32imac or noarch)\n", arch);
        return false;
    }
    if (file_count == 0 || file_count > 32) {
        out.fmt("epkg: invalid file count %u\n", file_count);
        return false;
    }

    auto* payload = static_cast<uint8_t*>(malloc(payload_len));
    if (!payload) {
        out.putStr("epkg: out of heap memory verifying package\n");
        return false;
    }

    size_t got_payload = 0;
    if (!vfs.readBinary(pkg_abs_path, EPKG_HEADER_LEN, payload, payload_len, &got_payload) ||
        got_payload != payload_len) {
        free(payload);
        out.putStr("epkg: truncated package payload\n");
        return false;
    }

    uint8_t actual_sha[32];
    crypto::sha256(payload, payload_len, actual_sha);
    if (!crypto::constantTimeEqual(expected_sha, actual_sha, 32)) {
        free(payload);
        out.putStr("epkg: SHA-256 integrity verification FAILED (corrupt or tampered package)\n");
        return false;
    }

    // Dry-run validate all file entries before touching filesystem
    size_t pos = 0;
    for (uint16_t i = 0; i < file_count; ++i) {
        if (pos + 8 > payload_len) {
            free(payload);
            out.putStr("epkg: malformed entry header\n");
            return false;
        }
        uint16_t path_len = getBe16(payload + pos);
        uint32_t data_len = getBe32(payload + pos + 4);
        pos += 8;
        if (path_len == 0 || path_len >= 120 || pos + path_len + data_len > payload_len) {
            free(payload);
            out.putStr("epkg: malformed entry bounds\n");
            return false;
        }
        char vpath[128];
        memcpy(vpath, payload + pos, path_len);
        vpath[path_len] = '\0';
        if (vpath[0] != '/' || strstr(vpath, "..") != nullptr || vfs.isVirtualPath(vpath)) {
            free(payload);
            out.fmt("epkg: forbidden destination path '%s'\n", vpath);
            return false;
        }
        pos += path_len + data_len;
    }

    // Extract files and write /var/pkg/<name>.manifest
    HeapArray<char> manifest_buf(1024);
    if (!manifest_buf) {
        free(payload);
        out.putStr("epkg: out of heap memory\n");
        return false;
    }
    BufWriter mw(manifest_buf.get(), manifest_buf.bytes());
    mw.fmt("name=%s\nversion=%s\narch=%s\ndescription=%s\n", name, version, arch, desc);

    pos = 0;
    for (uint16_t i = 0; i < file_count; ++i) {
        uint16_t path_len = getBe16(payload + pos);
        uint32_t data_len = getBe32(payload + pos + 4);
        pos += 8;
        char vpath[128];
        memcpy(vpath, payload + pos, path_len);
        vpath[path_len] = '\0';
        pos += path_len;

        if (!vfs.writeBinary(state, vpath, payload + pos, data_len, false)) {
            free(payload);
            out.fmt("epkg: failed extracting '%s'\n", vpath);
            return false;
        }
        mw.fmt("file=%s\n", vpath);
        pos += data_len;
    }
    free(payload);

    vfs.mkdirPath("/var/pkg");
    char manifest_path[96];
    snprintf(manifest_path, sizeof(manifest_path), "/var/pkg/%s.manifest", name);
    vfs.writeFile(state, manifest_path, manifest_buf.get(), false);

    out.fmt("epkg: installed %s %s (%s, %u file%s, SHA-256 verified)\n",
            name, version, arch, file_count, file_count == 1 ? "" : "s");
    return true;
}

bool removePackage(SystemState& state, VirtualFs& vfs, const char* pkg_name, BufWriter& out) {
    if (!isValidPkgName(pkg_name)) {
        out.putStr("epkg: invalid package name\n");
        return false;
    }
    char manifest_path[96];
    snprintf(manifest_path, sizeof(manifest_path), "/var/pkg/%s.manifest", pkg_name);

    HeapArray<char> content(1024);
    if (!content || !vfs.readFile(state, manifest_path, content.get(), content.bytes())) {
        out.fmt("epkg: package '%s' is not installed\n", pkg_name);
        return false;
    }

    unsigned removed_files = 0;
    char* saveptr = nullptr;
    char* line = strtok_r(content.get(), "\r\n", &saveptr);
    while (line != nullptr) {
        if (strncmp(line, "file=", 5) == 0) {
            if (vfs.removeFile(state, line + 5)) {
                ++removed_files;
            }
        }
        line = strtok_r(nullptr, "\r\n", &saveptr);
    }

    vfs.removeFile(state, manifest_path);
    out.fmt("epkg: removed %s (%u file%s deleted)\n",
            pkg_name, removed_files, removed_files == 1 ? "" : "s");
    return true;
}

bool listPackages(const VirtualFs& vfs, BufWriter& out) {
    HeapArray<VfsDirEntry> entries(32);
    HeapArray<SystemState> dummy(1);
    HeapArray<char> content(512);
    if (!entries || !dummy || !content) return false;
    size_t count = vfs.readDirEntries("/var/pkg", entries.get(), 32);
    unsigned pkg_count = 0;

    dummy[0].initDefaults();

    for (size_t i = 0; i < count; ++i) {
        const char* dot = strstr(entries[i].name, ".manifest");
        if (!dot) continue;
        char full[128];
        snprintf(full, sizeof(full), "/var/pkg/%.96s", entries[i].name);
        if (!vfs.readFile(dummy[0], full, content.get(), content.bytes())) continue;

        char name[36] = "", ver[20] = "", arch[24] = "", desc[68] = "";
        char* saveptr = nullptr;
        for (char* ln = strtok_r(content.get(), "\r\n", &saveptr); ln; ln = strtok_r(nullptr, "\r\n", &saveptr)) {
            if (strncmp(ln, "name=", 5) == 0) strncpy(name, ln + 5, sizeof(name) - 1);
            else if (strncmp(ln, "version=", 8) == 0) strncpy(ver, ln + 8, sizeof(ver) - 1);
            else if (strncmp(ln, "arch=", 5) == 0) strncpy(arch, ln + 5, sizeof(arch) - 1);
            else if (strncmp(ln, "description=", 12) == 0) strncpy(desc, ln + 12, sizeof(desc) - 1);
        }
        out.fmt("%-18s %-10s %-10s %s\n", name, ver, arch, desc);
        ++pkg_count;
    }

    if (pkg_count == 0) {
        out.putStr("(no packages installed; upload .epkg via sftp and run 'epkg install <file>')\n");
    }
    return true;
}

bool packageInfo(const SystemState& state, const VirtualFs& vfs, const char* pkg_name, BufWriter& out) {
    if (!isValidPkgName(pkg_name)) {
        out.putStr("epkg: invalid package name\n");
        return false;
    }
    char manifest_path[96];
    snprintf(manifest_path, sizeof(manifest_path), "/var/pkg/%s.manifest", pkg_name);
    HeapArray<char> content(1024);
    if (!content || !vfs.readFile(state, manifest_path, content.get(), content.bytes())) {
        out.fmt("epkg: package '%s' is not installed\n", pkg_name);
        return false;
    }
    out.putStr(content.get());
    return true;
}

size_t buildPackageBlob(const char* name, const char* version, const char* arch,
                        const char* description, const char* target_vpath,
                        uint16_t mode, const uint8_t* file_data, size_t file_len,
                        uint8_t* out_blob, size_t out_cap) {
    if (!name || !version || !arch || !description || !target_vpath || !out_blob) return 0;
    size_t path_len = strlen(target_vpath);
    size_t payload_len = 8 + path_len + file_len;
    size_t total_len = EPKG_HEADER_LEN + payload_len;
    if (total_len > out_cap) return 0;

    memset(out_blob, 0, total_len);
    uint8_t* payload = out_blob + EPKG_HEADER_LEN;
    putBe16(payload + 0, static_cast<uint16_t>(path_len));
    putBe16(payload + 2, mode);
    putBe32(payload + 4, static_cast<uint32_t>(file_len));
    memcpy(payload + 8, target_vpath, path_len);
    if (file_len > 0 && file_data) {
        memcpy(payload + 8 + path_len, file_data, file_len);
    }

    uint8_t digest[32];
    crypto::sha256(payload, payload_len, digest);

    memcpy(out_blob, "EPK1", 4);
    putBe16(out_blob + 4, 1);
    putBe16(out_blob + 6, 1);
    strncpy(reinterpret_cast<char*>(out_blob + 8), name, 31);
    strncpy(reinterpret_cast<char*>(out_blob + 40), version, 15);
    strncpy(reinterpret_cast<char*>(out_blob + 56), arch, 23);
    strncpy(reinterpret_cast<char*>(out_blob + 80), description, 63);
    memcpy(out_blob + 144, digest, 32);
    putBe16(out_blob + 176, 1);
    return total_len;
}

}  // namespace epkg
}  // namespace c6os
