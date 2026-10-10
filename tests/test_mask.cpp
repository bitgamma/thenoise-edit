// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/image.h"
#include "core/mask.h"

#include <algorithm>
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

// The boundary as edges, checked against the plane rebuilt pixel by pixel: every edge between a
// covered pixel and an uncovered one - out-of-bounds uncovered - is in exactly one run, on the side
// it faces, and nothing else is.
void test_the_boundary_is_every_edge_between_covered_and_uncovered_once() {
    // Two rectangles with a gap between them as well as single ones: on a line that holds two
    // separate stretches of boundary, a run that ran on through the gap would list pixels that are
    // not edges at all, and a fixture of single rectangles would let it pass.
    const std::vector<std::vector<Rect>> shapes{
        {Rect{2, 1, 5, 4}}, {Rect{0, 0, 16, 9}}, {Rect{0, 0, 1, 1}}, {Rect{15, 8, 1, 1}},
        {Rect{2, 2, 3, 3}, Rect{8, 2, 3, 3}}, {Rect{1, 1, 2, 6}, Rect{6, 3, 8, 2}}};
    for (const std::vector<Rect>& shape : shapes) {
        const Extent size{16, 9};
        Mask mask(size);
        for (std::size_t i = 0; i < shape.size(); ++i) {
            mask.op(shape[i], i == 0 ? MaskOp::replace : MaskOp::add);
        }

        // Counted per (pixel, side), which is what an edge is. A boundary *pixel* set cannot express
        // this: it would say a corner is one thing when it is two edges meeting.
        int listed_edges = 0;
        for (const MaskRun& run : mask.boundary()) {
            const bool horizontal = run.side == MaskSide::up || run.side == MaskSide::down;
            CHECK(run.fixed >= 0 && run.fixed < (horizontal ? size.h : size.w));
            CHECK(run.from < run.to);
            for (int cell = run.from; cell < run.to; ++cell) {
                const int x = horizontal ? cell : run.fixed;
                const int y = horizontal ? run.fixed : cell;
                const int dx = run.side == MaskSide::left ? -1 : run.side == MaskSide::right ? 1 : 0;
                const int dy = run.side == MaskSide::up ? -1 : run.side == MaskSide::down ? 1 : 0;
                CHECK(mask.coverage(x, y) != 0);
                CHECK(mask.coverage(x + dx, y + dy) == 0);
                ++listed_edges;
            }
        }
        int actual_edges = 0;
        for (int y = 0; y < size.h; ++y) {
            for (int x = 0; x < size.w; ++x) {
                if (mask.coverage(x, y) == 0) {
                    continue;
                }
                for (const MaskSide side : kMaskSides) {
                    const int dx = side == MaskSide::left ? -1 : side == MaskSide::right ? 1 : 0;
                    const int dy = side == MaskSide::up ? -1 : side == MaskSide::down ? 1 : 0;
                    actual_edges += mask.coverage(x + dx, y + dy) == 0 ? 1 : 0;
                }
            }
        }
        CHECK_EQ(listed_edges, actual_edges);
    }
}

// Runs are sorted and maximal on their own line: two runs on one line with no gap between them mean
// one straight edge was broken in two, and a dashed overlay shows the break as a mark that is not on
// the selection.
void test_boundary_runs_are_sorted_and_maximal() {
    const std::vector<std::vector<Rect>> shapes{{Rect{2, 1, 5, 4}}, {Rect{0, 0, 16, 9}},
                                                {Rect{7, 0, 1, 9}},
                                                // Two blocks on one row: the line has to break into
                                                // two runs, and merging them across the gap is the
                                                // failure this checks for.
                                                {Rect{2, 2, 3, 3}, Rect{8, 2, 3, 3}},
                                                {Rect{1, 1, 2, 6}, Rect{6, 3, 8, 2}}};
    for (const std::vector<Rect>& shape : shapes) {
        const Extent size{16, 9};
        Mask mask(size);
        for (std::size_t i = 0; i < shape.size(); ++i) {
            mask.op(shape[i], i == 0 ? MaskOp::replace : MaskOp::add);
        }
        MaskSide last_side = MaskSide::up;
        int last_fixed = -1;
        int last_to = -1;
        for (const MaskRun& run : mask.boundary()) {
            const bool same_line = run.side == last_side && run.fixed == last_fixed;
            // A new line is allowed anywhere; a second run on the same line has to have a gap
            // between the two, or one straight edge was reported as two.
            CHECK(!same_line || run.from > last_to + 1);
            last_side = run.side;
            last_fixed = run.fixed;
            last_to = run.to;
        }
        // And maximal in the other direction too: walking the plane again, every edge on a line is
        // in *one* run, so a run that stopped short of a neighbour it could have joined - which
        // leaves a hairline gap in the outline - is caught here rather than on screen.
        for (const MaskSide side : kMaskSides) {
            const bool horizontal = side == MaskSide::up || side == MaskSide::down;
            const int dx = side == MaskSide::left ? -1 : side == MaskSide::right ? 1 : 0;
            const int dy = side == MaskSide::up ? -1 : side == MaskSide::down ? 1 : 0;
            const int lines = horizontal ? size.h : size.w;
            const int cells = horizontal ? size.w : size.h;
            for (int fixed = 0; fixed < lines; ++fixed) {
                int expected = -1;  // end of the stretch of edges being walked
                for (int cell = 0; cell < cells; ++cell) {
                    const int x = horizontal ? cell : fixed;
                    const int y = horizontal ? fixed : cell;
                    const bool edge =
                        mask.coverage(x, y) != 0 && mask.coverage(x + dx, y + dy) == 0;
                    if (!edge) {
                        expected = -1;
                        continue;
                    }
                    if (expected < 0) {
                        // A fresh stretch: exactly one run starts here and ends where the stretch
                        // does, which the inner walk finds.
                        int end = cell;
                        while (end < cells) {
                            const int ex = horizontal ? end : fixed;
                            const int ey = horizontal ? fixed : end;
                            if (!(mask.coverage(ex, ey) != 0 &&
                                  mask.coverage(ex + dx, ey + dy) == 0)) {
                                break;
                            }
                            ++end;
                        }
                        const MaskRun wanted{side, fixed, cell, end};
                        CHECK(std::find(mask.boundary().begin(), mask.boundary().end(), wanted) !=
                              mask.boundary().end());
                        expected = end;
                    }
                }
            }
        }
    }
}

// The case a row grouping cannot express on its own: the vertical sides of a rectangle are runs
// along y, one per side, as long as the rectangle is tall. Report the boundary as covered pixels
// grouped by rows and those sides vanish - which is exactly how the ants came to be drawn as a
// blinking frame, the pixels of one vertical edge stroked across it once per row.
void test_a_rectangles_vertical_sides_are_runs_of_their_own() {
    const Extent size{16, 9};
    Mask mask(size);
    mask.op(Rect{4, 2, 6, 5}, MaskOp::replace);

    for (const MaskRun& wanted : {MaskRun{MaskSide::up, 2, 4, 10}, MaskRun{MaskSide::down, 6, 4, 10},
                                  MaskRun{MaskSide::left, 4, 2, 7},
                                  MaskRun{MaskSide::right, 9, 2, 7}}) {
        CHECK(std::find(mask.boundary().begin(), mask.boundary().end(), wanted) !=
              mask.boundary().end());
    }
    // Four runs, and that is the whole boundary: no edge of the rectangle is reported twice, which is
    // what an overlay stroking each run along its length depends on.
    CHECK_EQ(mask.boundary().size(), 4u);

    mask.invert();
    // Inverted, the boundary is the canvas frame and the ring around the hole. The covered region is
    // now everywhere but the rectangle, so the hole's edge is the ring of pixels just outside it:
    // the up side of that ring is row 1 (the pixel above it, row 0, is covered; row 2 is the hole),
    // and so on - all four sides, so an overlay dashes a hole instead of drawing its sides solid.
    const std::vector<MaskRun> canvas_frame{
        MaskRun{MaskSide::up, 0, 0, 16}, MaskRun{MaskSide::down, 8, 0, 16},
        MaskRun{MaskSide::left, 0, 0, 9}, MaskRun{MaskSide::right, 15, 0, 9}};
    for (const MaskRun& wanted : canvas_frame) {
        CHECK(std::find(mask.boundary().begin(), mask.boundary().end(), wanted) !=
              mask.boundary().end());
    }
    // The hole's own four sides. Each faces the hole from the covered ring around it: the pixel above
    // the hole's top row is row 1 and faces *down* into it, the pixel left of its left column is
    // column 3 and faces *right*, and so on. Getting this backwards puts the ants on the far side of
    // the hole, one pixel out.
    const std::vector<MaskRun> hole{MaskRun{MaskSide::down, 1, 4, 10},
                                    MaskRun{MaskSide::up, 7, 4, 10},
                                    MaskRun{MaskSide::right, 3, 2, 7},
                                    MaskRun{MaskSide::left, 10, 2, 7}};
    for (const MaskRun& wanted : hole) {
        CHECK(std::find(mask.boundary().begin(), mask.boundary().end(), wanted) !=
              mask.boundary().end());
    }
    // Nothing else: the frame and the hole are the whole boundary of an inverted rectangle.
    CHECK_EQ(mask.boundary().size(), canvas_frame.size() + hole.size());
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
    CHECK(!mask.boundary().empty());
    // Reading them twice hands back the same computed values, which is what a per-frame overlay needs.
    CHECK(&mask.facts() == &mask.facts());

    mask.op(Rect{1, 1, 3, 2}, MaskOp::subtract);
    CHECK_EQ(mask.covered(), 0u);
    CHECK(mask.bounds().empty());
    CHECK(mask.boundary().empty());

    mask.fill(255);
    CHECK_EQ(mask.covered(), 48u);
    CHECK(mask.bounds() == Rect{0, 0, 8, 6});
    mask.invert();
    CHECK_EQ(mask.covered(), 0u);
    CHECK(mask.boundary().empty());
    mask.invert();
    CHECK_EQ(mask.covered(), 48u);
}

// A layer seen through a mask, checked against arithmetic written out again here rather than
// against `scale_by_coverage`: the layer's own premultiplied amount, scaled by coverage / 255.
void test_a_layer_through_a_mask_keeps_its_colour_and_its_amount() {
    const Extent size{9, 7};
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.w) *
                                     static_cast<std::size_t>(size.h) * kChannels);
    const std::uint8_t alphas[]{0, 1, 17, 128, 254, 255};
    const std::uint8_t hues[]{0, 33, 128, 211, 255};
    std::size_t cell = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::size_t index = static_cast<std::size_t>(x) * 3 +
                                      static_cast<std::size_t>(y) * 5;
            const std::uint8_t alpha = alphas[index % (sizeof(alphas) / sizeof(alphas[0]))];
            for (int channel = 0; channel < 3; ++channel) {
                pixels[cell++] = hues[(static_cast<std::size_t>(x) + static_cast<std::size_t>(y) +
                                       static_cast<std::size_t>(channel)) %
                                      (sizeof(hues) / sizeof(hues[0]))];
            }
            pixels[cell++] = alpha;
        }
    }
    const Image layer = Image::from_rgba(size, std::move(pixels));
    const std::optional<Mask> mask = Mask::from_plane(size, plane_of(size, 7));
    CHECK(mask.has_value());

    Image shown;
    scale_by_coverage(*mask, Rect{0, 0, size.w, size.h}, layer, shown);
    CHECK_EQ(shown.size().w, size.w);
    CHECK_EQ(shown.size().h, size.h);

    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t* from = layer.at(x, y);
            const std::uint8_t* got = shown.at(x, y);
            const double coverage = mask->coverage(x, y) / 255.0;
            const double amount = from[3] / 255.0 * coverage;
            if (from[3] == 255) {
                // An opaque pixel is the one case where masking has an exact answer: the amount is
                // the coverage and the colour cannot move, because colour and amount scale by the
                // same factor. A masking that mixed straight alpha would fail exactly here.
                CHECK_EQ(got[3], mask->coverage(x, y));
                for (int channel = 0; channel < 3; ++channel) {
                    CHECK_EQ(got[channel], from[channel]);
                }
                continue;
            }
            if (mask->coverage(x, y) == 255) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    CHECK_EQ(got[channel], from[channel]);
                }
                continue;
            }
            if (mask->coverage(x, y) == 0 || from[3] == 0) {
                for (int channel = 0; channel < kChannels; ++channel) {
                    CHECK_EQ(got[channel], 0);
                }
                continue;
            }
            CHECK(std::abs(static_cast<int>(got[3]) - static_cast<int>(std::lround(amount * 255))) <=
                  1);
            const double expected_alpha = got[3] > 0 ? got[3] : 1;
            for (int channel = 0; channel < 3; ++channel) {
                const double wanted_premul = from[channel] / 255.0 * (from[3] / 255.0) * coverage;
                const double got_premul = got[channel] / 255.0 * (expected_alpha / 255.0);
                CHECK(std::abs(wanted_premul - got_premul) * 255.0 <= 2.0);
            }
        }
    }
}

// Coverage outside the mask is 0, and a reused scratch buffer must not remember the last layer that
// was scaled through it.
void test_a_mask_hides_what_it_does_not_cover_and_leaves_nothing_behind() {
    const Extent size{6, 5};
    Image layer = Image::filled(size, 200, 100, 50, 255);
    Mask mask(size);
    mask.op(Rect{1, 1, 2, 2}, MaskOp::replace);

    Image shown;
    scale_by_coverage(mask, Rect{0, 0, size.w, size.h}, layer, shown);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
            if (inside) {
                CHECK_EQ(shown.at(x, y)[3], 255);
            } else {
                CHECK_EQ(shown.at(x, y)[3], 0);
            }
        }
    }

    // Same buffer, mask the other way round: every pixel of the region is written again, so a
    // caller that blends the region cannot see a pixel from the previous pass.
    mask.invert();
    scale_by_coverage(mask, Rect{0, 0, size.w, size.h}, layer, shown);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
            CHECK_EQ(shown.at(x, y)[3], inside ? 0 : 255);
        }
    }

    // A mask with no pixels scales nothing, which is what makes "no mask" cheap rather than a
    // special case in the caller.
    Image untouched;
    scale_by_coverage(Mask{}, Rect{0, 0, size.w, size.h}, layer, untouched);
    CHECK(untouched.empty() || untouched.checksum_hex() == layer.checksum_hex());
}

// Coverage ink travels towards one end of the scale rather than maxing against it: two coats at half
// strength come to three quarters, which is what the same brush does to colour and what a union of two
// claims would not. The ends are exact, and no ink means no move.
void test_ink_lays_coverage_in_coats() {
    Mask mask(Extent{2, 1});
    mask.fill(kMaskOut);

    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, 128), MaskInk::reveal));
    CHECK_NEAR(static_cast<int>(mask.coverage(0, 0)), 128, 1);
    CHECK_EQ(mask.coverage(1, 0), mask.coverage(0, 0));
    const int first = mask.coverage(0, 0);
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, 128), MaskInk::reveal));
    const int second = mask.coverage(0, 0);
    CHECK_NEAR(second, first + (255 - first) * 128 / 255, 1);
    CHECK(second > first);

    // A whole ink is exact at the far end, and no ink is exact where it started.
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, kMaskIn), MaskInk::reveal));
    CHECK_EQ(static_cast<int>(mask.coverage(0, 0)), kMaskIn);
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, kMaskOut), MaskInk::reveal));
    CHECK_EQ(static_cast<int>(mask.coverage(0, 0)), kMaskIn);

    // Hiding travels the other way, coat by coat, and is exact at both ends.
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, 128), MaskInk::hide));
    CHECK_NEAR(static_cast<int>(mask.coverage(0, 0)), 127, 1);
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, kMaskIn), MaskInk::hide));
    CHECK_EQ(static_cast<int>(mask.coverage(0, 0)), kMaskOut);
    CHECK(mask.paint(Rect{0, 0, 2, 1}, std::vector<std::uint8_t>(2, kMaskIn), MaskInk::hide));
    CHECK_EQ(static_cast<int>(mask.coverage(0, 0)), kMaskOut);

    // The derived facts followed the ink, in both directions.
    CHECK_NEAR(mask.area(), 0.0, 1e-9);
    CHECK(mask.paint(Rect{0, 0, 1, 1}, std::vector<std::uint8_t>(1, kMaskIn), MaskInk::reveal));
    CHECK_NEAR(mask.area(), 1.0, 1e-9);
    CHECK(mask.bounds() == Rect{0, 0, 1, 1});
}

// A patch that does not describe the rectangle it is offered for writes nothing at all: half a stroke
// landing on the plane would be a coverage nobody painted.
void test_ink_of_another_length_is_refused() {
    Mask mask(Extent{4, 4});
    mask.fill(kMaskIn);
    CHECK(!mask.paint(Rect{1, 1, 2, 2}, std::vector<std::uint8_t>(3, kMaskOut), MaskInk::hide));
    CHECK(!mask.paint(Rect{1, 1, 2, 2}, std::vector<std::uint8_t>(5, kMaskOut), MaskInk::hide));
    CHECK_EQ(static_cast<int>(mask.coverage(1, 1)), kMaskIn);
    // An empty rectangle with an empty patch is the nothing the caller asked for, not an error.
    CHECK(mask.paint(Rect{1, 1, 0, 0}, std::vector<std::uint8_t>{}, MaskInk::hide));
    // Past the plane there is no cell to write, and that is not an error either.
    CHECK(mask.paint(Rect{10, 10, 2, 2}, std::vector<std::uint8_t>(4, kMaskOut), MaskInk::hide));
    CHECK_EQ(static_cast<int>(mask.coverage(0, 0)), kMaskIn);
}

// The selection fences a coverage write the same way it fences a colour one: exact at the ends of the
// scale, and within one step of the arithmetic `coverage / 255` of the way.
void test_a_selection_reaches_a_coverage_write_as_far_as_it_covers() {
    const Extent size{6, 3};
    std::vector<std::uint8_t> below;
    std::vector<std::uint8_t> written;
    std::vector<std::uint8_t> selection;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const std::uint8_t start = static_cast<std::uint8_t>(30 + x * 30);
            below.push_back(start);
            written.push_back(x < 3 ? kMaskIn : kMaskOut);  // a write that both reveals and hides
            selection.push_back(x == 0   ? kMaskOut
                                  : (x == 5 ? kMaskIn
                                            : static_cast<std::uint8_t>(40 + x * 35)));
        }
    }
    const Mask before = Mask::from_plane(size, below).value();
    const Mask after = Mask::from_plane(size, written).value();
    const Mask selection_mask = Mask::from_plane(size, selection).value();
    Mask result = after;
    result.restrict_to(selection_mask, Rect{0, 0, size.w, size.h}, before,
                       Rect{0, 0, size.w, size.h});

    for (int x = 0; x < size.w; ++x) {
        for (int y = 0; y < size.h; ++y) {
            const int had = before.coverage(x, y);
            const int wrote = after.coverage(x, y);
            const int reach = selection_mask.coverage(x, y);
            const int want = reach == kMaskIn   ? wrote
                             : (reach == kMaskOut ? had
                                                  : had + (wrote - had) * reach / 255);
            if (reach == kMaskIn || reach == kMaskOut) {
                CHECK_EQ(static_cast<int>(result.coverage(x, y)), want);
            } else {
                CHECK_NEAR(static_cast<int>(result.coverage(x, y)), want, 1);
            }
            // The rule that matters more than the rounding: a partial selection never carries a pixel
            // further than the write itself went.
            CHECK(result.coverage(x, y) <= std::max(had, wrote));
            CHECK(result.coverage(x, y) >= std::min(had, wrote));
        }
    }
    // No selection size means no claim, so the write stands whole.
    Mask whole = after;
    whole.restrict_to(Mask{}, Rect{0, 0, size.w, size.h}, before, Rect{0, 0, size.w, size.h});
    CHECK(whole == after);
}

// A snapshot of a region is the same coverage, addressed from its own corner, and the rows are the
// plane's own bytes - which is what lets a gesture's held-aside copy be moved without a re-scan.
void test_a_region_is_the_same_coverage_indexed_from_zero() {
    const Extent size{7, 5};
    Mask mask = Mask::from_plane(size, plane_of(size, 7u)).value();
    const Rect area{2, 1, 4, 3};
    const Mask cut = mask.region(area);
    CHECK_EQ(cut.size().w, area.w);
    CHECK_EQ(cut.size().h, area.h);
    for (int y = 0; y < area.h; ++y) {
        CHECK(std::equal(cut.row(y).begin(), cut.row(y).end(), mask.row(area.y + y).begin() + area.x));
    }
    double want_area = 0.0;
    std::size_t covered = 0;
    for (int y = 0; y < area.h; ++y) {
        for (int x = 0; x < area.w; ++x) {
            covered += cut.coverage(x, y) > 0 ? 1u : 0u;
            want_area += static_cast<double>(cut.coverage(x, y)) / 255.0;
            CHECK_EQ(static_cast<int>(cut.coverage(x, y)),
                     static_cast<int>(mask.coverage(area.x + x, area.y + y)));
        }
    }
    CHECK_EQ(cut.covered(), covered);
    CHECK_NEAR(cut.area(), want_area, 1e-9);

    // A region reaching outside the plane is no plane at all, rather than a padded one.
    CHECK_EQ(mask.region(Rect{5, 5, 4, 4}).size().w, 0);
    CHECK_EQ(mask.region(Rect{5, 5, 4, 4}).size().h, 0);
}

}  // namespace

int main() {
    test_restrict_matches_scaling_the_write();
    test_restrict_matches_scaling_the_erase();
    test_restrict_never_moves_a_pixel_further_than_the_write();
    test_a_mask_only_restricts_its_own_region();
    test_mask_ops();
    test_ink_lays_coverage_in_coats();
    test_ink_of_another_length_is_refused();
    test_a_selection_reaches_a_coverage_write_as_far_as_it_covers();
    test_a_region_is_the_same_coverage_indexed_from_zero();
    test_from_plane_refuses_a_plane_of_another_size();
    test_a_write_past_the_mask_is_left_alone();
    test_bounds_are_tight();
    test_the_boundary_is_every_edge_between_covered_and_uncovered_once();
    test_boundary_runs_are_sorted_and_maximal();
    test_a_rectangles_vertical_sides_are_runs_of_their_own();
    test_the_facts_follow_every_write();
    test_a_layer_through_a_mask_keeps_its_colour_and_its_amount();
    test_a_mask_hides_what_it_does_not_cover_and_leaves_nothing_behind();
    return noise::test::summary("mask");
}
