// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace noise::ai {

// What `image_defaults` says about a model. These are the model's own numbers, and they differ per
// model (measured on the dev box: Klein 4 steps, Krea 8), so a recipe that carried a step count would
// be restating a fact it could read here.
struct ImageDefaults {
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
    ImageDefaults image_defaults;

    bool has_label(const std::string& label) const;
    // A `thenoise` entry that carries `image` is an image model; the entry for a VAE-swap alias does
    // too, which is why preference order names candidates instead of trusting a label set.
    bool is_image_model() const;
};

// `GET /models` -> entries. An entry that is missing its id is skipped and reported in `error` rather
// than aborting the whole list: one bad row must not cost the commands that another model serves.
std::vector<ModelEntry> parse_models(const nlohmann::json& body, std::string& error);

// `GET /health`. `max_models.image == 1` is the constraint the swap/pin/warm-up design is built on, so
// it is parsed rather than assumed; `model_loaded` is empty until the server is asked for something.
struct ServerHealth {
    bool ok = false;
    std::string version;
    std::string model_loaded;
    int max_models_image = 0;
    std::vector<std::string> loaded;
};

ServerHealth parse_health(const nlohmann::json& body, std::string& error);

}  // namespace noise::ai
