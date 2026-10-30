// SPDX-License-Identifier: Apache-2.0
// Document geometry, outpaint selection, and editing the AI landing's mask.
#include "test.h"

#include "ai/compose.h"
#include "core/document.h"
#include "core/maskops.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace noise;

namespace {

Image pattern(Extent extent, int bias = 0) {
    Image image = Image::filled(extent, 0, 0, 0, 0);
    for (int y = 0; y < extent.h; ++y) {
        for (int x = 0; x < extent.w; ++x) {
            auto* pixel = image.at(x, y);
            pixel[0] = static_cast<std::uint8_t>((x * 17 + y + bias) % 256);
            pixel[1] = static_cast<std::uint8_t>((y * 23 + x + bias) % 256);
            pixel[2] = static_cast<std::uint8_t>((x * 3 + y * 7 + bias) % 256);
            pixel[3] = static_cast<std::uint8_t>(x % 4 == 0 ? 128 : 255);
        }
    }
    return image;
}

std::vector<Layer> layers(Extent extent) {
    Layer bottom;
    bottom.name = "bottom";
    bottom.pixels = pattern(extent);
    Layer top;
    top.name = "top";
    top.pixels = pattern(extent, 63);
    top.opacity = 0.75;
    Mask mask(extent);
    mask.op(Rect{1, 2, 7, 5}, MaskOp::replace);
    top.mask = std::move(mask);
    std::vector<Layer> result;
    result.push_back(std::move(bottom));
    result.push_back(std::move(top));
    return result;
}

bool same_pixels(const Image& a, const Image& b) {
    return a.size() == b.size() && std::equal(a.pixels().begin(), a.pixels().end(),
                                              b.pixels().begin());
}

bool same_pixel(const Image& a, int ax, int ay, const Image& b, int bx, int by) {
    return std::memcmp(a.at(ax, ay), b.at(bx, by), kChannels) == 0;
}

void test_canvas_resize_moves_every_plane_and_marks_only_new_pixels() {
    const Extent original{13, 10};
    const Extent expanded{19, 15};
    Mask selected(original);
    selected.op(Rect{4, 3, 3, 2}, MaskOp::replace);
    Document doc;
    doc.reset(layers(original), selected, "fixture.noise");
    const std::vector<Layer> before(doc.layers().begin(), doc.layers().end());
    const Image before_composite = doc.composite();
    const std::string before_checksum = doc.checksum();
    const int depth_before = doc.history().undo_depth;
    const std::uint64_t first_geometry = doc.geometry_revision();

    CHECK(doc.resize_canvas(expanded, 4, 2, true));
    CHECK(doc.size() == expanded);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 1);
    CHECK_EQ(doc.history().undo_depth, depth_before + 1);
    REQUIRE(doc.selection().has_value());
    for (int y = 0; y < expanded.h; ++y) {
        for (int x = 0; x < expanded.w; ++x) {
            const bool inside_old = x >= 4 && x < original.w + 4 && y >= 2 && y < original.h + 2;
            CHECK_EQ(doc.selection()->coverage(x, y), inside_old ? kMaskOut : kMaskIn);
            for (std::size_t index = 0; index < before.size(); ++index) {
                const Image& changed = doc.layer(index).pixels;
                if (inside_old) {
                    CHECK(same_pixel(changed, x, y, before[index].pixels, x - 4, y - 2));
                } else {
                    CHECK_EQ(changed.at(x, y)[3], 0);
                }
            }
            if (inside_old) {
                CHECK(same_pixel(doc.composite(), x, y, before_composite, x - 4, y - 2));
            } else {
                CHECK_EQ(doc.composite().at(x, y)[3], 0);
            }
            REQUIRE(doc.layer(1).mask.has_value());
            CHECK_EQ(doc.layer(1).mask->coverage(x, y),
                     inside_old ? before[1].mask->coverage(x - 4, y - 2) : kMaskOut);
        }
    }

    const Image expanded_composite = doc.composite();
    const std::string expanded_checksum = doc.checksum();
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 2);
    CHECK_EQ(doc.checksum(), before_checksum);
    CHECK(doc.selection().has_value() && *doc.selection() == selected);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK_EQ(doc.layer(i).id, before[i].id);
        CHECK(same_pixels(doc.layer(i).pixels, before[i].pixels));
        CHECK(doc.layer(i).mask == before[i].mask);
    }
    CHECK(doc.redo());
    CHECK(doc.size() == expanded);
    CHECK_EQ(doc.geometry_revision(), first_geometry + 3);
    CHECK_EQ(doc.checksum(), expanded_checksum);
    CHECK(same_pixels(doc.composite(), expanded_composite));
    REQUIRE(doc.selection().has_value());
    CHECK_EQ(doc.selection()->coverage(0, 0), kMaskIn);
    CHECK_EQ(doc.selection()->coverage(4, 2), kMaskOut);
}

void test_crop_and_undo_keep_geometry_and_previous_edits() {
    const Extent original{16, 12};
    Document doc;
    doc.reset(layers(original), std::nullopt, "fixture.noise");
    doc.select_layer(0);
    doc.paint(Rect{4, 3, 2, 3}, PaintFence::own_coverage, [](Image& layer) {
        const std::uint8_t ink[4]{200, 80, 17, 255};
        layer.fill_rect(Rect{4, 3, 2, 3}, ink);
    });
    const Image painted = doc.composite();
    const std::string after_paint = doc.checksum();
    CHECK_EQ(doc.history().undo_depth, 1);
    CHECK(!doc.crop_canvas(Rect{-1, 1, 4, 4}));
    CHECK(!doc.crop_canvas(Rect{0, 0, 0, 4}));
    CHECK_EQ(doc.history().undo_depth, 1);

    CHECK(doc.crop_canvas(Rect{3, 2, 8, 7}));
    CHECK(doc.size() == (Extent{8, 7}));
    CHECK_EQ(doc.history().undo_depth, 2);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK(same_pixel(doc.composite(), x, y, painted, x + 3, y + 2));
        }
    }
    const std::string cropped = doc.checksum();
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_EQ(doc.checksum(), after_paint);
    CHECK(doc.undo());
    CHECK(doc.size() == original);
    CHECK_NE(doc.checksum(), after_paint);
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), after_paint);
    CHECK(doc.redo());
    CHECK_EQ(doc.checksum(), cropped);
    CHECK(doc.size() == (Extent{8, 7}));

    CHECK(doc.resize_canvas(Extent{8, 7}, 0, 0, true));
    CHECK_EQ(doc.history().undo_depth, 2); // no-op never creates history
    CHECK(!doc.resize_canvas(Extent{9, 8}, -20, 0, true));
    CHECK(!doc.resize_canvas(Extent{8193, 8193}, 0, 0, true));
}

void test_oversized_geometry_step_can_always_be_undone() {
    const Extent old_size{6, 5};
    const Extent new_size{9, 7};
    History journal(1); // One saved full-canvas geometry step exceeds this deliberately.
    std::vector<Layer> old = layers(old_size);
    std::vector<Layer> current = layers(new_size);
    Mask old_mark(old_size);
    old_mark.op(Rect{1, 1, 2, 2}, MaskOp::replace);
    std::optional<Mask> marked = Mask(new_size);
    marked->op(Rect{0, 0, 2, 7}, MaskOp::replace);
    journal.record_canvas_replaced(std::move(old), old_mark);
    CHECK_EQ(journal.stats().undo_depth, 1);
    REQUIRE(journal.undo(current, marked).has_value());
    CHECK(current[0].pixels.size() == old_size);
    CHECK(marked.has_value() && *marked == old_mark);
    REQUIRE(journal.redo(current, marked).has_value());
    CHECK(current[0].pixels.size() == new_size);
    REQUIRE(marked.has_value());
    CHECK_EQ(marked->coverage(0, 0), kMaskIn);
    CHECK_EQ(marked->coverage(2, 0), kMaskOut);
}

void test_mask_edge_is_hard_soft_and_can_shift_inside_its_canvas() {
    Mask original(Extent{24, 20});
    original.op(Rect{8, 7, 8, 6}, MaskOp::replace);
    const Mask grown = adjusted_edge(original, 2, true, 0);
    const Mask shrunk = adjusted_edge(original, -2, true, 0);
    CHECK_EQ(grown.coverage(6, 9), kMaskIn);
    CHECK_EQ(grown.coverage(5, 9), kMaskOut);
    CHECK_EQ(shrunk.coverage(9, 9), kMaskOut);
    CHECK_EQ(shrunk.coverage(10, 9), kMaskIn);
    CHECK_EQ(grown.coverage(7, 6), kMaskIn);
    CHECK_EQ(shrunk.coverage(15, 9), kMaskOut);
    CHECK_EQ(adjusted_edge(original, 0, false, 0), original);
    const Mask feathered = adjusted_edge(original, 0, true, 2);
    CHECK(feathered.coverage(7, 9) > kMaskOut);
    CHECK(feathered.coverage(7, 9) < kMaskIn);
    CHECK(feathered.coverage(12, 10) > feathered.coverage(7, 9));
}

void test_ai_layer_arrival_keeps_its_mask_in_one_undo_step() {
    const Extent canvas{18, 14};
    Document doc;
    doc.reset(layers(canvas), std::nullopt, "fixture.noise");
    const int before = doc.history().undo_depth;
    const int index = doc.add_layer_written("AI", Rect{3, 4, 10, 8}, [&](Layer& layer) {
        layer.pixels.fill_rect(Rect{3, 4, 10, 8},
                               std::array<std::uint8_t, 4>{30, 190, 80, 255}.data());
        Mask edge(canvas);
        edge.op(Rect{6, 6, 4, 3}, MaskOp::replace);
        layer.mask = std::move(edge);
        return true;
    });
    REQUIRE(index == 2);
    REQUIRE(doc.layer(2).mask.has_value());
    const Image after = doc.composite();
    CHECK_EQ(doc.history().undo_depth, before + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.layers().size(), std::size_t(2));
    CHECK(doc.redo());
    REQUIRE(doc.layer(2).mask.has_value());
    CHECK_EQ(doc.layer(2).mask->coverage(5, 6), kMaskOut);
    CHECK_EQ(doc.layer(2).mask->coverage(6, 6), kMaskIn);
    CHECK(same_pixels(doc.composite(), after));
}

void test_ai_result_stays_editable_without_changing_uncovered_pixels() {
    const Extent canvas{26, 20};
    const Image base = pattern(canvas);
    Mask selection(canvas);
    selection.op(Rect{8, 7, 6, 5}, MaskOp::replace);
    ai::RegionPlan plan;
    plan.crop = Rect{4, 4, 14, 12};
    plan.requested = Extent{14, 12};
    const Image answer = Image::filled(plan.requested, 250, 12, 30, 255);
    const ai::CompositePolicy policy{0, false, 0};

    Image baked = Image::filled(canvas, 0, 0, 0, 0);
    Image editable = Image::filled(canvas, 0, 0, 0, 0);
    Mask edge;
    const auto legacy = ai::compose_result(baked, base, answer, plan, selection, policy);
    const auto modern = ai::compose_editable_result(editable, edge, base, answer, plan, selection,
                                                     policy);
    REQUIRE(legacy.composed);
    REQUIRE(modern.composed);
    CHECK(legacy.coverage == edge);
    CHECK_EQ(modern.feather_px, 0);
    CHECK_EQ(modern.pixels_changed, legacy.pixels_changed);
    // Pixel data exists behind the initially hidden rim of the editable answer.
    CHECK_EQ(baked.at(7, 9)[3], 0);
    CHECK_EQ(editable.at(7, 9)[3], 255);
    CHECK_EQ(edge.coverage(7, 9), 0);

    Layer paper;
    paper.name = "paper";
    paper.pixels = base;
    Layer old_result;
    old_result.name = "old result";
    old_result.pixels = baked;
    Layer new_result;
    new_result.name = "new result";
    new_result.pixels = editable;
    new_result.mask = edge;
    Document flattened;
    Document adjustable;
    flattened.reset({paper, old_result}, std::nullopt, "a.noise");
    adjustable.reset({paper, new_result}, std::nullopt, "b.noise");
    CHECK(same_pixels(flattened.composite(), adjustable.composite()));
    const Image original_display = adjustable.composite();

    const Mask wider = adjusted_edge(edge, 1, true, 0);
    CHECK(adjustable.set_layer_mask(1, wider));
    CHECK_EQ(adjustable.composite().at(6, 9)[0], base.at(6, 9)[0]);
    CHECK_NE(adjustable.composite().at(7, 9)[0], base.at(7, 9)[0]);
    CHECK_NE(adjustable.composite().at(8, 6)[0], base.at(8, 6)[0]);
    for (int y = 0; y < canvas.h; ++y) {
        for (int x = 0; x < canvas.w; ++x) {
            if (wider.coverage(x, y) == kMaskOut) {
                CHECK(same_pixel(adjustable.composite(), x, y, base, x, y));
            }
        }
    }
    CHECK(adjustable.undo());
    CHECK(same_pixels(adjustable.composite(), original_display));
    CHECK(adjustable.redo());
    CHECK(adjustable.layer(1).mask == wider);
}

}  // namespace

int main() {
    test_canvas_resize_moves_every_plane_and_marks_only_new_pixels();
    test_crop_and_undo_keep_geometry_and_previous_edits();
    test_oversized_geometry_step_can_always_be_undone();
    test_mask_edge_is_hard_soft_and_can_shift_inside_its_canvas();
    test_ai_layer_arrival_keeps_its_mask_in_one_undo_step();
    test_ai_result_stays_editable_without_changing_uncovered_pixels();
    return noise::test::summary("canvas_edit");
}
