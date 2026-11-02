// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

#include <optional>
#include <string>
#include <vector>

namespace noise {

// The shapes that can put coverage on a mask. A shape is a *producer* of coverage, never a type of
// its own: `Mask` stays the one representation of "which pixels, and how much", so an added shape
// costs a row here and nothing in the document, the composite, the file format or the overlay.
enum class MaskShape {
    rect,     // every pixel the box holds, full coverage
    ellipse,  // the ellipse inscribed in the box, anti-aliased coverage
};

struct MaskShapeInfo {
    MaskShape shape;
    const char* name;
};

struct MaskPatch {
    Rect area;
    std::vector<std::uint8_t> coverage;
};

// The rows the tool table, the commands and the error messages read: a shape is added here and
// nowhere else, and a request naming one the table does not have is refused by name.
inline constexpr MaskShapeInfo kMaskShapes[]{
    {MaskShape::rect, "rect"},
    {MaskShape::ellipse, "ellipse"},
};

std::optional<MaskShape> mask_shape_from_name(const std::string& name);
const char* mask_shape_name(MaskShape shape);
// "rect, ellipse" - from the table, so an error cannot list a name that no longer exists.
std::string mask_shape_names();

// Puts the shape inscribed in `box` onto `mask` through `op`, clipped to the mask's own pixels.
// `rect` runs exactly the path `Mask::op(rect, op)` runs, so a rectangle selected as a shape and a
// rectangle selected as a rectangle are the same bytes by construction rather than by a test that
// happens to agree.
void op_shape(Mask& mask, MaskShape shape, Rect box, MaskOp op);
std::optional<MaskPatch> rasterize_lasso(Extent canvas, const std::vector<Vec2>& path);
bool op_lasso(Mask& mask, const std::vector<Vec2>& path, MaskOp op);

inline constexpr int kMaxEdgeShiftPx = 128;
inline constexpr int kMaxEdgeFeatherPx = 64;

// Adjust a coverage edge without changing the pixels it reveals: a signed shift grows
// (positive) or contracts (negative), optional threshold removes the old soft transition,
// and feather softens the resulting outline. All operations preserve the mask extent.
Mask adjusted_edge(const Mask& source, int shift_px, bool hard, int feather_px);

// The only canvas pixels an absolute edge adjustment can affect. The halo includes all
// morphology and box-blur passes; outside it the result is guaranteed to be empty.
Rect adjusted_edge_bounds(const Mask& source, int shift_px, int feather_px);

// Compute only the requested document-space rectangle, including its necessary halo.
// It is byte-identical to adjusted_edge(source, ...).region(area), without a
// canvas-sized morphology, blur or output allocation on each slider step.
Mask adjusted_edge_region(const Mask& source, int shift_px, bool hard, int feather_px,
                          Rect area);

}  // namespace noise
