// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/image.h"
#include "core/mask.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace noise;
using noise::test::checks;

namespace {

// A deterministic plane of coverage, so the mask tests are reproducible without depending on a
// library's random numbers.
std::vector<std::uint8_t> plane_of(Extent size, std::uint32_t seed) {
    const std::size_t cells =
        static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h);
    std::vector<std::uint8_t> plane(cells);
    std::uint32_t state = seed | 1u;
    for (std::uint8_t& cell : plane) {
        state = state * 1664525u + 1013904223u;
        cell = static_cast<std::uint8_t>((state >> 24) & 0xFFu);
    }
    return plane;
}

std::string rect_text(const Rect& r) {
    return "x=" + std::to_string(r.x) + " y=" + std::to_string(r.y) + " w=" + std::to_string(r.w) +
           " h=" + std::to_string(r.h);
}

bool rect_equal(const Rect& got, const Rect& want) {
    if (got == want) {
        return true;
    }
    noise::test::report("bounds", __FILE__, __LINE__,
                        "got " + rect_text(got) + " want " + rect_text(want));
    return false;
}

std::size_t cell(Extent size, int x, int y) {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w) + static_cast<std::size_t>(x);
}

Image image_of(Extent size, std::uint32_t seed) {
    Image image = Image::filled(size, 0, 0, 0, 0);
    std::uint32_t state = (seed ^ 0x9e3779b9u) | 1u;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* pixel = image.at(x, y);
            for (int channel = 0; channel < kChannels; ++channel) {
                state = state * 1664525u + 1013904223u;
                pixel[channel] = static_cast<std::uint8_t>((state >> 23) & 0xFFu);
            }
        }
    }
    return image;
}

// Source-over written out again from the definition in `core/blend.h`, with the amount painted as
// its own factor: `amount` is what the mask multiplies. Deliberately independent of the
// implementation, because the claim under test is about the two ways of applying it agreeing.
Rgba8 over(const Rgba8& src, double amount, const Rgba8& dst) {
    const double as = (static_cast<double>(src.a) / 255.0) * amount;
    const double ad = static_cast<double>(dst.a) / 255.0;
    const double out = as + ad * (1.0 - as);
    auto channel = [&](std::uint8_t source, std::uint8_t below) {
        if (out <= 0.0) {
            return std::uint8_t{0};
        }
        const double mixed = (static_cast<double>(source) / 255.0 * as +
                              static_cast<double>(below) / 255.0 * ad * (1.0 - as)) / out;
        return static_cast<std::uint8_t>(mixed * 255.0 + 0.5);
    };
    return Rgba8{channel(src.r, dst.r), channel(src.g, dst.g), channel(src.b, dst.b),
                 static_cast<std::uint8_t>(out * 255.0 + 0.5)};
}

// Erasing takes alpha away and leaves the colour alone.
Rgba8 erase(const Rgba8& dst, double amount) {
    const double ad = static_cast<double>(dst.a) / 255.0;
    return Rgba8{dst.r, dst.g, dst.b, static_cast<std::uint8_t>(ad * (1.0 - amount) * 255.0 + 0.5)};
}

void test_restrict_matches_scaling_the_write() {
    const Extent size{9, 7};
    const Image backdrop = image_of(size, 7u);
    const Rgba8 src{210, 40, 90, 160};

    for (const std::uint8_t coverage : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{64},
                                        std::uint8_t{127}, std::uint8_t{128}, std::uint8_t{200},
                                        std::uint8_t{254}, std::uint8_t{255}}) {
        Mask mask(size);
        mask.fill(coverage);

        Image painted = backdrop;
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const std::uint8_t* from = backdrop.at(x, y);
                const Rgba8 got = over(src, 1.0,
                                       Rgba8{from[0], from[1], from[2], from[3]});
                std::uint8_t* to = painted.at(x, y);
                to[0] = got.r;
                to[1] = got.g;
                to[2] = got.b;
                to[3] = got.a;
            }
        }
        Image restricted = painted;
        restrict_to(mask, Rect{0, 0, size.w, size.h}, backdrop, Rect{0, 0, size.w, size.h},
                    restricted);

        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const std::uint8_t* from = backdrop.at(x, y);
                const std::uint8_t* got = restricted.at(x, y);
                const Rgba8 wanted =
                    over(src, static_cast<double>(coverage) / 255.0,
                         Rgba8{from[0], from[1], from[2], from[3]});
                const std::uint8_t expected[kChannels] = {wanted.r, wanted.g, wanted.b, wanted.a};
                // Rounding the move costs one LSB on the alpha channel and two on a colour channel,
                // the extra unit being the unpremultiply the reference does not pay. Anything past
                // that is a wrong formula, not slack - which is what the red-check of this test
                // (flip the coverage, or mix straight-alpha) demonstrates. Both ends are exact
                // bytes whatever the channel, which is what a rectangular selection is made of.
                for (int channel = 0; channel < kChannels; ++channel) {
                    const int gap = std::abs(static_cast<int>(got[channel]) -
                                             static_cast<int>(expected[channel]));
                    CHECK(gap <= (channel == 3 ? 1 : 2));
                    if (coverage == 0 || coverage == 255) {
                        const std::uint8_t* exact = coverage == 0 ? from : painted.at(x, y);
                        CHECK_EQ(got[channel], exact[channel]);
                    }
                }
            }
        }
    }
}

void test_restrict_matches_scaling_the_erase() {
    const Extent size{8, 5};
    const Image backdrop = image_of(size, 11u);

    for (const std::uint8_t coverage : {std::uint8_t{0}, std::uint8_t{64}, std::uint8_t{255}}) {
        Mask mask(size);
        mask.fill(coverage);
        Image erased = backdrop;
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const std::uint8_t* from = backdrop.at(x, y);
                const Rgba8 gone = erase(Rgba8{from[0], from[1], from[2], from[3]}, 0.7);
                std::uint8_t* to = erased.at(x, y);
                to[0] = gone.r;
                to[1] = gone.g;
                to[2] = gone.b;
                to[3] = gone.a;
            }
        }
        Image restricted = erased;
        restrict_to(mask, Rect{0, 0, size.w, size.h}, backdrop, Rect{0, 0, size.w, size.h},
                    restricted);
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                const std::uint8_t* from = backdrop.at(x, y);
                const std::uint8_t* got = restricted.at(x, y);
                const Rgba8 wanted =
                    erase(Rgba8{from[0], from[1], from[2], from[3]},
                          0.7 * static_cast<double>(coverage) / 255.0);
                CHECK(std::abs(static_cast<int>(got[3]) - static_cast<int>(wanted.a)) <= 1);
                // Erasing never touches the colour, with a mask on it or without one.
                for (int channel = 0; channel < 3; ++channel) {
                    CHECK_EQ(got[channel], from[channel]);
                }
            }
        }
    }
}

void test_restrict_never_moves_a_pixel_further_than_the_write() {
    const int ends[] = {0, 1, 17, 127, 128, 200, 255};
    const int coverages[] = {0, 1, 64, 127, 128, 192, 254, 255};
    const Extent size{static_cast<int>(sizeof(ends) / sizeof(int)),
                      static_cast<int>(sizeof(coverages) / sizeof(int))};
    Image before = Image::filled(size, 0, 0, 0, 0);
    Image after = Image::filled(size, 0, 0, 0, 0);
    for (int row = 0; row < size.h; ++row) {
        for (int col = 0; col < size.w; ++col) {
            for (int channel = 0; channel < kChannels; ++channel) {
                before.at(col, row)[channel] = static_cast<std::uint8_t>(ends[col]);
                after.at(col, row)[channel] = static_cast<std::uint8_t>(ends[(col + 3) % size.w]);
            }
        }
    }
    for (int row = 0; row < size.h; ++row) {
        const std::uint8_t coverage = static_cast<std::uint8_t>(coverages[row]);
        Mask mask(size);
        mask.fill(coverage);
        Image got = after;
        restrict_to(mask, Rect{0, 0, size.w, size.h}, before, Rect{0, 0, size.w, size.h}, got);
        for (int col = 0; col < size.w; ++col) {
            for (int channel = 0; channel < kChannels; ++channel) {
                const int from = before.at(col, row)[channel];
                const int to = after.at(col, row)[channel];
                const int value = got.at(col, row)[channel];
                const int low = from < to ? from : to;
                const int high = from < to ? to : from;
                CHECK(value >= low && value <= high);
                CHECK(std::abs(value - from) <= std::abs(to - from));
                if (coverage == 0) {
                    CHECK_EQ(value, from);
                }
                if (coverage == 255) {
                    CHECK_EQ(value, to);
                }
            }
        }
    }
}

void test_a_mask_only_restricts_its_own_region() {
    const Extent size{12, 6};
    const Image before = image_of(size, 3u);
    const Image written = image_of(size, 5u);
    Image after = written;

    Mask mask(size);
    mask.op(Rect{2, 1, 4, 3}, MaskOp::replace);
    restrict_to(mask, Rect{0, 0, size.w, size.h}, before, Rect{0, 0, size.w, size.h}, after);

    // Everything the mask does not claim is byte-identical to the original, whole image, and
    // everything it claims is what the write put there. This is the pixel-lock the document door
    // relies on, checked where it is actually implemented.
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const bool inside = mask.coverage(x, y) == 255;
            for (int channel = 0; channel < kChannels; ++channel) {
                const int expected = inside ? written.at(x, y)[channel] : before.at(x, y)[channel];
                CHECK_EQ(static_cast<int>(after.at(x, y)[channel]), expected);
            }
        }
    }
}

void test_mask_ops() {
    const Extent size{10, 4};
    Mask mask(size);
    mask.op(Rect{1, 1, 3, 2}, MaskOp::replace);
    CHECK_EQ(mask.covered(), 6u);
    CHECK_EQ(mask.coverage(1, 1), 255);
    CHECK_EQ(mask.coverage(3, 2), 255);
    CHECK_EQ(mask.coverage(4, 1), 0);

    // A region past the canvas is not an error and must not be written anywhere.
    mask.op(Rect{8, 3, 9, 9}, MaskOp::replace);
    CHECK_EQ(mask.covered(), 8u);
    CHECK_EQ(mask.coverage(9, 3), 255);

    mask.op(Rect{8, 3, 9, 9}, MaskOp::add);
    CHECK_EQ(mask.covered(), 8u);

    mask.op(Rect{0, 0, 10, 4}, MaskOp::subtract);
    CHECK_EQ(mask.covered(), 0u);
    CHECK(mask.bounds().empty());

    // Inverting a partial mask once claims what it gave up and gives up what it claimed; twice is
    // the mask that started.
    const std::vector<std::uint8_t> seeded = plane_of(size, 91u);
    std::optional<Mask> copy = Mask::from_plane(size, seeded);
    CHECK(copy.has_value());
    Mask original = *copy;
    original.invert();
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            CHECK_EQ(static_cast<int>(original.coverage(x, y)),
                     255 - static_cast<int>(copy->coverage(x, y)));
        }
    }
    original.invert();
    CHECK(original == *copy);
}

void test_from_plane_refuses_a_plane_of_another_size() {
    CHECK(!Mask::from_plane(Extent{4, 4}, std::vector<std::uint8_t>(15, 255)).has_value());
    CHECK(!Mask::from_plane(Extent{4, 4}, std::vector<std::uint8_t>(17, 255)).has_value());
    CHECK(!Mask::from_plane(Extent{0, 4}, std::vector<std::uint8_t>()).has_value());
    CHECK(Mask::from_plane(Extent{4, 4}, std::vector<std::uint8_t>(16, 255)).has_value());

    // Coverage outside the mask is absence. Every pixel of this one is claimed, so a reader that
    // clamped to the nearest edge or wrapped would report coverage where the mask has none - and a
    // selection that ran off a resized canvas would let writes through past its own end.
    const std::optional<Mask> full = Mask::from_plane(Extent{4, 4}, std::vector<std::uint8_t>(16, 255));
    CHECK(full.has_value());
    CHECK_EQ(full->coverage(0, 0), 255);
    CHECK_EQ(full->coverage(4, 0), 0);
    CHECK_EQ(full->coverage(-1, 2), 0);
    CHECK_EQ(full->coverage(2, 4), 0);
    CHECK_EQ(full->coverage(2, -1), 0);
}

void test_a_write_past_the_mask_is_left_alone() {
    const Extent canvas{8, 2};
    const Image before = image_of(canvas, 17u);
    const Image written = image_of(canvas, 19u);
    Image after = written;
    // A mask covering half the canvas: the other half keeps the bytes the write found.
    const std::optional<Mask> half = Mask::from_plane(Extent{4, 2}, std::vector<std::uint8_t>(8, 255));
    CHECK(half.has_value());
    restrict_to(*half, Rect{0, 0, canvas.w, canvas.h}, before, Rect{0, 0, canvas.w, canvas.h}, after);
    for (int y = 0; y < canvas.h; ++y) {
        for (int x = 0; x < canvas.w; ++x) {
            for (int channel = 0; channel < kChannels; ++channel) {
                const int expected = x < 4 ? written.at(x, y)[channel] : before.at(x, y)[channel];
                CHECK_EQ(static_cast<int>(after.at(x, y)[channel]), expected);
            }
        }
    }
}

void test_bounds_are_tight() {
    const Extent size{16, 9};
    Mask mask(size);
    mask.op(Rect{2, 1, 3, 3}, MaskOp::replace);
    mask.op(Rect{9, 4, 4, 4}, MaskOp::replace);
    CHECK(rect_equal(mask.bounds(), Rect{2, 1, 11, 7}));

    // Tight: the first and last row and column of the box each hold covered pixels, or the box is
    // bigger than the mask and `bounds` has stopped meaning anything to the region compiler.
    const Rect box = mask.bounds();
    bool first_row = false;
    bool last_row = false;
    bool first_column = false;
    bool last_column = false;
    for (int x = box.x; x < box.right(); ++x) {
        first_row = first_row || mask.coverage(x, box.y) != 0;
        last_row = last_row || mask.coverage(x, box.bottom() - 1) != 0;
    }
    for (int y = box.y; y < box.bottom(); ++y) {
        first_column = first_column || mask.coverage(box.x, y) != 0;
        last_column = last_column || mask.coverage(box.right() - 1, y) != 0;
    }
    CHECK(first_row);
    CHECK(last_row);
    CHECK(first_column);
    CHECK(last_column);

    // Taking the covered column at the far edge off moves the bound in by exactly that.
    mask.op(Rect{12, 0, 1, 9}, MaskOp::subtract);
    CHECK(rect_equal(mask.bounds(), Rect{2, 1, 10, 7}));
}

void test_outline_is_the_covered_edge_and_nothing_else() {
    for (const Rect selection : {Rect{2, 1, 5, 4}, Rect{0, 0, 16, 9}, Rect{0, 0, 1, 1},
                                 Rect{15, 8, 1, 1}}) {
        const Extent size{16, 9};
        Mask mask(size);
        mask.op(selection, MaskOp::replace);
        const std::vector<MaskRun> runs = mask.outline();

        // The set the runs cover, rebuilt independently of how they were produced.
        std::vector<std::uint8_t> listed(
            static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h), 0);
        for (const MaskRun& run : runs) {
            CHECK(run.y >= 0 && run.y < size.h);
            CHECK(run.x0 < run.x1);
            for (int x = run.x0; x < run.x1; ++x) {
                CHECK(mask.coverage(x, run.y) != 0);
                listed[cell(size, x, run.y)] = 1;
            }
        }
        // Sorted, and maximal: two runs on one row must have a gap between them, or the outline
        // broke one line into two and a dashed overlay would show it.
        int last_y = -1;
        int last_x1 = -1;
        for (const MaskRun& run : runs) {
            CHECK(run.y > last_y || (run.y == last_y && run.x0 > last_x1 + 1));
            last_y = run.y;
            last_x1 = run.x1;
        }
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                if (mask.coverage(x, y) == 0) {
                    CHECK_EQ(static_cast<int>(listed[cell(size, x, y)]), 0);
                    continue;
                }
                const bool touches_out = mask.coverage(x - 1, y) == 0 || mask.coverage(x + 1, y) == 0 ||
                                         mask.coverage(x, y - 1) == 0 || mask.coverage(x, y + 1) == 0;
                CHECK_EQ(static_cast<int>(listed[cell(size, x, y)]), touches_out ? 1 : 0);
            }
        }
    }
}

// The facts a mask reports - how much it covers, where, and its outline - are held by the mask and
// dropped by every write. A cache that outlived the coverage would show the ants in the old place
// after the selection moved, and the region compiler would work to the old bounds.
void test_the_facts_follow_every_write() {
    const Extent size{8, 6};
    Mask mask(size);
    mask.op(Rect{1, 1, 3, 2}, MaskOp::replace);
    CHECK_EQ(mask.covered(), 6u);
    CHECK(mask.bounds() == Rect{1, 1, 3, 2});
    CHECK(!mask.outline().empty());
    // Reading them twice hands back the same computed values, which is what a per-frame overlay needs.
    CHECK(&mask.facts() == &mask.facts());

    mask.op(Rect{1, 1, 3, 2}, MaskOp::subtract);
    CHECK_EQ(mask.covered(), 0u);
    CHECK(mask.bounds().empty());
    CHECK(mask.outline().empty());

    mask.fill(255);
    CHECK_EQ(mask.covered(), 48u);
    CHECK(mask.bounds() == Rect{0, 0, 8, 6});
    mask.invert();
    CHECK_EQ(mask.covered(), 0u);
    CHECK(mask.outline().empty());
    mask.invert();
    CHECK_EQ(mask.covered(), 48u);
}

}  // namespace

int main() {
    test_restrict_matches_scaling_the_write();
    test_restrict_matches_scaling_the_erase();
    test_restrict_never_moves_a_pixel_further_than_the_write();
    test_a_mask_only_restricts_its_own_region();
    test_mask_ops();
    test_from_plane_refuses_a_plane_of_another_size();
    test_a_write_past_the_mask_is_left_alone();
    test_bounds_are_tight();
    test_outline_is_the_covered_edge_and_nothing_else();
    test_the_facts_follow_every_write();
    return noise::test::summary("mask");
}
