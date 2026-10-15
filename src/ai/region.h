// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>

#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise::ai {

// What a region compile grows for, snaps to and stays under. `roles.json` carries the size policy and
// the default context, a recipe may name its own context, and the app assembles the two into this:
// nothing here states a number of its own, because a compile that disagreed with the table would be a
// compile whose output the server then snaps to something nobody planned.
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

    // Request pixels to document pixels and back. The map is by the two sizes - the request's and
    // the crop's - and not by `scale`, because the answer covers the crop and arrives at `requested`
    // dimensions: the round up to the policy multiple is why those two are not proportional. It is
    // the same map the result compositor applies when it resamples the whole answer onto the whole
    // crop, so a point sent through one and back through the other is the point it was.
    Extent request_size() const { return requested; }
    Vec2 to_document(Vec2 request_point) const;
    Vec2 to_request(Vec2 document_point) const;
};

// The largest rectangle of `ratio` that fits the canvas, centred, with one axis running the whole
// canvas. The size policy's 16-pixel grid is deliberately not chased here: the largest on-grid 16:9
// rectangle inside a 1000x1000 canvas is 768x432, so aligning would drop a fifth of the frame to save a
// snap `plan_region` performs on the requested size anyway. The answer is mapped back onto this
// rectangle, so the ratio asked for is the ratio that lands.
Rect frame_for_aspect(Extent canvas, AspectRatio ratio);

// The geometry of one compile: bbox -> grow -> clamp -> snap -> fit. A mask with no coverage has no
// region, which is a refusal rather than a plan over the whole canvas - the difference between
// "nothing selected" and "everything selected" is the difference the document already draws, and a
// compile must not invent a region nobody asked for.
std::optional<RegionPlan> plan_region(Extent canvas, const Mask& mask, const RegionPolicy& policy);

// Destroy the identity of the *covered* pixels and leave every other pixel exactly as it was given.
// This is the difference between asking for a hole to be reconstructed and handing the model a smeared
// picture of everything: the surroundings are the instruction, so they go up sharp, and only the area the
// operation means to replace is blurred / averaged / whitened. `coverage_in_crop` is in `crop`'s own
// coordinates, the same plane `flatten_over_ring` takes; any coverage above zero is hole.
//
// Which recipes may ask for this is not a matter of taste: an in-place edit (`Replace the eyes with
// green eyes`) must send `prep: none`, because destroying the subject destroys what it is to be changed
// from - the recipes enforce that pairing and the parser refuses the contradiction.
// `average` and `noise` fill from the mean of the *context*, not of the hole: the hole's own mean is the
// colour of the thing being removed, and painting with it is the bug this prevents.
//
// The output is `crop` sized, in the same straight-alpha space as the input; the resample to `requested`
// and the PNG encode are the request builder's business, and keeping them out of here is what lets prep
// be checked against the statistics it preserves (context) and destroys (hole).
Image prep_region(const Image& crop, const Mask& coverage_in_crop, Prep prep, std::uint64_t seed);

// Step 6, the half `prep_region` does not do: a crop with alpha flattened over a colour drawn from the
// ring the coverage does not reach, rather than over black or white. Uploading a partly transparent crop
// over black tells the model the scene goes dark at the edges of the region and it obliges, which is a
// colour cast in the answer before any editing happens.
//
// The ring is the pixels in `crop` the coverage does not reach and where the base has alpha; no ring
// (a full-frame compile, or a base that is empty outside the mask) falls back to the mean of what the
// base does hold, and an entirely empty base to mid grey, which is the least wrong colour to invent.
// The result is opaque: the request carries no alpha, because the engine has no alpha.
Image flatten_over_ring(const Image& crop, const Mask& coverage_in_crop);

// The blur radius a prep uses, from the region it blurs: fine enough to remove a face at any region
// size, coarse enough that a small crop still loses its identity.
int prep_blur_radius(Extent crop);

}  // namespace noise::ai
