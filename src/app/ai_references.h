// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/command.h"
#include "core/image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace noise {
struct AppState;
}

namespace noise::app {

struct AiReference {
    std::string id;
    std::string source;
    std::string name;
    std::uint64_t source_layer_id = 0;
    std::uint64_t source_document_epoch = 0;
    std::uint64_t source_revision = 0;
    double source_opacity = 1.0;
    std::uint64_t revision = 0;
    Extent size{};
    std::vector<std::uint8_t> png;
    Image thumbnail;
    std::string sha256;
    std::string error;
};

void register_ai_reference_commands(AppState& state, CommandTable& table);
void refresh_ai_references(AppState& state);
nlohmann::json ai_references_json(AppState& state);

}  // namespace noise::app
