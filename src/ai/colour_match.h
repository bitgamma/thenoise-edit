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

// Undo what the model did to the pixels it was not asked to change.
//
// An instruction edit drifts the tone and chroma of the frame it returns. The region compiler grows the
// crop by a band of context the operation is not about, the answer contains that same band, and the
// document holds the very same pixels - so the band can be *paired*, pixel against pixel, and the mean
// Lab difference across it is the drift. Subtracting it from every pixel of the answer puts the frame
// back where it was, including inside the area nobody has a reference for.
//
// What this deliberately does **not** do is pull the new content towards the colour of what surrounds
// it. That was this function's definition (`colour_match_ring`) and it is wrong for anything but a hole:
// a change that turns a face's eyes green also returns the face, and mapping that face onto the mean of
// the hair and background around it is how an edit came back as a visibly dark patch. The band is a
// reference for drift, not a palette.
//
// `reach` is the coverage the composite is about to paste with, in the same coordinates as the two
// images. The band is where `reach` does not reach **and both images hold colour**: a pixel transparent
// on either side is excluded, because pairing a colour with black reads as a drift towards dark.
//
// Spread is matched from the same paired band, which is why the two deviations are of one pixel set and
// a scale factor between them means something.
//
// Identity - nothing written, and `false` - when the band holds fewer than two pixels (a mask covering
// the whole crop has no band: nothing to compare against, which is the full-frame case), when a
// deviation is below `kMinLabDeviation` on either side (the shift is applied and the spread left alone,
// so a flat band does not flatten a textured patch), or when the shift and the ratios come out at
// nothing - which is the case a correct edit in place should land in: the model changed the thing it was
// told to change and nothing else, so there is nothing to correct.
//
// `patch`'s alpha is its own: this corrects colour, never coverage.
bool colour_match_drift(Image& patch, const Image& original, const Mask& reach);

}  // namespace noise::ai
