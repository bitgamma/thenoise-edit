// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/brush.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace noise;

namespace {

Brush make_brush(double size, double hardness, double opacity, double flow, double spacing) {
    return Brush{size, hardness, opacity, flow, spacing};
}

// The brush the tests stamp with: a 6 px circle at the centre of an 8x8 tile.
const Brush kTile = {6.0, 1.0, 1.0, 1.0, 0.5};

int alpha_centre(const Image& image) { return image.at(image.size().w / 2, image.size().h / 2)[3]; }

int channel_sum(const Image& image) {
    int sum = 0;
    for (const std::uint8_t byte : image.pixels()) {
        sum += byte;
    }
    return sum;
}

void test_dabs_are_spaced_from_the_brush_size() {
    const Brush brush = make_brush(10.0, 1.0, 1.0, 1.0, 0.5);  // 5 px between dab centres
    const std::vector<Vec2> straight = stroke_dabs({{0.0, 0.0}, {10.0, 0.0}}, brush);
    CHECK_EQ(straight.size(), 3u);
    if (straight.size() == 3) {
        CHECK_NEAR(straight[1].x, 5.0, 1e-9);
        CHECK_NEAR(straight[2].x, 10.0, 1e-9);
    }

    // Spacing carries across a corner rather than restarting on each segment, so the dab
    // density along a path is a property of the path, not of how it was sampled.
    const std::vector<Vec2> corner = stroke_dabs({{0.0, 0.0}, {8.0, 0.0}, {8.0, 8.0}}, brush);
    CHECK_EQ(corner.size(), 4u);
    CHECK_EQ(stroke_dabs({{0.0, 0.0}}, brush).size(), 1u);
    CHECK(stroke_dabs({}, brush).empty());

    const std::vector<Vec2> long_path =
        stroke_dabs({{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {13.0, 10.0}}, brush);
    CHECK_EQ(long_path.size(), 5u);  // 1 + floor(23 / 5)

    // A spacing of 0 still cannot place dabs closer than one pixel: the cost of a stroke is
    // bounded by its length in pixels, not by how finely the pointer was sampled.
    const std::vector<Vec2> dense = stroke_dabs({{0.0, 0.0}, {10.0, 0.0}},
                                                make_brush(10.0, 1.0, 1.0, 1.0, 0.0));
    CHECK_EQ(dense.size(), 11u);
}

void test_coverage_is_a_round_ramped_stamp() {
    // Distances are document pixels from the dab centre; the radius is 10.
    CHECK_NEAR(stamp_coverage(0.0, 0.5, 10.0), 1.0, 1e-12);
    CHECK_EQ(stamp_coverage(10.5, 0.5, 10.0), 0.0);          // past the rim band
    CHECK_NEAR(stamp_coverage(9.0, 1.0, 10.0), 1.0, 1e-12);  // a hard brush holds to the rim
    CHECK_NEAR(stamp_coverage(10.0, 1.0, 10.0), 0.5, 1e-12);  // on the rim it is half in
    // Half way from the centre to the outer edge of the rim band, a fully soft brush is at
    // half strength. The tolerance is the smoothstep's own asymmetry about that point.
    CHECK_NEAR(stamp_coverage(5.0, 0.0, 10.0), 0.5, 0.05);

    // The smallest brush in the UI still paints its own pixel, and bleeds a little into the
    // ones it clips.
    CHECK_NEAR(stamp_coverage(0.0, 1.0, 0.5), 1.0, 1e-12);
    CHECK(stamp_coverage(0.707, 1.0, 0.5) > 0.0);

    for (const double hardness : {0.0, 0.25, 0.75, 1.0}) {
        double previous = 1.0;
        for (int step = 1; step <= 200; ++step) {
            const double distance = step / 20.0;
            const double coverage = stamp_coverage(distance, hardness, 8.0);
            CHECK(coverage <= previous + 1e-12);
            CHECK(coverage >= 0.0 && coverage <= 1.0);
            previous = coverage;
        }
    }
}

void test_a_stroke_keeps_its_colour_and_only_spreads_where_the_stamp_reaches() {
    Image canvas = Image::filled(Extent{8, 8}, 0, 0, 0, 0);
    const Rgba8 red{255, 128, 0, 255};
    const std::optional<Rect> touched =
        paint_stroke(canvas, {{4.0, 4.0}}, make_brush(6.0, 1.0, 1.0, 1.0, 0.5), red,
                     PaintMode::Over);
    CHECK(touched.has_value());
    if (!touched) {
        return;
    }
    // Source-over of one flat colour over transparency returns that colour exactly, at any
    // coverage, so every painted pixel must carry the brush colour and nothing else.
    for (int y = 0; y < canvas.size().h; ++y) {
        for (int x = 0; x < canvas.size().w; ++x) {
            const std::uint8_t* pixel = canvas.at(x, y);
            const bool inside = touched->contains(x, y) && pixel[3] > 0;
            if (!inside) {
                CHECK_EQ(pixel[0] + pixel[1] + pixel[2] + pixel[3], 0);
                continue;
            }
            CHECK_EQ(static_cast<int>(pixel[0]), 255);
            CHECK_EQ(static_cast<int>(pixel[1]), 128);
            CHECK_EQ(static_cast<int>(pixel[2]), 0);
        }
    }
    CHECK_EQ(alpha_centre(canvas), 255);
    CHECK_EQ(canvas.at(1, 1)[3], 0);   // hypot(3.5,3.5) is past the 3 px radius
    // The stamp is symmetric, and the rim is feathered rather than staircased.
    // The stamp is symmetric about the dab, and its rim is feathered, not staircased.
    CHECK_EQ(canvas.at(2, 4)[3], canvas.at(5, 4)[3]);
    CHECK_EQ(canvas.at(4, 2)[3], canvas.at(4, 5)[3]);
    bool feathered = false;
    for (const std::uint8_t byte : canvas.pixels()) {
        feathered = feathered || byte > 0;
    }
    CHECK(feathered);
    bool partial = false;
    for (int y = 0; y < canvas.size().h; ++y) {
        for (int x = 0; x < canvas.size().w; ++x) {
            partial = partial || (canvas.at(x, y)[3] > 0 && canvas.at(x, y)[3] < 255);
        }
    }
    CHECK(partial);
}

void test_the_blend_matches_the_reference_arithmetic() {
    Image canvas = Image::filled(Extent{8, 8}, 0, 0, 255, 255);
    const Rgba8 red{255, 0, 0, 255};
    // Centre pixel of a hard brush at half opacity over an opaque blue: src-over gives
    // (255*0.5 + 0*0.5) = 127.5 -> 128 red, 0 green, 128 blue, fully opaque.
    paint_stroke(canvas, {{4.0, 4.0}}, make_brush(8.0, 1.0, 0.5, 1.0, 0.5), red, PaintMode::Over);
    const std::uint8_t* centre = canvas.at(4, 4);
    CHECK_EQ(static_cast<int>(centre[0]), 128);
    CHECK_EQ(static_cast<int>(centre[1]), 0);
    CHECK_EQ(static_cast<int>(centre[2]), 128);
    CHECK_EQ(static_cast<int>(centre[3]), 255);

    // Two strokes at half flow stack as "over", not as a sum: 0.5 then 0.75, never 1.0.
    Image thin = Image::filled(Extent{8, 8}, 0, 0, 0, 0);
    const Brush flow_half = make_brush(4.0, 1.0, 1.0, 0.5, 0.5);
    paint_stroke(thin, {{4.0, 4.0}}, flow_half, red, PaintMode::Over);
    CHECK_NEAR(alpha_centre(thin), 128.0, 1.0);
    paint_stroke(thin, {{4.0, 4.0}}, flow_half, red, PaintMode::Over);
    CHECK_NEAR(alpha_centre(thin), 191.0, 1.0);
    for (int i = 0; i < 12; ++i) {
        paint_stroke(thin, {{4.0, 4.0}}, flow_half, red, PaintMode::Over);
    }
    CHECK_EQ(alpha_centre(thin), 255);

    // Erasing takes alpha and leaves the colour alone until the pixel is empty.
    Image erased = Image::filled(Extent{8, 8}, 0, 0, 255, 255);
    paint_stroke(erased, {{4.0, 4.0}}, make_brush(8.0, 1.0, 0.5, 1.0, 0.5), Rgba8{0, 0, 0, 255},
                 PaintMode::Erase);
    const std::uint8_t* half = erased.at(4, 4);
    CHECK_NEAR(half[3], 128.0, 1.0);
    CHECK_EQ(static_cast<int>(half[2]), 255);
    paint_stroke(erased, {{4.0, 4.0}}, make_brush(8.0, 1.0, 1.0, 1.0, 0.5), Rgba8{0, 0, 0, 255},
                 PaintMode::Erase);
    CHECK_EQ(static_cast<int>(erased.at(4, 4)[3]), 0);
    CHECK_EQ(static_cast<int>(erased.at(4, 4)[0] + erased.at(4, 4)[1] + erased.at(4, 4)[2]), 0);
}

void test_a_stroke_outside_the_document_changes_nothing() {
    Image canvas = Image::filled(Extent{4, 4}, 10, 20, 30, 255);
    const std::string before = canvas.checksum_hex();
    CHECK(!paint_stroke(canvas, {{40.0, 40.0}}, kTile, Rgba8{0, 0, 0, 255}, PaintMode::Over)
               .has_value());
    CHECK_EQ(canvas.checksum_hex(), before);
    CHECK(stroke_bounds({{40.0, 40.0}}, kTile, canvas.size()).empty());
}

// The document captures the pixels inside `stroke_bounds` before painting and puts them back
// on undo. If the two rectangles ever disagree, undo leaves paint behind - silently.
void test_stroke_bounds_is_exactly_the_painted_rectangle() {
    const std::vector<Vec2> paths[] = {
        {{12.0, 12.0}},
        {{0.0, 0.0}},
        {{31.0, 31.0}},
        {{-4.0, 6.0}, {36.0, 6.0}},
        {{2.0, 2.0}, {30.0, 30.0}, {4.0, 28.0}},
    };
    for (const std::vector<Vec2>& path : paths) {
        for (const double size : {1.0, 7.0, 24.0}) {
            const Brush brush = make_brush(size, 0.6, 1.0, 1.0, 0.25);
            Image canvas = Image::filled(Extent{32, 32}, 0, 0, 0, 0);
            const Rect bounds = stroke_bounds(path, brush, canvas.size());
            const std::optional<Rect> touched =
                paint_stroke(canvas, path, brush, Rgba8{9, 9, 9, 255}, PaintMode::Over);
            if (bounds.empty()) {
                CHECK(!touched.has_value());
                continue;
            }
            CHECK(touched.has_value());
            if (touched) {
                CHECK_EQ(bounds.x == touched->x && bounds.y == touched->y &&
                             bounds.w == touched->w && bounds.h == touched->h,
                         true);
            }
            CHECK(channel_sum(canvas) > 0);
        }
    }
}

}  // namespace

// A live gesture stamps a few dabs per pointer event and repaints from its own buffer. If
// growing that buffer lost what was already in it, the committed stroke would be shorter than
// the one the user dragged - and shorter than the same path painted in one call. The scratch
// has to start away from the image corner for this to bite: the bug it catches read the old
// coverage as if the buffer always began at (0,0).
void test_a_scratch_stamped_in_pieces_holds_the_same_coverage_as_one_stamp() {
    const Extent size{640, 400};
    const Brush brush{24.0, 0.8, 1.0, 1.0, 0.25};
    const Rgba8 color{0, 0, 0, 255};
    std::vector<Vec2> path;
    for (int step = 0; step <= 8; ++step) {
        path.push_back(Vec2{190.0 + 30.0 * step, 200.0 + 9.0 * step});
    }

    Image once = Image::filled(size, 200, 100, 50, 255);
    CHECK(paint_stroke(once, path, brush, color, PaintMode::Over).has_value());

    const Image base = Image::filled(size, 200, 100, 50, 255);
    Image live = base;
    StrokeScratch scratch;
    DabWalk walk{0.0, false};
    double previous_coverage = 0.0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const std::vector<Vec2> segment =
            i == 0 ? std::vector<Vec2>{path[0]} : std::vector<Vec2>{path[i - 1], path[i]};
        scratch_stamp(scratch, walk_dabs(segment, brush, walk), brush, size);
        double total = 0.0;
        for (const double value : scratch.coverage) {
            total += value;
        }
        CHECK(total > previous_coverage);  // growth adds, it never replaces
        previous_coverage = total;
        live = base;  // what a provisional repaint does before compositing the stroke
        scratch_composite(live, scratch, color, brush, PaintMode::Over, scratch.rect);
    }
    CHECK_EQ(live.checksum_hex(), once.checksum_hex());
}

// A live gesture restores and composites only the rectangle its new dabs reached. That is exact,
// not an approximation, because a pixel's coverage already holds every earlier dab that touched
// it - and this is where that is pinned: the piecewise gesture and the whole buffer must land on
// the same bytes, or the optimisation has quietly changed what a brush paints.
void test_repainting_only_what_changed_equals_repainting_the_whole_scratch() {
    const Extent size{640, 400};
    const Brush brush{24.0, 0.8, 0.7, 1.0, 0.25};
    const Rgba8 color{10, 20, 30, 255};
    std::vector<Vec2> path;
    for (int step = 0; step <= 12; ++step) {
        path.push_back(Vec2{150.0 + 32.0 * step, 190.0 + 11.0 * step});
    }

    const Image base = Image::filled(size, 200, 100, 50, 255);

    StrokeScratch whole;
    scratch_stamp(whole, stroke_dabs(path, brush), brush, size);
    Image flattened = base;
    scratch_composite(flattened, whole, color, brush, PaintMode::Over, whole.rect);

    Image stepwise = base;
    StrokeScratch scratch;
    DabWalk walk{0.0, false};
    for (std::size_t i = 0; i < path.size(); ++i) {
        const std::vector<Vec2> segment =
            i == 0 ? std::vector<Vec2>{path[0]} : std::vector<Vec2>{path[i - 1], path[i]};
        const Rect touched = scratch_stamp(scratch, walk_dabs(segment, brush, walk), brush, size);
        stepwise.blit(base.region(touched), touched.x, touched.y);  // what the document restores
        scratch_composite(stepwise, scratch, color, brush, PaintMode::Over, touched);
    }
    CHECK_EQ(stepwise.checksum_hex(), flattened.checksum_hex());
    CHECK_NE(stepwise.checksum_hex(), base.checksum_hex());
}

// What a stroke costs a mask. The dab's strength is the ink: the scratch is the single source of
// coverage for both planes, so the same path reaches the same cells and lands at the same strength
// whichever plane it is aimed at, and `opacity` scales that strength rather than the geometry.
void test_a_stroke_of_a_plane_is_woven_from_one_scratch() {
    const Extent size{200, 120};
    const Brush brush{22.0, 0.75, 1.0, 0.9, 0.3};
    const std::vector<Vec2> path{Vec2{40.0, 90.0}, Vec2{160.0, 30.0}};

    StrokeScratch colour;
    const Rect painted = scratch_stamp(colour, stroke_dabs(path, brush), brush, size);
    StrokeScratch coverage;
    const Rect stamped = scratch_stamp(coverage, stroke_dabs(path, brush), brush, size);
    CHECK(painted == stamped);
    CHECK(!stamped.empty());

    // The patch is exactly the rectangle the dabs reached - which is what lets the document write one
    // rectangle and repair one rectangle.
    const std::vector<std::uint8_t> ink = scratch_coverage(coverage, stamped, 1.0);
    CHECK_EQ(ink.size(), static_cast<std::size_t>(stamped.w) * static_cast<std::size_t>(stamped.h));
    // A region the scratch does not hold is refused rather than read out of the buffer.
    CHECK(scratch_coverage(coverage, Rect{0, 0, size.w, size.h}, 1.0).empty());
    CHECK(scratch_coverage(coverage, Rect{stamped.x - 1, stamped.y, stamped.w, stamped.h}, 1.0).empty());
    CHECK(scratch_coverage(coverage, Rect{}, 1.0).empty());

    // And the two planes agree cell for cell: `paint_stroke_mask` and `scratch_composite` started from
    // the same scratch, so the mask's coverage is the colour stroke's alpha and not a recomputation
    // that could drift by a pixel.
    Mask mask(size);
    CHECK(paint_stroke_mask(mask, path, brush, PaintMode::Over).has_value());
    Image image = Image::filled(size, 0, 0, 0, 0);
    scratch_composite(image, colour, Rgba8{255, 255, 255, 255}, brush, PaintMode::Over, painted);
    std::size_t differing = 0;
    std::size_t reached = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const int alpha = image.at(x, y)[3];
            const int cell = mask.coverage(x, y);
            differing += alpha != cell ? 1u : 0u;
            reached += cell > 0 || alpha > 0 ? 1u : 0u;
        }
    }
    CHECK_EQ(differing, 0u);
    CHECK(reached > 100u);
}

// Opacity scales the ink, and the eraser travels the other way with the same arithmetic.
void test_opacity_and_the_eraser_move_coverage_both_ways() {
    const Extent size{120, 80};
    const std::vector<Vec2> path{Vec2{30.0, 40.0}, Vec2{90.0, 40.0}};

    Mask full(size);
    full.fill(kMaskIn);
    Mask empty(size);

    const Brush soft{20.0, 0.6, 1.0, 1.0, 0.25};
    CHECK(paint_stroke_mask(full, path, soft, PaintMode::Erase).has_value());
    CHECK(paint_stroke_mask(empty, path, soft, PaintMode::Over).has_value());
    CHECK(full.plane() != empty.plane());

    const Brush faint{20.0, 0.6, 0.25, 1.0, 0.25};
    Mask faint_hide(size);
    faint_hide.fill(kMaskIn);
    CHECK(paint_stroke_mask(faint_hide, path, faint, PaintMode::Erase).has_value());
    Mask faint_reveal(size);
    CHECK(paint_stroke_mask(faint_reveal, path, faint, PaintMode::Over).has_value());

    // The symmetry is the rule `had + (target - had) * ink`: what a quarter-strength eraser takes off a
    // fully shown mask, a quarter-strength brush puts back on an empty one, to within the byte's own
    // rounding on each side.
    std::size_t worst = 0;
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            const int taken = kMaskIn - faint_hide.coverage(x, y);
            const int given = faint_reveal.coverage(x, y);
            worst = std::max(worst, static_cast<std::size_t>(std::abs(taken - given)));
        }
    }
    CHECK(worst <= 2u);
    // Per cell the two are the same move from opposite ends of the scale, so across the whole plane
    // their areas add up to the plane - up to a byte of rounding each way. That is the invariant a
    // `MaskInk` reversal has to hold: the eraser is not a different brush.
    CHECK_NEAR(faint_hide.area() + faint_reveal.area(), static_cast<double>(size.w * size.h), 40.0);
    CHECK(faint_hide.area() > faint_reveal.area());

    // A stroke at half opacity reveals about half of what the same stroke at full opacity reveals - a
    // property the pointer path relies on, because the panel's opacity slider is the only thing
    // between a mask stroke and a full reveal.
    Mask half(size);
    Mask whole(size);
    const Brush weak{20.0, 1.0, 0.5, 1.0, 0.25};
    const Brush strong{20.0, 1.0, 1.0, 1.0, 0.25};
    CHECK(paint_stroke_mask(half, path, weak, PaintMode::Over).has_value());
    CHECK(paint_stroke_mask(whole, path, strong, PaintMode::Over).has_value());
    CHECK(whole.area() > 0.0);
    const double ratio = half.area() / whole.area();
    CHECK(ratio > 0.35 && ratio < 0.7);
}

int main() {
    test_dabs_are_spaced_from_the_brush_size();
    test_coverage_is_a_round_ramped_stamp();
    test_a_stroke_keeps_its_colour_and_only_spreads_where_the_stamp_reaches();
    test_the_blend_matches_the_reference_arithmetic();
    test_a_stroke_outside_the_document_changes_nothing();
    test_stroke_bounds_is_exactly_the_painted_rectangle();
    test_a_scratch_stamped_in_pieces_holds_the_same_coverage_as_one_stamp();
    test_repainting_only_what_changed_equals_repainting_the_whole_scratch();
    test_a_stroke_of_a_plane_is_woven_from_one_scratch();
    test_opacity_and_the_eraser_move_coverage_both_ways();
    return noise::test::summary("brush");
}
