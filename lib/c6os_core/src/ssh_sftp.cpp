#include "ssh_sftp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace c6os {
namespace ssh {

namespace {

constexpr uint8_t SSH_FXP_INIT     = 1;
constexpr uint8_t SSH_FXP_VERSION  = 2;
constexpr uint8_t SSH_FXP_OPEN     = 3;
constexpr uint8_t SSH_FXP_CLOSE    = 4;
constexpr uint8_t SSH_FXP_READ     = 5;
constexpr uint8_t SSH_FXP_WRITE    = 6;
constexpr uint8_t SSH_FXP_LSTAT    = 7;
constexpr uint8_t SSH_FXP_FSTAT    = 8;
constexpr uint8_t SSH_FXP_SETSTAT  = 9;
constexpr uint8_t SSH_FXP_FSETSTAT = 10;
constexpr uint8_t SSH_FXP_OPENDIR  = 11;
constexpr uint8_t SSH_FXP_READDIR  = 12;
constexpr uint8_t SSH_FXP_REMOVE   = 13;
constexpr uint8_t SSH_FXP_MKDIR    = 14;
constexpr uint8_t SSH_FXP_RMDIR    = 15;
constexpr uint8_t SSH_FXP_REALPATH = 16;
constexpr uint8_t SSH_FXP_STAT     = 17;
constexpr uint8_t SSH_FXP_RENAME   = 18;

constexpr uint8_t SSH_FXP_STATUS   = 101;
constexpr uint8_t SSH_FXP_HANDLE   = 102;
constexpr uint8_t SSH_FXP_DATA     = 103;
constexpr uint8_t SSH_FXP_NAME     = 104;
constexpr uint8_t SSH_FXP_ATTRS    = 105;

constexpr uint32_t SSH_FX_OK                = 0;
constexpr uint32_t SSH_FX_EOF               = 1;
constexpr uint32_t SSH_FX_NO_SUCH_FILE      = 2;
constexpr uint32_t SSH_FX_PERMISSION_DENIED = 3;
constexpr uint32_t SSH_FX_FAILURE           = 4;
constexpr uint32_t SSH_FX_OP_UNSUPPORTED    = 8;

constexpr uint32_t SSH_FXF_READ   = 0x00000001U;
constexpr uint32_t SSH_FXF_WRITE  = 0x00000002U;
constexpr uint32_t SSH_FXF_APPEND = 0x00000004U;
constexpr uint32_t SSH_FXF_CREAT  = 0x00000008U;
constexpr uint32_t SSH_FXF_TRUNC  = 0x00000010U;

constexpr uint32_t ATTR_SIZE        = 0x00000001U;
constexpr uint32_t ATTR_UIDGID      = 0x00000002U;
constexpr uint32_t ATTR_PERMISSIONS = 0x00000004U;
constexpr uint32_t ATTR_ACMODTIME   = 0x00000008U;

uint32_t getU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t getU64(const uint8_t* p) {
    return (static_cast<uint64_t>(getU32(p)) << 32) | static_cast<uint64_t>(getU32(p + 4));
}

void putU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<uint8_t>(v & 0xFFU);
}

void putU64(uint8_t* p, uint64_t v) {
    putU32(p, static_cast<uint32_t>(v >> 32));
    putU32(p + 4, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
}

int readStr(const uint8_t*& p, const uint8_t* end, char* out, size_t out_cap) {
    if (static_cast<size_t>(end - p) < 4) return -1;
    uint32_t len = getU32(p);
    p += 4;
    if (static_cast<size_t>(end - p) < len) return -1;
    if (out && out_cap > 0) {
        size_t copy = (len < out_cap - 1) ? len : (out_cap - 1);
        if (copy > 0) memcpy(out, p, copy);
        out[copy] = '\0';
    }
    p += len;
    return static_cast<int>(len);
}

size_t putStr(uint8_t* dst, const char* s) {
    uint32_t len = s ? static_cast<uint32_t>(strlen(s)) : 0U;
    putU32(dst, len);
    if (len > 0) memcpy(dst + 4, s, len);
    return 4 + len;
}

size_t encodeAttrs(uint8_t* dst, uint64_t size, uint32_t mode, uint32_t mtime) {
    uint32_t flags = ATTR_SIZE | ATTR_UIDGID | ATTR_PERMISSIONS | ATTR_ACMODTIME;
    putU32(dst, flags);
    putU64(dst + 4, size);
    putU32(dst + 12, 0);
    putU32(dst + 16, 0);
    putU32(dst + 20, mode);
    putU32(dst + 24, mtime);
    putU32(dst + 28, mtime);
    return 32;
}

}  // namespace

SftpSession::SftpSession()
    : state_(nullptr), vfs_(nullptr), send_fn_(nullptr), send_ctx_(nullptr),
      rx_buf_(nullptr), rx_len_(0), rx_cap_(0) {
    memset(handles_, 0, sizeof(handles_));
}

SftpSession::~SftpSession() {
    closeAll();
}

bool SftpSession::init(SystemState* state, VirtualFs* vfs, SftpSendFn send_fn, void* send_ctx) {
    closeAll();
    state_ = state;
    vfs_ = vfs;
    send_fn_ = send_fn;
    send_ctx_ = send_ctx;
    rx_cap_ = 4096;
    rx_len_ = 0;
    rx_buf_ = static_cast<uint8_t*>(malloc(rx_cap_));
    return rx_buf_ != nullptr;
}

void SftpSession::freeHandle(SftpHandle& h) {
    if (!h.used) return;
    if (h.fp) {
        fclose(h.fp);
        h.fp = nullptr;
    }
    if (h.mem_data) {
        free(h.mem_data);
        h.mem_data = nullptr;
    }
    if (h.dir_entries) {
        free(h.dir_entries);
        h.dir_entries = nullptr;
    }
    if (h.writable && state_ && vfs_) {
        state_->fs_sync_requested = true;
        if (strncmp(h.vpath, "/etc/", 5) == 0) {
            vfs_->initDefaultFiles(*state_);
        }
    }
    memset(&h, 0, sizeof(h));
}

void SftpSession::closeAll() {
    for (size_t i = 0; i < SFTP_MAX_HANDLES; ++i) {
        freeHandle(handles_[i]);
    }
    if (rx_buf_) {
        free(rx_buf_);
        rx_buf_ = nullptr;
    }
    rx_len_ = 0;
    rx_cap_ = 0;
}

int SftpSession::allocHandle() {
    for (size_t i = 0; i < SFTP_MAX_HANDLES; ++i) {
        if (!handles_[i].used) return static_cast<int>(i);
    }
    return -1;
}

SftpHandle* SftpSession::lookupHandle(const char* hstr, int hlen) {
    if (hlen != 4) return nullptr;
    int idx = atoi(hstr);
    if (idx < 0 || static_cast<size_t>(idx) >= SFTP_MAX_HANDLES || !handles_[idx].used) {
        return nullptr;
    }
    return &handles_[idx];
}

bool SftpSession::sendPacket(const uint8_t* payload, size_t len) {
    if (!send_fn_) return false;
    uint8_t hdr[4];
    putU32(hdr, static_cast<uint32_t>(len));
    if (send_fn_(send_ctx_, hdr, 4) != 0) return false;
    return send_fn_(send_ctx_, payload, len) == 0;
}

bool SftpSession::sendStatus(uint32_t req_id, uint32_t code, const char* msg) {
    uint8_t buf[256];
    size_t pos = 0;
    buf[pos++] = SSH_FXP_STATUS;
    putU32(buf + pos, req_id); pos += 4;
    putU32(buf + pos, code); pos += 4;
    pos += putStr(buf + pos, msg ? msg : "");
    pos += putStr(buf + pos, "en");
    return sendPacket(buf, pos);
}

bool SftpSession::dispatchPacket(const uint8_t* pkt, size_t len) {
    if (len < 5 || !state_ || !vfs_) return false;
    uint8_t type = pkt[0];
    const uint8_t* p = pkt + 1;
    const uint8_t* end = pkt + len;

    if (type == SSH_FXP_INIT) {
        uint8_t resp[5];
        resp[0] = SSH_FXP_VERSION;
        putU32(resp + 1, 3U);
        return sendPacket(resp, 5);
    }

    uint32_t req_id = getU32(p);
    p += 4;

    if (type == SSH_FXP_REALPATH) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        }
        if (req_path[0] == '\0' || strcmp(req_path, ".") == 0) {
            strncpy(norm, "/root", sizeof(norm) - 1);
            norm[sizeof(norm) - 1] = '\0';
        } else {
            VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        }
        uint64_t sz = 0;
        uint32_t mode = 0040755U;
        uint32_t mtime = 0;
        vfs_->statPath(norm, &sz, &mode, &mtime);

        uint8_t resp[420];
        size_t pos = 0;
        resp[pos++] = SSH_FXP_NAME;
        putU32(resp + pos, req_id); pos += 4;
        putU32(resp + pos, 1U); pos += 4;
        pos += putStr(resp + pos, norm);
        pos += putStr(resp + pos, norm);
        pos += encodeAttrs(resp + pos, sz, mode, mtime);
        return sendPacket(resp, pos);
    }

    if (type == SSH_FXP_STAT || type == SSH_FXP_LSTAT) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        }
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        uint64_t sz = 0;
        uint32_t mode = 0;
        uint32_t mtime = 0;
        if (!vfs_->statPath(norm, &sz, &mode, &mtime)) {
            return sendStatus(req_id, SSH_FX_NO_SUCH_FILE, "no such file");
        }
        uint8_t resp[64];
        resp[0] = SSH_FXP_ATTRS;
        putU32(resp + 1, req_id);
        size_t alen = encodeAttrs(resp + 5, sz, mode, mtime);
        return sendPacket(resp, 5 + alen);
    }

    if (type == SSH_FXP_FSTAT) {
        char hstr[8];
        int hlen = readStr(p, end, hstr, sizeof(hstr));
        SftpHandle* h = lookupHandle(hstr, hlen);
        if (!h || h->is_dir) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad handle");
        }
        uint64_t sz = 0;
        uint32_t mode = 0100644U;
        uint32_t mtime = 0;
        if (h->fp) {
            long cur = ftell(h->fp);
            fseek(h->fp, 0, SEEK_END);
            long end_pos = ftell(h->fp);
            if (cur >= 0) fseek(h->fp, cur, SEEK_SET);
            if (end_pos >= 0) sz = static_cast<uint64_t>(end_pos);
        } else if (h->mem_data) {
            sz = h->mem_len;
        }
        uint8_t resp[64];
        resp[0] = SSH_FXP_ATTRS;
        putU32(resp + 1, req_id);
        size_t alen = encodeAttrs(resp + 5, sz, mode, mtime);
        return sendPacket(resp, 5 + alen);
    }

    if (type == SSH_FXP_SETSTAT || type == SSH_FXP_FSETSTAT) {
        return sendStatus(req_id, SSH_FX_OK, "ok");
    }

    if (type == SSH_FXP_OPEN) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0 || static_cast<size_t>(end - p) < 4) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad open");
        }
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        uint32_t pflags = getU32(p);
        int slot = allocHandle();
        if (slot < 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "too many handles");
        }

        const bool want_write = (pflags & SSH_FXF_WRITE) != 0;
        const bool want_read  = (pflags & SSH_FXF_READ) != 0;
        if (want_write && vfs_->isVirtualPath(norm)) {
            return sendStatus(req_id, SSH_FX_PERMISSION_DENIED, "virtual path read-only");
        }

        FILE* fp = nullptr;
        uint8_t* mem_data = nullptr;
        size_t mem_len = 0;

        if (want_write) {
            const char* fmode = "wb";
            if (pflags & SSH_FXF_APPEND) {
                fmode = want_read ? "a+b" : "ab";
            } else if (pflags & SSH_FXF_TRUNC) {
                fmode = want_read ? "w+b" : "wb";
            } else {
                uint64_t existing_sz = 0;
                bool exists = vfs_->statPath(norm, &existing_sz, nullptr, nullptr);
                if (exists) {
                    fmode = "r+b";
                } else if (pflags & SSH_FXF_CREAT) {
                    fmode = "w+b";
                } else {
                    return sendStatus(req_id, SSH_FX_NO_SUCH_FILE, "no such file");
                }
            }
            fp = vfs_->openPosix(norm, fmode);
            if (!fp) {
                return sendStatus(req_id, SSH_FX_FAILURE, "open failed");
            }
        } else {
            fp = vfs_->openPosix(norm, "rb");
            if (!fp) {
                // Support reading synthetic /proc and /sys files via SFTP GET
                auto* tmp = static_cast<char*>(malloc(2048));
                if (tmp && vfs_->readFile(*state_, norm, tmp, 2048)) {
                    mem_len = strlen(tmp);
                    mem_data = reinterpret_cast<uint8_t*>(tmp);
                } else {
                    free(tmp);
                    return sendStatus(req_id, SSH_FX_NO_SUCH_FILE, "no such file");
                }
            }
        }

        SftpHandle& h = handles_[slot];
        memset(&h, 0, sizeof(h));
        h.used = true;
        h.is_dir = false;
        h.writable = want_write;
        strncpy(h.vpath, norm, sizeof(h.vpath) - 1);
        h.fp = fp;
        h.mem_data = mem_data;
        h.mem_len = mem_len;

        char hid[16];
        snprintf(hid, sizeof(hid), "%04d", slot);
        uint8_t resp[28];
        resp[0] = SSH_FXP_HANDLE;
        putU32(resp + 1, req_id);
        size_t n = putStr(resp + 5, hid);
        return sendPacket(resp, 5 + n);
    }

    if (type == SSH_FXP_READ) {
        char hstr[8];
        int hlen = readStr(p, end, hstr, sizeof(hstr));
        if (hlen < 0 || static_cast<size_t>(end - p) < 12) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad read");
        }
        SftpHandle* h = lookupHandle(hstr, hlen);
        if (!h || h->is_dir) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad handle");
        }
        uint64_t offset = getU64(p);
        uint32_t rlen = getU32(p + 8);
        if (rlen > 4096U) rlen = 4096U;

        auto* resp = static_cast<uint8_t*>(malloc(9 + rlen));
        if (!resp) return sendStatus(req_id, SSH_FX_FAILURE, "oom");

        size_t got = 0;
        if (h->fp) {
            if (fseek(h->fp, static_cast<long>(offset), SEEK_SET) == 0) {
                got = fread(resp + 9, 1, rlen, h->fp);
            }
        } else if (h->mem_data && offset < h->mem_len) {
            size_t avail = h->mem_len - static_cast<size_t>(offset);
            got = (rlen < avail) ? rlen : avail;
            memcpy(resp + 9, h->mem_data + offset, got);
        }

        if (got == 0) {
            free(resp);
            return sendStatus(req_id, SSH_FX_EOF, "eof");
        }
        resp[0] = SSH_FXP_DATA;
        putU32(resp + 1, req_id);
        putU32(resp + 5, static_cast<uint32_t>(got));
        bool ok = sendPacket(resp, 9 + got);
        free(resp);
        return ok;
    }

    if (type == SSH_FXP_WRITE) {
        char hstr[8];
        int hlen = readStr(p, end, hstr, sizeof(hstr));
        if (hlen < 0 || static_cast<size_t>(end - p) < 12) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad write");
        }
        SftpHandle* h = lookupHandle(hstr, hlen);
        if (!h || h->is_dir || !h->fp || !h->writable) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad handle");
        }
        uint64_t offset = getU64(p);
        uint32_t dlen = getU32(p + 8);
        p += 12;
        if (static_cast<size_t>(end - p) < dlen) {
            return sendStatus(req_id, SSH_FX_FAILURE, "truncated write");
        }
        if (fseek(h->fp, static_cast<long>(offset), SEEK_SET) != 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "seek failed");
        }
        if (dlen > 0 && fwrite(p, 1, dlen, h->fp) != dlen) {
            return sendStatus(req_id, SSH_FX_FAILURE, "write failed");
        }
        fflush(h->fp);
        return sendStatus(req_id, SSH_FX_OK, "ok");
    }

    if (type == SSH_FXP_CLOSE) {
        char hstr[8];
        int hlen = readStr(p, end, hstr, sizeof(hstr));
        SftpHandle* h = lookupHandle(hstr, hlen);
        if (!h) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad handle");
        }
        freeHandle(*h);
        return sendStatus(req_id, SSH_FX_OK, "closed");
    }

    if (type == SSH_FXP_OPENDIR) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        }
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        if (!vfs_->isDirectory(norm)) {
            return sendStatus(req_id, SSH_FX_NO_SUCH_FILE, "not a directory");
        }
        int slot = allocHandle();
        if (slot < 0) return sendStatus(req_id, SSH_FX_FAILURE, "no handles");

        constexpr size_t MAX_DIR = 32;
        auto* entries = static_cast<VfsDirEntry*>(malloc(sizeof(VfsDirEntry) * MAX_DIR));
        if (!entries) return sendStatus(req_id, SSH_FX_FAILURE, "oom");
        size_t count = vfs_->readDirEntries(norm, entries, MAX_DIR);

        SftpHandle& h = handles_[slot];
        memset(&h, 0, sizeof(h));
        h.used = true;
        h.is_dir = true;
        strncpy(h.vpath, norm, sizeof(h.vpath) - 1);
        h.dir_entries = entries;
        h.dir_count = count;
        h.dir_idx = 0;

        char hid[16];
        snprintf(hid, sizeof(hid), "%04d", slot);
        uint8_t resp[28];
        resp[0] = SSH_FXP_HANDLE;
        putU32(resp + 1, req_id);
        size_t n = putStr(resp + 5, hid);
        return sendPacket(resp, 5 + n);
    }

    if (type == SSH_FXP_READDIR) {
        char hstr[8];
        int hlen = readStr(p, end, hstr, sizeof(hstr));
        SftpHandle* h = lookupHandle(hstr, hlen);
        if (!h || !h->is_dir) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad dir handle");
        }
        if (h->dir_idx >= h->dir_count) {
            return sendStatus(req_id, SSH_FX_EOF, "eof");
        }
        const VfsDirEntry& e = h->dir_entries[h->dir_idx++];
        char longname[160];
        snprintf(longname, sizeof(longname), "%s   1 root root %8llu Jan  1 00:00 %s",
                 e.is_dir ? "drwxr-xr-x" : "-rw-r--r--",
                 static_cast<unsigned long long>(e.size), e.name);

        uint8_t resp[420];
        size_t pos = 0;
        resp[pos++] = SSH_FXP_NAME;
        putU32(resp + pos, req_id); pos += 4;
        putU32(resp + pos, 1U); pos += 4;
        pos += putStr(resp + pos, e.name);
        pos += putStr(resp + pos, longname);
        pos += encodeAttrs(resp + pos, e.size, e.mode, e.mtime);
        return sendPacket(resp, pos);
    }

    if (type == SSH_FXP_REMOVE) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        if (!vfs_->removeFile(*state_, norm)) return sendStatus(req_id, SSH_FX_NO_SUCH_FILE, "remove failed");
        return sendStatus(req_id, SSH_FX_OK, "removed");
    }

    if (type == SSH_FXP_MKDIR) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        if (!vfs_->mkdirPath(norm)) return sendStatus(req_id, SSH_FX_FAILURE, "mkdir failed");
        return sendStatus(req_id, SSH_FX_OK, "created");
    }

    if (type == SSH_FXP_RMDIR) {
        char req_path[160], norm[160];
        if (readStr(p, end, req_path, sizeof(req_path)) < 0) return sendStatus(req_id, SSH_FX_FAILURE, "bad path");
        VirtualFs::normalizePath("/root", req_path, norm, sizeof(norm));
        if (!vfs_->rmdirPath(norm)) return sendStatus(req_id, SSH_FX_FAILURE, "rmdir failed");
        return sendStatus(req_id, SSH_FX_OK, "rmdir ok");
    }

    if (type == SSH_FXP_RENAME) {
        char oldp[160], newp[160], old_norm[160], new_norm[160];
        if (readStr(p, end, oldp, sizeof(oldp)) < 0 || readStr(p, end, newp, sizeof(newp)) < 0) {
            return sendStatus(req_id, SSH_FX_FAILURE, "bad rename");
        }
        VirtualFs::normalizePath("/root", oldp, old_norm, sizeof(old_norm));
        VirtualFs::normalizePath("/root", newp, new_norm, sizeof(new_norm));
        if (!vfs_->renamePath(old_norm, new_norm)) return sendStatus(req_id, SSH_FX_FAILURE, "rename failed");
        return sendStatus(req_id, SSH_FX_OK, "renamed");
    }

    return sendStatus(req_id, SSH_FX_OP_UNSUPPORTED, "unsupported op");
}

bool SftpSession::feed(const uint8_t* data, size_t len) {
    if (!rx_buf_ || (!data && len > 0)) return false;
    if (rx_len_ + len > rx_cap_) {
        size_t new_cap = rx_len_ + len + 2048;
        if (new_cap > 36864U) return false;
        auto* nb = static_cast<uint8_t*>(realloc(rx_buf_, new_cap));
        if (!nb) return false;
        rx_buf_ = nb;
        rx_cap_ = new_cap;
    }
    if (len > 0) {
        memcpy(rx_buf_ + rx_len_, data, len);
        rx_len_ += len;
    }

    while (rx_len_ >= 4) {
        uint32_t pkt_len = getU32(rx_buf_);
        if (pkt_len == 0 || pkt_len > 36000U) return false;
        if (rx_len_ < 4U + pkt_len) break;
        if (!dispatchPacket(rx_buf_ + 4, pkt_len)) return false;
        size_t consumed = 4U + pkt_len;
        if (rx_len_ > consumed) {
            memmove(rx_buf_, rx_buf_ + consumed, rx_len_ - consumed);
        }
        rx_len_ -= consumed;
    }
    return true;
}

}  // namespace ssh
}  // namespace c6os
