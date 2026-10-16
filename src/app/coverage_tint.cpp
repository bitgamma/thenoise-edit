// SPDX-License-Identifier: Apache-2.0
#include "app/coverage_tint.h"

#include "core/mask.h"

#include <vector>

namespace noise::app {
namespace {

// Enough to read a stroke against the picture under it, little enough that the picture is still the
// thing being judged. A flat alpha on the coverage byte, so a half-covered edge washes half as hard.
constexpr std::uint8_t kTintAlpha = 96;
constexpr std::uint8_t kTintR = 150;
constexpr std::uint8_t kTintG = 190;
constexpr std::uint8_t kTintB = 255;

// FNV-1a over the bytes of the covered box, with the box itself in the seed: two different shapes of
// the same size, and the same shape moved, both change it.
std::uint64_t stamp_of(const Mask& coverage, Rect bounds) {
    std::uint64_t hash = 1469598103934665603ULL;
    const std::uint64_t geometry = (static_cast<std::uint64_t>(bounds.x) << 32) |
                                   (static_cast<std::uint64_t>(bounds.y) << 0);
    for (const std::uint8_t part : {static_cast<std::uint8_t>(geometry >> 56),
                                    static_cast<std::uint8_t>(geometry >> 40),
                                    static_cast<std::uint8_t>(bounds.w),
                                    static_cast<std::uint8_t>(bounds.h)}) {
        hash = (hash ^ part) * 1099511628211ULL;
    }
    for (int y = 0; y < bounds.h; ++y) {
        const std::span<const std::uint8_t> row = coverage.row(bounds.y + y);
        for (int x = bounds.x; x < bounds.right(); ++x) {
            hash = (hash ^ row[static_cast<std::size_t>(x)]) * 1099511628211ULL;
        }
    }
    return hash;
}

}  // namespace

void CoverageTint::sync(const Mask& coverage) {
    const Rect box = coverage.bounds();
    if (box.empty()) {
        drawn = false;
        bounds = {};
        texture.discard();
        return;
    }
    const std::uint64_t next = stamp_of(coverage, box);
    if (next == stamp && box == bounds && texture.valid()) {
        drawn = true;
        return;
    }
    const std::size_t width = static_cast<std::size_t>(box.w);
    std::vector<std::uint8_t> rgba(width * static_cast<std::size_t>(box.h) * 4, 0);
    for (int y = 0; y < box.h; ++y) {
        const std::span<const std::uint8_t> row = coverage.row(box.y + y);
        for (int x = 0; x < box.w; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
            rgba[at] = kTintR;
            rgba[at + 1] = kTintG;
            rgba[at + 2] = kTintB;
            rgba[at + 3] = static_cast<std::uint8_t>(
                (row[static_cast<std::size_t>(box.x + x)] * kTintAlpha + kMaskIn / 2) / kMaskIn);
        }
    }
    texture.sync(Image::from_rgba(Extent{box.w, box.h}, std::move(rgba)), next);
    stamp = next;
    bounds = box;
    drawn = true;
}

}  // namespace noise::app
