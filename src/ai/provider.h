// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ai/profile.h"
#include "ai/recipes.h"
#include "ai/registry.h"

namespace noise::ai {

// The tier-1 field set, per endpoint. This function is the only place a field can enter a request:
// `build_request` adds nothing it has not checked here, and a field absent from this table is
// unreachable. Tier 2 (`negative_prompt`, `sigmas`, `sampler`, `kv_cache`, the in-generate upscale
// knobs, `film_grain`) is deliberately absent - it is out of the MVP, and the server forwards
// unrecognised keys to the engine silently, so an unlisted field cannot be detected downstream.
bool field_allowed(Endpoint endpoint, const std::string& name);
std::vector<std::string> allowed_fields(Endpoint endpoint);

// The path an endpoint answers on, out of the same table that says which fields it may carry: a price
// that named another path than the request would be a price for something else.
std::string request_path(Endpoint endpoint);

// What a request is, in the terms the cache keys on. Every value here went into the bytes the server
// sees, and nothing the server does not see (a key, a hostname beyond the profile name) is here.
struct RequestParams {
    std::string server_name;
    std::string model_id;
    std::string prompt;
    int width = 0;
    int height = 0;
    int steps = 0;
    double cfg_scale = 0.0;
    std::uint64_t seed = 0;
    // The prepped region this request uploads, by content. `generations` uploads nothing and the id is
    // that of the empty byte string, so a generation still keys on something real.
    std::string blob_id;
};

// A built request: ready to send, and describable without sending.
// One field of a request body. The value keeps its JSON type: the same number is a JSON number on a JSON
// endpoint and the digits of it in a multipart part, and a stringified `steps` is refused.
struct RequestField {
    std::string name;
    nlohmann::json value;

    // What goes in a multipart part for this value.
    std::string text() const;
};

struct BuiltRequest {
    Endpoint endpoint = Endpoint::edits;
    // Relative to the profile's base URL, which already carries `/v1`.
    std::string path;
    RequestParams params;

    // The body, in the order the endpoint's table lists the fields. This is the only representation:
    // `json_body()` and the multipart parts are both encoded from it, so a preview, a retry and the wire
    // cannot disagree about what was asked.
    std::vector<RequestField> fields;
    // The region, for an endpoint that takes it as a file part.
    std::vector<std::uint8_t> png;

    bool multipart() const { return endpoint == Endpoint::edits; }
    // The body to post as JSON. Null when the request is multipart, which has no JSON body at all.
    nlohmann::json json_body() const;

    // The stable text the cache key is a hash of. Ordered, exhaustive over `params`, and it names the
    // endpoint - the same prompt and size against `edits` and `generations` are different requests.
    std::string canonical_text() const;
    std::string cache_key() const;
    // What `ai.preview` answers: the exact field list in the order it goes out (`fields` is an array of
    // [name, value] pairs), the resolved size, and the blob the image part holds, by hash. No key
    // material, because the builder never sees any.
    nlohmann::json preview_json() const;
};

// A request read back from a job record, with its uploaded bytes. The field policy applies here as
// well: a record written by an older build, or edited by anyone, cannot smuggle a field into a request
// that the builder would never have added. `png` is read from the blob the record names.
std::optional<BuiltRequest> request_from_preview(const nlohmann::json& preview,
                                                 const std::vector<std::uint8_t>& png,
                                                 std::string& error);

struct RequestInput {
    const Recipe* recipe = nullptr;
    std::string server_name;
    std::string model_id;
    // The entry the model resolved to, for `image_defaults`. Null means the caller has no discovery
    // data, which is only acceptable together with both overrides below.
    const ModelEntry* model = nullptr;
    std::optional<int> steps_override;
    std::optional<double> cfg_scale_override;
    // `plan.requested`: the size the region compiler says to ask for. Explicit always - an edit without
    // `size` returns the model's default 1024² rather than the reference's size, at 4x the time.
    int width = 0;
    int height = 0;
    std::string prompt;
    std::uint64_t seed = 0;
    // The prepped region, PNG-encoded. These are the bytes the cache key refers to.
    std::vector<std::uint8_t> png;
};

// The builder. It validates its own values because the server does not: a `seed` of
// "not-a-number" is answered with 200 and a default, and a misspelt field name is ignored, so the only
// place either typo can be caught is here. Every refusal names the field and the value it was given.
std::optional<BuiltRequest> build_request(const RequestInput& input, const SizePolicy& size_policy,
                                          std::string& error);

}  // namespace noise::ai
