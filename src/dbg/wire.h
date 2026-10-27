// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/command.h"
#include "core/lines.h"
#include "platform/net.h"

#include <cstdint>
#include <string>
#include <vector>

namespace noise::dbg {

// Loopback JSON-lines server. It has no capabilities of its own: `cmd` is a
// CommandTable id, so everything reachable here is reachable from the UI and
// vice versa. Requests execute on the thread that calls poll() (the render
// thread), which is what makes screenshots and document reads safe.
//
//   -> {"id":7,"cmd":"command.invoke","args":{"name":"view.zoom","args":{...}}}
//   <- {"id":7,"ok":true,"result":{...}}
//
// When token is non-empty the first frame must be {"auth":"<token>"}.
struct WireConfig {
    std::string bind_address;
    std::uint16_t port = 0;  // 0 = ephemeral
    std::string token;
    std::size_t max_frame_bytes = 8u * 1024u * 1024u;
    std::size_t max_clients = 4;
};

class DebugWire {
public:
    explicit DebugWire(CommandTable& commands);
    ~DebugWire();
    DebugWire(const DebugWire&) = delete;
    DebugWire& operator=(const DebugWire&) = delete;

    bool start(const WireConfig& config, std::string& error);
    void stop();
    void poll();

    std::uint16_t port() const;
    std::size_t client_count() const;

private:
    struct Client {
        platform::SocketHandle fd = platform::kInvalidSocket;
        LineFramer framer;
        bool authenticated = false;

        explicit Client(platform::SocketHandle fd_, std::size_t max_frame) : fd(fd_), framer(max_frame) {}
    };

    void close_client(std::size_t index);
    void handle_frame(Client& client, const std::string& frame);
    void send(Client& client, const std::string& payload);

    CommandTable& commands_;
    WireConfig config_;
    platform::TcpServer server_;
    std::vector<Client> clients_;
};

}
