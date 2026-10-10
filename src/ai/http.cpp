// SPDX-License-Identifier: Apache-2.0
#include "ai/http.h"

#include "core/base64.h"
#include "io/png.h"

#include <algorithm>

namespace noise::ai {

namespace {

constexpr const char* kVersionPrefix = "/v1";
constexpr std::size_t kDetailLimit = 400;

std::string truncate(std::string text) {
    if (text.size() > kDetailLimit) {
        text.resize(kDetailLimit);
        text += "…";
    }
    return text;
}

const std::atomic<bool>& never_cancelled() {
    static const std::atomic<bool> kFalse{false};
    return kFalse;
}

// `{"error":{"message":…,"code":…,"type":…}}` is the shape both refusal bodies use; anything else is
// treated as a body with no error object rather than guessed at.
std::string error_field(const nlohmann::json& body, const char* key) {
    if (!body.is_object() || !body.contains("error")) {
        return {};
    }
    const nlohmann::json& error = body.at("error");
    if (error.is_object() && error.contains(key) && error.at(key).is_string()) {
        return error.at(key).get<std::string>();
    }
    if (error.is_string() && std::string(key) == "message") {
        return error.get<std::string>();
    }
    return {};
}

std::string lower(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string says(const nlohmann::json& body) {
    return lower(error_field(body, "message") + " " + error_field(body, "code") + " " +
                 error_field(body, "type"));
}

bool names_any(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The body is what says what went wrong; the endpoint says nothing about the cause. Classifying by path
// would call a missing `prompt` on `/images/edits` a model that cannot edit, and the instruction that
// follows would have the user mark a working editor as incapable.
bool body_says_size(const nlohmann::json& body) {
    return names_any(says(body), {"size", "width", "height", "resolution", "dimension"});
}

bool body_says_edit_unsupported(const nlohmann::json& body) {
    return names_any(says(body), {"edit", "instruct", "image-to-image"});
}

// `{"data":[{"b64_json":"…"}]}` is what generations, edits and upscale all answer with.
const nlohmann::json* image_part(const nlohmann::json& body) {
    if (!body.is_object() || !body.contains("data") || !body.at("data").is_array() ||
        body.at("data").empty()) {
        return nullptr;
    }
    const nlohmann::json& first = body.at("data")[0];
    if (!first.is_object() || !first.contains("b64_json") || !first.at("b64_json").is_string()) {
        return nullptr;
    }
    return &first;
}

}  // namespace

const char* failure_name(FailureClass kind) {
    switch (kind) {
        case FailureClass::none:
            return "none";
        case FailureClass::cancelled:
            return "cancelled";
        case FailureClass::server_down:
            return "server_down";
        case FailureClass::timeout:
            return "timeout";
        case FailureClass::auth:
            return "auth";
        case FailureClass::no_model_loaded:
            return "no_model_loaded";
        case FailureClass::model_not_found:
            return "model_not_found";
        case FailureClass::not_edit_capable:
            return "not_edit_capable";
        case FailureClass::size_rejected:
            return "size_rejected";
        case FailureClass::refusal:
            return "refusal";
        case FailureClass::bad_image:
            return "bad_image";
        case FailureClass::interrupted:
            return "interrupted";
    }
    return "refusal";
}

bool failure_is_terminal(FailureClass kind) {
    switch (kind) {
        case FailureClass::none:
        case FailureClass::cancelled:
        case FailureClass::timeout:
            return false;
        case FailureClass::server_down:
        case FailureClass::auth:
        case FailureClass::no_model_loaded:
        case FailureClass::model_not_found:
        case FailureClass::not_edit_capable:
        case FailureClass::size_rejected:
        case FailureClass::refusal:
        case FailureClass::bad_image:
        case FailureClass::interrupted:
            return true;
    }
    return true;
}

std::string multipart_boundary(const BuiltRequest& request) {
    // Derived from the request rather than random: a replayed request then produces the same bytes,
    // which is what lets a fixture and a live capture be compared at all. The blob hash is in the
    // boundary and the PNG is the blob, so a collision means the body contained its own hash - the
    // probability of which is not a design concern.
    return "----noise-" + request.params.blob_id.substr(0, 16);
}

std::vector<std::uint8_t> multipart_body(const BuiltRequest& request, const std::string& boundary) {
    std::vector<std::uint8_t> out;
    const std::string dash = "--" + boundary;
    for (const RequestField& field : request.fields) {
        out.insert(out.end(), dash.begin(), dash.end());
        const std::string head =
            "\r\nContent-Disposition: form-data; name=\"" + field.name + "\"\r\n\r\n" +
            field.text() + "\r\n";
        out.insert(out.end(), head.begin(), head.end());
    }
    out.insert(out.end(), dash.begin(), dash.end());
    const std::string image_head =
        "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"image.png\"\r\n"
        "Content-Type: image/png\r\n\r\n";
    out.insert(out.end(), image_head.begin(), image_head.end());
    out.insert(out.end(), request.png.begin(), request.png.end());
    const std::string crlf = "\r\n";
    out.insert(out.end(), crlf.begin(), crlf.end());
    const std::string closing = dash + "--\r\n";
    out.insert(out.end(), closing.begin(), closing.end());
    return out;
}

nlohmann::json outgoing_preview(const ServerProfile& profile, const BuiltRequest& request) {
    nlohmann::json out = request.preview_json();
    out["method"] = "POST";
    out["url"] = profile.url_for(request.path);
    out["transport"] = request.endpoint == Endpoint::edits ? "multipart/form-data"
                                                           : "application/json";
    if (request.endpoint == Endpoint::edits) {
        out["boundary"] = multipart_boundary(request);
    }
    // The header's *name* and whether it is sent, never its value: a preview is something the app prints.
    out["auth"] = profile.api_key.empty() ? "none"
                                          : (profile.auth == "query" ? "query api_key"
                                                                      : "Authorization: Bearer");
    return out;
}

Client::Client(Transport& transport, ServerProfile profile, Clock clock)
    : transport_(transport), profile_(std::move(profile)), clock_(std::move(clock)) {}

std::string Client::url_for(const std::string& path) const {
    if (alternate_prefix_) {
        const std::string alternate = profile_.alternate_prefix_url_for(path);
        if (!alternate.empty()) {
            return alternate;
        }
    }
    return profile_.url_for(path);
}

AiError Client::classify(const std::string& path, long status, const nlohmann::json& body,
                        const std::string& transport_error) const {
    AiError error;
    error.detail = body.is_null() ? std::string() : truncate(body.dump());
    if (!transport_error.empty()) {
        error.kind = transport_error == kErrorCancelled            ? FailureClass::cancelled
                     : transport_error == kErrorTimeout            ? FailureClass::timeout
                                                                   : FailureClass::server_down;
        error.message = path + ": " + transport_error;
        error.hint = error.kind == FailureClass::cancelled
                         ? "cancelled"
                         : "is the server reachable at " + profile_.base_url +
                               "? check it is running and that the port is open";
        return error;
    }
    const std::string message = error_field(body, "message");
    switch (status) {
        case 401:
        case 403:
            error.kind = FailureClass::auth;
            error.message = path + ": http " + std::to_string(status) +
                            (message.empty() ? "" : " - " + message);
            error.hint = profile_.api_key.empty()
                             ? "this server wants a key and none was configured (set $THENOISE_API_KEY)"
                             : "the key was sent as " +
                                   std::string(profile_.auth == "query" ? "?api_key=" : "Authorization: Bearer") +
                                   "; try the other form";
            return error;
        case 404:
            error.kind = FailureClass::model_not_found;
            error.message = path + ": http 404 - " +
                            (error_field(body, "code").empty() ? "not found"
                                                               : error_field(body, "code")) +
                            (message.empty() ? "" : " - " + message);
            error.hint = "the model is not on this server; import it or let discovery pick another";
            return error;
        case 503:
            error.kind = FailureClass::no_model_loaded;
            error.message = path + ": http 503" + (message.empty() ? "" : " - " + message);
            error.hint = "no model is resident; loading one costs tens of seconds, so it is offered "
                         "rather than done silently";
            return error;
        case 400:
            if (body_says_size(body)) {
                error.kind = FailureClass::size_rejected;
                error.hint = "clamp to the size policy and retry; an odd size is normally snapped, so "
                             "this is a size out of range";
            } else if (body_says_edit_unsupported(body)) {
                error.kind = FailureClass::not_edit_capable;
                error.hint = "this model refused an edit; resolve the edit role elsewhere";
            } else {
                error.kind = FailureClass::refusal;
                error.hint = "the request was refused; check the field list in the preview";
            }
            error.message = path + ": http 400" + (message.empty() ? "" : " - " + message);
            return error;
        default:
            error.kind = FailureClass::refusal;
            error.message = path + ": http " + std::to_string(status) +
                            (message.empty() ? "" : " - " + message);
            error.hint = "unexpected response; the body is in the job record";
            return error;
    }
}

Client::RawReply Client::post_json(const std::string& path, const nlohmann::json& body,
                                   int timeout_s, const std::atomic<bool>& cancelled) {
    OutgoingRequest request;
    request.method = "POST";
    request.url = url_for(path);
    request.bearer = profile_.auth == "header" ? profile_.api_key : std::string();
    if (profile_.auth == "query" && !profile_.api_key.empty()) {
        request.url += "?api_key=" + profile_.api_key;
    }
    request.content_type = "application/json";
    const std::string text = body.dump();
    request.body.assign(text.begin(), text.end());
    request.connect_timeout_s = profile_.connect_timeout_s;
    request.timeout_s = timeout_s;
    request.verify_tls = profile_.verify_tls;

    RawReply reply;
    const double started = clock_();
    const IncomingResponse response = transport_.send(request, cancelled);
    reply.elapsed_s = clock_() - started;
    if (response.transport_error.empty() && response.status >= 200 && response.status < 300) {
        try {
            reply.body = nlohmann::json::parse(response.body);
            reply.ok = true;
            return reply;
        } catch (const nlohmann::json::parse_error&) {
            reply.error.kind = FailureClass::refusal;
            reply.error.message = path + ": the reply is not JSON";
            reply.error.detail = truncate(response.text());
            reply.error.hint = "a proxy or a login page answered instead of the server";
            return reply;
        }
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response.body);
    } catch (const nlohmann::json::parse_error&) {
        parsed = nullptr;
    }
    reply.error = classify(path, response.status, parsed, response.transport_error);
    if (reply.error.detail.empty()) {
        reply.error.detail = truncate(response.text());
    }
    return reply;
}

Client::RawReply Client::get(const std::string& path, int timeout_s,
                             const std::atomic<bool>& cancelled) {
    OutgoingRequest request;
    request.method = "GET";
    request.url = url_for(path);
    request.bearer = profile_.auth == "header" ? profile_.api_key : std::string();
    if (profile_.auth == "query" && !profile_.api_key.empty()) {
        request.url += "?api_key=" + profile_.api_key;
    }
    request.connect_timeout_s = profile_.connect_timeout_s;
    request.timeout_s = timeout_s;
    request.verify_tls = profile_.verify_tls;

    RawReply reply;
    const double started = clock_();
    const IncomingResponse response = transport_.send(request, cancelled);
    reply.elapsed_s = clock_() - started;
    if (!response.transport_error.empty() || response.status < 200 || response.status >= 300) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(response.body);
        } catch (const nlohmann::json::parse_error&) {
            parsed = nullptr;
        }
        reply.error = classify(path, response.status, parsed, response.transport_error);
        reply.error.detail = truncate(response.text());
        return reply;
    }
    try {
        reply.body = nlohmann::json::parse(response.body);
        reply.ok = true;
    } catch (const nlohmann::json::parse_error& exception) {
        reply.error.kind = FailureClass::refusal;
        reply.error.message = path + ": the reply is not JSON";
        reply.error.detail = truncate(response.text());
        reply.error.hint = "a proxy or a login page answered instead of the server";
    }
    return reply;
}

std::optional<ServerHealth> Client::health(AiError& error) {
    static const std::atomic<bool>& no_cancel = never_cancelled();
    RawReply reply = get("/health", profile_.connect_timeout_s * 2, no_cancel);
    // The other documented prefix is worth one try: `/v1` and `/api/v1` are both in the field, and a
    // server that answers on one is not a server that is down.
    if (!reply.ok && reply.error.kind == FailureClass::server_down && !alternate_prefix_ &&
        !profile_.alternate_prefix_url_for("/health").empty()) {
        alternate_prefix_ = true;
        const RawReply alternate = get("/health", profile_.connect_timeout_s * 2, no_cancel);
        if (alternate.ok) {
            reply = alternate;
        } else {
            alternate_prefix_ = false;
        }
    }
    if (!reply.ok) {
        error = reply.error;
        return std::nullopt;
    }
    std::string parse_error;
    const ServerHealth health = parse_health(reply.body, parse_error);
    if (!parse_error.empty()) {
        error.kind = FailureClass::refusal;
        error.message = "/health: " + parse_error;
        error.detail = truncate(reply.body.dump());
        error.hint = "the server answered something the health contract does not describe";
        return std::nullopt;
    }
    return health;
}

std::optional<std::vector<ModelEntry>> Client::models(AiError& error) {
    static const std::atomic<bool>& no_cancel = never_cancelled();
    const RawReply reply = get("/models", profile_.request_timeout_s, no_cancel);
    if (!reply.ok) {
        error = reply.error;
        return std::nullopt;
    }
    std::string parse_error;
    std::vector<ModelEntry> entries = parse_models(reply.body, parse_error);
    if (!parse_error.empty()) {
        // A skipped row is reported, not fatal: one malformed entry must not cost the commands another
        // model serves.
        models_warning_ = parse_error;
    }
    if (entries.empty()) {
        error.kind = FailureClass::refusal;
        error.message = "/models answered with no usable entries";
        error.detail = truncate(reply.body.dump());
        error.hint = "the server answered, but not with a model list this client can read";
        return std::nullopt;
    }
    return entries;
}

bool Client::load_model(const std::string& model_id, bool pinned, AiError& error) {
    static const std::atomic<bool>& no_cancel = never_cancelled();
    const RawReply reply = post_json("/load",
                                     nlohmann::json{{"model_name", model_id}, {"pinned", pinned}},
                                     profile_.request_timeout_s, no_cancel);
    if (!reply.ok) {
        error = reply.error;
        return false;
    }
    return true;
}

std::optional<Client::Answer> Client::run(const BuiltRequest& request, int timeout_s,
                                          const std::atomic<bool>& cancelled, AiError& error) {
    OutgoingRequest outgoing;
    outgoing.method = "POST";
    outgoing.url = url_for(request.path);
    outgoing.bearer = profile_.auth == "header" ? profile_.api_key : std::string();
    if (profile_.auth == "query" && !profile_.api_key.empty()) {
        outgoing.url += "?api_key=" + profile_.api_key;
    }
    if (request.endpoint == Endpoint::edits) {
        const std::string boundary = multipart_boundary(request);
        outgoing.content_type = "multipart/form-data; boundary=" + boundary;
        outgoing.body = multipart_body(request, boundary);
    } else {
        outgoing.content_type = "application/json";
        const std::string text = request.json_body().dump();
        outgoing.body.assign(text.begin(), text.end());
    }
    outgoing.connect_timeout_s = profile_.connect_timeout_s;
    outgoing.timeout_s = timeout_s;
    outgoing.verify_tls = profile_.verify_tls;

    const double started = clock_();
    const IncomingResponse response = transport_.send(outgoing, cancelled);
    const double elapsed = clock_() - started;

    if (!response.transport_error.empty() || response.status < 200 || response.status >= 300) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(response.body);
        } catch (const nlohmann::json::parse_error&) {
            parsed = nullptr;
        }
        error = classify(request.path, response.status, parsed, response.transport_error);
        if (error.detail.empty()) {
            error.detail = truncate(response.text());
        }
        return std::nullopt;
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(response.body);
    } catch (const nlohmann::json::parse_error&) {
        error.kind = FailureClass::bad_image;
        error.message = request.path + ": the reply is not JSON, so there is no image in it";
        error.detail = truncate(response.text());
        error.hint = "something that is not the API answered at " + profile_.base_url +
                     " - a proxy or a login page; a new seed will not change it";
        return std::nullopt;
    }
    const nlohmann::json* part = image_part(body);
    if (part == nullptr) {
        error.kind = FailureClass::bad_image;
        error.message = request.path + ": http 200 with no data[0].b64_json";
        error.detail = truncate(body.dump());
        error.hint = "the server accepted the request and returned nothing to composite";
        return std::nullopt;
    }
    std::string decode_error;
    const std::optional<std::vector<std::uint8_t>> png =
        base64_decode(part->at("b64_json").get<std::string>(), decode_error);
    if (!png.has_value()) {
        error.kind = FailureClass::bad_image;
        error.message = request.path + ": " + decode_error;
        error.hint = "retry once with a new seed and mark this blob bad";
        return std::nullopt;
    }
    Image image = io::decode_png(*png, decode_error);
    if (image.empty()) {
        error.kind = FailureClass::bad_image;
        error.message = request.path + ": the returned PNG does not decode (" + decode_error + ")";
        error.hint = "mark this blob bad and retry with a new seed";
        return std::nullopt;
    }
    Answer answer;
    answer.png = std::move(*png);
    answer.image = std::move(image);
    answer.elapsed_s = elapsed;
    return answer;
}

}  // namespace noise::ai
