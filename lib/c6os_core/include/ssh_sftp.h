#ifndef SSH_SFTP_H
#define SSH_SFTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {
namespace ssh {

constexpr size_t SFTP_MAX_HANDLES = 4;

using SftpSendFn = int (*)(void* ctx, const uint8_t* data, size_t len);

struct SftpHandle {
    bool used;
    bool is_dir;
    bool writable;
    char vpath[128];
    FILE* fp;
    uint8_t* mem_data;
    size_t mem_len;
    VfsDirEntry* dir_entries;
    size_t dir_count;
    size_t dir_idx;
};

class SftpSession {
public:
    SftpSession();
    ~SftpSession();

    SftpSession(const SftpSession&) = delete;
    SftpSession& operator=(const SftpSession&) = delete;

    bool init(SystemState* state, VirtualFs* vfs, SftpSendFn send_fn, void* send_ctx);
    void closeAll();

    // Feeds decrypted SSH_MSG_CHANNEL_DATA bytes from the client into the SFTP v3 state machine
    bool feed(const uint8_t* data, size_t len);

private:
    SystemState* state_;
    VirtualFs* vfs_;
    SftpSendFn send_fn_;
    void* send_ctx_;
    uint8_t* rx_buf_;
    size_t rx_len_;
    size_t rx_cap_;
    SftpHandle handles_[SFTP_MAX_HANDLES];

    bool sendPacket(const uint8_t* payload, size_t len);
    bool sendStatus(uint32_t req_id, uint32_t code, const char* msg);
    bool dispatchPacket(const uint8_t* pkt, size_t len);
    int allocHandle();
    SftpHandle* lookupHandle(const char* hstr, int hlen);
    void freeHandle(SftpHandle& h);
};

}  // namespace ssh
}  // namespace c6os

#endif  // SSH_SFTP_H
