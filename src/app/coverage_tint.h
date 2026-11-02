// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "core/geom.h"
#include "gfx/renderer.h"

namespace noise {
class Mask;
}

namespace noise::app {

// The wash over marked coverage: one small RGBA texture whose alpha is the coverage itself, stretched
// over the covered bounds. The plane is re-uploaded only when its bytes change - one pass recognises
// that, which is what keeps a live brush stroke from re-submitting the whole shape every dab - and the
// size follows the bounds rather than the canvas, so a small mark costs a small upload.
struct CoverageTint {
    gfx::Texture texture{};
    Rect bounds{};
    std::uint64_t stamp = 0;
    bool drawn = false;
    std::uint64_t source_layer = 0;
    std::uint64_t source_revision = 0;
    bool hidden = false;

    void sync(const Mask& coverage);
    void sync_hidden(const Mask& mask, std::uint64_t layer, std::uint64_t revision);
};

}  // namespace noise::app
