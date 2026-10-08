// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace noise::platform {

// Loopback-only TCP listener used by the debug wire. accept() is non-blocking:
// it returns -1 when no connection is pending.
class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool open(const std::string& bind_addr, std::uint16_t port, std::string& error);
    void close();
    std::uint16_t port() const;
    int accept();

private:
    int fd_ = -1;
    std::uint16_t port_ = 0;
};

// Writes everything or fails; on a non-blocking socket a full send buffer is
// waited on for at most timeout_ms, so a stalled peer cannot hang the caller.
bool write_all(int fd, const std::string& bytes, int timeout_ms);
std::int64_t read_some(int fd, char* buffer, std::size_t capacity);
void close_fd(int fd);

}
