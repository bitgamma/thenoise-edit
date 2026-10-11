// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ai/region.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise::ai {

// What the compositor does to an answer before it lands. A recipe's `composite` block states these;
// until recipes are loaded this is where they exist, and it stays the fallback rather than a second
// copy of them.
struct CompositePolicy {
    // `nullopt` is the recipe's word `auto`: the radius comes from the mask's own shape, see
    // `auto_feather_radius`. A number is what the recipe insisted on, and 0 is a hard edge.
    std::optional<int> feather_px;
    // On for remove and heal, where the patch has to go on being the surface around it; off for
    // reimagine, where the whole frame is allowed to come back a different colour.
    bool colour_match;
    // How far inside the crop the original is enforced, on the crop edges that have canvas outside
    // them. The model drifts a whole frame; this is the band that guarantees the drift never meets a
    // pixel that was never in the request.
    int lock_band_px;
};

inline constexpr CompositePolicy kCompositePolicy{std::nullopt, true, 6};

// The feather range, from `gauss(feather 2-8 px)`: below 2 px a hard mask edge still reads as a cut
// at any zoom worth looking at, above 8 the transition costs more of the feature than it hides.
inline constexpr int kAutoFeatherMinPx = 2;
inline constexpr int kAutoFeatherMaxPx = 8;
// The feather is a fraction of the feature it softens, so that the transition cannot eat the thing it
// is softening: half of a radius lands on each side, so a feature ends up at most a quarter of its
// own width in transition.
inline constexpr int kAutoFeatherThicknessDivisor = 8;

// What the feather an auto policy picks is a fraction of. `2 * area / perimeter` is the width of a
// band with that area and perimeter and the radius of a disc with the same, so one expression answers
// "how wide is this feature" for a blob, a stroke and a speck alike. Region size alone cannot: a big
// blob and a big spidery mask sit in the same region and want different feathers, and the wider the
// feather is relative to the feature, the more of the feature the paste throws away.
double mean_thickness_px(const Mask& coverage);

// The radius an auto policy uses: `mean_thickness_px / kAutoFeatherThicknessDivisor` kept inside
// `kAutoFeatherMinPx .. kAutoFeatherMaxPx`.
int auto_feather_radius(const Mask& coverage);

// What a composite did, or why it did nothing. Every field is something the wire reports and a probe
// can assert on, because "it looked fine" is not evidence of pixel-lock.
struct ComposeReport {
    bool composed = false;
    // What was refused, naming the input: empty exactly when `composed`.
    std::string refusal;

    // The feather actually used, after an `auto` policy resolved it.
    int feather_px = 0;
    // The ring correction ran and moved pixels. The policy's `colour_match` is the request for it;
    // this is the fact that it was needed - a patch that already agrees with its ring is corrected by
    // nothing, and reporting the request as the effect would make a probe believe a match happened.
    bool colour_corrected = false;
    // The crop clipped to `target`: what the composite could reach at all.
    Rect area{};

    // The size the answer came back at, and whether it is the size the plan asked for. A difference
    // is not a failure - the engine answers a snapped size and the resample copes - but it is the
    // fact a drift investigation starts from, so it is reported rather than absorbed.
    Extent result_dims{};
    bool answered_as_requested = false;

    // The coverage that was pasted, in document pixels: feathered, cut by the lock band and multiplied
    // cut by the lock band. Every byte of `target` outside it is the byte that was there.
    Mask coverage{};
    std::size_t pixels_changed = 0;
};

// Lands an answer in `target`, inside `plan.crop`, and measures its colour drift against `reference`:
// resample to the crop, colour match to the ring, `out = orig*(1-m) + result*m` with `m` the
// feathered `coverage`, cut by the lock band.
//
// The blend is `restrict_to`, so it is the same premultiplied mix a brush write goes through and
// exact at both ends of the coverage scale: `m == 0` leaves the byte alone, `m == 255` leaves the
// answer alone. That is the pixel-lock invariant, and it holds for every byte of `target` outside
// `report.coverage` - which is the whole image, not the crop, because the composite never writes
// outside the crop at all.
//
// `target` and `reference` are the same size and they are **not the same image**: an answer is received
// by a fresh empty layer, so the pixels it has to agree with colour-wise are the document it was
// compiled from, not the destination. `reference` is that. `target` keeps its own pixels anywhere outside
// the coverage.
//
// A refusal (an empty answer, a plan off `target`, a mask with no coverage inside the crop) composes
// nothing, and the report says which.
ComposeReport compose_result(Image& target, const Image& reference, const Image& result,
                             const RegionPlan& plan, const Mask& coverage,
                             const CompositePolicy& policy);

}  // namespace noise::ai
