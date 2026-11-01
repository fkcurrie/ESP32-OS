#include "ssh_server.h"

#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/aes.h>

#include "ssh_auth.h"
#include "ssh_packet.h"
#include "ssh_sftp.h"

namespace c6os {

namespace {

constexpr uint8_t SSH_MSG_USERAUTH_PK_OK = 60;
constexpr uint32_t INITIAL_LOCAL_WINDOW  = 65536U;
constexpr uint32_t MAX_LOCAL_PACKET      = 2048U;
constexpr uint32_t WINDOW_REPLENISH_STEP = 16384U;

struct SshTransportState {
    bool encrypted = false;
    uint32_t seq_c2s = 0;
    uint32_t seq_s2c = 0;

    uint8_t session_id[32];
    uint8_t iv_c2s[16];
    uint8_t iv_s2c[16];
    uint8_t key_c2s[16];
    uint8_t key_s2c[16];
    uint8_t mac_c2s[32];
    uint8_t mac_s2c[32];

    mbedtls_aes_context aes_c2s;
    mbedtls_aes_context aes_s2c;
    size_t nc_off_c2s = 0;
    size_t nc_off_s2c = 0;
    uint8_t stream_block_c2s[16];
    uint8_t stream_block_s2c[16];

    SshTransportState() {
        mbedtls_aes_init(&aes_c2s);
        mbedtls_aes_init(&aes_s2c);
        memset(session_id, 0, sizeof(session_id));
        memset(iv_c2s, 0, sizeof(iv_c2s));
        memset(iv_s2c, 0, sizeof(iv_s2c));
        memset(key_c2s, 0, sizeof(key_c2s));
        memset(key_s2c, 0, sizeof(key_s2c));
        memset(mac_c2s, 0, sizeof(mac_c2s));
        memset(mac_s2c, 0, sizeof(mac_s2c));
        memset(stream_block_c2s, 0, sizeof(stream_block_c2s));
        memset(stream_block_s2c, 0, sizeof(stream_block_s2c));
    }

    ~SshTransportState() {
        mbedtls_aes_free(&aes_c2s);
        mbedtls_aes_free(&aes_s2c);
    }
};

struct SshWorkspace {
    uint8_t tx_pkt_buf[2304];
    uint8_t tx_payload_buf[1536];
    uint8_t rx_pkt_buf[3584];
    uint8_t client_kexinit[2560];
    uint8_t rx_buf[2560];
    char auth_keys_buf[1536];
    char io_buf[4096];
};

bool readExactBytes(WiFiClient& client, uint8_t* buf, size_t count, uint32_t timeout_ms = 30000) {
    size_t got = 0;
    uint32_t start = millis();
    while (got < count) {
        if (!client.connected() && client.available() == 0) {
            return false;
        }
        int avail = client.available();
        if (avail > 0) {
            size_t to_read = count - got;
            if (static_cast<size_t>(avail) < to_read) {
                to_read = static_cast<size_t>(avail);
            }
            int r = client.read(buf + got, to_read);
            if (r > 0) {
                got += static_cast<size_t>(r);
                start = millis();
                continue;
            }
        }
        if (millis() - start >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

bool readVersionLine(WiFiClient& client, char* out_line, size_t max_len) {
    size_t pos = 0;
    uint32_t start = millis();
    while (millis() - start < 10000) {
        if (!client.connected() && client.available() == 0) return false;
        if (client.available() > 0) {
            int c = client.read();
            if (c < 0) continue;
            if (c == '\n') {
                if (pos > 0 && out_line[pos - 1] == '\r') {
                    --pos;
                }
                out_line[pos] = '\0';
                if (strncmp(out_line, "SSH-", 4) == 0) {
                    return true;
                }
                pos = 0;
                continue;
            }
            if (pos + 1 < max_len) {
                out_line[pos++] = static_cast<char>(c);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return false;
}

bool sendSshPacket(WiFiClient& client, SshTransportState& ts, SshWorkspace& ws,
                   const uint8_t* payload, size_t payload_len) {
    uint8_t pad_rand[32];
    esp_fill_random(pad_rand, sizeof(pad_rand));

    size_t block_size = ts.encrypted ? 16 : 8;
    size_t framed_len = ssh::framePacket(payload, payload_len, block_size,
                                         pad_rand, ws.tx_pkt_buf, sizeof(ws.tx_pkt_buf) - 32);
    if (framed_len == 0) return false;

    if (!ts.encrypted) {
        size_t written = client.write(ws.tx_pkt_buf, framed_len);
        ts.seq_s2c++;
        return (written == framed_len);
    }

    uint8_t mac[32];
    crypto::computeSshPacketMac(ts.mac_s2c, ts.seq_s2c, ws.tx_pkt_buf, framed_len, mac);

    if (mbedtls_aes_crypt_ctr(&ts.aes_s2c, framed_len, &ts.nc_off_s2c,
                              ts.iv_s2c, ts.stream_block_s2c,
                              ws.tx_pkt_buf, ws.tx_pkt_buf) != 0) {
        return false;
    }

    memcpy(ws.tx_pkt_buf + framed_len, mac, 32);
    size_t total_wire = framed_len + 32;
    size_t written = client.write(ws.tx_pkt_buf, total_wire);
    ts.seq_s2c++;
    return (written == total_wire);
}

bool recvSshPacket(WiFiClient& client, SshTransportState& ts, SshWorkspace& ws,
                   uint8_t* out_payload, size_t max_payload,
                   size_t& out_payload_len, uint32_t timeout_ms = 60000) {
    uint8_t* pkt_buf = ws.rx_pkt_buf;
    const size_t pkt_buf_cap = sizeof(ws.rx_pkt_buf);

    if (!ts.encrypted) {
        if (!readExactBytes(client, pkt_buf, 4, timeout_ms)) return false;
        uint32_t pkt_len = (static_cast<uint32_t>(pkt_buf[0]) << 24) |
                           (static_cast<uint32_t>(pkt_buf[1]) << 16) |
                           (static_cast<uint32_t>(pkt_buf[2]) << 8) |
                           static_cast<uint32_t>(pkt_buf[3]);
        if (pkt_len < 2 || pkt_len + 4 > pkt_buf_cap) return false;
        if (!readExactBytes(client, pkt_buf + 4, pkt_len, timeout_ms)) return false;

        uint8_t pad_len = pkt_buf[4];
        if (1 + pad_len > pkt_len) return false;
        size_t pay_len = pkt_len - 1 - pad_len;
        if (pay_len > max_payload) return false;

        memcpy(out_payload, pkt_buf + 5, pay_len);
        out_payload_len = pay_len;
        ts.seq_c2s++;
        return true;
    }

    uint8_t cipher_block[16];
    if (!readExactBytes(client, cipher_block, 16, timeout_ms)) return false;

    if (mbedtls_aes_crypt_ctr(&ts.aes_c2s, 16, &ts.nc_off_c2s,
                              ts.iv_c2s, ts.stream_block_c2s,
                              cipher_block, pkt_buf) != 0) {
        return false;
    }

    uint32_t pkt_len = (static_cast<uint32_t>(pkt_buf[0]) << 24) |
                       (static_cast<uint32_t>(pkt_buf[1]) << 16) |
                       (static_cast<uint32_t>(pkt_buf[2]) << 8) |
                       static_cast<uint32_t>(pkt_buf[3]);
    size_t total_len = 4 + pkt_len;
    if (total_len < 16 || total_len > pkt_buf_cap || (total_len % 16) != 0) {
        return false;
    }

    size_t rem_len = total_len - 16;
    if (rem_len > 0) {
        if (!readExactBytes(client, pkt_buf + 16, rem_len, timeout_ms)) return false;
        if (mbedtls_aes_crypt_ctr(&ts.aes_c2s, rem_len, &ts.nc_off_c2s,
                                  ts.iv_c2s, ts.stream_block_c2s,
                                  pkt_buf + 16, pkt_buf + 16) != 0) {
            return false;
        }
    }

    uint8_t rx_mac[32];
    if (!readExactBytes(client, rx_mac, 32, timeout_ms)) return false;

    uint8_t expected_mac[32];
    crypto::computeSshPacketMac(ts.mac_c2s, ts.seq_c2s, pkt_buf, total_len, expected_mac);
    if (!crypto::constantTimeEqual(rx_mac, expected_mac, 32)) {
        return false;
    }

    uint8_t pad_len = pkt_buf[4];
    if (1 + pad_len > pkt_len) return false;
    size_t pay_len = pkt_len - 1 - pad_len;
    if (pay_len > max_payload) return false;

    memcpy(out_payload, pkt_buf + 5, pay_len);
    out_payload_len = pay_len;
    ts.seq_c2s++;
    return true;
}

// RFC 4253 §7.2 Key Derivation: SHA256(K_mpint || H || char(X) || session_id)
void deriveSshKey(const uint8_t* k_mpint, size_t k_mpint_len,
                  const uint8_t h[32], char x, const uint8_t session_id[32],
                  uint8_t out_key[32]) {
    crypto::Sha256Ctx st;
    crypto::sha256Init(st);
    crypto::sha256Update(st, k_mpint, k_mpint_len);
    crypto::sha256Update(st, h, 32);
    uint8_t x_byte = static_cast<uint8_t>(x);
    crypto::sha256Update(st, &x_byte, 1);
    crypto::sha256Update(st, session_id, 32);
    crypto::sha256Final(st, out_key);
}

bool sendWindowAdjust(WiFiClient& client, SshTransportState& ts, SshWorkspace& ws,
                      uint32_t recipient_chan, uint32_t bytes_to_add) {
    uint8_t pkt[16];
    ssh::SshWriter w(pkt, sizeof(pkt));
    w.putByte(ssh::SSH_MSG_CHANNEL_WINDOW_ADJUST);
    w.putUint32(recipient_chan);
    w.putUint32(bytes_to_add);
    return sendSshPacket(client, ts, ws, pkt, w.length());
}

bool sendChannelDataChunked(WiFiClient& client, SshTransportState& ts, SshWorkspace& ws,
                            uint32_t recipient_chan, uint32_t& peer_window, uint32_t peer_max_pkt,
                            const uint8_t* data, size_t len) {
    size_t max_chunk = (peer_max_pkt >= 256U && peer_max_pkt < 1280U) ? peer_max_pkt : 1024U;
    size_t offset = 0;
    while (offset < len) {
        // Wait for SSH_MSG_CHANNEL_WINDOW_ADJUST if peer window is exhausted
        uint32_t wait_start = millis();
        while (peer_window == 0) {
            if (!client.connected() && client.available() == 0) return false;
            if (millis() - wait_start > 15000U) return false;
            if (client.available() > 0) {
                uint8_t ctrl_buf[256];
                size_t ctrl_len = 0;
                if (!recvSshPacket(client, ts, ws, ctrl_buf, sizeof(ctrl_buf), ctrl_len, 5000)) {
                    return false;
                }
                if (ctrl_len > 0) {
                    ssh::SshReader cr(ctrl_buf, ctrl_len);
                    uint8_t ctype = 0;
                    cr.getByte(ctype);
                    if (ctype == ssh::SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                        uint32_t rchan = 0, add = 0;
                        if (cr.getUint32(rchan) && cr.getUint32(add)) {
                            peer_window += add;
                        }
                    } else if (ctype == ssh::SSH_MSG_DISCONNECT ||
                               ctype == ssh::SSH_MSG_CHANNEL_CLOSE) {
                        return false;
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }

        size_t chunk = len - offset;
        if (chunk > max_chunk) chunk = max_chunk;
        if (chunk > peer_window) chunk = peer_window;

        ssh::SshWriter w(ws.tx_payload_buf, sizeof(ws.tx_payload_buf));
        w.putByte(ssh::SSH_MSG_CHANNEL_DATA);
        w.putUint32(recipient_chan);
        w.putString(data + offset, chunk);
        if (!sendSshPacket(client, ts, ws, ws.tx_payload_buf, w.length())) {
            return false;
        }
        peer_window -= static_cast<uint32_t>(chunk);
        offset += chunk;
    }
    return true;
}

struct SftpBridgeCtx {
    WiFiClient* client;
    SshTransportState* ts;
    SshWorkspace* ws;
    uint32_t* peer_window;
    uint32_t peer_max_pkt;
    uint32_t client_chan;
    uint32_t tx_bytes;
};

int sftpSendCallback(void* raw_ctx, const uint8_t* data, size_t len) {
    SftpBridgeCtx* ctx = static_cast<SftpBridgeCtx*>(raw_ctx);
    if (!ctx || !ctx->client || !ctx->ts || !ctx->ws || !ctx->peer_window) return -1;
    if (!sendChannelDataChunked(*ctx->client, *ctx->ts, *ctx->ws,
                                ctx->client_chan, *ctx->peer_window, ctx->peer_max_pkt,
                                data, len)) {
        return -1;
    }
    ctx->tx_bytes += static_cast<uint32_t>(len);
    return 0;
}

}  // namespace

void SshServer::begin(SystemState& state, SemaphoreHandle_t mutex) {
    Preferences prefs;
    prefs.begin("c6os_ssh", false);
    uint8_t seed[32];
    if (prefs.getBytesLength("ed25519_seed") == 32) {
        prefs.getBytes("ed25519_seed", seed, 32);
    } else {
        esp_fill_random(seed, 32);
        prefs.putBytes("ed25519_seed", seed, 32);
    }
    prefs.end();

    crypto::ed25519KeypairFromSeed(host_pk_, host_sk_, seed);
    memset(seed, 0, sizeof(seed));

    uint8_t ks_blob[64];
    size_t ks_len = ssh::encodeEd25519HostKeyBlob(host_pk_, ks_blob, sizeof(ks_blob));
    uint8_t digest[32];
    crypto::sha256(ks_blob, ks_len, digest);

    char b64[64] = {0};
    crypto::base64Encode(digest, 32, b64, sizeof(b64), /*strip_pad=*/true);

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        snprintf(state.ssh_hostkey_fp, sizeof(state.ssh_hostkey_fp), "SHA256:%.48s", b64);
        state.appendDmesg(millis(), "sshd: Listening on 0.0.0.0:22 (ed25519/rsa-sha2 + sftp)");
        xSemaphoreGive(mutex);
    }

    server_.begin();
}

struct SshClientTaskCtx {
    SshServer* server;
    WiFiClient client;
    SystemState* state;
    VirtualFs* vfs;
    SemaphoreHandle_t mutex;
};

void sshClientWorkerTask(void* param) {
    SshClientTaskCtx* ctx = static_cast<SshClientTaskCtx*>(param);
    if (ctx) {
        ctx->server->serveAcceptedClient(ctx->client, *ctx->state, *ctx->vfs, ctx->mutex);
        delete ctx;
    }
    vTaskDelete(nullptr);
}

void SshServer::pollAcceptAndServe(SystemState& state, VirtualFs& vfs, SemaphoreHandle_t mutex) {
    WiFiClient client = server_.accept();
    if (!client) {
        return;
    }

    uint8_t active = 0;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = state.ssh_active_sessions;
        xSemaphoreGive(mutex);
    }

    if (active >= 3 || ESP.getFreeHeap() < 42000) {
        client.stop();
        return;
    }

    SshClientTaskCtx* ctx = new SshClientTaskCtx{this, client, &state, &vfs, mutex};
    if (!ctx) {
        client.stop();
        return;
    }

    if (xTaskCreate(sshClientWorkerTask, "sshd_priv", 10240, ctx, 2, nullptr) != pdPASS) {
        client.stop();
        delete ctx;
    }
}

void SshServer::serveAcceptedClient(WiFiClient client, SystemState& state, VirtualFs& vfs, SemaphoreHandle_t mutex) {
    client.setNoDelay(true);
    String remote_ip = client.remoteIP().toString();

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state.ssh_active_sessions++;
        strncpy(state.ssh_client_ip, remote_ip.c_str(), sizeof(state.ssh_client_ip) - 1);
        state.ssh_client_ip[sizeof(state.ssh_client_ip) - 1] = '\0';
        char log_msg[72];
        snprintf(log_msg, sizeof(log_msg), "sshd: Connection from %s", state.ssh_client_ip);
        state.appendDmesg(millis(), log_msg);
        state.markDirty(DIRTY_HEADER | DIRTY_CARDS);
        xSemaphoreGive(mutex);
    }

    handleClient(client, state, vfs, mutex);
    client.stop();

    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (state.ssh_active_sessions > 0) {
            state.ssh_active_sessions--;
        }
        char log_msg[72];
        snprintf(log_msg, sizeof(log_msg), "sshd: Closed session from %s", remote_ip.c_str());
        state.appendDmesg(millis(), log_msg);
        state.markDirty(DIRTY_HEADER | DIRTY_CARDS);
        xSemaphoreGive(mutex);
    }
}

void SshServer::handleClient(WiFiClient& client, SystemState& state,
                             VirtualFs& vfs, SemaphoreHandle_t mutex) {
    SshWorkspace* ws_ptr = static_cast<SshWorkspace*>(malloc(sizeof(SshWorkspace)));
    if (!ws_ptr) return;

    ssh::SftpSession* sftp = nullptr;
    struct SessionGuard {
        SshWorkspace* ws;
        ssh::SftpSession** sftp_ref;
        ~SessionGuard() {
            if (sftp_ref && *sftp_ref) {
                delete *sftp_ref;
                *sftp_ref = nullptr;
            }
            free(ws);
        }
    } guard{ws_ptr, &sftp};

    SshWorkspace& ws = *ws_ptr;
    SshTransportState ts;

    // 1. Send Server Identification String
    char server_banner[64];
    snprintf(server_banner, sizeof(server_banner), "%s\r\n", ssh::SERVER_VERSION_STRING);
    client.write(reinterpret_cast<const uint8_t*>(server_banner), strlen(server_banner));

    // 2. Read Client Identification String (V_C)
    char client_version[128];
    if (!readVersionLine(client, client_version, sizeof(client_version))) {
        return;
    }

    // 3. Build and Send Server SSH_MSG_KEXINIT (I_S)
    uint8_t cookie[16];
    esp_fill_random(cookie, sizeof(cookie));
    uint8_t server_kexinit[384];
    size_t server_kexinit_len = ssh::buildServerKexInitPayload(cookie, server_kexinit, sizeof(server_kexinit));
    if (server_kexinit_len == 0 || !sendSshPacket(client, ts, ws, server_kexinit, server_kexinit_len)) {
        return;
    }

    // 4. Receive Client SSH_MSG_KEXINIT (I_C)
    size_t client_kexinit_len = 0;
    if (!recvSshPacket(client, ts, ws, ws.client_kexinit, sizeof(ws.client_kexinit), client_kexinit_len)) {
        return;
    }
    if (client_kexinit_len < 1 || ws.client_kexinit[0] != ssh::SSH_MSG_KEXINIT) {
        return;
    }

    // 5. Receive Client SSH_MSG_KEX_ECDH_INIT (30)
    size_t rx_len = 0;
    if (!recvSshPacket(client, ts, ws, ws.rx_buf, sizeof(ws.rx_buf), rx_len)) {
        return;
    }
    ssh::SshReader kex_reader(ws.rx_buf, rx_len);
    uint8_t msg_type = 0;
    const uint8_t* qc_ptr = nullptr;
    size_t qc_len = 0;
    if (!kex_reader.getByte(msg_type) || msg_type != ssh::SSH_MSG_KEX_ECDH_INIT ||
        !kex_reader.getString(qc_ptr, qc_len) || qc_len != 32) {
        return;
    }

    // 6. Compute X25519 Ephemeral Keypair (Q_S) and Shared Secret (K_raw)
    uint8_t e_s[32];
    uint8_t q_s[32];
    uint8_t k_raw[32];
    esp_fill_random(e_s, sizeof(e_s));
    crypto::x25519Base(q_s, e_s);
    if (crypto::x25519Shared(k_raw, e_s, qc_ptr) != 0) {
        memset(e_s, 0, sizeof(e_s));
        return;
    }
    memset(e_s, 0, sizeof(e_s));

    uint8_t k_s[64];
    size_t k_s_len = ssh::encodeEd25519HostKeyBlob(host_pk_, k_s, sizeof(k_s));

    uint8_t k_mpint[40];
    ssh::SshWriter kw(k_mpint, sizeof(k_mpint));
    kw.putMpint(k_raw, 32);
    size_t k_mpint_len = kw.length();
    memset(k_raw, 0, sizeof(k_raw));

    // 7. Compute Exchange Hash H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K_mpint)
    crypto::Sha256Ctx h_ctx;
    crypto::sha256Init(h_ctx);

    auto hashString = [&h_ctx](const uint8_t* data, size_t len) {
        uint8_t len_be[4] = {
            static_cast<uint8_t>((len >> 24) & 0xFF),
            static_cast<uint8_t>((len >> 16) & 0xFF),
            static_cast<uint8_t>((len >> 8) & 0xFF),
            static_cast<uint8_t>(len & 0xFF)
        };
        crypto::sha256Update(h_ctx, len_be, 4);
        crypto::sha256Update(h_ctx, data, len);
    };

    hashString(reinterpret_cast<const uint8_t*>(client_version), strlen(client_version));
    hashString(reinterpret_cast<const uint8_t*>(ssh::SERVER_VERSION_STRING),
               strlen(ssh::SERVER_VERSION_STRING));
    hashString(ws.client_kexinit, client_kexinit_len);
    hashString(server_kexinit, server_kexinit_len);
    hashString(k_s, k_s_len);
    hashString(qc_ptr, 32);
    hashString(q_s, 32);
    crypto::sha256Update(h_ctx, k_mpint, k_mpint_len);

    uint8_t h_hash[32];
    crypto::sha256Final(h_ctx, h_hash);
    memcpy(ts.session_id, h_hash, 32);

    // 8. Sign H using Host Ed25519 Secret Key and Send SSH_MSG_KEX_ECDH_REPLY (31)
    uint8_t sig_raw[64];
    crypto::ed25519Sign(sig_raw, h_hash, 32, host_sk_);
    uint8_t sig_blob[96];
    size_t sig_blob_len = ssh::encodeEd25519SignatureBlob(sig_raw, sig_blob, sizeof(sig_blob));

    uint8_t reply_payload[256];
    ssh::SshWriter rw(reply_payload, sizeof(reply_payload));
    rw.putByte(ssh::SSH_MSG_KEX_ECDH_REPLY);
    rw.putString(k_s, k_s_len);
    rw.putString(q_s, 32);
    rw.putString(sig_blob, sig_blob_len);
    if (!sendSshPacket(client, ts, ws, reply_payload, rw.length())) {
        return;
    }

    // 9. Send and Receive SSH_MSG_NEWKEYS (21)
    uint8_t newkeys_msg = ssh::SSH_MSG_NEWKEYS;
    if (!sendSshPacket(client, ts, ws, &newkeys_msg, 1)) {
        return;
    }
    if (!recvSshPacket(client, ts, ws, ws.rx_buf, sizeof(ws.rx_buf), rx_len) ||
        rx_len < 1 || ws.rx_buf[0] != ssh::SSH_MSG_NEWKEYS) {
        return;
    }

    // 10. Activate Encrypted Transport (aes128-ctr + hmac-sha2-256)
    uint8_t key_tmp[32];
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'A', ts.session_id, key_tmp);
    memcpy(ts.iv_c2s, key_tmp, 16);
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'B', ts.session_id, key_tmp);
    memcpy(ts.iv_s2c, key_tmp, 16);
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'C', ts.session_id, key_tmp);
    memcpy(ts.key_c2s, key_tmp, 16);
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'D', ts.session_id, key_tmp);
    memcpy(ts.key_s2c, key_tmp, 16);
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'E', ts.session_id, ts.mac_c2s);
    deriveSshKey(k_mpint, k_mpint_len, h_hash, 'F', ts.session_id, ts.mac_s2c);
    memset(key_tmp, 0, sizeof(key_tmp));
    memset(k_mpint, 0, sizeof(k_mpint));

    mbedtls_aes_setkey_enc(&ts.aes_c2s, ts.key_c2s, 128);
    mbedtls_aes_setkey_enc(&ts.aes_s2c, ts.key_s2c, 128);
    ts.encrypted = true;

    // 10b. Send SSH_MSG_EXT_INFO (RFC 8308) advertising verified signature algorithms
    {
        uint8_t ext_pkt[128];
        ssh::SshWriter ew(ext_pkt, sizeof(ext_pkt));
        ew.putByte(ssh::SSH_MSG_EXT_INFO);
        ew.putUint32(1);
        ew.putCString("server-sig-algs");
        ew.putCString("ssh-ed25519,rsa-sha2-256,rsa-sha2-512");
        sendSshPacket(client, ts, ws, ext_pkt, ew.length());
    }

    // 11. Main SSH Authenticated Session & Channel Loop
    bool authenticated = false;
    bool channel_open = false;
    bool sftp_active = false;
    uint32_t client_chan = 0;
    uint32_t peer_window = 0;
    uint32_t peer_max_pkt = 1024;
    uint32_t local_rx_consumed = 0;

    ShellSession shell;
    SftpBridgeCtx sftp_ctx{&client, &ts, &ws, &peer_window, peer_max_pkt, 0, 0};
    char* io_buf = ws.io_buf;
    uint32_t last_mon_tick = millis();

    while (client.connected()) {
        uint32_t wait_start = millis();
        while (client.connected() && client.available() == 0) {
            if (channel_open && !sftp_active && shell.isInteractiveMonitor()) {
                uint32_t now = millis();
                if (now - last_mon_tick >= shell.monitorIntervalMs()) {
                    last_mon_tick = now;
                    io_buf[0] = '\0';
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        state.uptime_sec = millis() / 1000U;
                        state.free_sram_bytes = ESP.getFreeHeap();
                        shell.renderMonitorTick(state, vfs, io_buf, sizeof(ws.io_buf));
                        xSemaphoreGive(mutex);
                    }
                    if (io_buf[0] != '\0') {
                        size_t n = strlen(io_buf);
                        sendChannelDataChunked(client, ts, ws, client_chan, peer_window, peer_max_pkt,
                                               reinterpret_cast<const uint8_t*>(io_buf), n);
                    }
                }
            }
            if (millis() - wait_start >= 300000U) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!client.connected() && client.available() == 0) {
            break;
        }

        if (!recvSshPacket(client, ts, ws, ws.rx_buf, sizeof(ws.rx_buf), rx_len, 300000U)) {
            break;
        }
        last_mon_tick = millis();
        if (rx_len == 0) continue;

        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            state.net_rx_bytes += static_cast<uint32_t>(rx_len);
            xSemaphoreGive(mutex);
        }

        ssh::SshReader r(ws.rx_buf, rx_len);
        uint8_t type = 0;
        r.getByte(type);

        if (type == ssh::SSH_MSG_DISCONNECT) {
            break;
        } else if (type == ssh::SSH_MSG_IGNORE || type == ssh::SSH_MSG_DEBUG) {
            continue;
        } else if (type == ssh::SSH_MSG_SERVICE_REQUEST) {
            char svc[48];
            if (r.getCString(svc, sizeof(svc))) {
                uint8_t pkt[64];
                ssh::SshWriter sw(pkt, sizeof(pkt));
                sw.putByte(ssh::SSH_MSG_SERVICE_ACCEPT);
                sw.putCString(svc);
                sendSshPacket(client, ts, ws, pkt, sw.length());
            }
        } else if (type == ssh::SSH_MSG_GLOBAL_REQUEST) {
            char req_name[64];
            bool want_reply = false;
            if (r.getCString(req_name, sizeof(req_name)) && r.getBool(want_reply) && want_reply) {
                uint8_t fail = ssh::SSH_MSG_REQUEST_FAILURE;
                sendSshPacket(client, ts, ws, &fail, 1);
            }
        } else if (type == ssh::SSH_MSG_USERAUTH_REQUEST) {
            char username[48], svc[48], method[48];
            if (!r.getCString(username, sizeof(username)) ||
                !r.getCString(svc, sizeof(svc)) ||
                !r.getCString(method, sizeof(method))) {
                break;
            }

            if (strcmp(method, "publickey") == 0) {
                bool has_sig = false;
                char alg_name[48];
                const uint8_t* blob_ptr = nullptr;
                size_t blob_len = 0;
                if (r.getBool(has_sig) &&
                    r.getCString(alg_name, sizeof(alg_name)) &&
                    r.getString(blob_ptr, blob_len)) {
                    ws.auth_keys_buf[0] = '\0';
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        vfs.readFile(state, "/etc/ssh/authorized_keys",
                                     ws.auth_keys_buf, sizeof(ws.auth_keys_buf));
                        xSemaphoreGive(mutex);
                    }

                    bool key_ok = crypto::isPubkeyAuthorized(ws.auth_keys_buf, blob_ptr, blob_len);
                    if (key_ok && !has_sig) {
                        ssh::SshWriter pw(ws.tx_payload_buf, sizeof(ws.tx_payload_buf));
                        pw.putByte(SSH_MSG_USERAUTH_PK_OK);
                        pw.putCString(alg_name);
                        pw.putString(blob_ptr, blob_len);
                        if (!pw.overflow()) {
                            sendSshPacket(client, ts, ws, ws.tx_payload_buf, pw.length());
                            continue;
                        }
                    } else if (key_ok && has_sig) {
                        const uint8_t* sig_ptr = nullptr;
                        size_t sig_len = 0;
                        if (r.getString(sig_ptr, sig_len) &&
                            crypto::verifyUserauthSignature(ts.session_id, username, svc,
                                                            alg_name, blob_ptr, blob_len,
                                                            sig_ptr, sig_len)) {
                            authenticated = true;
                            uint8_t succ = ssh::SSH_MSG_USERAUTH_SUCCESS;
                            sendSshPacket(client, ts, ws, &succ, 1);
                            if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                                state.ssh_total_logins++;
                                state.appendDmesg(millis(), "sshd: Accepted verified publickey for root");
                                state.markDirty(DIRTY_HEADER | DIRTY_CARDS);
                                xSemaphoreGive(mutex);
                            }
                            continue;
                        }
                    }
                }
            } else if (strcmp(method, "password") == 0) {
                bool change_req = false;
                char pass[64];
                if (r.getBool(change_req) && !change_req && r.getCString(pass, sizeof(pass))) {
                    uint8_t salt_copy[16];
                    uint8_t hash_copy[32];
                    bool configured = false;
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        configured = state.pass_configured;
                        memcpy(salt_copy, state.pass_salt, sizeof(salt_copy));
                        memcpy(hash_copy, state.pass_hash, sizeof(hash_copy));
                        xSemaphoreGive(mutex);
                    }

                    bool ok = configured && crypto::verifyPassword(pass, salt_copy, hash_copy);
                    memset(pass, 0, sizeof(pass));
                    memset(salt_copy, 0, sizeof(salt_copy));
                    memset(hash_copy, 0, sizeof(hash_copy));

                    if (ok) {
                        authenticated = true;
                        uint8_t succ = ssh::SSH_MSG_USERAUTH_SUCCESS;
                        sendSshPacket(client, ts, ws, &succ, 1);
                        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            state.ssh_total_logins++;
                            state.appendDmesg(millis(), "sshd: Accepted password for root");
                            state.markDirty(DIRTY_HEADER | DIRTY_CARDS);
                            xSemaphoreGive(mutex);
                        }
                        continue;
                    }
                }
            }

            uint8_t fail_pkt[64];
            ssh::SshWriter fw(fail_pkt, sizeof(fail_pkt));
            fw.putByte(ssh::SSH_MSG_USERAUTH_FAILURE);
            fw.putCString("publickey,password");
            fw.putBool(false);
            sendSshPacket(client, ts, ws, fail_pkt, fw.length());
        } else if (authenticated && type == ssh::SSH_MSG_CHANNEL_OPEN) {
            char chan_type[32];
            uint32_t sender_chan = 0, init_win = 0, max_pkt = 0;
            if (r.getCString(chan_type, sizeof(chan_type)) &&
                r.getUint32(sender_chan) &&
                r.getUint32(init_win) &&
                r.getUint32(max_pkt)) {
                client_chan = sender_chan;
                peer_window = init_win;
                peer_max_pkt = (max_pkt >= 256U && max_pkt < 1280U) ? max_pkt : 1024U;
                local_rx_consumed = 0;
                channel_open = true;

                uint8_t conf[32];
                ssh::SshWriter cw(conf, sizeof(conf));
                cw.putByte(ssh::SSH_MSG_CHANNEL_OPEN_CONFIRMATION);
                cw.putUint32(client_chan);
                cw.putUint32(0);
                cw.putUint32(INITIAL_LOCAL_WINDOW);
                cw.putUint32(MAX_LOCAL_PACKET);
                sendSshPacket(client, ts, ws, conf, cw.length());
            }
        } else if (authenticated && channel_open && type == ssh::SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            uint32_t recip_chan = 0, bytes_to_add = 0;
            if (r.getUint32(recip_chan) && r.getUint32(bytes_to_add)) {
                peer_window += bytes_to_add;
            }
        } else if (authenticated && channel_open && type == ssh::SSH_MSG_CHANNEL_REQUEST) {
            uint32_t recip_chan = 0;
            char req_type[32];
            bool want_reply = false;
            if (!r.getUint32(recip_chan) ||
                !r.getCString(req_type, sizeof(req_type)) ||
                !r.getBool(want_reply)) {
                continue;
            }

            if (strcmp(req_type, "pty-req") == 0 ||
                strcmp(req_type, "env") == 0 ||
                strcmp(req_type, "window-change") == 0) {
                if (want_reply) {
                    uint8_t s_pkt[8];
                    ssh::SshWriter sw(s_pkt, sizeof(s_pkt));
                    sw.putByte(ssh::SSH_MSG_CHANNEL_SUCCESS);
                    sw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, s_pkt, sw.length());
                }
            } else if (strcmp(req_type, "shell") == 0) {
                if (want_reply) {
                    uint8_t s_pkt[8];
                    ssh::SshWriter sw(s_pkt, sizeof(s_pkt));
                    sw.putByte(ssh::SSH_MSG_CHANNEL_SUCCESS);
                    sw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, s_pkt, sw.length());
                }
                io_buf[0] = '\0';
                if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    bool start_menu = (strstr(state.login_shell, "menu") != nullptr);
                    shell.begin(state, vfs, start_menu, io_buf, sizeof(ws.io_buf));
                    xSemaphoreGive(mutex);
                }
                size_t n = strlen(io_buf);
                sendChannelDataChunked(client, ts, ws, client_chan, peer_window, peer_max_pkt,
                                       reinterpret_cast<const uint8_t*>(io_buf), n);
            } else if (strcmp(req_type, "exec") == 0) {
                char cmd_str[256];
                if (r.getCString(cmd_str, sizeof(cmd_str))) {
                    if (want_reply) {
                        uint8_t s_pkt[8];
                        ssh::SshWriter sw(s_pkt, sizeof(s_pkt));
                        sw.putByte(ssh::SSH_MSG_CHANNEL_SUCCESS);
                        sw.putUint32(client_chan);
                        sendSshPacket(client, ts, ws, s_pkt, sw.length());
                    }
                    io_buf[0] = '\0';
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        shell.executeCommandLine(state, vfs, cmd_str, io_buf, sizeof(ws.io_buf));
                        xSemaphoreGive(mutex);
                    }
                    if (io_buf[0] != '\0') {
                        size_t n = strlen(io_buf);
                        sendChannelDataChunked(client, ts, ws, client_chan, peer_window, peer_max_pkt,
                                               reinterpret_cast<const uint8_t*>(io_buf), n);
                    }
                    uint8_t exit_pkt[32];
                    ssh::SshWriter ew(exit_pkt, sizeof(exit_pkt));
                    ew.putByte(ssh::SSH_MSG_CHANNEL_REQUEST);
                    ew.putUint32(client_chan);
                    ew.putCString("exit-status");
                    ew.putBool(false);
                    ew.putUint32(0);
                    sendSshPacket(client, ts, ws, exit_pkt, ew.length());

                    uint8_t eof_pkt[8];
                    ssh::SshWriter eofw(eof_pkt, sizeof(eof_pkt));
                    eofw.putByte(ssh::SSH_MSG_CHANNEL_EOF);
                    eofw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, eof_pkt, eofw.length());

                    uint8_t cls_pkt[8];
                    ssh::SshWriter clsw(cls_pkt, sizeof(cls_pkt));
                    clsw.putByte(ssh::SSH_MSG_CHANNEL_CLOSE);
                    clsw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, cls_pkt, clsw.length());
                    break;
                }
            } else if (strcmp(req_type, "subsystem") == 0) {
                char subsys[32];
                if (r.getCString(subsys, sizeof(subsys)) && strcmp(subsys, "sftp") == 0) {
                    if (!sftp) {
                        sftp = new ssh::SftpSession();
                    }
                    sftp_ctx.peer_max_pkt = peer_max_pkt;
                    sftp_ctx.client_chan = client_chan;
                    sftp_ctx.tx_bytes = 0;
                    if (sftp && sftp->init(&state, &vfs, &sftpSendCallback, &sftp_ctx)) {
                        sftp_active = true;
                        if (want_reply) {
                            uint8_t s_pkt[8];
                            ssh::SshWriter sw(s_pkt, sizeof(s_pkt));
                            sw.putByte(ssh::SSH_MSG_CHANNEL_SUCCESS);
                            sw.putUint32(client_chan);
                            sendSshPacket(client, ts, ws, s_pkt, sw.length());
                        }
                        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                            state.appendDmesg(millis(), "sshd: Started SFTP v3 subsystem");
                            xSemaphoreGive(mutex);
                        }
                        continue;
                    }
                }
                if (want_reply) {
                    uint8_t f_pkt[8];
                    ssh::SshWriter fw(f_pkt, sizeof(f_pkt));
                    fw.putByte(ssh::SSH_MSG_CHANNEL_FAILURE);
                    fw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, f_pkt, fw.length());
                }
            } else if (want_reply) {
                uint8_t f_pkt[8];
                ssh::SshWriter fw(f_pkt, sizeof(f_pkt));
                fw.putByte(ssh::SSH_MSG_CHANNEL_FAILURE);
                fw.putUint32(client_chan);
                sendSshPacket(client, ts, ws, f_pkt, fw.length());
            }
        } else if (authenticated && channel_open && type == ssh::SSH_MSG_CHANNEL_DATA) {
            uint32_t recip_chan = 0;
            const uint8_t* data_ptr = nullptr;
            size_t data_len = 0;
            if (r.getUint32(recip_chan) && r.getString(data_ptr, data_len)) {
                local_rx_consumed += static_cast<uint32_t>(data_len);
                if (local_rx_consumed >= WINDOW_REPLENISH_STEP) {
                    sendWindowAdjust(client, ts, ws, client_chan, local_rx_consumed);
                    local_rx_consumed = 0;
                }

                if (sftp_active && sftp) {
                    sftp_ctx.peer_max_pkt = peer_max_pkt;
                    sftp_ctx.client_chan = client_chan;
                    sftp_ctx.tx_bytes = 0;
                    bool ok = false;
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                        ok = sftp->feed(data_ptr, data_len);
                        state.net_tx_bytes += sftp_ctx.tx_bytes;
                        xSemaphoreGive(mutex);
                    }
                    if (!ok) {
                        break;
                    }
                    continue;
                }

                io_buf[0] = '\0';
                bool keep_alive = true;
                if (xSemaphoreTake(mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    keep_alive = shell.consumeBytes(state, vfs,
                                                    reinterpret_cast<const char*>(data_ptr),
                                                    data_len, io_buf, sizeof(ws.io_buf));
                    xSemaphoreGive(mutex);
                }
                if (io_buf[0] != '\0') {
                    size_t n = strlen(io_buf);
                    sendChannelDataChunked(client, ts, ws, client_chan, peer_window, peer_max_pkt,
                                           reinterpret_cast<const uint8_t*>(io_buf), n);
                    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                        state.net_tx_bytes += static_cast<uint32_t>(n);
                        xSemaphoreGive(mutex);
                    }
                }
                if (!keep_alive) {
                    uint8_t eof_pkt[8];
                    ssh::SshWriter eofw(eof_pkt, sizeof(eof_pkt));
                    eofw.putByte(ssh::SSH_MSG_CHANNEL_EOF);
                    eofw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, eof_pkt, eofw.length());

                    uint8_t cls_pkt[8];
                    ssh::SshWriter clsw(cls_pkt, sizeof(cls_pkt));
                    clsw.putByte(ssh::SSH_MSG_CHANNEL_CLOSE);
                    clsw.putUint32(client_chan);
                    sendSshPacket(client, ts, ws, cls_pkt, clsw.length());
                    break;
                }
            }
        } else if (type == ssh::SSH_MSG_CHANNEL_EOF || type == ssh::SSH_MSG_CHANNEL_CLOSE) {
            uint8_t cls_pkt[8];
            ssh::SshWriter clsw(cls_pkt, sizeof(cls_pkt));
            clsw.putByte(ssh::SSH_MSG_CHANNEL_CLOSE);
            clsw.putUint32(client_chan);
            sendSshPacket(client, ts, ws, cls_pkt, clsw.length());
            break;
        }
    }
}

}  // namespace c6os
