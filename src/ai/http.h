// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ai/provider.h"
#include "ai/profile.h"
#include "ai/server.h"
#include "ai/transport.h"
#include "core/image.h"

namespace noise::ai {

// The failure classes of §Failure handling, as an enum the app can branch on instead of parsing
// prose. There is no `unsupported_field` member: an unrecognised field is *ignored* by this engine
// (measured - a `seed` of "not-a-number" is answered with 200), so the row cannot be detected at run
// time and the request builder is what enforces it.
enum class FailureClass {
    none,
    cancelled,
    server_down,
    timeout,
    auth,
    no_model_loaded,
    model_not_found,
    not_edit_capable,
    size_rejected,
    refusal,
    bad_image,
};

const char* failure_name(FailureClass kind);

struct AiError {
    FailureClass kind = FailureClass::none;
    // What happened, naming the endpoint and the status.
    std::string message;
    // What to do about it, in the user's terms. Every class has one; an error without a hint is a bug.
    std::string hint;
    // The server's own body, truncated. Kept for `ai.log` and the job record, never parsed for
    // decisions - the class is decided from the status and the error object's shape.
    std::string detail;
};

// A wall clock, injected. Latency measurement is the one place wall time belongs, and a test that wants
// a deterministic ETA supplies its own.
using Clock = std::function<double()>;

// One server, one set of endpoints. Concurrency is one request at a time per server by the queue's rule,
// not by anything here.
class Client {
public:
    Client(Transport& transport, ServerProfile profile, Clock clock);

    const ServerProfile& profile() const { return profile_; }
    void set_profile(ServerProfile profile) { profile_ = std::move(profile); }
    // True once a request has answered on the `/api/v1` layout instead of `/v1`.
    bool using_alternate_prefix() const { return alternate_prefix_; }

    // `GET /health`. Answers `nullopt` with a classified error when the server is unreachable, so the
    // "Is Lemonade running?" row is a state the UI can show rather than an exception.
    std::optional<ServerHealth> health(AiError& error);
    std::optional<std::vector<ModelEntry>> models(AiError& error);
    bool load_model(const std::string& model_id, bool pinned, AiError& error);

    struct Answer {
        std::vector<std::uint8_t> png;
        Image image;
        double elapsed_s = 0.0;
    };

    // Sends a built request and decodes the image it returns. `cancelled` is polled by the transport.
    std::optional<Answer> run(const BuiltRequest& request, int timeout_s,
                              const std::atomic<bool>& cancelled, AiError& error);

    double now() const { return clock_(); }

    // A malformed entry in an otherwise readable model list is skipped and reported here: one bad row
    // must not cost the commands another model serves.
    const std::string& models_warning() const { return models_warning_; }

private:
    struct RawReply {
        bool ok = false;
        nlohmann::json body;
        AiError error;
        double elapsed_s = 0.0;
    };

    std::string url_for(const std::string& path) const;
    RawReply post_json(const std::string& path, const nlohmann::json& body, int timeout_s,
                       const std::atomic<bool>& cancelled);
    RawReply get(const std::string& path, int timeout_s, const std::atomic<bool>& cancelled);
    // The failure table: status and transport error in, one class and one hint out.
    AiError classify(const std::string& path, long status, const nlohmann::json& body,
                     const std::string& transport_error) const;

    Transport& transport_;
    ServerProfile profile_;
    Clock clock_;
    bool alternate_prefix_ = false;
    std::string models_warning_;
};

// The bytes a request will go out as, for `ai.preview` and for tests that assert field policy on
// serialized bytes rather than on a double. No credentials: this is displayable.
nlohmann::json outgoing_preview(const ServerProfile& profile, const BuiltRequest& request);

// The multipart body of an `edits` request. Image parts follow the request's ordered image list.
std::vector<std::uint8_t> multipart_body(const BuiltRequest& request, const std::string& boundary);
std::string multipart_boundary(const BuiltRequest& request);

}  // namespace noise::ai
