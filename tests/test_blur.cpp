// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/blur.h"
#include "core/mask.h"

#include <cstdint>
#include <random>
#include <vector>

using namespace noise;

namespace {

constexpr int kSize = 48;
constexpr Extent kCanvas{kSize, kSize};

Mask hard_edge(int edge_x, std::uint8_t left, std::uint8_t right) {
    Mask mask(kCanvas);
    std::vector<std::uint8_t> plane;
    plane.reserve(static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            plane.push_back(x < edge_x ? left : right);
        }
    }
    return Mask::from_plane(kCanvas, plane).value();
}

// The reach of `passes` box passes of this radius: each pass moves influence by `radius` along its
// own axis, and the axes alternate, so two along one and one along the other.
constexpr int kSupportMargin = 3;

void test_a_feather_of_zero_is_the_identity() {
    const Mask edge = hard_edge(24, 255, 0);
    const Mask same = feathered_mask(edge, 0);
    CHECK(same == edge);
}

void test_a_soft_plane_feathers_to_itself() {
    // A mask that already covers everything has no edge to soften, and clamping the edge instead of
    // wrapping it is what keeps it that way: coverage that ran off the canvas would come back as a
    // fade at the border, which is a lie about the scene.
    Mask full(kCanvas);
    full.fill(255);
    const Mask feathered = feathered_mask(full, 6);
    CHECK(feathered.covers_all(Rect{0, 0, kSize, kSize}));
    CHECK_EQ(feathered.coverage(0, 0), 255);
    CHECK_EQ(feathered.coverage(kSize - 1, kSize - 1), 255);

    Mask empty(kCanvas);
    empty.fill(0);
    CHECK(feathered_mask(empty, 6).covers_nothing(Rect{0, 0, kSize, kSize}));
}

void test_a_hard_edge_gains_a_ramp_and_no_coverage() {
    const Mask edge = hard_edge(24, 255, 0);
    const int radius = 3;
    const Mask feathered = feathered_mask(edge, radius);

    // A feather softens coverage, it never invents any: nothing was covered to the right of the
    // edge, and a blur that put coverage there would be pasting outside the selection.
    CHECK_EQ(feathered.coverage(kSize - 1, 20), 0);
    CHECK_EQ(feathered.coverage(40, 24), 0);
    // ... and the same on the covered side, well inside it.
    CHECK_EQ(feathered.coverage(2, 24), 255);

    // The ramp runs across the boundary and never overshoots what the source had.
    int ramp = 0;
    for (int x = 24 - kSupportMargin * radius; x < 24 + kSupportMargin * radius; ++x) {
        const std::uint8_t value = feathered.coverage(x, 24);
        CHECK(value <= 255);
        if (value > 0 && value < 255) {
            ++ramp;
        }
    }
    CHECK(ramp >= radius);

    // A blur of a monotone profile stays monotone: ringing on a feather would show as a light fringe
    // beside a dark one, and no box pass can do it - so if it appears, the pass order is wrong.
    std::uint8_t previous = feathered.coverage(0, 24);
    for (int x = 1; x < kSize; ++x) {
        const std::uint8_t value = feathered.coverage(x, 24);
        CHECK(value <= previous);
        previous = value;
    }
}

void test_a_feather_keeps_a_partial_pixel_partial() {
    const Mask edge = hard_edge(24, 128, 0);
    const Mask feathered = feathered_mask(edge, 3);
    // The half-covered pixel is a half claim before and a weaker claim after, never a full one: a
    // feather that thresholded would turn an anti-aliased selection edge into a hard one.
    CHECK(feathered.coverage(23, 24) <= 128);
    CHECK(feathered.coverage(23, 24) > 0);
    CHECK(feathered.coverage(24, 24) < 128);
}

void test_the_blur_is_exact_on_a_sample_plane() {
    // A plane that is constant is constant after: that is the arithmetic, not the filter, and it is
    // what lets a prep and a feather agree about flat regions.
    std::vector<int> samples(static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize) * 4u,
                             77);
    box_blur(samples, kCanvas, 4, 5, kGaussianBoxPasses);
    std::size_t differing = 0;
    for (const int sample : samples) {
        if (sample != 77) {
            ++differing;
        }
    }
    CHECK_EQ(differing, std::size_t(0));

    // One sample of one channel moves its neighbourhood by a known amount: the mean of a box of one
    // lit sample is the sample over the box size, and never more.
    std::vector<int> single(static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize), 0);
    const std::size_t lit =
        static_cast<std::size_t>(24) * static_cast<std::size_t>(kSize) + 24u;
    single[lit] = 255;
    box_blur(single, kCanvas, 1, 1, 1);
    CHECK_EQ(single[lit], 85);  // 255 over a window of three, rounded
    CHECK_EQ(single[lit + 2], 0);
    CHECK_EQ(single[lit + 1], 85);  // the window slides, the sample it holds does not
}

void test_byte_blur_matches_integer_blur_exactly() {
    const Extent size{37, 21};
    std::mt19937 random(641);
    for (int channels : {1, 3, 4}) {
        std::vector<std::uint8_t> source(static_cast<std::size_t>(size.w) *
                                         static_cast<std::size_t>(size.h) *
                                         static_cast<std::size_t>(channels));
        for (std::uint8_t& value : source) {
            value = static_cast<std::uint8_t>(random() % 256u);
        }
        for (int radius : {0, 1, 3, 11}) {
            for (int passes : {1, 2, 3, 4}) {
                std::vector<int> integers(source.begin(), source.end());
                std::vector<std::uint8_t> bytes = source;
                box_blur(integers, size, channels, radius, passes);
                box_blur(bytes, size, channels, radius, passes);
                REQUIRE(integers.size() == bytes.size());
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    CHECK_EQ(integers[index], static_cast<int>(bytes[index]));
                }
            }
        }
    }
}

}  // namespace

int main() {
    test_a_feather_of_zero_is_the_identity();
    test_a_soft_plane_feathers_to_itself();
    test_a_hard_edge_gains_a_ramp_and_no_coverage();
    test_a_feather_keeps_a_partial_pixel_partial();
    test_the_blur_is_exact_on_a_sample_plane();
    test_byte_blur_matches_integer_blur_exactly();
    return noise::test::summary(__FILE__);
}
