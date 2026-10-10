// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace noise;

namespace {

Image make_pattern(Extent size) {
    Image image = Image::filled(size, 0, 0, 0, 0);
    for (int y = 0; y < size.h; ++y) {
        for (int x = 0; x < size.w; ++x) {
            std::uint8_t* pixel = image.at(x, y);
            pixel[0] = static_cast<std::uint8_t>((x * 3 + y) & 0xff);
            pixel[1] = static_cast<std::uint8_t>((x + y * 7) & 0xff);
            pixel[2] = static_cast<std::uint8_t>((x * y) & 0xff);
            pixel[3] = static_cast<std::uint8_t>((x % 3 == 0) ? 0 : 255);
        }
    }
    return image;
}

// The stack a document opens with, in the shape `Document` takes.
std::vector<Layer> one_layer(Image contents) {
    std::vector<Layer> layers;
    Layer layer;
    layer.name = default_layer_name(0);
    layer.pixels = std::move(contents);
    layers.push_back(std::move(layer));
    return layers;
}

std::uint64_t id_of(const Document& doc, std::size_t index) { return doc.layer(index).id; }

void paint_rect(Image& image, Rect rect, std::uint8_t grey) {
    const std::uint8_t rgba[4] = {grey, grey, grey, 255};
    image.fill_rect(rect, rgba);
}

// A mutation the way a tool makes one: fill a rectangle and declare which one it was.
void fill(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, [rect, grey](Image& image) { paint_rect(image, rect, grey); });
}

// A layer holding both planes, so an entry naming one of them can be shown not to touch the other.
std::vector<Layer> one_layer_masked(std::uint8_t coverage) {
    std::vector<Layer> layers = one_layer(Image::filled(Extent{8, 8}, 200, 10, 10, 255));
    Mask mask(Extent{8, 8});
    mask.fill(coverage);
    layers.front().mask = mask;
    return layers;
}

std::size_t differing_coverage(const Mask& a, const Mask& b) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < a.plane().size(); ++i) {
        changed += a.plane()[i] != b.plane()[i] ? 1u : 0u;
    }
    return changed;
}

std::size_t differing_pixels(const Image& a, const Image& b) {
    std::size_t changed = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        changed += a.pixels()[i] != b.pixels()[i] ? 1u : 0u;
    }
    return changed;
}

void test_undo_after_many_mutations_restores_every_byte() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{64, 48})), std::nullopt, "pattern.png");
    const std::string original = doc.checksum();

    int grey = 8;
    for (int i = 0; i < 12; ++i) {
        fill(doc, Rect{i * 3, (i * 5) % 40, 9 + i, 7 + (i % 6)}, static_cast<std::uint8_t>(grey += 13));
        CHECK(doc.can_undo());
    }
    const std::string painted = doc.checksum();
    CHECK_NE(painted, original);
    CHECK_EQ(doc.history().undo_depth, 12);
    CHECK_EQ(doc.history().redo_depth, 0);

    for (int i = 11; i >= 0; --i) {
        CHECK(doc.undo());
        CHECK_EQ(doc.history().undo_depth, i);
        CHECK_EQ(doc.history().redo_depth, 12 - i);
    }
    CHECK_EQ(doc.checksum(), original);
    CHECK(!doc.can_undo());

    // Redo runs the same steps forward and lands on the same bytes, not an approximation.
    for (int i = 0; i < 12; ++i) {
        CHECK(doc.redo());
    }
    CHECK_EQ(doc.checksum(), painted);
    CHECK(!doc.can_redo());
}

void test_undo_touches_only_the_rectangle_it_recorded() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{40, 30})), std::nullopt, "pattern.png");
    const std::string untouched = doc.checksum();

    fill(doc, Rect{5, 4, 12, 9}, 200);
    const std::string after_first_step = doc.checksum();
    fill(doc, Rect{20, 2, 6, 6}, 30);
    CHECK(doc.undo());

    // Putting one rectangle back must leave every other pixel exactly as it was, so the
    // result has to equal a document that only ever ran the first step.
    Document reference;
    reference.reset(one_layer(make_pattern(Extent{40, 30})), std::nullopt, "pattern.png");
    fill(reference, Rect{5, 4, 12, 9}, 200);
    CHECK_EQ(differing_pixels(reference.layer(0).pixels, doc.layer(0).pixels), 0u);
    CHECK_EQ(doc.checksum(), after_first_step);
    CHECK_NE(doc.checksum(), untouched);
}

void test_a_mutation_after_an_undo_forks_the_timeline() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{24, 24})), std::nullopt, "pattern.png");
    fill(doc, Rect{0, 0, 4, 4}, 10);
    fill(doc, Rect{8, 8, 4, 4}, 20);
    CHECK_EQ(doc.history().redo_depth, 0);
    CHECK(doc.undo());
    CHECK_EQ(doc.history().redo_depth, 1);
    fill(doc, Rect{16, 0, 4, 4}, 30);
    CHECK_EQ(doc.history().redo_depth, 0);
}

void test_opening_a_document_starts_a_new_timeline() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "a.png");
    fill(doc, Rect{0, 0, 4, 4}, 40);
    CHECK(doc.can_undo());
    doc.reset(one_layer(make_pattern(Extent{4, 4})), std::nullopt, "b.png");
    CHECK(!doc.can_undo());
    CHECK(!doc.can_redo());
    CHECK_EQ(doc.history().bytes, 0u);
    CHECK(!doc.undo());
    CHECK(!doc.redo());
}

void test_a_change_outside_the_pixels_is_not_a_step() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{12, 12})), std::nullopt, "pattern.png");
    const std::string before = doc.checksum();
    const std::uint64_t revision = doc.revision();
    doc.paint(Rect{40, 40, 8, 8},
              [](Image& image) { paint_rect(image, Rect{0, 0, 12, 12}, 0); });
    CHECK_EQ(doc.checksum(), before);
    CHECK_EQ(doc.revision(), revision);
    CHECK(!doc.can_undo());
}

void test_the_revision_moves_with_every_undoable_change() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{12, 12})), std::nullopt, "pattern.png");
    const std::uint64_t opened = doc.revision();
    fill(doc, Rect{1, 1, 3, 3}, 90);
    CHECK_EQ(doc.revision(), opened + 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.revision(), opened + 2);
    CHECK(doc.redo());
    CHECK_EQ(doc.revision(), opened + 3);
}

void test_the_budget_drops_whole_steps_oldest_first() {
    const std::size_t entry_bytes = 8u * 8u * 4u;
    History history(entry_bytes * 2u + entry_bytes / 2u);  // room for two, never three
    std::vector<Layer> layers = one_layer(Image::filled(Extent{8, 8}, 0, 0, 0, 0));
    Image& image = layers.front().pixels;
    const std::uint64_t only = layers.front().id;
    CHECK_EQ(history.stats().bytes, 0u);

    for (const std::uint8_t grey : {std::uint8_t{10}, std::uint8_t{20}, std::uint8_t{30}}) {
        history.record(only, LayerPlane::pixels, Rect{0, 0, 8, 8},
                     std::vector<std::uint8_t>(entry_bytes, grey));
        paint_rect(image, Rect{0, 0, 8, 8}, grey);
    }
    CHECK_EQ(history.stats().undo_depth, 2);
    CHECK_EQ(history.stats().bytes, entry_bytes * 2u);

    // The steps that survive are the newest two: dropping the oldest is a shorter history,
    // dropping the newest would be a lost edit.
    CHECK(history.undo(layers).has_value());
    CHECK(history.undo(layers).has_value());
    CHECK(!history.undo(layers).has_value());
    // The oldest step is gone, so undo stops at the before-image of the second one rather
    // than running all the way back to an empty image.
    CHECK_EQ(static_cast<int>(image.at(0, 0)[0]), 20);
}

// A step belongs to one layer, and putting it back must not disturb any other layer.
void test_undo_restores_one_layer_and_leaves_the_others_alone() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{20, 20})), std::nullopt, "pattern.png");
    doc.add_layer("Skies", Rgba8{0, 0, 0, 0});
    doc.add_layer("Ink", Rgba8{0, 0, 0, 0});
    CHECK_EQ(doc.layers().size(), 3u);
    const std::uint64_t ink = id_of(doc, 2);
    const std::uint64_t skies = id_of(doc, 1);
    const std::uint64_t base = id_of(doc, 0);

    fill(doc, Rect{2, 2, 6, 6}, 190);  // active is "Ink", the top layer
    const std::string base_after = doc.layer(0).pixels.checksum_hex();
    const std::string skies_after = doc.layer(1).pixels.checksum_hex();
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), base_after);
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), skies_after);
    CHECK_EQ(doc.layer(2).pixels.checksum_hex(),
             Image::filled(Extent{20, 20}, 0, 0, 0, 0).checksum_hex());
    CHECK_NE(ink, skies);
    CHECK_NE(skies, base);
}

// Painting on another layer after an undo is a different target, not a fork of the same one:
// each layer's steps stay undoable in order.
void test_each_layer_undoes_independently_and_in_order() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "pattern.png");
    doc.add_layer("Top", Rgba8{0, 0, 0, 0});
    fill(doc, Rect{0, 0, 4, 4}, 240);  // Top
    CHECK(doc.select_layer(0));
    fill(doc, Rect{8, 8, 4, 4}, 10);  // bottom
    CHECK_EQ(doc.history().undo_depth, 2);

    CHECK(doc.undo());  // the bottom layer's change
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(),
             make_pattern(Extent{16, 16}).checksum_hex());
    CHECK_NE(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK(doc.undo());  // Top's change
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK(!doc.can_undo());
}

// A change to a layer that no longer exists cannot be put back; the rest of the timeline can,
// so only the entries naming the deleted layer leave the stacks.
void test_deleting_a_layer_drops_only_its_own_steps() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "pattern.png");
    doc.add_layer("Temp", Rgba8{0, 0, 0, 0});
    fill(doc, Rect{0, 0, 4, 4}, 200);  // Temp
    CHECK(doc.select_layer(0));
    fill(doc, Rect{4, 4, 4, 4}, 60);  // bottom, still there
    CHECK_EQ(doc.history().undo_depth, 2);

    CHECK(doc.remove_layer(1));
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.history().undo_depth, 1);
    CHECK(doc.undo());
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), make_pattern(Extent{16, 16}).checksum_hex());
    CHECK(!doc.can_undo());
}

// A history naming a layer that was lost without `drop_layer` is a lie about where bytes go:
// it is dropped whole rather than written somewhere plausible.
// An entry names a plane as well as a rectangle: coverage goes back to the mask and not to the
// pixels, and the plane it did not name is left byte for byte as it stands.
void test_a_coverage_step_restores_the_mask_and_not_the_pixels() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    Layer& layer = layers.front();
    const std::uint64_t only = layer.id;
    const std::string pixels_before = layer.pixels.checksum_hex();
    History history(1u << 20);

    // The way a paint door uses it: record what was there, then change it.
    history.record(only, LayerPlane::mask, Rect{2, 2, 4, 4},
                   std::vector<std::uint8_t>(16, kMaskIn));
    layer.mask->op(Rect{2, 2, 4, 4}, MaskOp::subtract);
    CHECK_EQ(differing_coverage(*layer.mask, *one_layer_masked(kMaskIn).front().mask), 16u);

    const History::Applied undone = history.undo(layers).value();
    CHECK_EQ(undone.layer, only);
    CHECK_EQ(undone.rect.w * undone.rect.h, 16);
    CHECK_EQ(differing_coverage(*layer.mask, *one_layer_masked(kMaskIn).front().mask), 0u);
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);

    // And redo puts the hidden square back, still without touching a pixel.
    history.redo(layers);
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskOut);
    CHECK_EQ(layer.mask->coverage(1, 1), kMaskIn);
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);
}

// Taking a mask off leaves nothing for a coverage entry to restore, so those entries go - and the
// colour steps of the same layer stay, because losing an unrelated undo is worse.
void test_taking_a_mask_off_drops_only_its_coverage_steps() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    Layer& layer = layers.front();
    const std::uint64_t only = layer.id;
    History history(1u << 20);

    history.record(only, LayerPlane::pixels, Rect{0, 0, 2, 2},
                   std::vector<std::uint8_t>(2u * 2u * 4u, 7));
    history.record(only, LayerPlane::mask, Rect{4, 4, 2, 2},
                   std::vector<std::uint8_t>(4, kMaskIn));
    CHECK_EQ(history.stats().undo_depth, 2);

    history.drop_mask(only);
    CHECK_EQ(history.stats().undo_depth, 1);
    layer.mask = std::nullopt;
    // The colour step still works: it was never about the mask.
    CHECK(history.undo(layers).has_value());
    CHECK_EQ(history.stats().undo_depth, 0);
}

// A coverage entry whose rectangle no longer fits is the same class of lie as a resized layer: the
// whole timeline goes rather than half restoring a document.
void test_a_coverage_step_that_no_longer_fits_drops_the_timeline() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    const std::uint64_t only = layers.front().id;
    History history(1u << 20);
    history.record(only, LayerPlane::mask, Rect{4, 4, 2, 2},
                   std::vector<std::uint8_t>(4, kMaskIn));
    layers.front().mask = Mask(Extent{4, 4});   // a mask for a smaller canvas
    CHECK(!history.undo(layers).has_value());
    CHECK(!history.can_undo());
    CHECK(!history.can_redo());
}

void test_a_history_that_no_longer_fits_its_layers_is_dropped() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "pattern.png");
    fill(doc, Rect{0, 0, 4, 4}, 90);
    CHECK(doc.can_undo());
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "pattern.png");
    fill(doc, Rect{0, 0, 4, 4}, 91);
    CHECK(doc.can_undo());
    // Undo must still be exact for the live document: the recorded ids are fresh, and the old
    // document's steps are gone with it.
    CHECK(doc.undo());
    CHECK(!doc.can_undo());
}

}  // namespace

int main() {
    test_undo_after_many_mutations_restores_every_byte();
    test_undo_touches_only_the_rectangle_it_recorded();
    test_a_mutation_after_an_undo_forks_the_timeline();
    test_opening_a_document_starts_a_new_timeline();
    test_a_change_outside_the_pixels_is_not_a_step();
    test_the_revision_moves_with_every_undoable_change();
    test_the_budget_drops_whole_steps_oldest_first();
    test_a_coverage_step_restores_the_mask_and_not_the_pixels();
    test_taking_a_mask_off_drops_only_its_coverage_steps();
    test_a_coverage_step_that_no_longer_fits_drops_the_timeline();
    test_undo_restores_one_layer_and_leaves_the_others_alone();
    test_each_layer_undoes_independently_and_in_order();
    test_deleting_a_layer_drops_only_its_own_steps();
    test_a_history_that_no_longer_fits_its_layers_is_dropped();
    return noise::test::summary("history");
}
