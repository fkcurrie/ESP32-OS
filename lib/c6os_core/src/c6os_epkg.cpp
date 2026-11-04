#include "c6os_epkg.h"
#include "ssh_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {
namespace epkg {

namespace {

constexpr size_t EPKG_V1_HEADER_LEN = 178;
constexpr size_t EPKG_SIG_LEN       = 64;
constexpr size_t EPKG_V2_HEADER_LEN = EPKG_V1_HEADER_LEN + EPKG_SIG_LEN;  // 242 B
constexpr size_t EPKG_MAX_PAYLOAD   = 98304;  // 96 KB max package payload
constexpr size_t EPKG_STREAM_CHUNK  = 2048;   // 2 KB streamed verification/extract buffer
constexpr size_t EPKG_MAX_TRUSTED   = 8;

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

bool installPackage(SystemState& state, VirtualFs& vfs, const char* pkg_abs_path,
                    BufWriter& out, bool allow_unsigned) {
    size_t total_sz = 0;
    if (!vfs.fileSize(pkg_abs_path, &total_sz) || total_sz < EPKG_V1_HEADER_LEN) {
        out.fmt("epkg: cannot open valid package '%s'\n", pkg_abs_path ? pkg_abs_path : "");
        return false;
    }

    FILE* pkg_fp = vfs.openPosix(pkg_abs_path, "rb");
    if (!pkg_fp) {
        out.fmt("epkg: cannot open '%s'\n", pkg_abs_path ? pkg_abs_path : "");
        return false;
    }

    uint8_t hdr[EPKG_V2_HEADER_LEN];
    memset(hdr, 0, sizeof(hdr));
    if (fread(hdr, 1, EPKG_V1_HEADER_LEN, pkg_fp) != EPKG_V1_HEADER_LEN) {
        fclose(pkg_fp);
        out.putStr("epkg: failed reading package header\n");
        return false;
    }

    bool is_v1 = (memcmp(hdr, "EPK1", 4) == 0);
    bool is_v2 = (memcmp(hdr, "EPK2", 4) == 0);
    if (!is_v1 && !is_v2) {
        fclose(pkg_fp);
        out.putStr("epkg: bad magic (expected EPK1 or EPK2)\n");
        return false;
    }

    uint16_t fmt_ver = getBe16(hdr + 4);
    uint16_t abi_ver = getBe16(hdr + 6);
    uint16_t expected_fmt = is_v2 ? 2U : 1U;
    if (fmt_ver != expected_fmt || abi_ver != 1U) {
        fclose(pkg_fp);
        out.fmt("epkg: unsupported version %u.%u\n", fmt_ver, abi_ver);
        return false;
    }

    size_t header_len = is_v2 ? EPKG_V2_HEADER_LEN : EPKG_V1_HEADER_LEN;
    if (total_sz <= header_len) {
        fclose(pkg_fp);
        out.putStr("epkg: empty package payload\n");
        return false;
    }
    if (is_v2 && fread(hdr + EPKG_V1_HEADER_LEN, 1, EPKG_SIG_LEN, pkg_fp) != EPKG_SIG_LEN) {
        fclose(pkg_fp);
        out.putStr("epkg: truncated EPK2 signature\n");
        return false;
    }

    size_t payload_len = total_sz - header_len;
    if (payload_len == 0 || payload_len > EPKG_MAX_PAYLOAD) {
        fclose(pkg_fp);
        out.fmt("epkg: invalid payload size (%u bytes)\n", static_cast<unsigned>(payload_len));
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
        fclose(pkg_fp);
        out.putStr("epkg: invalid package name\n");
        return false;
    }
    if (!isSupportedArch(arch)) {
        fclose(pkg_fp);
        out.fmt("epkg: incompatible architecture '%s' (expected rv32imac or noarch)\n", arch);
        return false;
    }
    if (file_count == 0 || file_count > 32) {
        fclose(pkg_fp);
        out.fmt("epkg: invalid file count %u\n", file_count);
        return false;
    }

    // Trust-on-populate Ed25519 signature policy (/etc/epkg/trusted_keys)
    HeapArray<char> tk_text(1536);
    HeapArray<uint8_t> tk_pks_raw(EPKG_MAX_TRUSTED * 32);
    if (!tk_text || !tk_pks_raw) {
        fclose(pkg_fp);
        out.putStr("epkg: out of heap memory\n");
        return false;
    }
    size_t trusted_count = 0;
    if (vfs.readFile(state, "/etc/epkg/trusted_keys", tk_text.get(), tk_text.bytes())) {
        trusted_count = crypto::parseEd25519TrustedKeys(
            tk_text.get(),
            reinterpret_cast<uint8_t(*)[32]>(tk_pks_raw.get()),
            EPKG_MAX_TRUSTED);
    }

    bool sig_verified = false;
    if (is_v2) {
        const uint8_t* sig = hdr + EPKG_V1_HEADER_LEN;
        if (trusted_count > 0) {
            auto* pks = reinterpret_cast<const uint8_t(*)[32]>(tk_pks_raw.get());
            for (size_t k = 0; k < trusted_count; ++k) {
                if (crypto::ed25519Verify(sig, hdr, EPKG_V1_HEADER_LEN, pks[k]) == 0) {
                    sig_verified = true;
                    break;
                }
            }
            if (!sig_verified) {
                fclose(pkg_fp);
                out.putStr("epkg: Ed25519 signature verification FAILED against /etc/epkg/trusted_keys\n");
                return false;
            }
        }
    } else if (trusted_count > 0 && !allow_unsigned) {
        fclose(pkg_fp);
        out.putStr("epkg: unsigned EPK1 rejected (/etc/epkg/trusted_keys active; pass --allow-unsigned to override)\n");
        return false;
    }

    // Pass 1: Streamed 2 KB SHA-256 verification over payload (0 B full-payload malloc)
    HeapArray<uint8_t> chunk(EPKG_STREAM_CHUNK);
    if (!chunk) {
        fclose(pkg_fp);
        out.putStr("epkg: out of heap memory verifying package\n");
        return false;
    }
    if (fseek(pkg_fp, static_cast<long>(header_len), SEEK_SET) != 0) {
        fclose(pkg_fp);
        out.putStr("epkg: seek error\n");
        return false;
    }

    crypto::Sha256Ctx sha_ctx;
    crypto::sha256Init(sha_ctx);
    size_t rem_hash = payload_len;
    while (rem_hash > 0) {
        size_t to_read = (rem_hash < EPKG_STREAM_CHUNK) ? rem_hash : EPKG_STREAM_CHUNK;
        if (fread(chunk.get(), 1, to_read, pkg_fp) != to_read) {
            fclose(pkg_fp);
            out.putStr("epkg: truncated package payload\n");
            return false;
        }
        crypto::sha256Update(sha_ctx, chunk.get(), to_read);
        rem_hash -= to_read;
    }
    uint8_t actual_sha[32];
    crypto::sha256Final(sha_ctx, actual_sha);
    if (!crypto::constantTimeEqual(expected_sha, actual_sha, 32)) {
        fclose(pkg_fp);
        out.putStr("epkg: SHA-256 integrity verification FAILED (corrupt or tampered package)\n");
        return false;
    }

    // Pass 2: Dry-run validate all file entry bounds & paths before touching filesystem
    size_t pos = 0;
    for (uint16_t i = 0; i < file_count; ++i) {
        if (pos + 8 > payload_len) {
            fclose(pkg_fp);
            out.putStr("epkg: malformed entry header\n");
            return false;
        }
        if (fseek(pkg_fp, static_cast<long>(header_len + pos), SEEK_SET) != 0) {
            fclose(pkg_fp);
            return false;
        }
        uint8_t ehdr[8];
        if (fread(ehdr, 1, 8, pkg_fp) != 8) {
            fclose(pkg_fp);
            out.putStr("epkg: malformed entry header\n");
            return false;
        }
        uint16_t path_len = getBe16(ehdr);
        uint32_t data_len = getBe32(ehdr + 4);
        pos += 8;
        if (path_len == 0 || path_len >= 120 || pos + path_len + data_len > payload_len) {
            fclose(pkg_fp);
            out.putStr("epkg: malformed entry bounds\n");
            return false;
        }
        char vpath[128];
        if (fread(vpath, 1, path_len, pkg_fp) != path_len) {
            fclose(pkg_fp);
            out.putStr("epkg: truncated entry path\n");
            return false;
        }
        vpath[path_len] = '\0';
        if (vpath[0] != '/' || strstr(vpath, "..") != nullptr || vfs.isVirtualPath(vpath)) {
            fclose(pkg_fp);
            out.fmt("epkg: forbidden destination path '%s'\n", vpath);
            return false;
        }
        pos += path_len + data_len;
    }

    // Pass 3: Stream-extract files in 2 KB chunks and write /var/pkg/<name>.manifest
    HeapArray<char> manifest_buf(1024);
    if (!manifest_buf) {
        fclose(pkg_fp);
        out.putStr("epkg: out of heap memory\n");
        return false;
    }
    BufWriter mw(manifest_buf.get(), manifest_buf.bytes());
    mw.fmt("name=%s\nversion=%s\narch=%s\ndescription=%s\nsigned=%s\n",
           name, version, arch, desc, sig_verified ? "ed25519" : "unsigned");

    pos = 0;
    for (uint16_t i = 0; i < file_count; ++i) {
        if (fseek(pkg_fp, static_cast<long>(header_len + pos), SEEK_SET) != 0) {
            fclose(pkg_fp);
            return false;
        }
        uint8_t ehdr[8];
        if (fread(ehdr, 1, 8, pkg_fp) != 8) {
            fclose(pkg_fp);
            return false;
        }
        uint16_t path_len = getBe16(ehdr);
        uint32_t data_len = getBe32(ehdr + 4);
        char vpath[128];
        if (fread(vpath, 1, path_len, pkg_fp) != path_len) {
            fclose(pkg_fp);
            return false;
        }
        vpath[path_len] = '\0';
        pos += 8 + path_len;

        FILE* dst_fp = vfs.openPosix(vpath, "wb");
        if (!dst_fp) {
            fclose(pkg_fp);
            out.fmt("epkg: failed opening '%s' for extraction\n", vpath);
            return false;
        }
        size_t rem_copy = data_len;
        while (rem_copy > 0) {
            size_t step = (rem_copy < EPKG_STREAM_CHUNK) ? rem_copy : EPKG_STREAM_CHUNK;
            if (fread(chunk.get(), 1, step, pkg_fp) != step ||
                fwrite(chunk.get(), 1, step, dst_fp) != step) {
                fclose(dst_fp);
                fclose(pkg_fp);
                out.fmt("epkg: failed extracting '%s'\n", vpath);
                return false;
            }
            rem_copy -= step;
        }
        fclose(dst_fp);
        state.fs_sync_requested = true;
        mw.fmt("file=%s\n", vpath);
        pos += data_len;
    }
    fclose(pkg_fp);

    vfs.mkdirPath("/var/pkg");
    char manifest_path[96];
    snprintf(manifest_path, sizeof(manifest_path), "/var/pkg/%s.manifest", name);
    vfs.writeFile(state, manifest_path, manifest_buf.get(), false);

    out.fmt("epkg: installed %s %s (%s, %u file%s, %s)\n",
            name, version, arch, file_count, file_count == 1 ? "" : "s",
            sig_verified ? "Ed25519 + SHA-256 verified" : "SHA-256 verified");
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
    size_t total_len = EPKG_V1_HEADER_LEN + payload_len;
    if (total_len > out_cap) return 0;

    memset(out_blob, 0, total_len);
    uint8_t* payload = out_blob + EPKG_V1_HEADER_LEN;
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

size_t buildSignedPackageBlob(const char* name, const char* version, const char* arch,
                              const char* description, const char* target_vpath,
                              uint16_t mode, const uint8_t* file_data, size_t file_len,
                              const uint8_t ed25519_sk[64],
                              uint8_t* out_blob, size_t out_cap) {
    if (!name || !version || !arch || !description || !target_vpath || !ed25519_sk || !out_blob) {
        return 0;
    }
    size_t path_len = strlen(target_vpath);
    size_t payload_len = 8 + path_len + file_len;
    size_t total_len = EPKG_V2_HEADER_LEN + payload_len;
    if (total_len > out_cap) return 0;

    memset(out_blob, 0, total_len);
    uint8_t* payload = out_blob + EPKG_V2_HEADER_LEN;
    putBe16(payload + 0, static_cast<uint16_t>(path_len));
    putBe16(payload + 2, mode);
    putBe32(payload + 4, static_cast<uint32_t>(file_len));
    memcpy(payload + 8, target_vpath, path_len);
    if (file_len > 0 && file_data) {
        memcpy(payload + 8 + path_len, file_data, file_len);
    }

    uint8_t digest[32];
    crypto::sha256(payload, payload_len, digest);

    memcpy(out_blob, "EPK2", 4);
    putBe16(out_blob + 4, 2);
    putBe16(out_blob + 6, 1);
    strncpy(reinterpret_cast<char*>(out_blob + 8), name, 31);
    strncpy(reinterpret_cast<char*>(out_blob + 40), version, 15);
    strncpy(reinterpret_cast<char*>(out_blob + 56), arch, 23);
    strncpy(reinterpret_cast<char*>(out_blob + 80), description, 63);
    memcpy(out_blob + 144, digest, 32);
    putBe16(out_blob + 176, 1);
    crypto::ed25519Sign(out_blob + EPKG_V1_HEADER_LEN, out_blob, EPKG_V1_HEADER_LEN, ed25519_sk);
    return total_len;
}

}  // namespace epkg
}  // namespace c6os
