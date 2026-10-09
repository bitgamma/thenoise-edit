// SPDX-License-Identifier: Apache-2.0
#include "platform/net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace noise::platform {

namespace {

void set_nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(const std::string& bind_addr, std::uint16_t port, std::string& error) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind address: " + bind_addr;
        close();
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = "bind " + bind_addr + ":" + std::to_string(port) + ": " + std::strerror(errno);
        close();
        return false;
    }
    if (::listen(fd_, 8) != 0) {
        error = std::string("listen: ") + std::strerror(errno);
        close();
        return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        error = std::string("getsockname: ") + std::strerror(errno);
        close();
        return false;
    }
    port_ = ntohs(addr.sin_port);
    set_nonblocking(fd_);
    return true;
}

void TcpServer::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    port_ = 0;
}

std::uint16_t TcpServer::port() const {
    return port_;
}

int TcpServer::accept() {
    if (fd_ < 0) {
        return -1;
    }
    // Non-blocking: the debug wire is serviced from the render thread and must
    // never wait on a client.
    const int client = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
        return -1;
    }
    const int yes = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return client;
}

bool write_all(int fd, const std::string& bytes, int timeout_ms) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto written = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (!(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
            return false;
        }
        // A client that is not draining its socket must not stall the render
        // thread: wait at most timeout_ms for writability, then drop it.
        pollfd waiter{fd, POLLOUT, 0};
        const int ready = ::poll(&waiter, 1, timeout_ms);
        if (ready <= 0) {
            return false;
        }
    }
    return true;
}

std::int64_t read_some(int fd, char* buffer, std::size_t capacity) {
    const auto got = ::recv(fd, buffer, capacity, 0);
    if (got > 0) {
        return got;
    }
    if (got == 0) {
        return -1;  // end of stream: the peer is gone
    }
    return (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

void close_fd(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

}
