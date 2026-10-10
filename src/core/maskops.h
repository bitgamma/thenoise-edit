// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

#include <optional>
#include <string>

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

// An image's alpha channel as coverage: "how much layer is here", stated in the units a mask is
// stated in. Which pixels may receive an answer is a claim about the operation, and a mask is how this
// codebase states one - so an edit confined to where its layer exists and a generation that fills empty
// pixels differ by which mask they carry, not by a rule of their own.
Mask alpha_coverage(const Image& layer);

}  // namespace noise
