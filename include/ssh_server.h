#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

#include "c6os_shell.h"
#include "c6os_types.h"
#include "c6os_vfs.h"

namespace c6os {

class SshServer {
public:
    explicit SshServer(uint16_t port = 22)
        : listen_fd_(-1), port_(port) {
        memset(host_pk_, 0, sizeof(host_pk_));
        memset(host_sk_, 0, sizeof(host_sk_));
    }

    ~SshServer();

    // Initializes or loads persistent ssh-ed25519 host key from NVS and binds port 22
    void begin(SystemState& state, SemaphoreHandle_t mutex);

    // Event-driven select() accept dispatcher called from the sshd FreeRTOS task loop
    void pollAcceptAndServe(SystemState& state, VirtualFs& vfs, SemaphoreHandle_t mutex);

    void serveAcceptedClient(int client_fd, const char* remote_ip,
                             SystemState& state, VirtualFs& vfs, SemaphoreHandle_t mutex);

private:
    void handleClient(int client_fd, SystemState& state, VirtualFs& vfs, SemaphoreHandle_t mutex);

    int listen_fd_;
    uint16_t port_;
    uint8_t host_pk_[32];
    uint8_t host_sk_[64];
};

}  // namespace c6os

#endif  // SSH_SERVER_H
