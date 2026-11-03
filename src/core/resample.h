// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/image.h"
#include "core/mask.h"

namespace noise {

// The one resampling filter the app uses, named because it is a contract and not an
// implementation detail: the GPU pair, when a resample moves to the shader, has to reproduce these
// numbers the way `core/blend`'s CPU/GPU pair does.
//
// Lanczos3, separable, with the support widened by the downscale factor - without the widening a
// reduction aliases, and an answer coming back from the server at whatever size the engine snapped
// to is exactly the case that reduces by a non-integer factor.
//
// Premultiplied, and that is not an optimisation: an image with real transparency resampled in
// straight alpha drags the black under its transparent pixels into the visible ones, so a generated
// answer with a soft edge would come back with a dark fringe. Colour under a zero alpha is
// therefore not preserved - premultiplication discards what cannot be seen, and the app keeps
// "nothing here" at one byte pattern anyway.
//
// A request for the size the image already is comes back byte-identical: Lanczos is zero at every
// integer offset but its own, so a 1:1 pass through it is a copy.
Image resample_lanczos3(const Image& src, Extent size);

// The filter's radius in source pixels at 1:1. A caller cropping a picture out of a bigger plane has to
// leave this much margin - widened by the reduction factor - or the window runs off the data at the edge
// of the crop and the border of the result is a lie about what was outside it.
inline constexpr int kResampleRadiusPx = 3;

// The margin a crop needs to carry so the filter never samples beyond real data at `factor`.
inline constexpr int resample_margin_px(double factor) {
    const double stretch = factor > 0.0 ? 1.0 / factor : 1.0;
    return static_cast<int>(kResampleRadiusPx * (stretch > 1.0 ? stretch : 1.0)) + 1;
}

// Coverage through the same filter. A mask holds no colour, so the plane is read straight rather than
// premultiplied and the negative lobes of Lanczos are clamped at the two ends of the coverage scale - a
// cell cannot claim less than nothing or more than the whole pixel. Ringing at a hard edge is therefore
// bounded instead of exact: resampling a mask is a soft operation by nature, and a mask whose edge was
// drawn hard comes back with an edge that is one pixel wider rather than one that has moved.
Mask resample_coverage_lanczos3(const Mask& src, Extent size);
// No negative lobes: soft selections stay soft, hard edges cannot grow ringing islands.
Mask resample_coverage_linear(const Mask& src, Extent size);

}  // namespace noise
