#include "ssh_packet.h"

namespace c6os {
namespace ssh {

bool SshWriter::putByte(uint8_t b) {
    if (len_ + 1 > capacity_) {
        overflow_ = true;
        return false;
    }
    buf_[len_++] = b;
    return true;
}

bool SshWriter::putBool(bool v) {
    return putByte(v ? 1 : 0);
}

bool SshWriter::putUint32(uint32_t v) {
    if (len_ + 4 > capacity_) {
        overflow_ = true;
        return false;
    }
    buf_[len_++] = static_cast<uint8_t>((v >> 24) & 0xFF);
    buf_[len_++] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf_[len_++] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf_[len_++] = static_cast<uint8_t>(v & 0xFF);
    return true;
}

bool SshWriter::putRaw(const uint8_t* data, size_t len) {
    if (len == 0) return true;
    if (!data || len_ + len > capacity_) {
        overflow_ = true;
        return false;
    }
    memcpy(buf_ + len_, data, len);
    len_ += len;
    return true;
}

bool SshWriter::putString(const uint8_t* data, size_t len) {
    if (!putUint32(static_cast<uint32_t>(len))) return false;
    return putRaw(data, len);
}

bool SshWriter::putCString(const char* str) {
    if (!str) return putUint32(0);
    return putString(reinterpret_cast<const uint8_t*>(str), strlen(str));
}

bool SshWriter::putMpint(const uint8_t* be_bytes, size_t len) {
    if (!be_bytes || len == 0) {
        return putUint32(0);
    }
    size_t start = 0;
    while (start < len && be_bytes[start] == 0x00) {
        ++start;
    }
    if (start == len) {
        return putUint32(0);
    }
    size_t sig_len = len - start;
    bool need_pad = (be_bytes[start] & 0x80) != 0;
    uint32_t total_len = static_cast<uint32_t>(sig_len + (need_pad ? 1 : 0));
    if (!putUint32(total_len)) return false;
    if (need_pad) {
        if (!putByte(0x00)) return false;
    }
    return putRaw(be_bytes + start, sig_len);
}

bool SshReader::getByte(uint8_t& out) {
    if (pos_ + 1 > len_) return false;
    out = buf_[pos_++];
    return true;
}

bool SshReader::getBool(bool& out) {
    uint8_t b = 0;
    if (!getByte(b)) return false;
    out = (b != 0);
    return true;
}

bool SshReader::getUint32(uint32_t& out) {
    if (pos_ + 4 > len_) return false;
    out = (static_cast<uint32_t>(buf_[pos_]) << 24) |
          (static_cast<uint32_t>(buf_[pos_ + 1]) << 16) |
          (static_cast<uint32_t>(buf_[pos_ + 2]) << 8) |
          static_cast<uint32_t>(buf_[pos_ + 3]);
    pos_ += 4;
    return true;
}

bool SshReader::getRaw(const uint8_t*& out_ptr, size_t count) {
    if (pos_ + count > len_) return false;
    out_ptr = buf_ + pos_;
    pos_ += count;
    return true;
}

bool SshReader::getString(const uint8_t*& out_ptr, size_t& out_len) {
    uint32_t slen = 0;
    if (!getUint32(slen)) return false;
    if (pos_ + slen > len_) return false;
    out_ptr = buf_ + pos_;
    out_len = slen;
    pos_ += slen;
    return true;
}

bool SshReader::getCString(char* out_str, size_t max_len) {
    if (!out_str || max_len == 0) return false;
    const uint8_t* ptr = nullptr;
    size_t slen = 0;
    if (!getString(ptr, slen)) return false;
    size_t copy_len = (slen < max_len - 1) ? slen : (max_len - 1);
    if (copy_len > 0) {
        memcpy(out_str, ptr, copy_len);
    }
    out_str[copy_len] = '\0';
    return true;
}

size_t framePacket(const uint8_t* payload, size_t payload_len,
                   size_t block_size, const uint8_t* random_pad,
                   uint8_t* out_packet, size_t out_capacity) {
    if (block_size < 8) block_size = 8;
    // Total packet bytes = 4 (packet_length) + 1 (padding_length) + payload_len + pad_len
    size_t unpadded = 4 + 1 + payload_len;
    size_t rem = unpadded % block_size;
    size_t pad_len = (rem == 0) ? 0 : (block_size - rem);
    while (pad_len < 4) {
        pad_len += block_size;
    }

    size_t total_len = unpadded + pad_len;
    if (!out_packet || total_len > out_capacity) {
        return 0;
    }

    uint32_t pkt_len_field = static_cast<uint32_t>(1 + payload_len + pad_len);
    SshWriter w(out_packet, out_capacity);
    w.putUint32(pkt_len_field);
    w.putByte(static_cast<uint8_t>(pad_len));
    w.putRaw(payload, payload_len);
    if (random_pad) {
        w.putRaw(random_pad, pad_len);
    } else {
        for (size_t i = 0; i < pad_len; ++i) {
            w.putByte(static_cast<uint8_t>(i & 0xFF));
        }
    }
    return w.overflow() ? 0 : w.length();
}

size_t buildServerKexInitPayload(const uint8_t cookie[16], uint8_t* out_buf, size_t capacity) {
    SshWriter w(out_buf, capacity);
    w.putByte(SSH_MSG_KEXINIT);
    w.putRaw(cookie, 16);
    // 1. kex_algorithms
    w.putCString("curve25519-sha256,curve25519-sha256@libssh.org");
    // 2. server_host_key_algorithms
    w.putCString("ssh-ed25519");
    // 3. encryption_algorithms_client_to_server
    w.putCString("aes128-ctr");
    // 4. encryption_algorithms_server_to_client
    w.putCString("aes128-ctr");
    // 5. mac_algorithms_client_to_server
    w.putCString("hmac-sha2-256");
    // 6. mac_algorithms_server_to_client
    w.putCString("hmac-sha2-256");
    // 7. compression_algorithms_client_to_server
    w.putCString("none");
    // 8. compression_algorithms_server_to_client
    w.putCString("none");
    // 9. languages_client_to_server
    w.putCString("");
    // 10. languages_server_to_client
    w.putCString("");
    // first_kex_packet_follows
    w.putBool(false);
    // reserved uint32(0)
    w.putUint32(0);
    return w.overflow() ? 0 : w.length();
}

size_t encodeEd25519HostKeyBlob(const uint8_t pubkey[32], uint8_t* out_buf, size_t capacity) {
    SshWriter w(out_buf, capacity);
    w.putCString("ssh-ed25519");
    w.putString(pubkey, 32);
    return w.overflow() ? 0 : w.length();
}

size_t encodeEd25519SignatureBlob(const uint8_t sig[64], uint8_t* out_buf, size_t capacity) {
    SshWriter w(out_buf, capacity);
    w.putCString("ssh-ed25519");
    w.putString(sig, 64);
    return w.overflow() ? 0 : w.length();
}

}  // namespace ssh
}  // namespace c6os
