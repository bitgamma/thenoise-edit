// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/command.h"
#include "core/lines.h"
#include "core/log.h"
#include "dbg/wire.h"
#include "platform/net.h"
#include "platform/platform.h"

#include "arpa/inet.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

using namespace noise;
using nlohmann::json;

namespace {

constexpr const char* kAddress = "127.0.0.1";
constexpr double kDeadlineSeconds = 5.0;
constexpr int kPollSleepMs = 2;
constexpr int kWriteTimeoutMs = 1000;
constexpr std::size_t kMaxFrameBytes = 1024;

dbg::WireConfig config_for(std::string token) {
    dbg::WireConfig config;
    config.bind_address = kAddress;
    config.port = 0;
    config.token = std::move(token);
    config.max_frame_bytes = kMaxFrameBytes;
    config.max_clients = 4;
    return config;
}

void build_table(CommandTable& table) {
    // The wire tests use this as a transport probe and send it several different payloads, so it
    // declares each key they use - the point of those tests is the framing, not the field policy.
    table.add("test.echo", "echo args back", {}, {"ok", "round", "v"}, [](const json& args) {
        return command_ok(json{{"echo", args}});
    });
    table.add("test.fail", "always fails", {}, {}, [](const json&) {
        return command_error("deliberate failure");
    });
    table.add("test.throw", "always throws", {}, {}, [](const json&) -> CommandResult {
        throw std::runtime_error("handler exploded");
    });
}

int connect_client(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, kAddress, &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    const timeval timeout{0, 100000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return fd;
}

bool send_line(int client, const std::string& line) {
    return platform::write_all(client, line + "\n", kWriteTimeoutMs);
}

// Drives the server until one response frame arrives, or the deadline passes.
// The server must never block, so a stalled or absent reply fails here rather
// than hanging the suite.
bool read_response(dbg::DebugWire& wire, int client, LineFramer& framer, json& out) {
    std::vector<char> buffer(8192);
    const double deadline = platform::now_seconds() + kDeadlineSeconds;
    while (platform::now_seconds() < deadline) {
        wire.poll();
        const std::int64_t got = platform::read_some(client, buffer.data(), buffer.size());
        if (got < 0) {
            return false;
        }
        if (got > 0) {
            framer.push(buffer.data(), static_cast<std::size_t>(got));
        }
        // Pop even when nothing new arrived: one read can carry several frames.
        std::string frame;
        if (framer.pop(frame)) {
            out = json::parse(frame, nullptr, false);
            return !out.is_discarded();
        }
        platform::sleep_ms(kPollSleepMs);
    }
    return false;
}

json wait_for_response(dbg::DebugWire& wire, int client, LineFramer& framer) {
    json out;
    CHECK(read_response(wire, client, framer, out));
    return out;
}

std::string error_of(const json& response) {
    return response.contains("error") ? response.at("error").get<std::string>() : std::string();
}

void test_request_response_round_trip() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));
    CHECK(wire.port() != 0);

    const int client = connect_client(wire.port());
    CHECK(client >= 0);
    wire.poll();
    CHECK_EQ(wire.client_count(), std::size_t(1));

    CHECK(send_line(client, R"({"id":7,"cmd":"test.echo","args":{"v":42}})"));
    LineFramer framer(1024u * 1024u);
    const json response = wait_for_response(wire, client, framer);
    CHECK_EQ(response["id"].get<int>(), 7);
    CHECK_EQ(response["ok"].get<bool>(), true);
    CHECK_EQ(response["result"]["echo"]["v"].get<int>(), 42);

    platform::close_fd(client);
    wire.stop();
}

void test_two_frames_in_one_write() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));

    const int client = connect_client(wire.port());
    const std::string both = std::string(R"({"id":1,"cmd":"test.echo"})") + "\n" +
                             R"({"id":2,"cmd":"test.fail"})" + "\n";
    CHECK(platform::write_all(client, both, kWriteTimeoutMs));

    LineFramer framer(1024u * 1024u);
    json first;
    json second;
    CHECK(read_response(wire, client, framer, first));
    CHECK(read_response(wire, client, framer, second));
    CHECK_EQ(first["id"].get<int>(), 1);
    CHECK_EQ(first["ok"].get<bool>(), true);
    CHECK_EQ(second["id"].get<int>(), 2);
    CHECK_EQ(second["ok"].get<bool>(), false);
    CHECK_EQ(error_of(second), std::string("deliberate failure"));

    platform::close_fd(client);
    wire.stop();
}

void test_malformed_and_unknown_requests() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));

    const int client = connect_client(wire.port());
    LineFramer framer(1024u * 1024u);

    CHECK(send_line(client, "this is not json"));
    const json malformed = wait_for_response(wire, client, framer);
    CHECK_EQ(malformed["ok"].get<bool>(), false);
    CHECK(error_of(malformed).find("json") != std::string::npos);

    CHECK(send_line(client, R"({"id":3,"cmd":"no.such.command"})"));
    const json unknown = wait_for_response(wire, client, framer);
    CHECK_EQ(unknown["ok"].get<bool>(), false);
    CHECK(error_of(unknown).find("no.such.command") != std::string::npos);

    CHECK(send_line(client, R"({"id":4,"cmd":"test.throw"})"));
    const json thrown = wait_for_response(wire, client, framer);
    CHECK_EQ(thrown["ok"].get<bool>(), false);
    CHECK(error_of(thrown).find("handler exploded") != std::string::npos);

    platform::close_fd(client);
    wire.stop();
}

void test_frame_over_bound_drops_client() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));

    const int client = connect_client(wire.port());
    wire.poll();
    CHECK_EQ(wire.client_count(), std::size_t(1));

    // Far above max_frame_bytes, far below any socket buffer, so the write
    // itself cannot stall the test.
    const std::string oversize(4 * kMaxFrameBytes, 'x');
    CHECK(platform::write_all(client, oversize, kWriteTimeoutMs));

    const double deadline = platform::now_seconds() + kDeadlineSeconds;
    while (wire.client_count() > 0 && platform::now_seconds() < deadline) {
        wire.poll();
        platform::sleep_ms(kPollSleepMs);
    }
    CHECK_EQ(wire.client_count(), std::size_t(0));

    platform::close_fd(client);
    wire.stop();
}

void test_token_required_before_any_command() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for("s3cret"), error));

    LineFramer framer(1024u * 1024u);

    const int unauthenticated = connect_client(wire.port());
    CHECK(send_line(unauthenticated, R"({"id":1,"cmd":"test.echo"})"));
    const json rejected = wait_for_response(wire, unauthenticated, framer);
    CHECK_EQ(rejected["ok"].get<bool>(), false);
    CHECK(error_of(rejected).find("authentication") != std::string::npos);

    const int wrong = connect_client(wire.port());
    wire.poll();
    CHECK(send_line(wrong, R"({"auth":"wrong"})"));
    const json bad_auth = wait_for_response(wire, wrong, framer);
    CHECK_EQ(bad_auth["ok"].get<bool>(), false);

    const int good = connect_client(wire.port());
    wire.poll();
    CHECK(send_line(good, R"({"auth":"s3cret"})"));
    const json ready = wait_for_response(wire, good, framer);
    CHECK_EQ(ready["ok"].get<bool>(), true);
    CHECK_EQ(ready["event"].get<std::string>(), std::string("ready"));

    CHECK(send_line(good, R"({"id":9,"cmd":"test.echo","args":{"ok":true}})"));
    const json echoed = wait_for_response(wire, good, framer);
    CHECK_EQ(echoed["id"].get<int>(), 9);
    CHECK_EQ(echoed["result"]["echo"]["ok"].get<bool>(), true);

    platform::close_fd(unauthenticated);
    platform::close_fd(wrong);
    platform::close_fd(good);
    wire.stop();
}

// The server holds a fixed number of clients and the debug wire is the way in for every driver,
// so a peer that hangs up has to give its slot back: with `recv` returning 0 read as "nothing
// yet", closed connections piled up until no new one was ever accepted.
void test_a_hung_up_client_gives_its_slot_back() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));
    const std::uint16_t port = wire.port();

    // Well past config max_clients, each one asked to answer before it hangs up.
    for (int round = 0; round < 12; ++round) {
        const int client = connect_client(port);
        CHECK(client >= 0);
        if (client < 0) {
            break;
        }
        CHECK(send_line(client, R"({"cmd":"test.echo","args":{"round":1}})"));
        LineFramer framer(kMaxFrameBytes);
        const json reply = wait_for_response(wire, client, framer);
        CHECK(reply["ok"].get<bool>());
        platform::close_fd(client);

        const double deadline = platform::now_seconds() + kDeadlineSeconds;
        while (wire.client_count() > 0 && platform::now_seconds() < deadline) {
            wire.poll();
        }
        CHECK_EQ(wire.client_count(), 0u);
    }
    wire.stop();
}

void test_start_reports_a_bad_bind_address() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    dbg::WireConfig config = config_for("");
    config.bind_address = "not-an-address";
    std::string error;
    CHECK(!wire.start(config, error));
    CHECK(!error.empty());
    CHECK(error.find("not-an-address") != std::string::npos);
}

void test_stops_listening_after_stop() {
    CommandTable table;
    build_table(table);
    dbg::DebugWire wire(table);
    std::string error;
    CHECK(wire.start(config_for(""), error));
    const std::uint16_t port = wire.port();
    wire.stop();
    CHECK_EQ(wire.port(), std::uint16_t(0));
    CHECK(connect_client(port) < 0);
}

}

int main() {
    log::set_level(log::Level::Error);
    test_request_response_round_trip();
    test_two_frames_in_one_write();
    test_malformed_and_unknown_requests();
    test_frame_over_bound_drops_client();
    test_token_required_before_any_command();
    test_a_hung_up_client_gives_its_slot_back();
    test_start_reports_a_bad_bind_address();
    test_stops_listening_after_stop();
    return test::summary("wire");
}
