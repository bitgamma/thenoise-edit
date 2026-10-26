// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ai/profile.h"

namespace noise::ai {

// Request-size policy, from `roles.json`. The engine snaps a request to a multiple of 16 and never
// refuses an odd one, so multiples are what make the crop you planned the crop you get.
struct SizePolicy {
    int multiple = 16;
    int max_area_px = 1024 * 1024;
};

// How much context a region grows by, from `roles.json`. A recipe may name its own; this is what a
// recipe that says nothing gets.
struct ContextPolicy {
    double ratio = 0.35;
    int min_px = 64;
};

struct RoleDefinition {
    std::string id;
    // Empty means the labels decide on their own, which is what a chat role needs: what a vision model
    // is called on this server (`llama-cpp`, `vllm`, ...) is the server's business, and a role that named
    // one would go unresolved the next time the box changed.
    std::string recipe;
    std::vector<std::string> requires_labels;
    // Most preferred first. Model ids appear in `roles.json` and nowhere else in the tree, so retiring
    // or re-ranking a model is a data change.
    std::vector<std::string> preference;
};

// `roles.json`: role ids, capability predicates, preference order and the size/context policy.
struct RoleTable {
    std::vector<RoleDefinition> roles;
    SizePolicy size_policy;
    ContextPolicy context_policy;

    const RoleDefinition* find(const std::string& role_id) const;
};

// A parse failure names the key and where it was, because JSON carries no comments and a silently
// half-read role table would resolve to nothing while looking fine.
std::optional<RoleTable> parse_roles(const nlohmann::json& body, const std::string& origin,
                                     std::string& error);
std::optional<RoleTable> load_roles(const std::string& path, std::string& error);

// True when an entry can serve a role: the recipe constraint when the role states one, and every label
// it requires. `downloaded` is not part of this question - a role that could serve but is not on the box
// is a different answer from one that could not, and the fix command depends on telling them apart.

// What a role resolved to, and what to say when it did not. `reason` is for the disabled command's
// tooltip and `fix_command` is the line that would change the server's state.
struct Resolution {
    std::string role;
    std::string model_id;  // empty when unresolved
    std::string reason;
    std::string fix_command;
    // What satisfied the predicate this moment. Reported so a probe can tell "resolved" from
    // "resolved the way roles.json prefers".
    std::vector<std::string> candidates;
    bool from_preference = false;

    bool resolved() const { return !model_id.empty(); }
};

// role -> exactly one model for one server profile, recomputed on every connect and every model list.
class Registry {
public:
    explicit Registry(RoleTable table);

    // The user's pick for a role, which outranks preference while it stays present and downloaded. An
    // empty id clears the assignment.
    void set_assignment(const std::string& role, const std::string& model_id);
    std::string assignment(const std::string& role) const;

    void update(const std::vector<ModelEntry>& models);
    // The canonical ID /health reports. Reuse it only when it satisfies the same preferred name.
    void set_resident(const std::string& id);

    // An unknown role resolves to nothing with a reason naming the role, rather than to a guess.
    const Resolution& resolve(const std::string& role) const;
    std::vector<Resolution> all() const;

    // The entry a resolved role points at, or null. Capability questions ("can this edit?") are
    // answered from the entry, never from the role name.
    const ModelEntry* model(const std::string& id) const;
    const ModelEntry* resolved_model(const std::string& role) const;

    const RoleTable& table() const { return table_; }
    const std::vector<ModelEntry>& models() const { return models_; }
    nlohmann::json json() const;

private:
    Resolution resolve_one(const RoleDefinition& role) const;

    RoleTable table_;
    std::vector<ModelEntry> models_;
    std::string resident_;
    std::map<std::string, std::string> assignments_;
    std::map<std::string, Resolution> resolved_;
};

}  // namespace noise::ai
