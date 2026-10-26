// SPDX-License-Identifier: Apache-2.0
#include "platform/net.h"

namespace noise::platform {

TcpServer::~TcpServer() = default;
bool TcpServer::open(const std::string&, std::uint16_t, std::string& error) {
    error = "TCP debug wire is not available in the browser";
    return false;
}
void TcpServer::close() {}
std::uint16_t TcpServer::port() const { return 0; }
int TcpServer::accept() { return -1; }
bool write_all(int, const std::string&, int) { return false; }
std::int64_t read_some(int, char*, std::size_t) { return -1; }
void close_fd(int) {}

}  // namespace noise::platform
