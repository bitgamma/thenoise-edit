// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/geom.h"
#include "core/image.h"

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

}  // namespace noise
