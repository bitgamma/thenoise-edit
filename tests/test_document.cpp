// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace noise;

namespace {

std::vector<Layer> stack(std::vector<std::pair<std::string, Image>> contents) {
    std::vector<Layer> layers;
    for (auto& [name, pixels] : contents) {
        Layer layer;
        layer.name = std::move(name);
        layer.pixels = std::move(pixels);
        layers.push_back(std::move(layer));
    }
    return layers;
}

void paint_on(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, [rect, grey](Image& image) {
        const std::uint8_t rgba[4] = {grey, grey, grey, 255};
        image.fill_rect(rect, rgba);
    });
}

// The composite is repaired over the rectangles that changed, so the load-bearing property is
// that it ends up identical to one built from scratch. A partial repair that missed a region, or
// blended a layer twice, shows up here and nowhere else.
void test_the_incremental_composite_equals_one_built_from_scratch() {
    Document live;
    live.reset(stack({{"Paper", Image::filled(Extent{24, 18}, 20, 20, 20, 255)}}), "x.noise");

    const auto check_against_scratch = [&live](const char* where) {
        std::vector<Layer> copy;
        for (const Layer& layer : live.layers()) {
            Layer clone;
            clone.name = layer.name;
            clone.pixels = layer.pixels;
            copy.push_back(std::move(clone));
        }
        Document fresh;
        fresh.reset(std::move(copy), "x.noise");
        const std::string built = live.composite().checksum_hex();
        const std::string scratch = fresh.composite().checksum_hex();
        if (built == scratch) {
            ++noise::test::checks;
            return;
        }
        noise::test::report("incremental composite == composite from scratch", __FILE__,
                            __LINE__, std::string(where) + ": got " + built + " want " + scratch);
    };

    check_against_scratch("one layer, nothing painted");
    paint_on(live, Rect{1, 1, 5, 4}, 200);
    check_against_scratch("after a stroke");
    live.add_layer("Mid", Rgba8{0, 0, 0, 0});
    check_against_scratch("after adding a layer");
    paint_on(live, Rect{10, 2, 6, 6}, 30);
    check_against_scratch("on the new layer");
    live.select_layer(0);
    paint_on(live, Rect{4, 10, 8, 3}, 120);
    check_against_scratch("on the bottom layer, under a semi-transparent one");
    live.add_layer("Top", Rgba8{0, 0, 0, 0});
    paint_on(live, Rect{0, 0, 24, 18}, 90);
    check_against_scratch("a layer covering everything");
    live.undo();
    check_against_scratch("after undo");
    live.undo();
    check_against_scratch("after a second undo");
    live.redo();
    check_against_scratch("after redo");
    live.remove_layer(1);
    check_against_scratch("after removing a middle layer");
    CHECK_EQ(live.layers().size(), 2u);
}

// Two layers of the same size and shape, one over the other: what lands on the canvas is the
// blend, not the top layer and not the sum.
void test_the_stack_blends_bottom_first() {
    const Extent size{6, 4};
    Image bottom = Image::filled(size, 255, 0, 0, 255);
    Image top = Image::filled(size, 0, 0, 0, 0);
    const std::uint8_t half_green[4] = {0, 255, 0, 128};
    top.fill_rect(Rect{0, 0, 6, 4}, half_green);

    Document doc;
    doc.reset(stack({{"Bottom", bottom}, {"Top", top}}), "x.noise");
    const Image& out = doc.composite();
    // Alpha 128 is not exactly a half: 128/255 over opaque red keeps 127/255 of the red, and
    // contributes 128/255 of the green. Both channels survive, alpha stays opaque.
    CHECK_EQ(static_cast<int>(out.at(0, 0)[0]), 127);
    CHECK_EQ(static_cast<int>(out.at(0, 0)[1]), 128);
    CHECK_EQ(static_cast<int>(out.at(0, 0)[3]), 255);
}

// A repair over one rectangle must not disturb the rest of the canvas, so a pixel far from the
// stroke is byte-identical before and after.
void test_repairing_a_rectangle_leaves_the_rest_of_the_canvas_alone() {
    const Extent size{40, 30};
    Document doc;
    doc.reset(stack({{"Bottom", Image::filled(size, 10, 20, 30, 255)},
                     {"Top", Image::filled(size, 0, 0, 0, 0)}}),
              "x.noise");
    paint_on(doc, Rect{2, 2, 4, 4}, 210);
    const std::span<const std::uint8_t> raw_before = doc.composite().pixels();
    const std::vector<std::uint8_t> before(raw_before.begin(), raw_before.end());
    paint_on(doc, Rect{30, 25, 3, 3}, 40);
    const std::span<const std::uint8_t> raw_after = doc.composite().pixels();
    const std::vector<std::uint8_t> after(raw_after.begin(), raw_after.end());

    std::size_t changed = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        changed += before[i] != after[i] ? 1u : 0u;
    }
    // Only the second rectangle's bytes differ, and only in the three colour channels: the
    // bottom layer is opaque there, so alpha is the one channel the repair cannot change.
    CHECK_EQ(changed, 3u * 3u * 3u);
}

// The top of the stack is what the user paints on after opening a file, and the layer under a
// removed one takes the selection rather than nothing.
void test_the_stack_keeps_a_usable_selection() {
    Document doc;
    doc.reset(stack({{"A", Image::filled(Extent{4, 4}, 0, 0, 0, 0)},
                     {"B", Image::filled(Extent{4, 4}, 0, 0, 0, 0)},
                     {"C", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}),
              "x.noise");
    CHECK_EQ(doc.active(), 2);
    CHECK_EQ(doc.layer(0).name, std::string("A"));
    CHECK(doc.select_layer(0));
    CHECK_EQ(doc.active(), 0);
    CHECK(!doc.select_layer(3));
    CHECK(!doc.select_layer(-1));
    CHECK(doc.remove_layer(0));
    CHECK_EQ(doc.layers().size(), 2u);
    // Removing the layer below the selection must not leave the selection pointing at nothing.
    CHECK(doc.select_layer(1));
    CHECK(doc.remove_layer(1));
    CHECK_EQ(doc.active(), 0);
    CHECK(!doc.remove_layer(0));
    CHECK_EQ(doc.layers().size(), 1u);
}

// Generated names belong to the position in the stack, not to a counter that remembers deleted
// layers.
void test_names_are_generated_from_the_stack_size() {
    Document doc;
    doc.reset(stack({{"Kept", Image::filled(Extent{4, 4}, 0, 0, 0, 0)}}), "x.noise");
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(1).name, std::string("Layer 2"));
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(2).name, std::string("Layer 3"));
    doc.remove_layer(1);
    doc.add_layer("", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layer(1).name, std::string("Layer 3"));
    CHECK_EQ(doc.layer(static_cast<std::size_t>(doc.active())).name, std::string("Layer 3"));
}

// A document checksum that ignored the stack could not tell two different documents apart.
void test_the_document_checksum_follows_the_stack() {
    Document one;
    one.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}), "x.noise");
    Document two;
    two.reset(stack({{"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)},
                     {"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)}}),
              "x.noise");
    CHECK_NE(one.checksum(), two.checksum());
    // Same two layers, opposite order: the flattened result differs, and so must the checksum.
    Document three;
    three.reset(stack({{"B", Image::filled(Extent{8, 8}, 0, 0, 0, 0)},
                       {"A", Image::filled(Extent{8, 8}, 1, 1, 1, 255)}}),
                "x.noise");
    CHECK_NE(two.checksum(), three.checksum());
}

}  // namespace

int main() {
    test_the_incremental_composite_equals_one_built_from_scratch();
    test_the_stack_blends_bottom_first();
    test_repairing_a_rectangle_leaves_the_rest_of_the_canvas_alone();
    test_the_stack_keeps_a_usable_selection();
    test_names_are_generated_from_the_stack_size();
    test_the_document_checksum_follows_the_stack();
    return noise::test::summary("document");
}
