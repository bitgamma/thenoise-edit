// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"
#include "core/mask.h"

namespace noise::ai {

// Below this a channel's spread is not a spread, it is quantisation: matching to it would scale the
// patch by a number derived from rounding noise.
inline constexpr double kMinLabDeviation = 1.0;

// A correction that doubles or halves a patch's spread is a palette change, not a drift correction,
// and a ring that happens to be far more varied than the patch can ask for one.
inline constexpr double kMaxDeviationRatio = 2.0;

// Pulls `patch` back to the colour of the picture it belongs to. An instruction edit drifts the tone
// and the chroma of the frame it returns; the pixels around the mask are the ones the patch has to
// continue, so `patch` takes their Lab mean, and their spread too when both sides have one.
//
// `reach` is the coverage the composite is about to paste with, in the same coordinates as the two
// images: the **ring** is where `reach` does not reach and `original` has some alpha, and the
// **patch set** is where `reach` does and `patch` has some alpha. A pixel with no colour tells this
// nothing about colour, which is why both sets exclude it - including one would drag the match
// towards black.
//
// Equal weight per pixel in the patch set, not weighted by coverage: the feather band is a few
// pixels wide and a weighted mean would make the correction depend on a radius the caller can change
// without changing what is in the patch.
//
// Identity - nothing written, and `false` - when either set holds fewer than two pixels (a mask
// covering the whole crop has no ring, which is the full-frame case: nothing around it to match),
// when a deviation is below `kMinLabDeviation` on either side (the mean shift is applied and the
// spread left alone, so a flat ring does not flatten a textured patch), or when the shift and the
// ratios come out at nothing. Pixels with no alpha are left exactly alone.
//
// `patch`'s alpha is its own: this corrects colour, never coverage.
bool colour_match_ring(Image& patch, const Image& original, const Mask& reach);

}  // namespace noise::ai
