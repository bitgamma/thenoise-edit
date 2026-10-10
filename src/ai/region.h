// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise::ai {

// What a region compile grows for, snaps to and stays under. A recipe states these and the request
// carries what the recipe said; this is the only place the values exist until `recipes/*.json` are
// loaded, and it stays the fallback rather than a second copy of them.
struct RegionPolicy {
    // Each side of the mask's box grows by `max(ratio * extent_of_that_axis, min_context_px)`:
    // context is what lets the model match a surface instead of inventing one.
    double context_ratio;
    int min_context_px;
    // Measured, not assumed: the engine snaps a request to a multiple of 16 and never refuses an odd
    // size (asking 500x300 answers 512x304), so sending multiples is what makes the crop you planned
    // the crop you get - and returned dims still differ, which the result compositor handles.
    int size_multiple;
    // The area a region is never compiled above, in source pixels. A bigger box is scaled down rather
    // than truncated, and never scaled up: the source has no detail to give.
    int max_area_px;
};

inline constexpr RegionPolicy kRegionPolicy{0.35, 64, 16, 1024 * 1024};

// What destroys the target's identity in the uploaded crop while keeping the scene's statistics. The
// whole point of PREP: a blurry copy says where the light comes from without saying what stood there.
enum class Prep {
    none,
    average,
    blur,
    pixelate,
    noise,
    white,
};

const char* prep_name(Prep prep);
std::optional<Prep> prep_from_name(const std::string& name);
std::string prep_names();

// Where a region sits and what size it is asked for. Pure geometry: no document, no request.
struct RegionPlan {
    Rect mask_bounds{};  // the mask's own tight box, before any growth
    Rect crop{};         // document pixels the request covers: grown, clamped to the canvas, snapped
    Extent requested{};  // the `size` field: `crop` scaled down for the budget, rounded up to the
                         // policy multiple, so it is always a multiple and never smaller than the
                         // crop it came from
    double scale = 1.0;  // <= 1: how much the crop was shrunk to fit the budget
    // The axes where a snap was impossible because the canvas itself is shorter than one multiple.
    // Reported rather than hidden: the engine will snap those for us, and a caller comparing dims
    // against the policy needs to know which mismatch is theirs to expect.
    bool snapped_width = true;
    bool snapped_height = true;

    // Document pixels per request pixel, and the inverse: a request pixel centre maps back into the
    // crop, and the four corners of the request map onto the four corners of the crop.
    Extent request_size() const { return requested; }
    Vec2 to_document(Vec2 request_point) const;
    Vec2 to_request(Vec2 document_point) const;
};

// The geometry of one compile: bbox -> grow -> clamp -> snap -> fit. A mask with no coverage has no
// region, which is a refusal rather than a plan over the whole canvas - the difference between
// "nothing selected" and "everything selected" is the difference the document already draws, and a
// compile must not invent a region nobody asked for.
std::optional<RegionPlan> plan_region(Extent canvas, const Mask& mask, const RegionPolicy& policy);

// The bytes that would go on the wire: `plan.crop` taken out of `base` and prepped. `base` is a
// flattened picture of the region the request covers and must contain `plan.crop`; the returned image
// is `plan.crop` sized, in the same premultiplied space as `base` - the resample to `requested` and
// the PNG encode are the request builder's business, and keeping them out of here is what lets the
// prep be checked against the statistics it is supposed to preserve.
Image prep_region(const Image& base, const RegionPlan& plan, Prep prep, std::uint64_t seed);

// The blur radius a prep uses, from the region it blurs: fine enough to remove a face at any region
// size, coarse enough that a small crop still loses its identity.
int prep_blur_radius(Rect crop);

}  // namespace noise::ai
