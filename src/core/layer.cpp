// SPDX-License-Identifier: Apache-2.0
#include "core/layer.h"

namespace noise {

std::optional<LayerPlane> layer_plane_from_name(const std::string& name) {
    for (const LayerPlaneInfo& info : kLayerPlanes) {
        if (name == info.name) {
            return info.plane;
        }
    }
    return std::nullopt;
}

const char* layer_plane_name(LayerPlane plane) {
    for (const LayerPlaneInfo& info : kLayerPlanes) {
        if (plane == info.plane) {
            return info.name;
        }
    }
    return "";
}

std::string layer_plane_names() {
    std::string names;
    for (const LayerPlaneInfo& info : kLayerPlanes) {
        if (!names.empty()) {
            names += ", ";
        }
        names += info.name;
    }
    return names;
}

std::string default_layer_name(std::size_t index) {
    return std::string(kLayerNamePrefix) + " " + std::to_string(index + 1);
}

std::optional<std::string> normalize_layer_name(std::string_view wanted) {
    constexpr std::string_view kSurrounding = " \t\n\v\f\r";
    const std::string_view::size_type first = wanted.find_first_not_of(kSurrounding);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view::size_type last = wanted.find_last_not_of(kSurrounding);
    const std::string_view trimmed = wanted.substr(first, last - first + 1);
    if (trimmed.size() > static_cast<std::size_t>(kLayerNameMaxChars)) {
        return std::nullopt;
    }
    return std::string(trimmed);
}

std::string layer_name_rule() {
    return "it needs at least one non-space character and at most " +
           std::to_string(kLayerNameMaxChars) + " of them";
}

}  // namespace noise
