// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace noise::ai {

// Relevant values from a model's `recipe_options`. Lemonade owns the defaults; the editor reads
// them for size planning and estimates, not to repeat them in every generation request.
struct ImageOptions {
    int steps = 0;
    double cfg_scale = 0.0;
    int width = 0;
    int height = 0;
    // The largest area this model serves, as the box publishes it (measured 2026-10-23: 2097152 on
    // Flux-2-Klein-9B-TheNoise, 4194304 on builtin.Qwen-Image-2.1-Turbo-INT8). Zero means the entry
    // says nothing - which is not a ceiling of zero, and does not become one: a region is then
    // compiled to its own size instead of being shrunk to a number the app invented.
    int soft_pixel_cap = 0;
};

// One `/v1/models` entry, as discovered. Nothing here is a fact the code assumes: whether a model can
// serve a role is labels plus `downloaded` at the moment of the request, because the same URL answered
// 187 and 163 entries on two runs and swapped which Klein was downloaded between them.
struct ModelEntry {
    std::string id;
    std::string recipe;
    std::vector<std::string> labels;
    bool downloaded = false;
    double size_gb = 0.0;
    ImageOptions image_options;

    bool has_label(const std::string& label) const;
    // A `thenoise` entry that carries `image` is an image model; the entry for a VAE-swap alias does
    // too, which is why preference order names candidates instead of trusting a label set.
    bool is_image_model() const;
};

// `GET /models` -> entries. An entry that is missing its id is skipped and reported in `error` rather
// than aborting the whole list: one bad row must not cost the commands that another model serves.
std::vector<ModelEntry> parse_models(const nlohmann::json& body, std::string& error);

// `model_loaded` is Lemonade's most recently used model (possibly a chat model), NOT the image
// model. The full typed `all_models_loaded` list is the authoritative source for residency.
struct LoadedModel {
    std::string id;
    std::string type;
};

struct ServerHealth {
    bool ok = false;
    std::string version;
    std::string model_loaded;
    int max_models_image = 0;
    std::vector<LoadedModel> loaded;
};

ServerHealth parse_health(const nlohmann::json& body, std::string& error);

}  // namespace noise::ai
