// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/image.h"

namespace noise {

// Names are generated from this prefix in one place; the container writes whatever a layer is
// actually called, so a file from elsewhere keeps its own naming.
inline constexpr const char* kLayerNamePrefix = "Layer";

std::string default_layer_name(std::size_t index);

// A new layer starts empty: an opaque one would hide everything under it, which is never what
// "add a layer" means.
inline constexpr Rgba8 kNewLayerFill{0, 0, 0, 0};

// One layer of a document. Layers are the same size as the document, and a stack is ordered
// bottom-first: index 0 is the layer nothing is painted under.
//
// `id` identifies the layer to the history for the length of the session and is deliberately
// not written to a file, so reading a project back assigns fresh ones and a file cannot name a
// layer this document does not have.
struct Layer {
    std::uint64_t id = 0;
    std::string name;
    Image pixels;
};

}  // namespace noise
