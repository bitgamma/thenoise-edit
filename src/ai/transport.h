// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace noise::ai {

// One HTTP call, in the terms both transports understand. Credentials are a header, never part of the
// URL, so a URL can be logged and a key cannot ride along with it.
struct OutgoingRequest {
    std::string method = "GET";
    std::string url;
    // Empty means no `Authorization` header at all.
    std::string bearer;
    // Empty for a GET. For a multipart POST it carries the boundary.
    std::string content_type;
    std::vector<std::uint8_t> body;
    int connect_timeout_s = 5;

    // Local diffusion is slow by design, so this is the adaptive one (`LatencyModel` predicts it), not
    // a constant a 20-minute refine would trip.
    int timeout_s = 600;
    bool verify_tls = true;

    std::string text() const { return std::string(body.begin(), body.end()); }
};

// A refusal is data, not an exception: the failure table in `ARCHITECTURE.md` branches on the status and
// the body, so both come back and the transport decides nothing.
struct IncomingResponse {
    // 0 when no status could be read at all.
    long status = 0;
    std::vector<std::uint8_t> body;
    // Non-empty exactly when no HTTP status was obtained: refused, unreachable, TLS, timed out, or
    // cancelled. Names the reason the transport saw.
    std::string transport_error;

    bool connected() const { return status != 0; }
    std::string text() const { return std::string(body.begin(), body.end()); }
};

// Cancelling is cooperative: the queue sets the flag and the transport aborts the transfer in flight,
// which for libcurl is what a closed connection on the server means.
class Transport {
public:
    virtual ~Transport() = default;
    virtual IncomingResponse send(const OutgoingRequest& request,
                                  const std::atomic<bool>& cancelled) = 0;
};

// Where a transport failed before an HTTP status existed. Kept to two words because `Client` maps them
// to failure classes and no prose is parsed.
inline constexpr const char* kErrorCancelled = "cancelled";
inline constexpr const char* kErrorTimeout = "timeout";

}  // namespace noise::ai
