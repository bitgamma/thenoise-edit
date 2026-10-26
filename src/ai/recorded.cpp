// SPDX-License-Identifier: Apache-2.0
#include "ai/recorded.h"

#include "core/hash.h"
#include "io/file.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace noise::ai {

namespace {

std::string trim(const std::string& text) {
    const char* ws = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(ws);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(ws);
    return text.substr(begin, end - begin + 1);
}

// `https://host:13305/v1/images/edits?x=1` -> `/v1/images/edits`. A recording names the endpoint, never
// a host: the dev box rotates by occupancy and a fixture with an address in it would be a leak and a lie.
std::string path_of_url(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t slash = url.find('/', scheme + 3);
    if (slash == std::string::npos) {
        return "/";
    }
    const std::size_t query = url.find('?', slash);
    return query == std::string::npos ? url.substr(slash) : url.substr(slash, query - slash);
}

// `name="size"` and `boundary=----noise-x` are both legal, and a recorder that understood only the
// quoted form would see no fields at all in the bodies this app itself sends.
std::string attribute_of(const std::string& headers, const char* key) {
    const std::string needle = std::string(key) + "=";
    for (std::size_t begin = headers.find(needle); begin != std::string::npos;
         begin = headers.find(needle, begin + needle.size())) {
        // A `name=` inside `filename=` is another attribute, not this one.
        if (begin != 0 && headers[begin - 1] != ' ' && headers[begin - 1] != ';') {
            continue;
        }
        const std::size_t start = begin + needle.size();
        if (start < headers.size() && headers[start] == '"') {
            const std::size_t end = headers.find('"', start + 1);
            return end == std::string::npos ? std::string() : headers.substr(start + 1, end - start - 1);
        }
        const std::size_t end = headers.find(';', start);
        return trim(end == std::string::npos ? headers.substr(start) : headers.substr(start, end - start));
    }
    return {};
}

std::string header_of(const std::string& headers, const std::string& key) {
    std::size_t begin = 0;
    while (begin < headers.size()) {
        const std::size_t end = headers.find("\r\n", begin);
        const std::string line = headers.substr(begin, end == std::string::npos
                                                          ? std::string::npos
                                                          : end - begin);
        const std::string prefix = key + ": ";
        if (line.rfind(prefix, 0) == 0) {
            return line.substr(prefix.size());
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 2;
    }
    return {};
}

std::map<std::string, std::string> json_fields(const std::string& text) {
    std::map<std::string, std::string> out;
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error&) {
        return out;
    }
    if (!body.is_object()) {
        return out;
    }
    for (auto it = body.begin(); it != body.end(); ++it) {
        out[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    }
    return out;
}

void multipart_parts(const std::string& body, const std::string& boundary,
                     CapturedRequest& captured) {
    const std::string dash = "--" + boundary;
    std::size_t at = body.find(dash);
    while (at != std::string::npos) {
        at += dash.size();
        if (body.compare(at, 2, "--") == 0) {
            return;
        }
        const std::size_t head_end = body.find("\r\n\r\n", at);
        if (head_end == std::string::npos) {
            return;
        }
        const std::string headers = body.substr(at, head_end - at);
        const std::size_t next = body.find(dash, head_end);
        if (next == std::string::npos) {
            return;
        }
        // The CRLF in front of a boundary is part of the delimiter, so the part ends before it: hashing
        // it in would make a capture disagree with the bytes that were uploaded.
        const std::size_t payload_end =
            next >= 2 && body.compare(next - 2, 2, "\r\n") == 0 ? next - 2 : next;
        const std::string payload = body.substr(head_end + 4, payload_end - (head_end + 4));
        const std::string name = attribute_of(headers, "name");
        captured.part_names.push_back(name);
        if (headers.find("filename=") != std::string::npos) {
            CapturedRequest::ImagePart image;
            image.field = name;
            image.filename = attribute_of(headers, "filename");
            image.content_type = header_of(headers, "Content-Type");
            image.bytes.assign(payload.begin(), payload.end());
            image.sha256 = sha256_hex(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
            captured.image_bytes += image.bytes.size();
            if (captured.images.empty()) {
                captured.image_sha = image.sha256;
            }
            captured.images.push_back(std::move(image));
        } else if (!name.empty()) {
            captured.fields[name] = trim(payload);
        }
        at = next;
    }
}

CapturedRequest capture(const OutgoingRequest& request) {
    CapturedRequest captured;
    captured.method = request.method;
    captured.url = request.url;
    captured.path = path_of_url(request.url);
    captured.content_type = request.content_type;
    captured.body_text = request.text();
    captured.request_body_bytes = request.body.size();
    if (captured.content_type.rfind("multipart/form-data", 0) == 0) {
        const std::string boundary = attribute_of(captured.content_type, "boundary");
        multipart_parts(captured.body_text, boundary, captured);
    } else if (!captured.body_text.empty()) {
        captured.fields = json_fields(captured.body_text);
    }
    return captured;
}

std::vector<std::uint8_t> body_of(const nlohmann::json& recording, const std::string& origin,
                                  std::string& error, bool& ok) {
    ok = true;
    if (recording.contains("json")) {
        const std::string text = recording.at("json").dump();
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (recording.contains("text")) {
        if (!recording.at("text").is_string()) {
            error = origin + ": 'text' must be a string";
            ok = false;
            return {};
        }
        const std::string text = recording.at("text").get<std::string>();
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (recording.contains("transport_error")) {
        return {};  // no body: the transfer itself failed
    }
    error = origin + ": a recording needs 'json', 'text' or 'transport_error' to answer with";
    ok = false;
    return {};
}

}  // namespace

void RecordedTransport::add(Recording recording) { recordings_.push_back(std::move(recording)); }

std::unique_ptr<RecordedTransport> RecordedTransport::from_json(
    const std::vector<nlohmann::json>& bodies, const std::string& origin, std::string& error) {
    auto transport = std::make_unique<RecordedTransport>();
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        const std::string where = origin + "[" + std::to_string(i) + "]";
        const nlohmann::json& body = bodies[i];
        if (!body.is_object() || !body.contains("path") || !body.at("path").is_string()) {
            error = where + ": a recording needs a string 'path'";
            return transport;
        }
        Recording recording;
        recording.name = body.contains("name") && body.at("name").is_string()
                             ? body.at("name").get<std::string>()
                             : where;
        recording.method = body.contains("method") && body.at("method").is_string()
                               ? body.at("method").get<std::string>()
                               : "POST";
        recording.path = body.at("path").get<std::string>();
        recording.transport_error = body.contains("transport_error")
                                        ? body.at("transport_error").get<std::string>()
                                        : std::string();
        if (body.contains("delay_ms")) {
            if (!body.at("delay_ms").is_number_integer()) {
                error = where + ": 'delay_ms' must be an integer";
                return transport;
            }
            recording.delay_ms = body.at("delay_ms").get<int>();
        }
        if (body.contains("match")) {
            if (!body.at("match").is_object()) {
                error = where + ": 'match' must be an object of field values";
                return transport;
            }
            for (auto it = body.at("match").begin(); it != body.at("match").end(); ++it) {
                recording.match[it.key()] = it.value().is_string()
                                                ? it.value().get<std::string>()
                                                : it.value().dump();
            }
        }
        if (body.contains("match_images")) {
            if (!body.at("match_images").is_array()) {
                error = where + ": 'match_images' must be an ordered array of SHA-256 strings";
                return transport;
            }
            recording.image_sha256s = std::vector<std::string>();
            for (const nlohmann::json& hash : body.at("match_images")) {
                if (!hash.is_string()) {
                    error = where + ": every 'match_images' entry must be a SHA-256 string";
                    return transport;
                }
                recording.image_sha256s->push_back(hash.get<std::string>());
            }
        }
        recording.status = body.contains("status") && body.at("status").is_number_integer()
                               ? static_cast<long>(body.at("status").get<long long>())
                               : 200;
        bool ok = false;
        recording.body = body_of(body, where, error, ok);
        if (!ok) {
            return transport;
        }
        transport->add(std::move(recording));
    }
    return transport;
}

std::unique_ptr<RecordedTransport> RecordedTransport::load(const std::string& directory,
                                                           std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        error = "no recording directory at '" + directory + "'";
        return nullptr;
    }
    std::vector<std::string> paths;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            paths.push_back(entry.path().string());
        }
    }
    std::sort(paths.begin(), paths.end());  // determinism: the answer must not depend on readdir order
    std::vector<nlohmann::json> bodies;
    for (const std::string& path : paths) {
        std::vector<std::uint8_t> bytes;
        if (!io::read_file(path, bytes, error)) {
            return nullptr;
        }
        try {
            bodies.push_back(nlohmann::json::parse(bytes));
        } catch (const nlohmann::json::parse_error& exception) {
            error = path + ": " + exception.what();
            return nullptr;
        }
    }
    std::unique_ptr<RecordedTransport> transport = from_json(bodies, directory, error);
    if (!error.empty()) {
        return nullptr;
    }
    if (bodies.empty()) {
        error = "'" + directory + "' holds no *.json recordings";
        return nullptr;
    }
    return transport;
}

IncomingResponse RecordedTransport::send(const OutgoingRequest& request,
                                         const std::atomic<bool>& cancelled) {
    IncomingResponse response;
    if (cancelled.load()) {
        response.transport_error = kErrorCancelled;
        return response;
    }
    const CapturedRequest captured = capture(request);
    for (const Recording& recording : recordings_) {
        if (recording.method != captured.method ||
            captured.path.size() < recording.path.size() ||
            captured.path.compare(captured.path.size() - recording.path.size(), recording.path.size(),
                                  recording.path) != 0) {
            continue;
        }
        bool fits = true;
        for (const auto& [name, value] : recording.match) {
            if (captured.field(name) != value) {
                fits = false;
                break;
            }
        }
        if (fits && recording.image_sha256s.has_value()) {
            std::vector<std::string> hashes;
            for (const CapturedRequest::ImagePart& image : captured.images) {
                hashes.push_back(image.sha256);
            }
            fits = hashes == *recording.image_sha256s;
        }
        if (!fits) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(seen_);
            ++hit_counts_[recording.name];
        }
        // The delay is polled rather than slept through: a test that wants the transfer to stop mid-way
        // must be able to stop it, and a transport that ignored `cancelled` would let every cancellation
        // test pass on a job that never actually stopped.
        for (int waited = 0; waited < recording.delay_ms; waited += 10) {
            if (cancelled.load()) {
                response.transport_error = kErrorCancelled;
                sent_.push_back(captured);
                return response;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!recording.transport_error.empty()) {
            response.transport_error = recording.transport_error;
            std::lock_guard<std::mutex> lock(seen_);
            sent_.push_back(captured);
            return response;
        }
        response.status = recording.status;
        response.body = recording.body;
        std::lock_guard<std::mutex> lock(seen_);
        sent_.push_back(captured);
        return response;
    }
    std::string asked = captured.method + " " + captured.path + " [" + captured.content_type + "]";
    for (const auto& [name, value] : captured.fields) {
        if (name == "image" || value.size() > 48) {
            continue;
        }
        asked += " " + name + "=" + value;
    }
    for (const CapturedRequest::ImagePart& image : captured.images) {
        asked += " " + image.field + "(" + image.sha256 + ", " +
                 std::to_string(image.bytes.size()) + " bytes)";
    }
    {
        std::lock_guard<std::mutex> lock(seen_);
        misses_.push_back(asked);
        sent_.push_back(captured);
    }
    response.transport_error = "no recording matches " + asked;
    return response;
}

std::vector<CapturedRequest> RecordedTransport::sent() const {
    std::lock_guard<std::mutex> lock(seen_);
    return sent_;
}

std::vector<std::pair<std::string, int>> RecordedTransport::hits() const {
    std::lock_guard<std::mutex> lock(seen_);
    return {hit_counts_.begin(), hit_counts_.end()};
}

std::vector<std::string> RecordedTransport::misses() const {
    std::lock_guard<std::mutex> lock(seen_);
    return misses_;
}

void RecordedTransport::clear() {
    std::lock_guard<std::mutex> lock(seen_);
    sent_.clear();
    misses_.clear();
}

}  // namespace noise::ai
