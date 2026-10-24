// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/geom.h"

namespace noise::ai {

// Connect is short: a refused port should be reported as "is Lemonade running?" in seconds, not after a
// long wait that looks like a slow generation.
inline constexpr int kConnectTimeoutS = 5;
// A request timeout is never below this, and never below four times the predicted time - local
// diffusion is slow by design and the ETA is the only reasonable ceiling.
//
// The floor is what governs the first ask of a shape this box has never run, and it has to survive one:
// measured on the dev box, a masked edit at the pixel ceiling (1920x1088, the model's default 8 steps)
// took 71 s, and a model that is not resident yet loads inside the request at 72-198 s. Cancel is the
// answer to a wedged server; a floor below a normal first ask makes every new mark fail once.
inline constexpr int kRequestTimeoutFloorS = 600;
inline constexpr int kRequestTimeoutEtaFactor = 4;

// A response under this is the server answering from its own cache (~0.1 s measured on the dev box)
// rather than running a model, so it must not feed the latency estimate: an ETA that drifts toward the
// cache's latency would make every job look instant. Far below the 2 s draft / 100 s refine it has to
// separate, which is why a threshold is honest here where a measured value would be a race.
inline constexpr double kServerCacheHitCeilingS = 1.0;

// One server the user can point the app at. The base URL is where the user put it - a hostname never
// enters the repo, and no default is baked in either: an unset URL is a refused connect, not a guess.
struct ServerProfile {
    std::string name = "local";
    // Includes the version prefix, no trailing slash: `https://host:13305/v1`.
    std::string base_url;
    std::string api_key;
    bool verify_tls = true;
    int connect_timeout_s = kConnectTimeoutS;
    int request_timeout_s = kRequestTimeoutFloorS;
    // "header" sends `Authorization: Bearer`; "query" is the legacy `?api_key=` form for servers that
    // only accept that. Nothing else is sent, and no admin key exists in this app at all.
    std::string auth = "header";

    // `path` starts with '/' and carries no prefix (`/images/edits`).
    std::string url_for(const std::string& path) const;

    // The same profile with the `/v1` prefix replaced by `/api/v1`, which is what the other documented
    // layout answers on; unchanged when there is no prefix to swap.
    std::string alternate_prefix_url_for(const std::string& path) const;
};

// The only way a profile is built without being typed into the UI: `$THENOISE_URL` and
// `$THENOISE_API_KEY`. A missing URL is a refusal that names the variable; a missing key is allowed,
// because a server on a trusted LAN may run without one.
std::optional<ServerProfile> profile_from_environment(std::string& error);

// Client-measured latency per `(model, size, steps)`, as an exponential moving average. The engine's
// published benchmarks seed nothing: every box differs, and which model answers a role changes with what
// that box has downloaded.
class LatencyModel {
public:
    // Records one *real* run. A response that came back inside `kServerCacheHitCeilingS` is the server's
    // cache, and is counted separately rather than learned from.
    void observe(const std::string& model, Extent size, int steps, double seconds);

    // This box's measured time for this exact shape, and nothing otherwise: the ETA is a promise about
    // waiting, and a scaled figure is not one this box has observed.
    std::optional<double> predict(const std::string& model, Extent size, int steps) const;

    // The ceiling to wait before calling a request dead. Not a promise, so it may reason where the ETA
    // cannot: with no run of this exact shape it scales the model's best-sampled run by pixels and steps,
    // and only falls back to `floor_seconds` when the model has never run here at all.
    int request_timeout_s(const std::string& model, Extent size, int steps,
                          int floor_seconds) const;

    struct Entry {
        std::string model;
        Extent size{0, 0};
        int steps = 0;
        double seconds = 0.0;
        int samples = 0;
        int cache_hits = 0;
    };
    std::vector<Entry> entries() const;
    nlohmann::json json() const;

private:
    static std::string key(const std::string& model, Extent size, int steps);
    // For the methods that already hold `mutex_`; taking it twice would deadlock.
    std::optional<double> predict_locked(const std::string& model, Extent size, int steps) const;
    std::optional<double> scaled_locked(const std::string& model, Extent size, int steps) const;

    // Guarded by the model's own lock, in every method. The queue's worker writes it the moment an
    // answer arrives and the panel reads an estimate on every frame it draws, and a `std::map`
    // traversed while another thread rebalances it does not fail - it hangs inside `find`. A call
    // site cannot be trusted with this, which is why the queue's mutex does not cover it.
    std::map<std::string, Entry> entries_;
    mutable std::mutex mutex_;
};

}  // namespace noise::ai
