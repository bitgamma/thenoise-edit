// SPDX-License-Identifier: Apache-2.0
#include "app/ai_session.h"

#include "app/state.h"

#include "ai/recorded.h"
#include "ai/transport_curl.h"
#include "core/log.h"
#include "platform/platform.h"

#include <filesystem>
#include <vector>

#include <utility>

namespace noise::app {

namespace {

constexpr const char* kFixturesVariable = "NOISE_AI_FIXTURES";
// A recording has no host, but a request still needs a URL to be shaped: `.invalid` is reserved and
// can never resolve, so a replay session carries an address that says what it is and cannot leak a
// real one into a log.
constexpr const char* kRecordedUrl = "http://recordings.invalid/v1";

ai::ServerProfile session_profile(bool replaying) {
    std::string error;
    std::optional<ai::ServerProfile> profile = ai::profile_from_environment(error);
    if (profile.has_value()) {
        return *profile;
    }
    if (replaying) {
        ai::ServerProfile recorded;
        recorded.base_url = kRecordedUrl;
        return recorded;
    }
    log::warn("ai: " + error);
    return ai::ServerProfile{};
}

}  // namespace

ai::RegionPolicy AiSession::table_region_policy() const {
    ai::RegionPolicy policy{};
    policy.size_multiple = roles.size_policy.multiple;
    policy.max_area_px = roles.size_policy.max_area_px;
    policy.context_ratio = roles.context_policy.ratio;
    policy.min_context_px = roles.context_policy.min_px;
    return policy;
}

ai::RegionPolicy AiSession::region_policy(const ai::Recipe& recipe) const {
    ai::RegionPolicy policy = table_region_policy();
    if (recipe.context_ratio.has_value()) {
        policy.context_ratio = *recipe.context_ratio;
    }
    if (recipe.context_min_px.has_value()) {
        policy.min_context_px = *recipe.context_min_px;
    }
    return policy;
}

void open_ai_session(noise::AppState& state) {
    AiSession& ai = state.ai;
    ai.cache = std::make_unique<ai::BlobCache>(state.runtime_dir + "/ai/cache");

    if (ai.resource_dir.empty()) {
        // The tables ship with the tree. `$NOISE_EDIT_ASSETS` says where explicitly, which is what a
        // packaged build and a probe do; otherwise the first place a `roles.json` actually is wins, and
        // a session with none reports that rather than resolving roles against an empty table.
        const std::vector<std::string> candidates = {
            platform::env("NOISE_EDIT_ASSETS").value_or(std::string()),
            "assets",
            state.runtime_dir + "/assets",
        };
        for (const std::string& candidate : candidates) {
            if (candidate.empty()) {
                continue;
            }
            if (std::filesystem::exists(std::filesystem::path(candidate) / "roles.json")) {
                ai.resource_dir = candidate;
                break;
            }
        }
        if (ai.resource_dir.empty()) {
            ai.resource_dir = candidates.back();
        }
    }

    std::string error;
    const std::optional<ai::RoleTable> roles =
        ai::load_roles(ai.resource_dir + "/roles.json", error);
    if (roles.has_value()) {
        ai.roles = *roles;
        ai.registry = std::make_unique<ai::Registry>(*roles);
    } else {
        ai.roles_error = error;
        log::error("ai: " + error);
    }

    const std::optional<ai::RecipeSet> recipes =
        ai::load_recipes(ai.resource_dir + "/recipes", error);
    if (recipes.has_value()) {
        ai.recipes = *recipes;
    } else {
        ai.recipes_error = error;
        log::error("ai: " + error);
    }

    // Recordings first: a probe that means to replay must never reach the network by accident, and a
    // fixture directory that fails to load is reported rather than falling through to a live server.
    bool replaying = false;
    if (const std::optional<std::string> fixtures = platform::env(kFixturesVariable)) {
        std::string load_error;
        std::unique_ptr<ai::RecordedTransport> recorded =
            ai::RecordedTransport::load(*fixtures, load_error);
        if (recorded != nullptr) {
            ai.transport = std::move(recorded);
            ai.replaying = true;
            replaying = true;
            log::info("ai: replaying recordings from the directory in $" +
                      std::string(kFixturesVariable));
        } else {
            ai.connect_error = load_error;
            log::error("ai: " + load_error);
        }
    } else {
        ai.transport = ai::make_curl_transport();
    }

    if (ai.transport != nullptr) {
        ai::ServerProfile profile = session_profile(replaying);
        profile.name = replaying ? "recordings" : "server";
        ai.client = std::make_unique<ai::Client>(*ai.transport, std::move(profile),
                                                 [] { return platform::now_seconds(); });
        ai.jobs = std::make_unique<ai::JobQueue>(
            ai::JobQueue::Deps{ai.client.get(), ai.cache.get(), state.runtime_dir + "/ai/jobs"},
            [] { return platform::now_seconds(); });
    }
}

}  // namespace noise::app
