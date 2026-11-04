#include "ssh_auth.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c6os_types.h"
#include "ssh_packet.h"

#ifdef ESP_PLATFORM
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>
#endif

namespace c6os {
namespace crypto {

namespace {

// Pre-seeded SSH public key SHA-256 digests from user's machines
struct SeededHostKeyDigest {
    const char* comment;
    uint8_t digest[32];
};

const SeededHostKeyDigest SEEDED_PUBKEY_DIGESTS[7] = {
    {"fcurrie@ubuntu-dev (ssh-rsa)",
     {0x2b, 0xa2, 0x43, 0xf4, 0x15, 0x9c, 0x03, 0xa2, 0x66, 0x18, 0x91, 0x6a, 0x65, 0x3c, 0x7b, 0x73,
      0xca, 0x1c, 0xda, 0xca, 0xfd, 0x0b, 0x0c, 0xc5, 0x0d, 0x19, 0xa1, 0x9a, 0x0f, 0xb4, 0x92, 0x06}},
    {"fcurrie@xubuntu (ssh-rsa)",
     {0x98, 0xe2, 0xfe, 0xd4, 0x6c, 0x58, 0xd4, 0x23, 0x35, 0x14, 0x1b, 0x92, 0x4f, 0x81, 0xbe, 0xb7,
      0xa3, 0x9e, 0x3c, 0x0d, 0xf8, 0x96, 0x01, 0xbb, 0x98, 0x0d, 0x58, 0x7f, 0x88, 0x76, 0x79, 0x16}},
    {"frank@Kitchen-Windows (ssh-ed25519)",
     {0xf1, 0x1d, 0x03, 0x87, 0x87, 0x11, 0x81, 0xcb, 0xc5, 0x75, 0xd8, 0xf4, 0x86, 0x2f, 0xe6, 0x3b,
      0x51, 0xeb, 0x98, 0xee, 0xca, 0x68, 0x27, 0x0b, 0xb8, 0x74, 0x41, 0x52, 0xb6, 0xa8, 0x6f, 0xf7}},
    {"fcurrie@fcurrie-Surface-Go-2 (ssh-ed25519)",
     {0xd1, 0xd7, 0xb1, 0x28, 0x17, 0x00, 0x5e, 0x94, 0x5e, 0xe6, 0x1d, 0x4e, 0xec, 0xe2, 0x8d, 0x29,
      0x58, 0x68, 0x46, 0x9d, 0xfb, 0xa0, 0xa5, 0x3a, 0xe2, 0xe6, 0xd6, 0x95, 0xfb, 0x47, 0x29, 0xc2}},
    {"fcurrie@remote-surface (ssh-ed25519)",
     {0xbe, 0x65, 0x05, 0x0d, 0x55, 0x21, 0x66, 0x38, 0x82, 0x83, 0xf5, 0xe5, 0x7f, 0xbb, 0xed, 0x3f,
      0x68, 0x63, 0xfc, 0xae, 0x83, 0x4e, 0xd7, 0x19, 0x04, 0xc0, 0x02, 0x06, 0x04, 0x25, 0x6d, 0x0a}},
    {"frank@crostini (ssh-ed25519)",
     {0x00, 0x06, 0x90, 0x3e, 0xd7, 0x7e, 0xbc, 0x8c, 0x51, 0xac, 0x0e, 0x29, 0xa2, 0xcb, 0x83, 0x77,
      0x44, 0x50, 0xb1, 0x78, 0x50, 0x39, 0x41, 0xdd, 0xa4, 0xe9, 0xe9, 0x28, 0xbb, 0x90, 0xfe, 0xde}},
    {"fcurrie@surface-go2-m3 (ssh-rsa)",
     {0x39, 0x4e, 0x43, 0x20, 0xa1, 0x7d, 0x09, 0x4e, 0x1e, 0x22, 0x80, 0x46, 0x1d, 0x3c, 0x67, 0x6c,
      0x99, 0x47, 0x7a, 0x37, 0x0b, 0xb4, 0x75, 0xe5, 0xa5, 0xca, 0xe8, 0xa8, 0x81, 0x7d, 0x20, 0x55}}
};

// Portable FIPS 180-4 SHA-256 transform (used by Sha256Ctx on both host & device)
static const uint32_t K256[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

inline uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32U - n));
}

void sha256Block(uint32_t st[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}

#ifndef ESP_PLATFORM
// Portable FIPS 180-4 SHA-512 for host native unit tests (ESP_PLATFORM uses mbedtls_sha512)
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

inline uint64_t rotr64(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64U - n));
}

void sha512Block(uint64_t st[8], const uint8_t block[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        uint64_t v = 0;
        for (int b = 0; b < 8; ++b) {
            v = (v << 8) | block[i * 8 + b];
        }
        w[i] = v;
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = st[0], b = st[1], c = st[2], d = st[3];
    uint64_t e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    st[4] += e; st[5] += f; st[6] += g; st[7] += h;
}
#endif

}  // namespace

void sha256Init(Sha256Ctx& ctx) {
    ctx.total_len = 0;
    ctx.buf_len = 0;
    ctx.state[0] = 0x6a09e667UL;
    ctx.state[1] = 0xbb67ae85UL;
    ctx.state[2] = 0x3c6ef372UL;
    ctx.state[3] = 0xa54ff53aUL;
    ctx.state[4] = 0x510e527fUL;
    ctx.state[5] = 0x9b05688cUL;
    ctx.state[6] = 0x1f83d9abUL;
    ctx.state[7] = 0x5be0cd19UL;
}

void sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    ctx.total_len += len;
    size_t idx = 0;
    if (ctx.buf_len > 0) {
        size_t take = 64 - ctx.buf_len;
        if (take > len) take = len;
        memcpy(ctx.buffer + ctx.buf_len, data, take);
        ctx.buf_len += take;
        idx += take;
        if (ctx.buf_len == 64) {
            sha256Block(ctx.state, ctx.buffer);
            ctx.buf_len = 0;
        }
    }
    while (idx + 64 <= len) {
        sha256Block(ctx.state, data + idx);
        idx += 64;
    }
    if (idx < len) {
        ctx.buf_len = len - idx;
        memcpy(ctx.buffer, data + idx, ctx.buf_len);
    }
}

void sha256Final(Sha256Ctx& ctx, uint8_t out[32]) {
    uint64_t bit_len = ctx.total_len * 8ULL;
    uint8_t pad = 0x80;
    sha256Update(ctx, &pad, 1);
    uint8_t zero = 0x00;
    while (ctx.buf_len != 56) {
        sha256Update(ctx, &zero, 1);
    }
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) {
        len_be[7 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFU);
    }
    sha256Update(ctx, len_be, 8);
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xFFU);
        out[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xFFU);
        out[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xFFU);
        out[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] & 0xFFU);
    }
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
#ifdef ESP_PLATFORM
    mbedtls_sha256(data, len, out, 0);
#else
    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, data, len);
    sha256Final(ctx, out);
#endif
}

void sha512(const uint8_t* data, size_t len, uint8_t out[64]) {
#ifdef ESP_PLATFORM
    mbedtls_sha512(data, len, out, 0);
#else
    uint64_t st[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };
    size_t idx = 0;
    while (idx + 128 <= len) {
        sha512Block(st, data + idx);
        idx += 128;
    }
    uint8_t tail[256];
    memset(tail, 0, sizeof(tail));
    size_t rem = len - idx;
    if (rem > 0) memcpy(tail, data + idx, rem);
    tail[rem] = 0x80;
    size_t blocks = (rem + 1 + 16 > 128) ? 2 : 1;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8ULL;
    size_t len_off = blocks * 128 - 8;
    for (int i = 0; i < 8; ++i) {
        tail[len_off + 7 - i] = static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFFU);
    }
    for (size_t b = 0; b < blocks; ++b) {
        sha512Block(st, tail + b * 128);
    }
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 8; ++b) {
            out[i * 8 + b] = static_cast<uint8_t>((st[i] >> ((7 - b) * 8)) & 0xFFU);
        }
    }
#endif
}

void hmacSha256(const uint8_t* key, size_t key_len,
                const uint8_t* data, size_t data_len,
                uint8_t out[32]) {
#ifdef ESP_PLATFORM
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, key, key_len, data, data_len, out);
#else
    uint8_t k_pad[64];
    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 64) {
        sha256(key, key_len, k_pad);
    } else if (key && key_len > 0) {
        memcpy(k_pad, key, key_len);
    }
    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = k_pad[i] ^ 0x36U;
        opad[i] = k_pad[i] ^ 0x5CU;
    }
    uint8_t inner[32];
    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, ipad, 64);
    sha256Update(ctx, data, data_len);
    sha256Final(ctx, inner);

    sha256Init(ctx);
    sha256Update(ctx, opad, 64);
    sha256Update(ctx, inner, 32);
    sha256Final(ctx, out);
#endif
}

void computeSshPacketMac(const uint8_t key[32], uint32_t seq_num,
                         const uint8_t* pkt, size_t pkt_len,
                         uint8_t out_mac[32]) {
    uint8_t seq_be[4] = {
        static_cast<uint8_t>((seq_num >> 24) & 0xFFU),
        static_cast<uint8_t>((seq_num >> 16) & 0xFFU),
        static_cast<uint8_t>((seq_num >> 8) & 0xFFU),
        static_cast<uint8_t>(seq_num & 0xFFU)
    };
    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < 32; ++i) {
        ipad[i] = key[i] ^ 0x36U;
        opad[i] = key[i] ^ 0x5CU;
    }
    memset(ipad + 32, 0x36U, 32);
    memset(opad + 32, 0x5CU, 32);

    uint8_t inner[32];
    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, ipad, 64);
    sha256Update(ctx, seq_be, 4);
    sha256Update(ctx, pkt, pkt_len);
    sha256Final(ctx, inner);

    sha256Init(ctx);
    sha256Update(ctx, opad, 64);
    sha256Update(ctx, inner, 32);
    sha256Final(ctx, out_mac);
}

bool constantTimeEqual(const void* a, const void* b, size_t len) {
    const volatile uint8_t* pa = static_cast<const volatile uint8_t*>(a);
    const volatile uint8_t* pb = static_cast<const volatile uint8_t*>(b);
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(pa[i] ^ pb[i]);
    }
    return diff == 0;
}

size_t base64Encode(const uint8_t* src, size_t src_len, char* out, size_t out_cap, bool strip_pad) {
    static const char B64_TAB[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!out || out_cap == 0) return 0;
    size_t pos = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        uint32_t v = static_cast<uint32_t>(src[i]) << 16;
        if (i + 1 < src_len) v |= static_cast<uint32_t>(src[i + 1]) << 8;
        if (i + 2 < src_len) v |= static_cast<uint32_t>(src[i + 2]);

        char quad[4];
        quad[0] = B64_TAB[(v >> 18) & 0x3FU];
        quad[1] = B64_TAB[(v >> 12) & 0x3FU];
        quad[2] = (i + 1 < src_len) ? B64_TAB[(v >> 6) & 0x3FU] : '=';
        quad[3] = (i + 2 < src_len) ? B64_TAB[v & 0x3FU] : '=';

        for (int q = 0; q < 4; ++q) {
            if (strip_pad && quad[q] == '=') break;
            if (pos + 1 >= out_cap) {
                out[pos] = '\0';
                return pos;
            }
            out[pos++] = quad[q];
        }
    }
    out[pos] = '\0';
    return pos;
}

int base64Decode(const char* b64, size_t b64_len, uint8_t* out, size_t out_cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t out_pos = 0;
    for (size_t i = 0; i < b64_len; ++i) {
        char c = b64[i];
        if (c == '=') break;
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = -1;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        if (v < 0) return -1;

        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_pos >= out_cap) return -1;
            out[out_pos++] = static_cast<uint8_t>((acc >> bits) & 0xFFU);
        }
    }
    return static_cast<int>(out_pos);
}

// --- Compact constant-time X25519 (RFC 7748) & Ed25519 (RFC 8032) ---
namespace {

using gf = int64_t[16];

const gf GF0 = {0};
const gf GF1 = {1};
const gf D_ED = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
const gf D2_ED = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
const gf X_BASE = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
const gf Y_BASE = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
const gf I_SQRT = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};
const uint64_t L_ORDER[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= static_cast<int64_t>(static_cast<uint64_t>(c) << 16);
    }
}

void sel25519(gf p, gf q, int b) {
    int64_t c = ~static_cast<int64_t>(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = static_cast<int>((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i]     = static_cast<uint8_t>(t[i] & 0xff);
        o[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return constantTimeEqual(c, d, 32) ? 0 : 1;
}

uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1U;
}

void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) {
        o[i] = static_cast<int64_t>(n[2 * i]) + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    }
    o[15] &= 0x7fff;
}

void gfAdd(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void gfSub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void gfMul(gf o, const gf a, const gf b) {
    int64_t t[31] = {0};
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

void gfSqr(gf o, const gf a) {
    gfMul(o, a, a);
}

void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        gfSqr(c, c);
        if (a != 2 && a != 4) gfMul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 250; a >= 0; --a) {
        gfSqr(c, c);
        if (a != 1) gfMul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void edAdd(gf p[4], gf q[4]) {
    gf a, b, c, d, t;
    gfSub(a, p[1], p[0]);
    gfSub(t, q[1], q[0]);
    gfMul(a, a, t);
    gfAdd(b, p[0], p[1]);
    gfAdd(t, q[0], q[1]);
    gfMul(b, b, t);
    gfMul(c, p[3], q[3]);
    gfMul(c, c, D2_ED);
    gfMul(d, p[2], q[2]);
    gfAdd(d, d, d);
    gfSub(t, b, a);
    gfAdd(b, b, a);
    memcpy(a, t, sizeof(gf));
    gfSub(t, d, c);
    gfAdd(d, d, c);
    memcpy(c, t, sizeof(gf));
    gfMul(p[0], a, c);
    gfMul(p[1], b, d);
    gfMul(p[2], d, c);
    gfMul(p[3], a, b);
}

void cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; ++i) sel25519(p[i], q[i], b);
}

void edPack(uint8_t* r, gf p[4]) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    gfMul(tx, p[0], zi);
    gfMul(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= static_cast<uint8_t>(par25519(tx) << 7);
}

void edScalarmult(gf p[4], gf q[4], const uint8_t* s) {
    memcpy(p[0], GF0, sizeof(gf));
    memcpy(p[1], GF1, sizeof(gf));
    memcpy(p[2], GF1, sizeof(gf));
    memcpy(p[3], GF0, sizeof(gf));
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1U;
        cswap(p, q, b);
        edAdd(q, p);
        edAdd(p, p);
        cswap(p, q, b);
    }
}

void edScalarbase(gf p[4], const uint8_t* s) {
    gf q[4];
    memcpy(q[0], X_BASE, sizeof(gf));
    memcpy(q[1], Y_BASE, sizeof(gf));
    memcpy(q[2], GF1, sizeof(gf));
    gfMul(q[3], X_BASE, Y_BASE);
    edScalarmult(p, q, s);
}

void modL(uint8_t* r, int64_t x[64]) {
    int64_t carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        for (int j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * static_cast<int64_t>(L_ORDER[j - (i - 32)]);
            carry = (x[j] + 128) >> 8;
            x[j] -= static_cast<int64_t>(static_cast<uint64_t>(carry) << 8);
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * static_cast<int64_t>(L_ORDER[j]);
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) {
        x[j] -= carry * static_cast<int64_t>(L_ORDER[j]);
    }
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<uint8_t>(x[i] & 255);
    }
}

void reduce(uint8_t* r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = static_cast<int64_t>(r[i]);
    memset(r, 0, 64);
    modL(r, x);
}

int unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    memcpy(r[2], GF1, sizeof(gf));
    unpack25519(r[1], p);
    gfSqr(num, r[1]);
    gfMul(den, num, D_ED);
    gfSub(num, num, r[2]);
    gfAdd(den, r[2], den);

    gfSqr(den2, den);
    gfSqr(den4, den2);
    gfMul(den6, den4, den2);
    gfMul(t, den6, num);
    gfMul(t, t, den);

    pow2523(t, t);
    gfMul(t, t, num);
    gfMul(t, t, den);
    gfMul(t, t, den);
    gfMul(r[0], t, den);

    gfSqr(chk, r[0]);
    gfMul(chk, chk, den);
    if (neq25519(chk, num)) gfMul(r[0], r[0], I_SQRT);

    gfSqr(chk, r[0]);
    gfMul(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) gfSub(r[0], GF0, r[0]);

    gfMul(r[3], r[0], r[1]);
    return 0;
}

}  // namespace

int x25519Shared(uint8_t out_shared[32], const uint8_t scalar[32], const uint8_t peer_pub[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    static const gf C121665 = {0xdb41, 1};

    memcpy(z, scalar, 32);
    z[31] = static_cast<uint8_t>((z[31] & 127U) | 64U);
    z[0] &= 248U;

    unpack25519(x, peer_pub);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;

    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        gfAdd(e, a, c);
        gfSub(a, a, c);
        gfAdd(c, b, d);
        gfSub(b, b, d);
        gfSqr(d, e);
        gfSqr(f, a);
        gfMul(a, c, a);
        gfMul(c, b, e);
        gfAdd(e, a, c);
        gfSub(a, a, c);
        gfSqr(b, a);
        gfSub(c, d, f);
        gfMul(a, c, C121665);
        gfAdd(a, a, d);
        gfMul(c, c, a);
        gfMul(a, d, f);
        gfMul(d, b, x);
        gfSqr(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }

    inv25519(c, c);
    gfMul(a, a, c);
    pack25519(out_shared, a);

    uint8_t acc = 0;
    for (int i = 0; i < 32; ++i) acc |= out_shared[i];
    return (acc == 0) ? -1 : 0;
}

void x25519Base(uint8_t out_pub[32], const uint8_t scalar[32]) {
    uint8_t base[32] = {9};
    x25519Shared(out_pub, scalar, base);
}

void ed25519KeypairFromSeed(uint8_t out_pk[32], uint8_t out_sk[64], const uint8_t seed[32]) {
    uint8_t d[64];
    gf p[4];
    sha512(seed, 32, d);
    d[0] &= 248U;
    d[31] &= 127U;
    d[31] |= 64U;

    edScalarbase(p, d);
    edPack(out_pk, p);

    memcpy(out_sk, seed, 32);
    memcpy(out_sk + 32, out_pk, 32);
    memset(d, 0, sizeof(d));
}

void ed25519Sign(uint8_t out_sig[64], const uint8_t* msg, size_t msg_len, const uint8_t sk[64]) {
    uint8_t d[64], r[64], hram[64];
    int64_t x[64];
    gf p[4];

    sha512(sk, 32, d);
    d[0] &= 248U;
    d[31] &= 127U;
    d[31] |= 64U;

    uint8_t* buf = static_cast<uint8_t*>(malloc(64 + msg_len));
    if (!buf) {
        memset(out_sig, 0, 64);
        return;
    }

    memcpy(buf, d + 32, 32);
    if (msg_len > 0) memcpy(buf + 32, msg, msg_len);
    sha512(buf, 32 + msg_len, r);
    reduce(r);

    edScalarbase(p, r);
    edPack(out_sig, p);

    memcpy(buf, out_sig, 32);
    memcpy(buf + 32, sk + 32, 32);
    if (msg_len > 0) memcpy(buf + 64, msg, msg_len);
    sha512(buf, 64 + msg_len, hram);
    reduce(hram);

    memset(x, 0, sizeof(x));
    for (int i = 0; i < 32; ++i) x[i] = static_cast<int64_t>(r[i]);
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += static_cast<int64_t>(hram[i]) * static_cast<int64_t>(d[j]);
        }
    }
    modL(out_sig + 32, x);

    free(buf);
    memset(d, 0, sizeof(d));
}

int ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t msg_len, const uint8_t pk[32]) {
    uint8_t t[32], h[64];
    gf p[4], q[4];

    if (sig[63] & 224U) return -1;
    if (unpackneg(q, pk) != 0) return -1;

    uint8_t* buf = static_cast<uint8_t*>(malloc(64 + msg_len));
    if (!buf) return -1;

    memcpy(buf, sig, 32);
    memcpy(buf + 32, pk, 32);
    if (msg_len > 0) memcpy(buf + 64, msg, msg_len);
    sha512(buf, 64 + msg_len, h);
    free(buf);
    reduce(h);

    edScalarmult(p, q, h);
    edScalarbase(q, sig + 32);
    edAdd(p, q);
    edPack(t, p);

    return constantTimeEqual(sig, t, 32) ? 0 : -1;
}

// --- RSA PKCS#1 v1.5 Signature Verification (RFC 8332: rsa-sha2-256 & rsa-sha2-512) ---
namespace {

constexpr size_t RSA_MAX_BYTES = 512;  // Up to 4096-bit RSA keys
constexpr size_t RSA_MAX_LIMBS = RSA_MAX_BYTES / 4;

void bytesToLimbs(const uint8_t* be, size_t len, uint32_t* limbs, size_t max_limbs) {
    memset(limbs, 0, max_limbs * sizeof(uint32_t));
    for (size_t i = 0; i < len && (i / 4) < max_limbs; ++i) {
        uint8_t b = be[len - 1 - i];
        limbs[i / 4] |= static_cast<uint32_t>(b) << ((i % 4) * 8U);
    }
}

bool limbsGreaterOrEqual(const uint32_t* rem, const uint32_t* mod, size_t nlimbs) {
    if (rem[nlimbs] != 0) return true;
    for (size_t k = nlimbs; k > 0; --k) {
        if (rem[k - 1] != mod[k - 1]) {
            return rem[k - 1] > mod[k - 1];
        }
    }
    return true;
}

void bigMulMod(uint32_t* out, const uint32_t* a, const uint32_t* b, const uint32_t* mod, size_t nlimbs) {
    uint32_t prod[2 * RSA_MAX_LIMBS + 2] = {0};
    for (size_t i = 0; i < nlimbs; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < nlimbs; ++j) {
            uint64_t cur = static_cast<uint64_t>(prod[i + j]) +
                           static_cast<uint64_t>(a[i]) * static_cast<uint64_t>(b[j]) + carry;
            prod[i + j] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        prod[i + nlimbs] = static_cast<uint32_t>(carry);
    }

    uint32_t rem[RSA_MAX_LIMBS + 1] = {0};
    const size_t total_bits = 2 * nlimbs * 32;
    for (size_t bi = total_bits; bi > 0; --bi) {
        size_t bit_idx = bi - 1;
        uint32_t in_bit = (prod[bit_idx >> 5] >> (bit_idx & 31U)) & 1U;
        for (size_t k = nlimbs; k > 0; --k) {
            rem[k] = (rem[k] << 1) | (rem[k - 1] >> 31);
        }
        rem[0] = (rem[0] << 1) | in_bit;

        if (limbsGreaterOrEqual(rem, mod, nlimbs)) {
            uint64_t borrow = 0;
            for (size_t k = 0; k <= nlimbs; ++k) {
                uint64_t m_k = (k < nlimbs) ? mod[k] : 0ULL;
                uint64_t sub = m_k + borrow;
                if (rem[k] >= sub) {
                    rem[k] -= static_cast<uint32_t>(sub);
                    borrow = 0;
                } else {
                    rem[k] = static_cast<uint32_t>((1ULL << 32) + rem[k] - sub);
                    borrow = 1;
                }
            }
        }
    }
    memcpy(out, rem, nlimbs * sizeof(uint32_t));
}

}  // namespace

bool rsaVerifyPkcs1Sha2(const uint8_t* pk_blob, size_t pk_blob_len,
                        const char* sig_alg,
                        const uint8_t* sig_raw, size_t sig_raw_len,
                        const uint8_t* msg, size_t msg_len) {
    if (!pk_blob || !sig_alg || !sig_raw || !msg) return false;

    bool use_sha512 = false;
    if (strcmp(sig_alg, "rsa-sha2-256") == 0) {
        use_sha512 = false;
    } else if (strcmp(sig_alg, "rsa-sha2-512") == 0) {
        use_sha512 = true;
    } else {
        return false;
    }

    ssh::SshReader pr(pk_blob, pk_blob_len);
    char key_type[32] = {0};
    const uint8_t *e_ptr = nullptr, *n_ptr = nullptr;
    size_t e_len = 0, n_len = 0;
    if (!pr.getCString(key_type, sizeof(key_type)) || strcmp(key_type, "ssh-rsa") != 0 ||
        !pr.getString(e_ptr, e_len) || !pr.getString(n_ptr, n_len) || pr.remaining() != 0) {
        return false;
    }

    while (e_len > 0 && e_ptr[0] == 0x00) { ++e_ptr; --e_len; }
    while (n_len > 0 && n_ptr[0] == 0x00) { ++n_ptr; --n_len; }
    while (sig_raw_len > n_len && sig_raw[0] == 0x00) { ++sig_raw; --sig_raw_len; }

    // Enforce >= 1024-bit and <= 4096-bit odd RSA modulus and odd exponent >= 3
    if (e_len == 0 || e_len > 32 || (e_ptr[e_len - 1] & 1U) == 0) return false;
    if (e_len == 1 && e_ptr[0] < 3) return false;
    if (n_len < 128 || n_len > RSA_MAX_BYTES || (n_ptr[0] & 0x80U) == 0 || (n_ptr[n_len - 1] & 1U) == 0) {
        return false;
    }
    if (sig_raw_len > n_len) return false;

    size_t nlimbs = (n_len + 3) / 4;
    uint32_t mod_limbs[RSA_MAX_LIMBS + 1] = {0};
    uint32_t sig_limbs[RSA_MAX_LIMBS + 1] = {0};
    uint32_t acc_limbs[RSA_MAX_LIMBS + 1] = {0};

    bytesToLimbs(n_ptr, n_len, mod_limbs, RSA_MAX_LIMBS);
    bytesToLimbs(sig_raw, sig_raw_len, sig_limbs, RSA_MAX_LIMBS);

    // Reject signature >= modulus
    if (limbsGreaterOrEqual(sig_limbs, mod_limbs, nlimbs)) {
        return false;
    }

    // Modular exponentiation: acc = sig^e mod n
    acc_limbs[0] = 1U;
    bool started = false;
    for (size_t i = 0; i < e_len; ++i) {
        uint8_t eb = e_ptr[i];
        for (int bit = 7; bit >= 0; --bit) {
            bool b = ((eb >> bit) & 1U) != 0;
            if (!started) {
                if (b) {
                    memcpy(acc_limbs, sig_limbs, nlimbs * sizeof(uint32_t));
                    started = true;
                }
            } else {
                bigMulMod(acc_limbs, acc_limbs, acc_limbs, mod_limbs, nlimbs);
                if (b) {
                    bigMulMod(acc_limbs, acc_limbs, sig_limbs, mod_limbs, nlimbs);
                }
            }
        }
    }

    uint8_t em[RSA_MAX_BYTES];
    for (size_t i = 0; i < n_len; ++i) {
        em[n_len - 1 - i] = static_cast<uint8_t>((acc_limbs[i / 4] >> ((i % 4) * 8U)) & 0xFFU);
    }

    // RFC 8017 §9.2 DER DigestInfo prefixes
    static const uint8_t DER_SHA256[19] = {
        0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20
    };
    static const uint8_t DER_SHA512[19] = {
        0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40
    };

    uint8_t digest[64];
    size_t hash_len = use_sha512 ? 64 : 32;
    const uint8_t* der = use_sha512 ? DER_SHA512 : DER_SHA256;
    if (use_sha512) {
        sha512(msg, msg_len, digest);
    } else {
        sha256(msg, msg_len, digest);
    }

    size_t t_len = 19 + hash_len;
    if (n_len < t_len + 11) return false;
    size_t ps_len = n_len - t_len - 3;

    uint8_t expected_em[RSA_MAX_BYTES];
    expected_em[0] = 0x00;
    expected_em[1] = 0x01;
    memset(expected_em + 2, 0xFF, ps_len);
    expected_em[2 + ps_len] = 0x00;
    memcpy(expected_em + 3 + ps_len, der, 19);
    memcpy(expected_em + 22 + ps_len, digest, hash_len);

    return constantTimeEqual(em, expected_em, n_len);
}

void hashPassword(const char* password, const uint8_t salt[16], uint8_t out_hash[32]) {
    const uint8_t* pw = reinterpret_cast<const uint8_t*>(password ? password : "");
    size_t pw_len = password ? strlen(password) : 0;

    uint8_t cur[32] = {0};
    for (uint32_t round = 0; round < 1024U; ++round) {
        uint8_t r_le[4] = {
            static_cast<uint8_t>(round & 0xFFU),
            static_cast<uint8_t>((round >> 8) & 0xFFU),
            static_cast<uint8_t>((round >> 16) & 0xFFU),
            static_cast<uint8_t>((round >> 24) & 0xFFU)
        };
        Sha256Ctx ctx;
        sha256Init(ctx);
        sha256Update(ctx, salt, 16);
        sha256Update(ctx, r_le, 4);
        if (round > 0) {
            sha256Update(ctx, cur, 32);
        }
        sha256Update(ctx, pw, pw_len);
        sha256Final(ctx, cur);
    }
    memcpy(out_hash, cur, 32);
}

bool verifyPassword(const char* password, const uint8_t salt[16], const uint8_t expected_hash[32]) {
    if (!password || !salt || !expected_hash) return false;
    uint8_t actual[32];
    hashPassword(password, salt, actual);
    return constantTimeEqual(actual, expected_hash, 32);
}

size_t formatDefaultAuthorizedKeys(char* out, size_t out_cap) {
    BufWriter w(out, out_cap);
    w.putStr("# /etc/ssh/authorized_keys - C6-MicroLinux SSHv2 public key store\n"
             "# Accepts standard OpenSSH lines (ssh-ed25519 <b64>, ssh-rsa <b64>)\n"
             "# and SHA256:<base64> public-key blob digests (signatures verified on login).\n");
    for (size_t i = 0; i < 7; ++i) {
        char b64[52];
        base64Encode(SEEDED_PUBKEY_DIGESTS[i].digest, 32, b64, sizeof(b64), /*strip_pad=*/true);
        w.fmt("SHA256:%s # %s\n", b64, SEEDED_PUBKEY_DIGESTS[i].comment);
    }
    return w.length();
}

bool isPubkeyAuthorized(const char* authorized_keys_text,
                        const uint8_t* pk_blob, size_t pk_blob_len) {
    if (!authorized_keys_text || !pk_blob || pk_blob_len == 0) return false;

    uint8_t target_digest[32];
    sha256(pk_blob, pk_blob_len, target_digest);

    const char* p = authorized_keys_text;
    while (*p) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        const char* line_end = p;
        while (*line_end && *line_end != '\n' && *line_end != '\r') ++line_end;

        if (*p != '#') {
            if (strncmp(p, "SHA256:", 7) == 0) {
                const char* b64_start = p + 7;
                const char* b64_end = b64_start;
                while (b64_end < line_end && *b64_end != ' ' && *b64_end != '\t' && *b64_end != '#') {
                    ++b64_end;
                }
                uint8_t entry_digest[40];
                int dec_len = base64Decode(b64_start, static_cast<size_t>(b64_end - b64_start),
                                           entry_digest, sizeof(entry_digest));
                if (dec_len == 32 && constantTimeEqual(entry_digest, target_digest, 32)) {
                    return true;
                }
            } else {
                // Parse OpenSSH authorized_keys line: [options] <keytype> <base64-blob> [comment]
                const char* cur = p;
                while (cur < line_end) {
                    while (cur < line_end && (*cur == ' ' || *cur == '\t')) ++cur;
                    if (cur >= line_end || *cur == '#') break;
                    const char* tok_start = cur;
                    while (cur < line_end && *cur != ' ' && *cur != '\t') ++cur;
                    size_t tok_len = static_cast<size_t>(cur - tok_start);
                    if ((tok_len == 11 && memcmp(tok_start, "ssh-ed25519", 11) == 0) ||
                        (tok_len == 7 && memcmp(tok_start, "ssh-rsa", 7) == 0)) {
                        while (cur < line_end && (*cur == ' ' || *cur == '\t')) ++cur;
                        const char* b64_start = cur;
                        while (cur < line_end && *cur != ' ' && *cur != '\t' && *cur != '#') ++cur;
                        size_t b64_len = static_cast<size_t>(cur - b64_start);
                        if (b64_len > 0 && b64_len <= 1400) {
                            uint8_t decoded[800];
                            int dec_len = base64Decode(b64_start, b64_len, decoded, sizeof(decoded));
                            if (dec_len > 0 && static_cast<size_t>(dec_len) == pk_blob_len &&
                                constantTimeEqual(decoded, pk_blob, pk_blob_len)) {
                                return true;
                            }
                        }
                        break;
                    }
                }
            }
        }
        p = line_end;
    }
    return false;
}

size_t parseEd25519TrustedKeys(const char* trusted_keys_text,
                               uint8_t (*out_pks)[32], size_t max_keys) {
    if (!trusted_keys_text || !out_pks || max_keys == 0) return 0;
    size_t count = 0;
    const char* p = trusted_keys_text;

    auto hexNibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    while (*p && count < max_keys) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        const char* line_end = p;
        while (*line_end && *line_end != '\n' && *line_end != '\r') ++line_end;

        if (*p != '#') {
            const char* cur = p;
            bool parsed_line = false;
            while (cur < line_end && !parsed_line) {
                while (cur < line_end && (*cur == ' ' || *cur == '\t')) ++cur;
                if (cur >= line_end || *cur == '#') break;
                const char* tok_start = cur;
                while (cur < line_end && *cur != ' ' && *cur != '\t') ++cur;
                size_t tok_len = static_cast<size_t>(cur - tok_start);

                if (tok_len == 11 && memcmp(tok_start, "ssh-ed25519", 11) == 0) {
                    while (cur < line_end && (*cur == ' ' || *cur == '\t')) ++cur;
                    const char* b64_start = cur;
                    while (cur < line_end && *cur != ' ' && *cur != '\t' && *cur != '#') ++cur;
                    size_t b64_len = static_cast<size_t>(cur - b64_start);
                    if (b64_len > 0 && b64_len <= 128) {
                        uint8_t blob[80];
                        int dec_len = base64Decode(b64_start, b64_len, blob, sizeof(blob));
                        if (dec_len == 51) {
                            ssh::SshReader pr(blob, static_cast<size_t>(dec_len));
                            char alg[16] = {0};
                            const uint8_t* raw_pk = nullptr;
                            size_t raw_len = 0;
                            if (pr.getCString(alg, sizeof(alg)) &&
                                strcmp(alg, "ssh-ed25519") == 0 &&
                                pr.getString(raw_pk, raw_len) &&
                                raw_len == 32 && pr.remaining() == 0) {
                                memcpy(out_pks[count++], raw_pk, 32);
                            }
                        }
                    }
                    parsed_line = true;
                } else if (tok_len == 64 && tok_start == p) {
                    uint8_t raw_pk[32];
                    bool hex_ok = true;
                    for (size_t i = 0; i < 32; ++i) {
                        int hi = hexNibble(tok_start[i * 2]);
                        int lo = hexNibble(tok_start[i * 2 + 1]);
                        if (hi < 0 || lo < 0) {
                            hex_ok = false;
                            break;
                        }
                        raw_pk[i] = static_cast<uint8_t>((hi << 4) | lo);
                    }
                    if (hex_ok) {
                        memcpy(out_pks[count++], raw_pk, 32);
                        parsed_line = true;
                    }
                }
            }
        }
        p = line_end;
    }
    return count;
}

size_t formatOpenSshEd25519Pubkey(const uint8_t pk[32], const char* comment,
                                  char* out_line, size_t out_cap) {
    if (!pk || !out_line || out_cap == 0) return 0;
    uint8_t blob[64];
    ssh::SshWriter pw(blob, sizeof(blob));
    pw.putCString("ssh-ed25519");
    pw.putString(pk, 32);
    char b64[96];
    base64Encode(blob, pw.length(), b64, sizeof(b64), /*strip_pad=*/false);
    BufWriter w(out_line, out_cap);
    if (comment && comment[0] != '\0') {
        w.fmt("ssh-ed25519 %s %s\n", b64, comment);
    } else {
        w.fmt("ssh-ed25519 %s\n", b64);
    }
    return w.length();
}

size_t buildUserauthSignedData(const uint8_t session_id[32],
                               const char* username,
                               const char* service,
                               const char* alg_name,
                               const uint8_t* pk_blob, size_t pk_blob_len,
                               uint8_t* out_buf, size_t out_cap) {
    ssh::SshWriter w(out_buf, out_cap);
    w.putString(session_id, 32);
    w.putByte(ssh::SSH_MSG_USERAUTH_REQUEST);
    w.putCString(username ? username : "");
    w.putCString(service ? service : "ssh-connection");
    w.putCString("publickey");
    w.putBool(true);
    w.putCString(alg_name ? alg_name : "");
    w.putString(pk_blob, pk_blob_len);
    return w.overflow() ? 0 : w.length();
}

bool verifyUserauthSignature(const uint8_t session_id[32],
                             const char* username,
                             const char* service,
                             const char* alg_name,
                             const uint8_t* pk_blob, size_t pk_blob_len,
                             const uint8_t* sig_blob, size_t sig_blob_len) {
    if (!session_id || !username || !service || !alg_name || !pk_blob || !sig_blob) {
        return false;
    }
    if (pk_blob_len > 800 || sig_blob_len > 600) {
        return false;
    }

    uint8_t signed_data[1024];
    size_t signed_len = buildUserauthSignedData(session_id, username, service, alg_name,
                                                pk_blob, pk_blob_len,
                                                signed_data, sizeof(signed_data));
    if (signed_len == 0) return false;

    ssh::SshReader sr(sig_blob, sig_blob_len);
    char sig_alg[32] = {0};
    const uint8_t* sig_raw = nullptr;
    size_t sig_raw_len = 0;
    if (!sr.getCString(sig_alg, sizeof(sig_alg)) ||
        !sr.getString(sig_raw, sig_raw_len) ||
        sr.remaining() != 0) {
        return false;
    }

    if (strcmp(alg_name, "ssh-ed25519") == 0) {
        if (strcmp(sig_alg, "ssh-ed25519") != 0 || sig_raw_len != 64) {
            return false;
        }
        ssh::SshReader pr(pk_blob, pk_blob_len);
        char pk_alg[32] = {0};
        const uint8_t* pk_raw = nullptr;
        size_t pk_raw_len = 0;
        if (!pr.getCString(pk_alg, sizeof(pk_alg)) ||
            strcmp(pk_alg, "ssh-ed25519") != 0 ||
            !pr.getString(pk_raw, pk_raw_len) ||
            pk_raw_len != 32 || pr.remaining() != 0) {
            return false;
        }
        return ed25519Verify(sig_raw, signed_data, signed_len, pk_raw) == 0;
    }

    if (strcmp(alg_name, "rsa-sha2-256") == 0 || strcmp(alg_name, "rsa-sha2-512") == 0) {
        if (strcmp(sig_alg, alg_name) != 0) {
            return false;
        }
        return rsaVerifyPkcs1Sha2(pk_blob, pk_blob_len, sig_alg,
                                  sig_raw, sig_raw_len, signed_data, signed_len);
    }

    return false;
}

}  // namespace crypto
}  // namespace c6os
