// SPDX-License-Identifier: Apache-2.0
#include "ai/server.h"

#include "platform/platform.h"

#include <cmath>

namespace noise::ai {

namespace {

constexpr long long pixels(const Extent& extent) {
    return static_cast<long long>(extent.w) * static_cast<long long>(extent.h);
}

// How much of a new sample an observation carries. Slow enough that one unlucky request - a model swap,
// a cold page cache - does not make the next ETA useless, fast enough that the estimate follows a box
// that genuinely got quicker.
inline constexpr double kEmaAlpha = 0.4;

std::string stripped(const std::string& url) {
    std::string out = url;
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool has_scheme(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// The part of the URL after the host, which is where a version prefix lives.
std::string path_of(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return {};
    }
    const std::size_t slash = url.find('/', scheme + 3);
    return slash == std::string::npos ? std::string() : url.substr(slash);
}

}  // namespace

std::string ServerProfile::url_for(const std::string& path) const {
    return base_url + (path.empty() || path.front() != '/' ? "/" + path : path);
}

std::string ServerProfile::alternate_prefix_url_for(const std::string& path) const {
    const std::string prefix = path_of(base_url);
    if (prefix != "/v1") {
        return {};
    }
    return base_url.substr(0, base_url.size() - prefix.size()) + "/api/v1" + path;
}

std::optional<ServerProfile> profile_from_environment(std::string& error) {
    const std::optional<std::string> url = platform::env("THENOISE_URL");
    if (!url.has_value() || url->empty()) {
        error = "THENOISE_URL is not set, so there is no server to reach";
        return std::nullopt;
    }
    if (!has_scheme(*url)) {
        error = "THENOISE_URL must start with http:// or https:// (got a value without a scheme)";
        return std::nullopt;
    }
    ServerProfile profile;
    profile.base_url = stripped(*url);
    // `https://host:13305` and `https://host:13305/v1` are the same server; the endpoints are written
    // without the prefix, so add it when the user gave only the host.
    if (path_of(profile.base_url).empty()) {
        profile.base_url += "/v1";
    }
    if (const std::optional<std::string> key = platform::env("THENOISE_API_KEY")) {
        profile.api_key = *key;
    }
    return profile;
}

void LatencyModel::observe(const std::string& model, Extent size, int steps, double seconds) {
    const std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[key(model, size, steps)];
    entry.model = model;
    entry.size = size;
    entry.steps = steps;
    // The ceiling is checked before anything is learned, including the first sample: a shape whose very
    // first answer came from the server's own cache would otherwise promise seconds for minutes.
    if (seconds < kServerCacheHitCeilingS) {
        ++entry.cache_hits;
        return;
    }
    if (entry.samples == 0) {
        entry.seconds = seconds;
        entry.samples = 1;
        return;
    }
    entry.seconds += kEmaAlpha * (seconds - entry.seconds);
    ++entry.samples;
}

std::optional<double> LatencyModel::predict(const std::string& model, Extent size,
                                           int steps) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return predict_locked(model, size, steps);
}

std::optional<double> LatencyModel::predict_locked(const std::string& model, Extent size,
                                                  int steps) const {
    const auto found = entries_.find(key(model, size, steps));
    if (found == entries_.end() || found->second.samples == 0) {
        return std::nullopt;
    }
    return found->second.seconds;
}

// A diffusion run costs its work per pixel per step, so a shape nobody has measured is scaled from one
// that has rather than left to the floor: the region is asked at its own size, which makes every new mark
// an unmeasured shape, and a floor that a normal ask outruns reports a healthy server as dead. Measured
// on the dev box against `seconds x pixels x steps`: 20.2 s at 1024^2/4 steps against 29.7 s at
// 1440x1088/4 (scaled 29.8), and 33.5 s at 1024^2/8 masked against 71.1 s at 1920x1088/8 masked
// (scaled 66.8).
std::optional<double> LatencyModel::scaled_locked(const std::string& model, Extent size,
                                                 int steps) const {
    if (steps <= 0 || pixels(size) <= 0) {
        return std::nullopt;
    }
    const Entry* best = nullptr;
    for (const auto& [entry_key, entry] : entries_) {
        if (entry.model != model || entry.samples == 0 || entry.steps <= 0 ||
            pixels(entry.size) <= 0) {
            continue;
        }
        if (best == nullptr || entry.samples > best->samples) {
            best = &entry;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    const double per_pixel = best->seconds /
                             (static_cast<double>(pixels(best->size)) *
                              static_cast<double>(best->steps));
    return per_pixel * static_cast<double>(pixels(size)) * static_cast<double>(steps);
}

int LatencyModel::request_timeout_s(const std::string& model, Extent size, int steps,
                                    int floor_seconds) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::optional<double> predicted = predict_locked(model, size, steps);
    if (!predicted.has_value()) {
        predicted = scaled_locked(model, size, steps);
    }
    if (!predicted.has_value()) {
        return floor_seconds;
    }
    const double wanted = *predicted * static_cast<double>(kRequestTimeoutEtaFactor);
    return static_cast<int>(wanted > static_cast<double>(floor_seconds) ? wanted
                                                                       : static_cast<double>(floor_seconds));
}

std::vector<LatencyModel::Entry> LatencyModel::entries() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> out;
    out.reserve(entries_.size());
    for (const auto& [key_text, entry] : entries_) {
        out.push_back(entry);
    }
    return out;
}

nlohmann::json LatencyModel::json() const {
    nlohmann::json rows = nlohmann::json::array();
    for (const Entry& entry : entries()) {
        rows.push_back(nlohmann::json{{"model", entry.model},
                                      {"size", std::to_string(entry.size.w) + "x" +
                                               std::to_string(entry.size.h)},
                                      {"steps", entry.steps},
                                      {"seconds", entry.seconds},
                                      {"samples", entry.samples},
                                      {"server_cache_hits", entry.cache_hits}});
    }
    return nlohmann::json{{"entries", rows}};
}

std::string LatencyModel::key(const std::string& model, Extent size, int steps) {
    return model + "|" + std::to_string(size.w) + "x" + std::to_string(size.h) + "|" +
           std::to_string(steps);
}

}  // namespace noise::ai
