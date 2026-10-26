// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/mask.h"
#include "core/maskops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

using namespace noise;

namespace {

std::size_t index_of(Rect box, int x, int y) {
    return static_cast<std::size_t>(y - box.y) * static_cast<std::size_t>(box.w) +
           static_cast<std::size_t>(x - box.x);
}

// The ellipse inscribed in a box of `box`'s size, in a mask of that size, so a test reads coverage
// by box-relative coordinates. Rasterizing is exact under a whole-pixel shift, which is what lets a
// clip be checked against the unclipped shape.
Mask ellipse_of(Rect box) {
    Mask mask{Extent{box.w, box.h}};
    op_shape(mask, MaskShape::ellipse, Rect{0, 0, box.w, box.h}, MaskOp::replace);
    return mask;
}

// Total coverage in pixels: the sum of every cell over 255.
double coverage_area(const Mask& mask) {
    double total = 0.0;
    for (const std::uint8_t cell : mask.plane()) {
        total += static_cast<double>(cell) / 255.0;
    }
    return total;
}

std::size_t partial_cells(const Mask& mask) {
    std::size_t partial = 0;
    for (const std::uint8_t cell : mask.plane()) {
        partial += cell != kMaskOut && cell != kMaskIn ? 1u : 0u;
    }
    return partial;
}

void test_the_shape_table_is_the_only_vocabulary() {
    for (const MaskShapeInfo& info : kMaskShapes) {
        CHECK_EQ(mask_shape_name(info.shape), std::string(info.name));
        const std::optional<MaskShape> parsed = mask_shape_from_name(info.name);
        CHECK(parsed.has_value());
        CHECK(*parsed == info.shape);
    }
    CHECK(!mask_shape_from_name("square").has_value());
    CHECK(!mask_shape_from_name("").has_value());
    // The error list comes from the table, so it cannot name a shape that has gone.
    std::string names = mask_shape_names();
    for (const MaskShapeInfo& info : kMaskShapes) {
        CHECK(names.find(info.name) != std::string::npos);
    }
    CHECK(names.find("square") == std::string::npos);
}

void test_a_rectangle_shape_is_a_rectangle_op() {
    // The claim on `op_shape`: it is another way to produce coverage, not another kind of coverage.
    // A rect through the shape seam and a rect through the mask's own door have to be the same
    // bytes for every operation, or the two paths drift and one of them is a lie.
    for (const MaskOp op : {MaskOp::replace, MaskOp::add, MaskOp::subtract}) {
        for (const Rect box :
             {Rect{2, 1, 5, 4}, Rect{0, 0, 16, 9}, Rect{-4, -4, 10, 10}, Rect{12, 6, 10, 10},
              Rect{1, 1, 0, 3}, Rect{1, 1, 3, 0}}) {
            Mask direct{Extent{16, 9}};
            direct.op(box, op);
            Mask shaped{Extent{16, 9}};
            op_shape(shaped, MaskShape::rect, box, op);
            CHECK(shaped == direct);
        }
    }

    // The combining operations keep what is already there the same way too.
    for (const MaskOp op : {MaskOp::add, MaskOp::subtract}) {
        Mask direct{Extent{16, 9}};
        direct.op(Rect{2, 2, 6, 6}, MaskOp::replace);
        direct.op(Rect{4, 4, 6, 6}, op);
        Mask shaped{Extent{16, 9}};
        shaped.op(Rect{2, 2, 6, 6}, MaskOp::replace);
        op_shape(shaped, MaskShape::rect, Rect{4, 4, 6, 6}, op);
        CHECK(shaped == direct);
    }
}

void test_the_ellipse_covers_what_the_arithmetic_says() {
    // Area is the property that catches a wrong centre, a wrong semi-axis or a wrong scale: every
    // one of those moves the total, and nothing else in the test does.
    for (const Rect box : {Rect{0, 0, 60, 40}, Rect{10, 5, 61, 41}, Rect{0, 0, 8, 8},
                           Rect{3, 3, 101, 21}}) {
        const Mask mask = ellipse_of(box);
        const double want = M_PI * (box.w / 2.0) * (box.h / 2.0);
        const double got = coverage_area(mask);
        // A few tenths of a percent: the edge is sub-sampled eight ways, so the error is a small
        // fraction of the perimeter and shrinks relative to the area as the shape grows.
        CHECK_NEAR(got, want, 0.005 * want);
    }
}

void test_the_ellipse_is_wholly_in_and_out_where_it_must_be() {
    const Rect box{0, 0, 60, 40};
    const Mask mask = ellipse_of(box);

    CHECK_EQ(mask.coverage(30, 20), kMaskIn);   // the centre
    CHECK_EQ(mask.coverage(0, 0), kMaskOut);    // the box corners, where the curve is farthest in
    CHECK_EQ(mask.coverage(59, 39), kMaskOut);
    CHECK_EQ(mask.coverage(1, 1), kMaskOut);
    CHECK_EQ(mask.coverage(58, 38), kMaskOut);

    // Where the curve touches the box it runs flat, so the pixel just inside the tangent is all but
    // covered - the ellipse bulges out to meet it. Asserting they are *not* empty is what keeps a
    // half-pixel off-by-one in the centre or the semi-axes visible: pull either in and these empty.
    CHECK(mask.coverage(30, 0) > kMaskIn - 8);
    CHECK(mask.coverage(0, 20) > kMaskIn - 8);

    // And the edge is really anti-aliased: a shape with no partial coverage at all is a staircase,
    // which is the thing this rasterizer exists not to be.
    CHECK(partial_cells(mask) > 40);

    // Along the middle row coverage falls away from the centre in both directions: a stray sign or
    // a swapped axis shows up here first.
    int previous = kMaskIn;
    for (int x = 30; x < 60; ++x) {
        const int cell = mask.coverage(x, 20);
        CHECK(cell <= previous);
        previous = cell;
    }
    previous = kMaskIn;
    for (int y = 20; y < 40; ++y) {
        const int cell = mask.coverage(30, y);
        CHECK(cell <= previous);
        previous = cell;
    }
}

void test_the_ellipse_is_symmetric_about_its_centre() {
    // An even box puts the centre on a pixel boundary, so the mirror is exact - coverage for
    // coverage, not within a tolerance. Anything that samples asymmetrically breaks it.
    const Rect box{0, 0, 60, 40};
    const Mask mask = ellipse_of(box);
    for (int y = box.y; y < box.bottom(); ++y) {
        for (int x = box.x; x < box.right(); ++x) {
            const int mirrored_x = box.right() - 1 - (x - box.x);
            const int mirrored_y = box.bottom() - 1 - (y - box.y);
            CHECK_EQ(mask.plane()[index_of(box, x, y)],
                     mask.plane()[index_of(box, mirrored_x, y)]);
            CHECK_EQ(mask.plane()[index_of(box, x, y)],
                     mask.plane()[index_of(box, x, mirrored_y)]);
        }
    }
}

void test_the_ellipse_is_clipped_by_the_mask_and_reaches_the_rest_untouched() {
    Mask mask{Extent{20, 20}};
    mask.op(Rect{0, 0, 20, 20}, MaskOp::replace);
    const Mask untouched = mask;

    // A box reaching off three edges: inside the canvas the curve is the curve, outside nothing can
    // have been written at all.
    op_shape(mask, MaskShape::ellipse, Rect{-20, -20, 40, 40}, MaskOp::replace);
    const Mask whole = ellipse_of(Rect{-20, -20, 40, 40});
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            CHECK_EQ(mask.coverage(x, y), whole.coverage(x + 20, y + 20));
        }
    }

    // Fully outside writes nothing, so the plane keeps the facts it had.
    Mask elsewhere{Extent{20, 20}};
    op_shape(elsewhere, MaskShape::ellipse, Rect{30, 30, 10, 10}, MaskOp::add);
    CHECK_EQ(elsewhere.covered(), 0u);
    op_shape(elsewhere, MaskShape::ellipse, Rect{0, 0, 0, 10}, MaskOp::add);
    CHECK_EQ(elsewhere.covered(), 0u);
    (void)untouched;
}

void test_the_operations_mix_coverage_rather_than_set_it() {
    // The point of partial coverage: a shape's edge over something already selected is a union, not
    // an overwrite, and a subtraction takes away exactly the part it claims.
    const Rect box{0, 0, 40, 40};
    const Mask ellipse = ellipse_of(box);

    Mask half{Extent{40, 40}};
    half.fill(128);

    Mask union_of = half;
    op_shape(union_of, MaskShape::ellipse, box, MaskOp::add);
    Mask difference = half;
    op_shape(difference, MaskShape::ellipse, box, MaskOp::subtract);

    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 40; ++x) {
            const int shape = ellipse.coverage(x, y);
            CHECK_EQ(union_of.coverage(x, y), std::max(128, shape));
            CHECK_EQ(difference.coverage(x, y), std::max(0, 128 - shape));
        }
    }

    // `replace` owns the whole area it was given, corners of the box included: the new selection is
    // the shape, which is why the box outside the curve comes back as nothing.
    Mask replaced{Extent{40, 40}};
    replaced.fill(255);
    op_shape(replaced, MaskShape::ellipse, box, MaskOp::replace);
    CHECK_EQ(replaced.coverage(0, 0), kMaskOut);
    CHECK_EQ(replaced.coverage(20, 20), kMaskIn);
}

void test_the_smallest_ellipses_stay_honest() {
    // A 1x1 box holds an ellipse covering pi/4 of the pixel, and the plane says so rather than
    // rounding a shape up to the staircase it is meant to avoid.
    Mask one{Extent{4, 4}};
    op_shape(one, MaskShape::ellipse, Rect{2, 2, 1, 1}, MaskOp::replace);
    CHECK_NEAR(static_cast<double>(one.coverage(2, 2)), 255.0 * M_PI / 4.0, 1.0);
    CHECK(one.coverage(2, 2) != kMaskIn);

    // A one-pixel-wide box is a vertical lens: covered where the ellipse reaches along the column,
    // and nothing in the columns either side of it.
    Mask lens{Extent{6, 6}};
    op_shape(lens, MaskShape::ellipse, Rect{2, 0, 1, 6}, MaskOp::replace);
    CHECK_EQ(lens.coverage(1, 3), kMaskOut);
    CHECK_EQ(lens.coverage(3, 3), kMaskOut);
    CHECK(lens.coverage(2, 3) > kMaskOut);
}

void test_a_patch_of_the_wrong_shape_writes_nothing() {
    Mask mask{Extent{8, 8}};
    const Rect area{2, 2, 4, 4};
    const std::vector<std::uint8_t> short_patch(15u, kMaskIn);
    const std::vector<std::uint8_t> long_patch(17u, kMaskIn);
    CHECK(!mask.op(area, short_patch, MaskOp::replace));
    CHECK(!mask.op(area, long_patch, MaskOp::replace));
    CHECK_EQ(mask.covered(), 0u);

    // An empty area with an empty patch is nothing to do, and nothing to refuse.
    CHECK(mask.op(Rect{4, 4, 0, 0}, std::vector<std::uint8_t>{}, MaskOp::replace));
    CHECK(mask.op(Rect{4, 4, 2, 0}, std::vector<std::uint8_t>{}, MaskOp::add));

    const std::vector<std::uint8_t> cells(16u, 64);
    CHECK(mask.op(area, cells, MaskOp::replace));
    CHECK_EQ(mask.coverage(2, 2), 64);
    CHECK_EQ(mask.coverage(5, 5), 64);
    CHECK_EQ(mask.coverage(6, 6), kMaskOut);
}

void test_the_facts_are_recomputed_after_a_patch() {
    Mask mask{Extent{8, 8}};
    const std::vector<std::uint8_t> cells(4u, kMaskIn);
    CHECK(mask.op(Rect{3, 3, 2, 2}, cells, MaskOp::replace));
    CHECK_EQ(mask.covered(), 4u);
    CHECK_EQ(mask.boundary().size(), 4u);
    CHECK(mask.bounds() == Rect{3, 3, 2, 2});
}

void test_a_lasso_rasterizes_a_clipped_triangle_with_partial_edges() {
    const std::vector<Vec2> triangle{{1.0, 1.0}, {9.0, 1.0}, {1.0, 9.0}};
    const std::optional<MaskPatch> patch = rasterize_lasso(Extent{12, 12}, triangle);
    CHECK(patch.has_value());
    CHECK(patch->area == Rect{1, 1, 8, 8});
    Mask mask{Extent{12, 12}};
    CHECK(op_lasso(mask, triangle, MaskOp::replace));
    CHECK_EQ(mask.coverage(2, 2), kMaskIn);
    CHECK_EQ(mask.coverage(8, 8), kMaskOut);
    CHECK_NEAR(mask.area(), 32.0, 0.35);
    CHECK(partial_cells(mask) > 0u);

    Mask clipped{Extent{6, 5}};
    const std::optional<MaskPatch> clipped_patch =
        rasterize_lasso(clipped.size(), {{-3.0, 1.0}, {4.0, 1.0}, {4.0, 4.0}, {-3.0, 4.0}});
    CHECK(clipped_patch.has_value());
    CHECK(clipped_patch->area == Rect{0, 1, 4, 3});
    CHECK(clipped.op(clipped_patch->area, clipped_patch->coverage, MaskOp::replace));
    CHECK_EQ(clipped.coverage(0, 2), kMaskIn);
    CHECK_EQ(clipped.coverage(3, 3), kMaskIn);
    CHECK_EQ(clipped.coverage(4, 2), kMaskOut);
    CHECK_EQ(clipped.covered(), 12u);
}

void test_a_lasso_fills_concave_and_self_intersecting_paths_even_odd() {
    const std::vector<Vec2> concave{{1.0, 1.0}, {9.0, 1.0}, {9.0, 3.0}, {4.0, 3.0},
                                    {4.0, 7.0}, {9.0, 7.0}, {9.0, 9.0}, {1.0, 9.0}};
    Mask shape{Extent{12, 12}};
    CHECK(op_lasso(shape, concave, MaskOp::replace));
    CHECK_EQ(shape.coverage(2, 5), kMaskIn);
    CHECK_EQ(shape.coverage(7, 2), kMaskIn);
    CHECK_EQ(shape.coverage(7, 5), kMaskOut);
    CHECK_NEAR(shape.area(), 44.0, 0.2);

    const std::vector<Vec2> bow_tie{{1.0, 1.0}, {9.0, 9.0}, {1.0, 9.0}, {9.0, 1.0}};
    Mask crossed{Extent{11, 10}};
    CHECK(op_lasso(crossed, bow_tie, MaskOp::replace));
    CHECK(crossed.coverage(2, 2) > kMaskOut);
    CHECK(crossed.coverage(2, 7) > kMaskOut);
    CHECK_EQ(crossed.coverage(2, 5), kMaskOut);
    CHECK(crossed.coverage(4, 4) > kMaskOut);
}

void test_lasso_uses_the_existing_replace_add_and_subtract_rules() {
    const std::vector<Vec2> square{{2.0, 2.0}, {7.0, 2.0}, {7.0, 7.0}, {2.0, 7.0}};
    Mask added{Extent{10, 10}};
    added.fill(100);
    CHECK(op_lasso(added, square, MaskOp::add));
    CHECK_EQ(added.coverage(4, 4), kMaskIn);
    CHECK_EQ(added.coverage(0, 0), 100);

    Mask removed{Extent{10, 10}};
    removed.fill(kMaskIn);
    CHECK(op_lasso(removed, square, MaskOp::subtract));
    CHECK_EQ(removed.coverage(4, 4), kMaskOut);
    CHECK_EQ(removed.coverage(0, 0), kMaskIn);

    Mask replaced{Extent{10, 10}};
    CHECK(op_lasso(replaced, square, MaskOp::replace));
    CHECK_EQ(replaced.coverage(4, 4), kMaskIn);
    CHECK_EQ(replaced.coverage(0, 0), kMaskOut);
}

void test_degenerate_lassos_leave_the_mask_unchanged() {
    Mask mask{Extent{10, 10}};
    mask.fill(77);
    const Mask before = mask;
    for (const std::vector<Vec2>& path : {
             std::vector<Vec2>{},
             std::vector<Vec2>{{1.0, 1.0}},
             std::vector<Vec2>{{1.0, 1.0}, {8.0, 8.0}},
             std::vector<Vec2>{{1.0, 1.0}, {8.0, 8.0}, {1.0, 1.0}, {8.0, 8.0}},
             std::vector<Vec2>{{1.0, 3.0}, {4.0, 3.0}, {9.0, 3.0}},
             std::vector<Vec2>{{20.0, 20.0}, {24.0, 20.0}, {24.0, 24.0}, {20.0, 24.0}},
             std::vector<Vec2>{{1.0, 1.0}, {std::numeric_limits<double>::infinity(), 2.0},
                               {3.0, 4.0}}}) {
        CHECK(!op_lasso(mask, path, MaskOp::replace));
        CHECK(mask == before);
    }
}

}  // namespace

int main() {
    test_the_shape_table_is_the_only_vocabulary();
    test_a_rectangle_shape_is_a_rectangle_op();
    test_the_ellipse_covers_what_the_arithmetic_says();
    test_the_ellipse_is_wholly_in_and_out_where_it_must_be();
    test_the_ellipse_is_symmetric_about_its_centre();
    test_the_ellipse_is_clipped_by_the_mask_and_reaches_the_rest_untouched();
    test_the_operations_mix_coverage_rather_than_set_it();
    test_the_smallest_ellipses_stay_honest();
    test_a_patch_of_the_wrong_shape_writes_nothing();
    test_the_facts_are_recomputed_after_a_patch();
    test_a_lasso_rasterizes_a_clipped_triangle_with_partial_edges();
    test_a_lasso_fills_concave_and_self_intersecting_paths_even_odd();
    test_lasso_uses_the_existing_replace_add_and_subtract_rules();
    test_degenerate_lassos_leave_the_mask_unchanged();
    return noise::test::summary("maskops");
}
