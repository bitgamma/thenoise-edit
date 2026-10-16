// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <vector>

#include "core/geom.h"
#include "gfx/renderer.h"

namespace noise {
struct Layer;
namespace app {

// What a layer's longest edge is fitted into, in panel pixels. Small on purpose: the card is a name
// tag with a picture on it, and a bigger thumbnail costs a bigger resample for detail the panel has no
// room to show.
inline constexpr int kLayerThumbLongEdge = 44;
// The colour a layer is shown against. The panel is dark, so a transparent layer reads as the panel
// showing through rather than as a hole in the picture next to it.
inline constexpr std::uint8_t kThumbBackdrop = 38;

// The panel's pictures of the layers: one per layer, plus one of its mask. Rebuilt against
// `Layer::content_revision`, which is why dragging an opacity slider costs nothing - that changes what
// the composite is made of and no layer's bytes.
//
// `live` means a gesture is in progress: a stroke moves the counter on every dab, and resampling the
// layer that often puts a canvas-sized pass on every frame of the drag. A card therefore updates when
// the hand lets go, which is when anyone looks at it.
struct LayerThumbs {
    struct Card {
        std::uint64_t layer = 0;
        std::uint64_t content = 0;
        gfx::Texture pixels{};
        gfx::Texture mask{};
        bool has_mask = false;
    };

    // `index` is the stack index; the card keeps the layer id so a reorder re-uses the picture rather
    // than rebuilding it.
    Card& card(std::size_t index);
    void sync(std::size_t index, const Layer& layer, bool live);
    // The cards this frame are exactly the layers `count`, in stack order; anything else is a layer
    // that has gone, and its textures go with it.
    void resize(std::size_t count);
    // Rebuilds since the run started, so a probe can tell a cache that holds from one that doesn't.
    std::uint64_t rebuilds = 0;

private:
    std::vector<Card> cards_;
};

// The box a layer of `size` is drawn into, keeping its proportions inside `kLayerThumbLongEdge`.
Extent thumb_box(Extent size);

}  // namespace noise::app
}  // namespace noise
