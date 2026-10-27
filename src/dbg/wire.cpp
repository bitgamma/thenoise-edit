// SPDX-License-Identifier: Apache-2.0
#include "dbg/wire.h"

#include "core/log.h"
#include "platform/net.h"

#include <nlohmann/json.hpp>

namespace noise::dbg {

using nlohmann::json;

namespace {

constexpr char kFrameSeparator = '\n';
constexpr int kWriteTimeoutMs = 500;

std::string frame(const json& payload) {
    return payload.dump() + kFrameSeparator;
}

std::string error_frame(const json& id, const std::string& message) {
    return frame(json{{"id", id}, {"ok", false}, {"error", message}});
}

std::string result_frame(const json& id, const json& value) {
    return frame(json{{"id", id}, {"ok", true}, {"result", value}});
}

}

DebugWire::DebugWire(CommandTable& commands) : commands_(commands) {}

DebugWire::~DebugWire() {
    stop();
}

bool DebugWire::start(const WireConfig& config, std::string& error) {
    config_ = config;
    if (!server_.open(config_.bind_address, config_.port, error)) {
        error = "debug wire " + error;
        return false;
    }
    log::info("debug wire listening on " + config_.bind_address + ":" + std::to_string(port()));
    return true;
}

void DebugWire::stop() {
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        platform::close_fd(clients_[i].fd);
    }
    clients_.clear();
    server_.close();
}

std::uint16_t DebugWire::port() const {
    return server_.port();
}

std::size_t DebugWire::client_count() const {
    return clients_.size();
}

void DebugWire::close_client(std::size_t index) {
    platform::close_fd(clients_[index].fd);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

void DebugWire::send(Client& client, const std::string& payload) {
    if (!platform::write_all(client.fd, payload, kWriteTimeoutMs)) {
        log::warn("debug wire: client write failed, dropping connection");
        client.fd = platform::kInvalidSocket;
    }
}

void DebugWire::handle_frame(Client& client, const std::string& text) {
    json parsed;
    try {
        parsed = json::parse(text);
    } catch (const json::exception& e) {
        send(client, error_frame(nullptr, std::string("malformed json: ") + e.what()));
        return;
    }
    if (!parsed.is_object()) {
        send(client, error_frame(nullptr, "request must be a JSON object"));
        return;
    }

    const json id = parsed.contains("id") ? parsed.at("id") : json(nullptr);

    if (!client.authenticated) {
        if (parsed.contains("auth") && parsed.at("auth").is_string() &&
            parsed.at("auth").get<std::string>() == config_.token) {
            client.authenticated = true;
            send(client, frame(json{{"event", "ready"}, {"ok", true}}));
        } else {
            send(client, error_frame(id, "authentication required"));
            client.fd = platform::kInvalidSocket;
        }
        return;
    }

    if (!parsed.contains("cmd") || !parsed.at("cmd").is_string()) {
        send(client, error_frame(id, "missing string field 'cmd'"));
        return;
    }

    json args = json::object();
    if (parsed.contains("args")) {
        args = parsed.at("args");
    }

    const std::string cmd = parsed.at("cmd").get<std::string>();
    CommandResult result;
    try {
        result = commands_.invoke(cmd, args);
    } catch (const std::exception& e) {
        result = command_error(std::string("command threw: ") + e.what());
    }

    if (result.ok) {
        send(client, result_frame(id, result.value));
    } else {
        send(client, error_frame(id, result.error));
    }
}

void DebugWire::poll() {
    if (server_.port() == 0) {
        return;
    }

    while (clients_.size() < config_.max_clients) {
        const platform::SocketHandle fd = server_.accept();
        if (fd == platform::kInvalidSocket) {
            break;
        }
        Client client(fd, config_.max_frame_bytes);
        client.authenticated = config_.token.empty();
        clients_.push_back(std::move(client));
    }

    std::vector<char> buffer(64 * 1024);
    for (std::size_t i = 0; i < clients_.size();) {
        Client& client = clients_[i];
        const std::int64_t got = platform::read_some(client.fd, buffer.data(), buffer.size());
        if (got < 0) {
            close_client(i);
            continue;
        }
        if (got > 0) {
            client.framer.push(buffer.data(), static_cast<std::size_t>(got));
            std::string text;
            while (client.framer.pop(text)) {
                handle_frame(client, text);
            }
        }
        if (client.framer.overflowed()) {
            log::warn("debug wire: frame larger than the configured bound, dropping client");
            close_client(i);
            continue;
        }
        if (client.fd == platform::kInvalidSocket) {
            close_client(i);
            continue;
        }
        ++i;
    }
}

}
