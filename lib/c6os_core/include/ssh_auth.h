#ifndef C6OS_SSH_AUTH_H
#define C6OS_SSH_AUTH_H

#include <stddef.h>
#include <stdint.h>

namespace c6os {
namespace crypto {

struct Sha256Ctx {
    uint64_t total_len;
    uint32_t state[8];
    uint8_t buffer[64];
    size_t buf_len;
};

void sha256Init(Sha256Ctx& ctx);
void sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len);
void sha256Final(Sha256Ctx& ctx, uint8_t out[32]);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

void sha512(const uint8_t* data, size_t len, uint8_t out[64]);

void hmacSha256(const uint8_t* key, size_t key_len,
                const uint8_t* data, size_t data_len,
                uint8_t out[32]);

void computeSshPacketMac(const uint8_t key[32], uint32_t seq_num,
                         const uint8_t* pkt, size_t pkt_len,
                         uint8_t out_mac[32]);

bool constantTimeEqual(const void* a, const void* b, size_t len);

size_t base64Encode(const uint8_t* src, size_t src_len, char* out, size_t out_cap, bool strip_pad = false);
int base64Decode(const char* b64, size_t b64_len, uint8_t* out, size_t out_cap);

int x25519Shared(uint8_t out_shared[32], const uint8_t scalar[32], const uint8_t peer_pub[32]);
void x25519Base(uint8_t out_pub[32], const uint8_t scalar[32]);

void ed25519KeypairFromSeed(uint8_t out_pk[32], uint8_t out_sk[64], const uint8_t seed[32]);
void ed25519Sign(uint8_t out_sig[64], const uint8_t* msg, size_t msg_len, const uint8_t sk[64]);
int ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len, const uint8_t pk[32]);

// Verify RSA PKCS#1 v1.5 signature (rsa-sha2-256 or rsa-sha2-512) against an OpenSSH ssh-rsa pk_blob
bool rsaVerifyPkcs1Sha2(const uint8_t* pk_blob, size_t pk_blob_len,
                        const char* sig_alg,
                        const uint8_t* sig_raw, size_t sig_raw_len,
                        const uint8_t* msg, size_t msg_len);

// Salted SHA-256 root password hashing (1024 rounds) & constant-time verification
void hashPassword(const char* password, const uint8_t salt[16], uint8_t out_hash[32]);
bool verifyPassword(const char* password, const uint8_t salt[16], const uint8_t expected_hash[32]);

// Format default /etc/ssh/authorized_keys containing the 7 host pubkey SHA-256 digests
size_t formatDefaultAuthorizedKeys(char* out, size_t out_cap);

// Check whether pk_blob matches an entry in /etc/ssh/authorized_keys (supports both
// OpenSSH 'ssh-ed25519 <b64>' / 'ssh-rsa <b64>' lines and 'SHA256:<b64>' digest lines)
bool isPubkeyAuthorized(const char* authorized_keys_text,
                        const uint8_t* pk_blob, size_t pk_blob_len);

// Build RFC 4252 §7 publickey signed data payload
size_t buildUserauthSignedData(const uint8_t session_id[32],
                               const char* username,
                               const char* service,
                               const char* alg_name,
                               const uint8_t* pk_blob, size_t pk_blob_len,
                               uint8_t* out_buf, size_t out_cap);

// Cryptographically verify RFC 4252 §7 publickey signature (ssh-ed25519, rsa-sha2-256, rsa-sha2-512)
bool verifyUserauthSignature(const uint8_t session_id[32],
                             const char* username,
                             const char* service,
                             const char* alg_name,
                             const uint8_t* pk_blob, size_t pk_blob_len,
                             const uint8_t* sig_blob, size_t sig_blob_len);

}  // namespace crypto
}  // namespace c6os

#endif  // C6OS_SSH_AUTH_H
