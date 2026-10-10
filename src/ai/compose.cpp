// SPDX-License-Identifier: Apache-2.0
#include "ai/compose.h"

#include "ai/colour_match.h"
#include "core/blur.h"
#include "core/resample.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace noise::ai {
namespace {

std::string describe(Extent size) {
    return std::to_string(size.w) + "x" + std::to_string(size.h);
}

std::string describe(Rect rect) {
    return "(" + std::to_string(rect.x) + "," + std::to_string(rect.y) + ") " +
           std::to_string(rect.w) + "x" + std::to_string(rect.h);
}

// A crop edge is locked only where there are pixels outside it that never went into the request.
// At a canvas edge there are none, and locking there would open a strip of unpasted original in the
// middle of the picture to protect nothing.
struct LockBand {
    int px = 0;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;

    bool holds(int x, int y, Rect crop) const {
        if (px <= 0) {
            return false;
        }
        return (left && x - crop.x < px) || (right && crop.right() - x <= px) ||
               (up && y - crop.y < px) || (down && crop.bottom() - y <= px);
    }
};

LockBand lock_band(Rect crop, Extent canvas, int px) {
    return LockBand{px, crop.x > 0, crop.right() < canvas.w, crop.y > 0, crop.bottom() < canvas.h};
}

// The coverage the blend will use, in the crop's own coordinates: the selection softened by the
// feather, then emptied in the lock band and wherever the extent says no write may exist.
//
// The feather is computed over the crop and not the canvas because the selection cannot reach past
// the crop - the plan grew the crop around its bounds - so a crop-sized blur sees exactly the same
// coverage and costs the crop.
Mask paste_coverage(const Mask& coverage, const Mask& extent, Rect crop, int feather_px,
                    int lock_band_px, std::size_t& clipped_by_extent) {
    Mask soft = feathered_mask(coverage.region(crop), feather_px);
    std::vector<std::uint8_t> plane = soft.plane();
    const Mask crop_extent = extent.region(crop);
    const LockBand band = lock_band(crop, extent.size(), lock_band_px);
    for (int y = 0; y < crop.h; ++y) {
        for (int x = 0; x < crop.w; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(crop.w) +
                                   static_cast<std::size_t>(x);
            if (band.holds(crop.x + x, crop.y + y, crop)) {
                plane[at] = kMaskOut;
                continue;
            }
            // The extent is multiplied after the feather rather than folded into the mask before it:
            // pre-feather clipping lets the feather bleed coverage past where the destination exists,
            // which is a silhouette the caller said the write may not extend.
            const std::uint8_t allowed = crop_extent.coverage(x, y);
            if (allowed == 0) {
                if (plane[at] > 0) {
                    ++clipped_by_extent;
                }
                plane[at] = kMaskOut;
            } else if (allowed < kMaskIn) {
                plane[at] = static_cast<std::uint8_t>(
                    (static_cast<int>(plane[at]) * static_cast<int>(allowed) + kMaskIn / 2) /
                    kMaskIn);
            }
        }
    }
    return Mask::from_plane(Extent{crop.w, crop.h}, plane).value();
}

// The same coverage in document pixels, which is what `restrict_to` indexes.
Mask in_document(const Mask& crop_coverage, Extent canvas, Rect crop) {
    std::vector<std::uint8_t> plane(static_cast<std::size_t>(canvas.w) *
                                        static_cast<std::size_t>(canvas.h),
                                    kMaskOut);
    for (int y = 0; y < crop.h; ++y) {
        std::copy(crop_coverage.row(y).begin(), crop_coverage.row(y).end(),
                  plane.begin() + static_cast<std::ptrdiff_t>(
                                      (static_cast<std::size_t>(crop.y + y) *
                                       static_cast<std::size_t>(canvas.w)) +
                                      static_cast<std::size_t>(crop.x)));
    }
    return Mask::from_plane(canvas, plane).value();
}

}  // namespace

double mean_thickness_px(const Mask& coverage) {
    double perimeter = 0.0;
    for (const MaskRun& run : coverage.boundary()) {
        perimeter += static_cast<double>(run.to - run.from);
    }
    if (perimeter <= 0.0) {
        return 0.0;  // nothing covered has no width
    }
    return 2.0 * coverage.area() / perimeter;
}

int auto_feather_radius(const Mask& coverage) {
    const int radius =
        static_cast<int>(std::lround(mean_thickness_px(coverage) / kAutoFeatherThicknessDivisor));
    return std::clamp(radius, kAutoFeatherMinPx, kAutoFeatherMaxPx);
}

ComposeReport compose_result(Image& target, const Image& result, const RegionPlan& plan,
                             const Mask& coverage, const Mask& extent,
                             const CompositePolicy& policy) {
    ComposeReport report;
    report.result_dims = result.size();
    report.answered_as_requested = result.size() == plan.requested;

    if (result.empty()) {
        report.refusal = "the answer is empty; the plan asked for " + describe(plan.requested);
        return report;
    }
    if (target.empty()) {
        report.refusal = "the image being composed into is empty";
        return report;
    }
    if (!plan.crop.inside(target.size())) {
        report.refusal = "plan crop " + describe(plan.crop) + " is not inside the " +
                         describe(target.size()) + " image it is being landed on";
        return report;
    }
    if (!coverage.sized()) {
        report.refusal =
            "the coverage mask has no pixels: nothing was selected, so nothing is composed";
        return report;
    }
    if (coverage.size() != target.size()) {
        report.refusal = "the coverage mask is " + describe(coverage.size()) + " and the image is " +
                         describe(target.size()) + "; both are document pixels and must agree";
        return report;
    }
    if (intersect(coverage.bounds(), plan.crop).empty()) {
        report.refusal = "the coverage mask, which covers " + describe(coverage.bounds()) +
                         ", reaches nowhere inside the plan crop " + describe(plan.crop);
        return report;
    }

    const Rect crop = plan.crop;
    report.area = crop;
    report.feather_px = policy.feather_px.has_value()
                            ? std::max(0, policy.feather_px.value())
                            : auto_feather_radius(coverage);

    const Image original = target.region(crop);
    const Mask crop_mask =
        paste_coverage(coverage, extent, crop, report.feather_px, policy.lock_band_px,
                       report.pixels_clipped_by_extent);

    Image patch = resample_lanczos3(result, Extent{crop.w, crop.h});
    report.colour_corrected = policy.colour_match && colour_match_ring(patch, original, crop_mask);

    report.coverage = in_document(crop_mask, target.size(), crop);
    target.blit(patch, crop.x, crop.y);
    restrict_to(report.coverage, crop, original, crop, target);

    for (int y = 0; y < crop.h; ++y) {
        for (int x = 0; x < crop.w; ++x) {
            if (std::memcmp(target.at(crop.x + x, crop.y + y), original.at(x, y), kChannels) != 0) {
                ++report.pixels_changed;
            }
        }
    }
    report.composed = true;
    return report;
}

}  // namespace noise::ai
