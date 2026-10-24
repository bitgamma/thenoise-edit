// SPDX-License-Identifier: Apache-2.0
#include "ai/profile.h"

namespace noise::ai {

namespace {

std::string text_at(const nlohmann::json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) {
        return {};
    }
    return object.at(key).get<std::string>();
}

double number_at(const nlohmann::json& object, const char* key, double missing) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_number()) {
        return missing;
    }
    return object.at(key).get<double>();
}

bool flag_at(const nlohmann::json& object, const char* key, bool missing) {
    if (!object.is_object() || !object.contains(key)) {
        return missing;
    }
    if (object.at(key).is_boolean()) {
        return object.at(key).get<bool>();
    }
    if (object.at(key).is_number()) {
        return object.at(key).get<double>() != 0.0;
    }
    return missing;
}

std::vector<std::string> list_at(const nlohmann::json& object, const char* key) {
    std::vector<std::string> out;
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_array()) {
        return out;
    }
    for (const nlohmann::json& value : object.at(key)) {
        if (value.is_string()) {
            out.push_back(value.get<std::string>());
        }
    }
    return out;
}

ImageDefaults defaults_at(const nlohmann::json& object) {
    ImageDefaults defaults;
    if (!object.is_object() || !object.contains("image_defaults") ||
        !object.at("image_defaults").is_object()) {
        return defaults;
    }
    const nlohmann::json& source = object.at("image_defaults");
    defaults.steps = static_cast<int>(number_at(source, "steps", 0.0));
    defaults.cfg_scale = number_at(source, "cfg_scale", 0.0);
    defaults.width = static_cast<int>(number_at(source, "width", 0.0));
    defaults.height = static_cast<int>(number_at(source, "height", 0.0));
    defaults.soft_pixel_cap = static_cast<int>(number_at(source, "soft_pixel_cap", 0.0));
    return defaults;
}

}  // namespace

bool ModelEntry::has_label(const std::string& label) const {
    for (const std::string& owned : labels) {
        if (owned == label) {
            return true;
        }
    }
    return false;
}

bool ModelEntry::is_image_model() const { return recipe == "thenoise" && has_label("image"); }

std::vector<ModelEntry> parse_models(const nlohmann::json& body, std::string& error) {
    error.clear();
    if (!body.is_object() || !body.contains("data") || !body.at("data").is_array()) {
        error = "model list has no 'data' array";
        return {};
    }
    std::vector<ModelEntry> out;
    int skipped = 0;
    for (const nlohmann::json& entry : body.at("data")) {
        if (!entry.is_object()) {
            ++skipped;
            continue;
        }
        ModelEntry model;
        model.id = text_at(entry, "id");
        if (model.id.empty()) {
            // `name` is what the load endpoint calls it; a row that carries only that is still usable.
            model.id = text_at(entry, "name");
        }
        if (model.id.empty()) {
            ++skipped;
            continue;
        }
        model.recipe = text_at(entry, "recipe");
        model.labels = list_at(entry, "labels");
        model.downloaded = flag_at(entry, "downloaded", false);
        model.size_gb = number_at(entry, "size", 0.0);
        model.image_defaults = defaults_at(entry);
        out.push_back(std::move(model));
    }
    if (skipped != 0) {
        error = std::to_string(skipped) + " entr" + (skipped == 1 ? "y" : "ies") +
                " in the model list had no id and were skipped";
    }
    return out;
}

ServerHealth parse_health(const nlohmann::json& body, std::string& error) {
    error.clear();
    ServerHealth health;
    if (!body.is_object()) {
        error = "health reply is not an object";
        return health;
    }
    health.ok = text_at(body, "status") == "ok";
    health.version = text_at(body, "version");
    health.model_loaded = text_at(body, "model_loaded");
    if (body.contains("max_models") && body.at("max_models").is_object()) {
        health.max_models_image =
            static_cast<int>(number_at(body.at("max_models"), "image", 0.0));
    }
    if (body.contains("all_models_loaded") && body.at("all_models_loaded").is_array()) {
        for (const nlohmann::json& entry : body.at("all_models_loaded")) {
            const std::string name = text_at(entry, "model_name");
            if (!name.empty()) {
                health.loaded.push_back(name);
            }
        }
    }
    if (!health.ok && health.version.empty()) {
        error = "health reply has neither 'status: ok' nor a version";
    }
    return health;
}

}  // namespace noise::ai
