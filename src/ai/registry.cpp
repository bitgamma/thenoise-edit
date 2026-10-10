// SPDX-License-Identifier: Apache-2.0
#include "ai/registry.h"

#include "io/file.h"

#include <algorithm>

namespace noise::ai {

namespace {

constexpr const char* kFixPrefix = "lemonade import ";

std::string text_member(const nlohmann::json& object, const char* key, std::string& error,
                        const std::string& origin, bool& ok) {
    ok = true;
    if (!object.contains(key)) {
        error = origin + ": missing '" + key + "'";
        ok = false;
        return {};
    }
    if (!object.at(key).is_string()) {
        error = origin + ": '" + key + "' must be a string, not a " +
                std::string(object.at(key).type_name());
        ok = false;
        return {};
    }
    return object.at(key).get<std::string>();
}

std::vector<std::string> string_array(const nlohmann::json& object, const char* key,
                                      std::string& error, const std::string& origin, bool& ok) {
    ok = true;
    std::vector<std::string> out;
    if (!object.contains(key)) {
        return out;
    }
    if (!object.at(key).is_array()) {
        error = origin + ": '" + key + "' must be an array, not a " +
                std::string(object.at(key).type_name());
        ok = false;
        return out;
    }
    for (std::size_t i = 0; i < object.at(key).size(); ++i) {
        const nlohmann::json& value = object.at(key)[i];
        if (!value.is_string()) {
            error = origin + ": '" + key + "[" + std::to_string(i) + "]' must be a string";
            ok = false;
            return {};
        }
        out.push_back(value.get<std::string>());
    }
    return out;
}

int integer_member(const nlohmann::json& object, const char* key, int missing,
                   std::string& error, const std::string& origin, bool& ok) {
    ok = true;
    if (!object.contains(key)) {
        return missing;
    }
    if (!object.at(key).is_number_integer() || object.at(key).get<long long>() <= 0) {
        error = origin + ": '" + key + "' must be a positive integer";
        ok = false;
        return missing;
    }
    return static_cast<int>(object.at(key).get<long long>());
}

double number_member(const nlohmann::json& object, const char* key, double missing,
                     std::string& error, const std::string& origin, bool& ok) {
    ok = true;
    if (!object.contains(key)) {
        return missing;
    }
    if (!object.at(key).is_number()) {
        error = origin + ": '" + key + "' must be a number, not a " +
                std::string(object.at(key).type_name());
        ok = false;
        return missing;
    }
    return object.at(key).get<double>();
}

// The first preference that the box could supply but did not have downloaded: the one row that turns
// an unresolved role into a resolved one, in the order we would have used it.
std::string missing_preference(const RoleDefinition& role,
                               const std::vector<ModelEntry>& models) {
    for (const std::string& wanted : role.preference) {
        for (const ModelEntry& entry : models) {
            if (entry.id == wanted && !entry.downloaded) {
                return wanted;
            }
        }
    }
    return {};
}

bool satisfies(const ModelEntry& entry, const RoleDefinition& role) {
    if (!role.recipe.empty() && entry.recipe != role.recipe) {
        return false;
    }
    for (const std::string& label : role.requires_labels) {
        if (!entry.has_label(label)) {
            return false;
        }
    }
    return true;
}

}  // namespace

const RoleDefinition* RoleTable::find(const std::string& role_id) const {
    for (const RoleDefinition& role : roles) {
        if (role.id == role_id) {
            return &role;
        }
    }
    return nullptr;
}

std::optional<RoleTable> parse_roles(const nlohmann::json& body, const std::string& origin,
                                     std::string& error) {
    RoleTable table;
    bool ok = false;
    if (!body.is_object()) {
        error = origin + ": expected an object";
        return std::nullopt;
    }
    if (body.contains("size_policy")) {
        if (!body.at("size_policy").is_object()) {
            error = origin + ": 'size_policy' must be an object";
            return std::nullopt;
        }
        const nlohmann::json& size = body.at("size_policy");
        table.size_policy.multiple =
            integer_member(size, "multiple", 16, error, origin + ".size_policy", ok);
        if (!ok) {
            return std::nullopt;
        }
        table.size_policy.max_area_px =
            integer_member(size, "max_area_px", 1024 * 1024, error, origin + ".size_policy", ok);
        if (!ok) {
            return std::nullopt;
        }
    }
    if (body.contains("context_policy")) {
        if (!body.at("context_policy").is_object()) {
            error = origin + ": 'context_policy' must be an object";
            return std::nullopt;
        }
        const nlohmann::json& context = body.at("context_policy");
        table.context_policy.ratio =
            number_member(context, "ratio", 0.35, error, origin + ".context_policy", ok);
        if (!ok) {
            return std::nullopt;
        }
        table.context_policy.min_px =
            integer_member(context, "min_px", 64, error, origin + ".context_policy", ok);
        if (!ok) {
            return std::nullopt;
        }
    }
    if (!body.contains("roles") || !body.at("roles").is_array()) {
        error = origin + ": 'roles' must be an array";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < body.at("roles").size(); ++i) {
        const std::string where = origin + ".roles[" + std::to_string(i) + "]";
        const nlohmann::json& entry = body.at("roles")[i];
        if (!entry.is_object()) {
            error = where + ": expected an object";
            return std::nullopt;
        }
        RoleDefinition role;
        role.id = text_member(entry, "id", error, where, ok);
        if (!ok) {
            return std::nullopt;
        }
        if (entry.contains("recipe")) {
            role.recipe = text_member(entry, "recipe", error, where, ok);
            if (!ok) {
                return std::nullopt;
            }
        }
        role.requires_labels = string_array(entry, "requires", error, where, ok);
        if (!ok) {
            return std::nullopt;
        }
        // A role with no predicate would resolve to whichever model of the recipe came first, which is
        // an arbitrary pick dressed up as a capability match.
        if (role.requires_labels.empty()) {
            error = where + ": 'requires' must list at least one label - without a predicate the role "
                            "resolves to any model of the recipe";
            return std::nullopt;
        }
        role.preference = string_array(entry, "preference", error, where, ok);
        if (!ok) {
            return std::nullopt;
        }
        if (table.find(role.id) != nullptr) {
            error = origin + ": duplicate role id '" + role.id + "'";
            return std::nullopt;
        }
        table.roles.push_back(std::move(role));
    }
    if (table.roles.empty()) {
        error = origin + ": 'roles' is empty";
        return std::nullopt;
    }
    return table;
}

std::optional<RoleTable> load_roles(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!io::read_file(path, bytes, error)) {
        return std::nullopt;
    }
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(bytes);
    } catch (const nlohmann::json::parse_error& exception) {
        error = path + ": " + exception.what();
        return std::nullopt;
    }
    return parse_roles(body, path, error);
}

Registry::Registry(RoleTable table) : table_(std::move(table)) {}

void Registry::set_assignment(const std::string& role, const std::string& model_id) {
    if (model_id.empty()) {
        assignments_.erase(role);
    } else {
        assignments_[role] = model_id;
    }
    update(models_);
}

std::string Registry::assignment(const std::string& role) const {
    const auto found = assignments_.find(role);
    return found == assignments_.end() ? std::string() : found->second;
}

void Registry::update(const std::vector<ModelEntry>& models) {
    models_ = models;
    resolved_.clear();
    for (const RoleDefinition& role : table_.roles) {
        resolved_[role.id] = resolve_one(role);
    }
}

const Resolution& Registry::resolve(const std::string& role) const {
    const auto found = resolved_.find(role);
    if (found != resolved_.end()) {
        return found->second;
    }
    static const Resolution kUnknown{};
    return kUnknown;
}

std::vector<Resolution> Registry::all() const {
    std::vector<Resolution> out;
    out.reserve(table_.roles.size());
    for (const RoleDefinition& role : table_.roles) {
        out.push_back(resolve(role.id));
    }
    return out;
}

const ModelEntry* Registry::model(const std::string& id) const {
    for (const ModelEntry& entry : models_) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

const ModelEntry* Registry::resolved_model(const std::string& role) const {
    return model(resolve(role).model_id);
}

Resolution Registry::resolve_one(const RoleDefinition& role) const {
    Resolution resolution;
    resolution.role = role.id;
    for (const ModelEntry& entry : models_) {
        if (entry.downloaded && satisfies(entry, role)) {
            resolution.candidates.push_back(entry.id);
        }
    }
    if (resolution.candidates.empty()) {
        const std::string missing = missing_preference(role, models_);
        if (missing.empty()) {
            // No command to suggest: a model this server does not list may not exist any more, and an
            // import that fails is worse than the instruction to look at the list.
            resolution.reason =
                role.preference.empty()
                    ? "no model here satisfies the '" + role.id +
                          "' predicate and roles.json names no preference for it"
                    : "none of the models roles.json prefers for '" + role.id +
                          "' is offered by this server";
            return resolution;
        }
        resolution.reason = "'" + missing + "' is what '" + role.id + "' wants and is not downloaded";
        resolution.fix_command = std::string(kFixPrefix) + missing;
        return resolution;
    }

    const std::string assigned = assignment(role.id);
    if (!assigned.empty()) {
        const ModelEntry* entry = model(assigned);
        if (entry != nullptr && entry->downloaded && satisfies(*entry, role)) {
            resolution.model_id = assigned;
            resolution.reason = "user assignment";
            return resolution;
        }
    }

    for (const std::string& wanted : role.preference) {
        if (std::find(resolution.candidates.begin(), resolution.candidates.end(), wanted) !=
            resolution.candidates.end()) {
            resolution.model_id = wanted;
            resolution.from_preference = true;
            resolution.reason = "preference order in roles.json";
            return resolution;
        }
    }

    // The predicate is what makes a model usable; preference is only an ordering over the ones we know
    // the name of. A box that has a capable model nobody named still gets to use it, and the reason
    // says so, because a role that stayed unresolved until a file was edited would be a role with a
    // second source of truth.
    const ModelEntry* smallest = nullptr;
    for (const std::string& candidate : resolution.candidates) {
        const ModelEntry* entry = model(candidate);
        if (entry != nullptr && (smallest == nullptr || entry->size_gb < smallest->size_gb)) {
            smallest = entry;
        }
    }
    resolution.model_id = smallest != nullptr ? smallest->id : resolution.candidates.front();
    resolution.reason = "capability match; not named in roles.json preference order";
    return resolution;
}

nlohmann::json Registry::json() const {
    nlohmann::json roles = nlohmann::json::array();
    for (const Resolution& resolution : all()) {
        roles.push_back(nlohmann::json{{"role", resolution.role},
                                       {"model", resolution.model_id},
                                       {"resolved", resolution.resolved()},
                                       {"reason", resolution.reason},
                                       {"fix", resolution.fix_command},
                                       {"from_preference", resolution.from_preference},
                                       {"candidates", resolution.candidates},
                                       {"assignment", assignment(resolution.role)}});
    }
    return nlohmann::json{{"roles", roles},
                          {"models", static_cast<std::size_t>(models_.size())},
                          {"downloaded",
                           static_cast<std::size_t>(std::count_if(
                               models_.begin(), models_.end(),
                               [](const ModelEntry& entry) { return entry.downloaded; }))}};
}

}  // namespace noise::ai
