#ifndef SSH_PACKET_H
#define SSH_PACKET_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace c6os {
namespace ssh {

// RFC 4253 / 4252 / 4254 Message Constants
constexpr uint8_t SSH_MSG_DISCONNECT                = 1;
constexpr uint8_t SSH_MSG_IGNORE                    = 2;
constexpr uint8_t SSH_MSG_UNIMPLEMENTED             = 3;
constexpr uint8_t SSH_MSG_DEBUG                     = 4;
constexpr uint8_t SSH_MSG_SERVICE_REQUEST           = 5;
constexpr uint8_t SSH_MSG_SERVICE_ACCEPT            = 6;
constexpr uint8_t SSH_MSG_EXT_INFO                  = 7;

constexpr uint8_t SSH_MSG_KEXINIT                   = 20;
constexpr uint8_t SSH_MSG_NEWKEYS                   = 21;
constexpr uint8_t SSH_MSG_KEX_ECDH_INIT             = 30;
constexpr uint8_t SSH_MSG_KEX_ECDH_REPLY            = 31;

constexpr uint8_t SSH_MSG_USERAUTH_REQUEST          = 50;
constexpr uint8_t SSH_MSG_USERAUTH_FAILURE          = 51;
constexpr uint8_t SSH_MSG_USERAUTH_SUCCESS          = 52;
constexpr uint8_t SSH_MSG_USERAUTH_BANNER           = 53;

constexpr uint8_t SSH_MSG_GLOBAL_REQUEST            = 80;
constexpr uint8_t SSH_MSG_REQUEST_SUCCESS           = 81;
constexpr uint8_t SSH_MSG_REQUEST_FAILURE           = 82;

constexpr uint8_t SSH_MSG_CHANNEL_OPEN              = 90;
constexpr uint8_t SSH_MSG_CHANNEL_OPEN_CONFIRMATION = 91;
constexpr uint8_t SSH_MSG_CHANNEL_OPEN_FAILURE      = 92;
constexpr uint8_t SSH_MSG_CHANNEL_WINDOW_ADJUST     = 93;
constexpr uint8_t SSH_MSG_CHANNEL_DATA              = 94;
constexpr uint8_t SSH_MSG_CHANNEL_EXTENDED_DATA     = 95;
constexpr uint8_t SSH_MSG_CHANNEL_EOF               = 96;
constexpr uint8_t SSH_MSG_CHANNEL_CLOSE             = 97;
constexpr uint8_t SSH_MSG_CHANNEL_REQUEST           = 98;
constexpr uint8_t SSH_MSG_CHANNEL_SUCCESS           = 99;
constexpr uint8_t SSH_MSG_CHANNEL_FAILURE           = 100;

constexpr const char* SERVER_VERSION_STRING = "SSH-2.0-OpenSSH_9.6_c6os";

class SshWriter {
public:
    SshWriter(uint8_t* buf, size_t capacity)
        : buf_(buf), capacity_(capacity), len_(0), overflow_(false) {}

    void reset() {
        len_ = 0;
        overflow_ = false;
    }

    size_t length() const { return len_; }
    const uint8_t* data() const { return buf_; }
    bool overflow() const { return overflow_; }

    bool putByte(uint8_t b);
    bool putBool(bool v);
    bool putUint32(uint32_t v);
    bool putRaw(const uint8_t* data, size_t len);
    bool putString(const uint8_t* data, size_t len);
    bool putCString(const char* str);
    bool putMpint(const uint8_t* be_bytes, size_t len);

private:
    uint8_t* buf_;
    size_t capacity_;
    size_t len_;
    bool overflow_;
};

class SshReader {
public:
    SshReader(const uint8_t* buf, size_t len)
        : buf_(buf), len_(len), pos_(0) {}

    size_t remaining() const { return (pos_ <= len_) ? (len_ - pos_) : 0; }
    size_t position() const { return pos_; }

    bool getByte(uint8_t& out);
    bool getBool(bool& out);
    bool getUint32(uint32_t& out);
    bool getRaw(const uint8_t*& out_ptr, size_t count);
    bool getString(const uint8_t*& out_ptr, size_t& out_len);
    bool getCString(char* out_str, size_t max_len);

private:
    const uint8_t* buf_;
    size_t len_;
    size_t pos_;
};

// Computes RFC 4253 binary packet padding and writes [packet_length][padding_length][payload][padding]
size_t framePacket(const uint8_t* payload, size_t payload_len,
                   size_t block_size, const uint8_t* random_pad,
                   uint8_t* out_packet, size_t out_capacity);

// Builds server SSH_MSG_KEXINIT payload
size_t buildServerKexInitPayload(const uint8_t cookie[16], uint8_t* out_buf, size_t capacity);

// Encodes ssh-ed25519 public key blob: string("ssh-ed25519") || string(pubkey_32)
size_t encodeEd25519HostKeyBlob(const uint8_t pubkey[32], uint8_t* out_buf, size_t capacity);

// Encodes ssh-ed25519 signature blob: string("ssh-ed25519") || string(sig_64)
size_t encodeEd25519SignatureBlob(const uint8_t sig[64], uint8_t* out_buf, size_t capacity);

}  // namespace ssh
}  // namespace c6os

#endif  // SSH_PACKET_H
