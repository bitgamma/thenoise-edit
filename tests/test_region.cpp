// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "ai/region.h"
#include "core/mask.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace noise;
using noise::ai::kRegionPolicy;
using noise::ai::Prep;
using noise::ai::RegionPlan;
using noise::ai::RegionPolicy;
using noise::ai::prep_blur_radius;
using noise::ai::prep_from_name;
using noise::ai::prep_name;
using noise::ai::prep_names;

namespace {

// A base with a hard vertical edge one pixel off the middle: the most demanding input a prep can be
// handed, and deliberately not on a block or tile boundary, so a prep that averages over a block has
// to be caught mixing the two sides rather than neatly aligned with them.
// because anything that averages or blurs has to move those two values and anything that does not,
// does not preserve them either.
// One pixel off the middle, so the edge falls inside a prep block rather than on its boundary.
int edge_x(int width) { return width / 2 + 1; }

Image hard_edge(Extent size) {
    Image image = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t value = x < edge_x(size.w) ? 255 : 0;
            std::uint8_t* pixel = image.at(x, y);
            pixel[0] = value;
            pixel[1] = value;
            pixel[2] = value;
            pixel[3] = 255;
        }
    }
    return image;
}

double mean_channel(const Image& image, int channel) {
    const Extent size = image.size();
    double sum = 0.0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            sum += static_cast<double>(image.at(x, y)[channel]);
        }
    }
    return sum / static_cast<double>(size.w * size.h);
}

Mask mask_of(Extent canvas, Rect covered) {
    Mask mask(canvas);
    mask.op(covered, MaskOp::replace);
    return mask;
}

int round_up(int value, int multiple) {
    return ((value + multiple - 1) / multiple) * multiple;
}

// A mask that claims nothing compiles to no region. This is the difference the document already draws
// between "nothing selected" and "everything selected", and a compile that guessed would edit the
// whole picture because a box had no contents.
void test_a_mask_with_no_coverage_compiles_to_nothing() {
    const Extent canvas{256, 256};
    CHECK(!plan_region(canvas, Mask(canvas), kRegionPolicy).has_value());
    Mask empty_covering_nothing(canvas);
    empty_covering_nothing.op(Rect{10, 10, 0, 5}, MaskOp::replace);
    CHECK(!plan_region(canvas, empty_covering_nothing, kRegionPolicy).has_value());
    CHECK(!plan_region(Extent{0, 0}, mask_of(canvas, Rect{10, 10, 8, 8}), kRegionPolicy).has_value());
}

// The crop is grown by the policy, kept inside the canvas, and contains the mask. Every claim here is
// computed from the policy in the test, not read back out of the plan.
void test_the_crop_grows_for_context_and_stays_on_the_canvas() {
    const Extent canvas{1024, 768};
    const Rect bounds{500, 300, 40, 30};
    const auto plan = plan_region(canvas, mask_of(canvas, bounds), kRegionPolicy).value();

    const int grow_x = std::max(static_cast<int>(std::lround(bounds.w * kRegionPolicy.context_ratio)),
                                kRegionPolicy.min_context_px);
    const int grow_y = std::max(static_cast<int>(std::lround(bounds.h * kRegionPolicy.context_ratio)),
                                kRegionPolicy.min_context_px);
    CHECK_EQ(plan.crop.w, round_up(bounds.w + 2 * grow_x, kRegionPolicy.size_multiple));
    CHECK_EQ(plan.crop.h, round_up(bounds.h + 2 * grow_y, kRegionPolicy.size_multiple));
    const Rect wanted{bounds.x - grow_x, bounds.y - grow_y, bounds.w + 2 * grow_x,
                      bounds.h + 2 * grow_y};
    CHECK(plan.crop.contains(wanted.x, wanted.y) &&
          plan.crop.contains(wanted.right() - 1, wanted.bottom() - 1));
    CHECK(plan.mask_bounds == bounds);
    CHECK(plan.crop.inside(canvas));
    CHECK(plan.crop.w % kRegionPolicy.size_multiple == 0);
    CHECK(plan.crop.h % kRegionPolicy.size_multiple == 0);
    CHECK_EQ(plan.scale, 1.0);
    CHECK(plan.snapped_width);
    CHECK(plan.snapped_height);

    // Against an edge the growth has nowhere to go, so the crop slides inward rather than hanging off
    // the canvas: the size the policy asked for, a different origin.
    const Rect near_corner{1010, 760, 10, 8};
    const auto corner = plan_region(canvas, mask_of(canvas, near_corner), kRegionPolicy).value();
    const int corner_grow_x =
        std::max(static_cast<int>(std::lround(near_corner.w * kRegionPolicy.context_ratio)),
                 kRegionPolicy.min_context_px);
    const int corner_grow_y =
        std::max(static_cast<int>(std::lround(near_corner.h * kRegionPolicy.context_ratio)),
                 kRegionPolicy.min_context_px);
    CHECK_EQ(corner.crop.w,
             round_up(near_corner.w + 2 * corner_grow_x, kRegionPolicy.size_multiple));
    CHECK_EQ(corner.crop.h,
             round_up(near_corner.h + 2 * corner_grow_y, kRegionPolicy.size_multiple));
    CHECK(corner.crop.inside(canvas));
    CHECK(corner.crop.contains(near_corner.x, near_corner.y) &&
          corner.crop.contains(near_corner.right() - 1, near_corner.bottom() - 1));

    // A canvas shorter than one multiple cannot be snapped; the plan says so instead of lying about
    // the policy it kept.
    const Extent tiny{20, 20};
    const auto small = plan_region(tiny, mask_of(tiny, Rect{9, 9, 2, 2}), kRegionPolicy).value();
    CHECK_EQ(small.crop.w, tiny.w);
    CHECK_EQ(small.crop.h, tiny.h);
    CHECK(!small.snapped_width);
    CHECK(!small.snapped_height);
}

// The size sent is always a multiple of the policy's, never smaller than the crop it came from, and
// never an upscale.
void test_the_requested_size_honours_the_policy_and_the_budget() {
    const int multiple = kRegionPolicy.size_multiple;
    const std::vector<Rect> boxes{Rect{4, 4, 1, 1}, Rect{500, 500, 60, 60},
                                  Rect{0, 0, 1024, 1024}, Rect{100, 100, 900, 700}};
    for (const Rect box : boxes) {
        const Extent canvas{1024, 1024};
        const auto plan = plan_region(canvas, mask_of(canvas, box), kRegionPolicy).value();
        CHECK(plan.requested.w % multiple == 0);
        CHECK(plan.requested.h % multiple == 0);
        CHECK(plan.requested.w >= plan.crop.w);
        CHECK(plan.requested.h >= plan.crop.h);
        CHECK(plan.scale <= 1.0);
        CHECK(plan.requested.w >= static_cast<int>(std::ceil(plan.crop.w * plan.scale)));
    }

    // A region over budget comes down to it: the area asked for is inside the budget plus the slack
    // the snap to a multiple has to allow for, and no pixel is invented.
    const Extent canvas{4096, 4096};
    const auto big = plan_region(canvas, mask_of(canvas, Rect{0, 0, 4000, 3000}), kRegionPolicy)
                         .value();
    CHECK(big.scale < 1.0);
    const double slack = static_cast<double>(2 * multiple) * static_cast<double>(4096) * 2.0;
    CHECK(static_cast<double>(big.requested.w) * static_cast<double>(big.requested.h) <=
          static_cast<double>(kRegionPolicy.max_area_px) + slack);
}

// The map back has to be exact where nothing was rescaled, and inside the crop where something was:
// the result lands on the pixels it came from or the edit is in the wrong place.
void test_the_map_back_lands_where_the_crop_was() {
    const Extent canvas{1024, 1024};
    const auto plan = plan_region(canvas, mask_of(canvas, Rect{400, 300, 120, 90}), kRegionPolicy)
                          .value();
    const Vec2 origin = plan.to_document(Vec2{0.0, 0.0});
    CHECK(std::abs(origin.x - static_cast<double>(plan.crop.x)) < 1e-9);
    CHECK(std::abs(origin.y - static_cast<double>(plan.crop.y)) < 1e-9);
    for (const Vec2 point : {Vec2{0.0, 0.0}, Vec2{60.0, 45.0},
                             Vec2{static_cast<double>(plan.crop.w - 1),
                                  static_cast<double>(plan.crop.h - 1)}}) {
        const Vec2 back = plan.to_document(plan.to_request(point));
        CHECK(std::abs(back.x - point.x) < 1e-9);
        CHECK(std::abs(back.y - point.y) < 1e-9);
    }

    // With a fit in play the request is a little larger than the scaled crop, so the far corner maps
    // past the crop's own corner - by at most one source pixel's worth of rounding.
    const Extent huge{4096, 4096};
    const auto fit = plan_region(huge, mask_of(huge, Rect{0, 0, 4000, 3000}), kRegionPolicy).value();
    const Vec2 far = fit.to_document(
        Vec2{static_cast<double>(fit.requested.w), static_cast<double>(fit.requested.h)});
    const double overshoot = 1.0 / fit.scale + 1.0;
    CHECK(far.x >= static_cast<double>(fit.crop.right()));
    CHECK(far.x - static_cast<double>(fit.crop.right()) <= overshoot);
    CHECK(far.y >= static_cast<double>(fit.crop.bottom()));
    CHECK(far.y - static_cast<double>(fit.crop.bottom()) <= overshoot);
}

// Each prep does the one thing it is named for, measured against the statistics it is supposed to
// keep or destroy.
void test_each_prep_keeps_what_it_says_and_loses_what_it_says() {
    const Extent size{96, 48};
    const Image base = hard_edge(size);
    const RegionPlan plan = [&size] {
        RegionPlan p;
        p.mask_bounds = Rect{0, 0, size.w, size.h};
        p.crop = Rect{0, 0, size.w, size.h};
        p.requested = size;
        return p;
    }();
    const double base_mean = mean_channel(base, 0);

    const Image none = prep_region(base, plan, Prep::none, 0);
    CHECK(none.size() == size);
    bool identical = true;
    for (int y = 0; y < size.h && identical; ++y) {
        for (int x = 0; x < size.w && identical; ++x) {
            identical = std::memcmp(none.at(x, y), base.at(x, y), 4) == 0;
        }
    }
    CHECK(identical);

    const Image white = prep_region(base, plan, Prep::white, 0);
    CHECK(white.size() == size);
    CHECK_EQ(static_cast<int>(white.at(10, 10)[0]), 255);
    CHECK_EQ(static_cast<int>(white.at(80, 30)[0]), 255);

    const Image average = prep_region(base, plan, Prep::average, 0);
    // A flat region is the rounded mean, exactly - "average" is not an approximation of anything.
    CHECK_NEAR(mean_channel(average, 0), static_cast<double>(std::lround(base_mean)), 1e-9);
    CHECK_EQ(static_cast<int>(average.at(4, 4)[0]), static_cast<int>(average.at(91, 43)[0]));

    const Image pixelated = prep_region(base, plan, Prep::pixelate, 0);
    CHECK_NEAR(mean_channel(pixelated, 0), base_mean, 2.0);
    const int block = prep_blur_radius(plan.crop);
    bool blocked = true;
    for (int by = 0; by < size.h && blocked; by += block) {
        for (int bx = 0; bx < size.w && blocked; bx += block) {
            for (int y = by; y < std::min(by + block, size.h); ++y) {
                for (int x = bx; x < std::min(bx + block, size.w); ++x) {
                    blocked = pixelated.at(x, y)[0] == pixelated.at(bx, by)[0];
                }
            }
        }
    }
    CHECK(blocked);
    CHECK(pixelated.at(2, 24)[0] != pixelated.at(94, 24)[0]);  // the two halves stay distinguishable
    // A block that straddles the edge has to come back as one value where the source had two. Without
    // this the claim above is vacuous for a block of one, which is no pixelation at all.
    bool straddling = false;
    for (int bx = 0; bx < size.w; bx += block) {
        if (bx < edge_x(size.w) && bx + block > edge_x(size.w)) {
            straddling = true;
            CHECK(base.at(bx, 24)[0] != base.at(bx + block - 1, 24)[0]);
            CHECK(pixelated.at(bx, 24)[0] == pixelated.at(bx + block - 1, 24)[0]);
        }
    }
    CHECK(straddling);

    const Image blurred = prep_region(base, plan, Prep::blur, 0);
    CHECK_NEAR(mean_channel(blurred, 0), base_mean, 2.0);
    CHECK(static_cast<int>(blurred.at(size.w / 2, 24)[0]) < 250);
    CHECK(static_cast<int>(blurred.at(size.w / 2 + 1, 24)[0]) > 5);
    CHECK(prep_region(base, plan, Prep::blur, 0).checksum_hex() == blurred.checksum_hex());

    const Image noise = prep_region(base, plan, Prep::noise, 7);
    CHECK_NEAR(mean_channel(noise, 0), base_mean, 3.0);
    CHECK(prep_region(base, plan, Prep::noise, 7).checksum_hex() == noise.checksum_hex());
    CHECK(prep_region(base, plan, Prep::noise, 8).checksum_hex() != noise.checksum_hex());
}

void test_the_prep_vocabulary_is_the_one_home() {
    for (const Prep prep : {Prep::none, Prep::average, Prep::blur, Prep::pixelate, Prep::noise,
                            Prep::white}) {
        const std::string name = prep_name(prep);
        CHECK(prep_from_name(name).has_value());
        CHECK(prep_from_name(name).value() == prep);
        CHECK(prep_names().find(name) != std::string::npos);
    }
    CHECK(!prep_from_name("sharpen").has_value());
    CHECK(!prep_from_name("").has_value());
}

}  // namespace

int main() {
    test_a_mask_with_no_coverage_compiles_to_nothing();
    test_the_crop_grows_for_context_and_stays_on_the_canvas();
    test_the_requested_size_honours_the_policy_and_the_budget();
    test_the_map_back_lands_where_the_crop_was();
    test_each_prep_keeps_what_it_says_and_loses_what_it_says();
    test_the_prep_vocabulary_is_the_one_home();
    return noise::test::summary(__FILE__);
}
