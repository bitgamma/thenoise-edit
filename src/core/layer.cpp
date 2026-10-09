// SPDX-License-Identifier: Apache-2.0
#include "core/layer.h"

namespace noise {

std::string default_layer_name(std::size_t index) {
    return std::string(kLayerNamePrefix) + " " + std::to_string(index + 1);
}

}  // namespace noise
