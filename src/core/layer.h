// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/blend.h"
#include "core/image.h"

namespace noise {

// Names are generated from this prefix in one place; the container writes whatever a layer is
// actually called, so a file from elsewhere keeps its own naming.
inline constexpr const char* kLayerNamePrefix = "Layer";

std::string default_layer_name(std::size_t index);

// A new layer starts empty: an opaque one would hide everything under it, which is never what
// "add a layer" means.
inline constexpr Rgba8 kNewLayerFill{0, 0, 0, 0};

// What a layer that has never been told otherwise looks like. Format 2 of the container writes
// all three and requires all three, so these are the state of a new layer rather than a guess
// made about a file that omitted them.
inline constexpr bool kLayerVisibleDefault = true;
inline constexpr double kLayerOpacityDefault = 1.0;

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

    // Neither changes a pixel, so neither enters history: they change what the composite is made
    // of. `opacity` is applied there and never baked into `pixels`, so a layer shown at 30 % is
    // still painted on and edited at full strength.
    bool visible = kLayerVisibleDefault;
    double opacity = kLayerOpacityDefault;  // 0..1
    BlendMode mode = BlendMode::normal;
};

}  // namespace noise
