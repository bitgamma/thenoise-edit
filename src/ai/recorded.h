// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/transport.h"

namespace noise::ai {

// What went out, decoded far enough to assert on. A field policy test wants the bytes that left, and a
// recorded transport that only counted hits would let a wrong field through unnoticed.
struct CapturedRequest {
    std::string method;
    std::string url;
    // The part after the profile's base URL, which is what a recording matches on.
    std::string path;
    std::string content_type;
    // Multipart text parts and JSON fields alike, so a test asks `field("size")` either way.
    std::map<std::string, std::string> fields;
    std::size_t image_bytes = 0;
    std::string image_sha;
    std::string body_text;

    std::string field(const std::string& name) const {
        const auto found = fields.find(name);
        return found == fields.end() ? std::string() : found->second;
    }
};

// A `Transport` that answers from committed recordings and never opens a socket. This is the hermetic
// tier `AGENTS.md` §2 asks for: request field policy, response parsing, error mapping and cache keys,
// all checked offline.
//
// A recording is one JSON file:
//   {"name": "…", "method": "POST", "path": "/images/edits",
//    "match": {"model": "…", "size": "64x64"},       // every field must be equal
//    "status": 200, "json": {"data": [...]}}         // or "text": "not json"
// The first recording whose method, path and `match` fields fit answers, and it may answer repeatedly:
// the same request replayed is the same recording, which is what a cache-miss test needs.
// A server made of recordings. It is stateful - it remembers what was asked and what it answered - and
// it answers on the queue's worker thread, so it is handed around as a `unique_ptr` rather than copied.
class RecordedTransport final : public Transport {
public:
    // Null with a message naming the file: a directory of recordings that half-parses is worse than no
    // fixtures at all, because every request would then be a miss.
    static std::unique_ptr<RecordedTransport> load(const std::string& directory, std::string& error);
    // The same recordings from an in-memory list, for a unit test that would rather not touch disk.
    static std::unique_ptr<RecordedTransport> from_json(const std::vector<nlohmann::json>& bodies,
                                                        const std::string& origin,
                                                        std::string& error);

    IncomingResponse send(const OutgoingRequest& request,
                          const std::atomic<bool>& cancelled) override;

    // Everything below is read by whoever queued the request while the transport answers on a worker
    // thread, so the capture list, the hit counts and the misses are guarded rather than handed out by
    // reference.
    std::vector<CapturedRequest> sent() const;
    std::vector<std::pair<std::string, int>> hits() const;
    // What nothing answered, in the order asked. Empty when every request found a recording.
    std::vector<std::string> misses() const;
    void clear();

private:
    struct Recording {
        std::string name;
        std::string method;
        std::string path;
        std::map<std::string, std::string> match;
        long status = 200;
        std::vector<std::uint8_t> body;
        // Empty for an HTTP answer. Set to `cancelled` / `timeout` / anything else to replay a transfer
        // that never completed, which no status code can express.
        std::string transport_error;
        // Hold the answer this long before replying, so a caller can observe a state that only exists
        // while a request is in flight - a queued job, a cancellation, a timeout.
        int delay_ms = 0;
    };

    void add(Recording recording);

    std::vector<Recording> recordings_;
    mutable std::mutex seen_;
    std::vector<CapturedRequest> sent_;
    std::vector<std::string> misses_;
    std::map<std::string, int> hit_counts_;
};

}  // namespace noise::ai
