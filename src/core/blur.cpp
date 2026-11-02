// SPDX-License-Identifier: Apache-2.0
#include "core/blur.h"

#include <algorithm>

namespace noise {

namespace {

template <typename Sample>
void box_blur_samples(std::vector<Sample>& samples, Extent size, int channels,
                      int radius, int passes) {
    if (samples.empty() || size.w <= 0 || size.h <= 0 || channels <= 0 || radius <= 0 ||
        passes <= 0) {
        return;  // nothing to blur, or a blur that would be the identity
    }
    const int span = 2 * radius + 1;
    std::vector<Sample> out(samples.size());
    std::vector<int> sum(static_cast<std::size_t>(channels));
    const auto index = [size, channels](int x, int y) {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                static_cast<std::size_t>(x)) *
               static_cast<std::size_t>(channels);
    };

    // A running sum, so a wider radius costs the same as a narrow one. Re-summing the window per output
    // pixel made prep cost area x min(w,h): 9.1 s measured for a 1200x900 region, spent on the thread
    // that draws, which is how drawing a marquee over a third of a photo stopped the window.
    for (int pass = 0; pass < passes; ++pass) {
        const bool horizontal = pass % 2 == 0;
        const int lines = horizontal ? size.h : size.w;
        const int along = horizontal ? size.w : size.h;
        for (int line = 0; line < lines; ++line) {
            std::fill(sum.begin(), sum.end(), 0);
            for (int offset = -radius; offset <= radius; ++offset) {
                const std::size_t from = horizontal ? index(std::clamp(offset, 0, along - 1), line)
                                                   : index(line, std::clamp(offset, 0, along - 1));
                for (int c = 0; c < channels; ++c) {
                    sum[static_cast<std::size_t>(c)] += samples[from + static_cast<std::size_t>(c)];
                }
            }
            for (int cell = 0; cell < along; ++cell) {
                if (cell > 0) {
                    // The window slides by one: the element that leaves and the one that enters, both
                    // counted at their clamped positions, so the edge pixels repeat exactly as before.
                    const int leaving = std::clamp(cell - 1 - radius, 0, along - 1);
                    const int entering = std::clamp(cell + radius, 0, along - 1);
                    for (int c = 0; c < channels; ++c) {
                        const std::size_t in_from =
                            horizontal ? index(entering, line) : index(line, entering);
                        const std::size_t out_from =
                            horizontal ? index(leaving, line) : index(line, leaving);
                        sum[static_cast<std::size_t>(c)] +=
                            samples[in_from + static_cast<std::size_t>(c)] -
                            samples[out_from + static_cast<std::size_t>(c)];
                    }
                }
                const std::size_t to = horizontal ? index(cell, line) : index(line, cell);
                for (int c = 0; c < channels; ++c) {
                    out[to + static_cast<std::size_t>(c)] = static_cast<Sample>(
                        (sum[static_cast<std::size_t>(c)] + span / 2) / span);
                }
            }
        }
        samples.swap(out);
    }
}

}  // namespace

void box_blur(std::vector<int>& samples, Extent size, int channels, int radius, int passes) {
    box_blur_samples(samples, size, channels, radius, passes);
}

void box_blur(std::vector<std::uint8_t>& samples, Extent size, int channels, int radius,
              int passes) {
    box_blur_samples(samples, size, channels, radius, passes);
}

Mask feathered_mask(const Mask& coverage, int radius) {
    if (radius <= 0 || !coverage.sized()) {
        return coverage;
    }
    std::vector<std::uint8_t> plane = coverage.plane();
    box_blur(plane, coverage.size(), 1, radius, kGaussianBoxPasses);
    return *Mask::from_plane(coverage.size(), std::move(plane));
}

}  // namespace noise
