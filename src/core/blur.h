// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/mask.h"

#include <vector>

namespace noise {

// A gaussian is what the prep and the feather both ask for and no box blur is one; three box passes
// are close enough that nothing downstream can see the difference, and cheaper than a kernel with
// negative lobes to get wrong. The one place that number exists.
inline constexpr int kGaussianBoxPasses = 3;

// `size.w * size.h * channels` samples, row-major, blurred in place. One pass averages
// `2 * radius + 1` samples along one axis and the passes alternate axes, starting horizontal.
//
// Edges clamp rather than wrap: a wrap pulls the far side of the picture into the border it is
// supposed to be continuing, which is a lie about the scene rather than a soft edge. Samples are
// integers so a blur of bytes stays exact - the rounding is one division per sample, and it rounds
// away from zero for the same reason a brush does.
void box_blur(std::vector<int>& samples, Extent size, int channels, int radius, int passes);

// The same coverage with a transition band instead of a hard edge: every cell becomes the mean of
// the cells within `radius` of it. A feather is a softening, not a threshold, so a partly covered
// pixel stays partly covered and coverage stays in `0..255`. `radius` 0 is the identity.
//
// Coverage outside the mask counts as none, which is the same rule the boundary uses: a mask cropped
// to a region feathers as if the region were the whole canvas, and that is what the composite wants
// when it feathers a crop whose selection cannot reach past it.
Mask feathered_mask(const Mask& coverage, int radius);

}  // namespace noise
