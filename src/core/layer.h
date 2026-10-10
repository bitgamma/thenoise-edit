// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/blend.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise {

// Names are generated from this prefix in one place; the container writes whatever a layer is
// actually called, so a file from elsewhere keeps its own naming.
inline constexpr const char* kLayerNamePrefix = "Layer";

std::string default_layer_name(std::size_t index);

// The two byte planes a layer holds, named so a write can say which one it lands on: the layer's own
// colour, or the coverage that hides it. A gesture writes exactly one of them per press, so this is
// what a stroke carries along with its brush - not a second kind of layer or a second kind of mask.
enum class LayerPlane { pixels, mask };

struct LayerPlaneInfo {
    LayerPlane plane;
    const char* name;
};

// The rows the commands, the reports and the errors read: a plane is added here and nowhere else.
inline constexpr LayerPlaneInfo kLayerPlanes[]{
    {LayerPlane::pixels, "pixels"},
    {LayerPlane::mask, "mask"},
};

std::optional<LayerPlane> layer_plane_from_name(const std::string& name);
const char* layer_plane_name(LayerPlane plane);
// "pixels, mask" - from the table, so an error cannot list a plane that no longer exists.
std::string layer_plane_names();

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

    // Coverage that hides part of the layer without editing it: the composite sees the layer
    // through this, while a paint still lands on pixels the mask hides, because a mask says what
    // shows, not what exists. `nullopt` is no mask; a sized one is the layer's size, and coverage
    // outside it is 0, which is a mask that hides the layer rather than a layer with no mask.
    std::optional<Mask> mask;
};

}  // namespace noise
