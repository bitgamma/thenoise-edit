// SPDX-License-Identifier: Apache-2.0
#include "core/blur.h"

#include <algorithm>

namespace noise {

void box_blur(std::vector<int>& samples, Extent size, int channels, int radius, int passes) {
    if (samples.empty() || size.w <= 0 || size.h <= 0 || channels <= 0 || radius <= 0 ||
        passes <= 0) {
        return;  // nothing to blur, or a blur that would be the identity
    }
    const int span = 2 * radius + 1;
    std::vector<int> out(samples.size());
    std::vector<int> sum(static_cast<std::size_t>(channels));
    const auto index = [size, channels](int x, int y) {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                static_cast<std::size_t>(x)) *
               static_cast<std::size_t>(channels);
    };

    for (int pass = 0; pass < passes; ++pass) {
        const bool horizontal = pass % 2 == 0;
        const int lines = horizontal ? size.h : size.w;
        const int along = horizontal ? size.w : size.h;
        for (int line = 0; line < lines; ++line) {
            for (int cell = 0; cell < along; ++cell) {
                std::fill(sum.begin(), sum.end(), 0);
                for (int offset = -radius; offset <= radius; ++offset) {
                    const int other = std::clamp(cell + offset, 0, along - 1);
                    const std::size_t from =
                        horizontal ? index(other, line) : index(line, other);
                    for (int c = 0; c < channels; ++c) {
                        sum[static_cast<std::size_t>(c)] += samples[from + static_cast<std::size_t>(c)];
                    }
                }
                const std::size_t to = horizontal ? index(cell, line) : index(line, cell);
                for (int c = 0; c < channels; ++c) {
                    out[to + static_cast<std::size_t>(c)] = (sum[static_cast<std::size_t>(c)] + span / 2) / span;
                }
            }
        }
        samples = out;
    }
}

Mask feathered_mask(const Mask& coverage, int radius) {
    if (radius <= 0 || !coverage.sized()) {
        return coverage;
    }
    const Extent size = coverage.size();
    std::vector<int> samples(static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h));
    for (int y = 0; y < size.h; ++y) {
        const std::span<const std::uint8_t> row = coverage.row(y);
        for (int x = 0; x < size.w; ++x) {
            samples[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) +
                    static_cast<std::size_t>(x)] = row[static_cast<std::size_t>(x)];
        }
    }
    box_blur(samples, size, 1, radius, kGaussianBoxPasses);

    std::vector<std::uint8_t> plane(samples.size());
    // A box average of bytes cannot leave the range, so no clamping: the samples are already means.
    for (std::size_t i = 0; i < samples.size(); ++i) {
        plane[i] = static_cast<std::uint8_t>(samples[i]);
    }
    return Mask::from_plane(size, plane).value();
}

}  // namespace noise
