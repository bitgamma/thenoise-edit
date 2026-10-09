// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"

#include "sokol_gfx.h"

namespace noise::gfx {

// float because that is the uv the vertex format carries.
struct CheckerUv {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

// Texture coordinates of a screen rect for the repeating tile. The square edge follows the
// view scale, clamped at both ends: mush when zoomed out, one flat shade filling the canvas
// when zoomed in. The pattern is anchored to the canvas top-left so it does not slide
// across the document while panning, and its start wraps into one repeat so UV magnitude
// stays independent of zoom and document size.
CheckerUv checker_uv(Vec2 rect_min, Vec2 rect_size, Vec2 canvas_min, double scale);

// The alpha checker under a canvas: one 2x2 texel tile, nearest-sampled with repeat wrap,
// drawn as a single quad over the canvas rect, so four vertices whatever the zoom, window
// or document size.
//
// One ImGui rect per screen square instead costs (canvas_w * canvas_h) / square^2 quads per
// frame, unbounded in zoom because the canvas rect grows with the scale. With the square at
// its floor, a fitted 2400x1600 document in a 1280x800 window demanded ~124k vertices
// against simgui's 65536-vertex staging buffer, which rejects the frame's draw commands and
// leaves the canvas black.
struct Checker {
    sg_view tile{SG_INVALID_ID};
    sg_sampler sampler{SG_INVALID_ID};

    void init();
    void discard();
    bool valid() const { return tile.id != SG_INVALID_ID && sampler.id != SG_INVALID_ID; }
};

}
