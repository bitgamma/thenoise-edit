// SPDX-License-Identifier: Apache-2.0
#include "platform/net.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <string>

namespace noise::platform {
namespace {

SOCKET to_socket(SocketHandle fd) {
    return static_cast<SOCKET>(fd);
}

SocketHandle from_socket(SOCKET socket) {
    return static_cast<SocketHandle>(socket);
}

std::string socket_error(const char* context) {
    return std::string(context) + " (Winsock " + std::to_string(WSAGetLastError()) + ")";
}

}  // namespace

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(const std::string& bind_addr, std::uint16_t port, std::string& error) {
    close();
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    wsa_started_ = true;
    const SOCKET server = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        error = socket_error("socket");
        close();
        return false;
    }
    fd_ = from_socket(server);
    const BOOL yes = TRUE;
    if (setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&yes), sizeof(yes)) == SOCKET_ERROR) {
        error = socket_error("exclusive bind");
        close();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &address.sin_addr) != 1) {
        error = "invalid bind address: " + bind_addr;
        close();
        return false;
    }
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = socket_error("bind");
        close();
        return false;
    }
    if (::listen(server, 8) == SOCKET_ERROR) {
        error = socket_error("listen");
        close();
        return false;
    }
    int length = sizeof(address);
    if (::getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        error = socket_error("getsockname");
        close();
        return false;
    }
    port_ = ntohs(address.sin_port);
    u_long nonblocking = 1;
    if (ioctlsocket(server, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        error = socket_error("ioctlsocket");
        close();
        return false;
    }
    return true;
}

void TcpServer::close() {
    if (fd_ != kInvalidSocket) {
        closesocket(to_socket(fd_));
        fd_ = kInvalidSocket;
    }
    port_ = 0;
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }
}

std::uint16_t TcpServer::port() const {
    return port_;
}

SocketHandle TcpServer::accept() {
    if (fd_ == kInvalidSocket) {
        return kInvalidSocket;
    }
    const SOCKET client = ::accept(to_socket(fd_), nullptr, nullptr);
    if (client == INVALID_SOCKET) {
        return kInvalidSocket;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(client, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        closesocket(client);
        return kInvalidSocket;
    }
    const BOOL yes = TRUE;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&yes), sizeof(yes));
    return from_socket(client);
}

bool write_all(SocketHandle fd, const std::string& bytes, int timeout_ms) {
    const SOCKET socket = to_socket(fd);
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto remaining = bytes.size() - sent;
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
        const int written = ::send(socket, bytes.data() + sent, chunk, 0);
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        const int error = WSAGetLastError();
        if (written == SOCKET_ERROR && error == WSAEINTR) {
            continue;
        }
        if (written != SOCKET_ERROR || error != WSAEWOULDBLOCK) {
            return false;
        }
        fd_set writable{};
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(0, nullptr, &writable, nullptr, &timeout) <= 0) {
            return false;
        }
    }
    return true;
}

std::int64_t read_some(SocketHandle fd, char* buffer, std::size_t capacity) {
    const int count = ::recv(to_socket(fd), buffer,
                             static_cast<int>(std::min<std::size_t>(capacity, INT_MAX)), 0);
    if (count > 0) {
        return count;
    }
    if (count == 0) {
        return -1;
    }
    const int error = WSAGetLastError();
    return (error == WSAEINTR || error == WSAEWOULDBLOCK || error == WSAETIMEDOUT) ? 0 : -1;
}

void close_fd(SocketHandle fd) {
    if (fd != kInvalidSocket) {
        closesocket(to_socket(fd));
    }
}

}  // namespace noise::platform
