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

}  // namespace noise
