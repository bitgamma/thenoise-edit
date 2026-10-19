// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/document.h"

#include <cstdint>
#include <cstring>
#include <optional>
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

// The stack as identities in order - the shape a shuffle is caught by and a name is not.
std::string ids_of_layers(const Document& doc) {
    std::string out;
    for (const Layer& layer : doc.layers()) {
        if (!out.empty()) {
            out += ",";
        }
        out += std::to_string(layer.id);
    }
    return out;
}

void paint_rect(Image& image, Rect rect, std::uint8_t grey) {
    const std::uint8_t rgba[4] = {grey, grey, grey, 255};
    image.fill_rect(rect, rgba);
}

// A mutation the way a tool makes one: fill a rectangle and declare which one it was.
void fill(Document& doc, Rect rect, std::uint8_t grey) {
    doc.paint(rect, PaintFence::selection,
              [rect, grey](Image& image) { paint_rect(image, rect, grey); });
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
    doc.paint(Rect{40, 40, 8, 8}, PaintFence::selection,
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
    std::optional<Mask> unmarked;  // these steps move layers, not the document's marked area
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
    CHECK(history.undo(layers, unmarked).has_value());
    CHECK(history.undo(layers, unmarked).has_value());
    CHECK(!history.undo(layers, unmarked).has_value());
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
    CHECK_EQ(doc.history().undo_depth, 3);  // the arrival, then one step per layer

    CHECK(doc.undo());  // the bottom layer's change
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(),
             make_pattern(Extent{16, 16}).checksum_hex());
    CHECK_NE(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK(doc.undo());  // Top's change
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK_EQ(doc.layers().size(), 2u);  // the layer is still there, empty
    CHECK(doc.undo());                  // and then Top's arrival, which takes it away
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK(!doc.can_undo());
}

// Deleting a layer is one step that hands the whole layer back: the id a panel card is keyed to, the
// name, the blend mode, the opacity, the mask and the picture. And the steps written on it are still
// underneath, waiting - a recorded departure leaves nothing to invalidate, which is the whole reason
// `History` no longer drops entries by layer id.
void test_deleting_a_layer_is_one_step_that_gives_it_back_whole() {
    Document doc;
    doc.reset(one_layer(make_pattern(Extent{16, 16})), std::nullopt, "pattern.png");
    doc.add_layer("Temp", Rgba8{0, 0, 0, 0});
    fill(doc, Rect{0, 0, 4, 4}, 200);  // Temp
    CHECK(doc.select_layer(0));
    fill(doc, Rect{4, 4, 4, 4}, 60);  // bottom, still there
    Mask hidden(Extent{16, 16});
    hidden.fill(kMaskOut);
    CHECK(doc.set_layer_mask(1, hidden));
    CHECK(doc.set_layer_opacity(1, 0.4));
    CHECK(doc.set_layer_mode(1, BlendMode::multiply));
    const std::uint64_t temp_id = doc.layer(1).id;
    const std::string temp_pixels = doc.layer(1).pixels.checksum_hex();
    const std::string bottom_pixels = doc.layer(0).pixels.checksum_hex();
    CHECK_EQ(doc.history().undo_depth, 4);  // arrival, Temp's paint, bottom's paint, the mask

    CHECK(doc.remove_layer(1));
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK_EQ(doc.history().undo_depth, 5);

    CHECK(doc.undo());
    REQUIRE(doc.layers().size() == 2u);
    CHECK_EQ(doc.layer(1).id, temp_id);      // the same layer, not a lookalike
    CHECK_EQ(doc.layer(1).name, std::string("Temp"));
    CHECK(doc.layer(1).mode == BlendMode::multiply);
    CHECK(doc.layer(1).opacity == 0.4);
    REQUIRE(doc.layer(1).mask.has_value());
    CHECK_EQ(doc.layer(1).mask->covered(), 0u);
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), temp_pixels);
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), bottom_pixels);  // the other layer did not move

    // The layer's own steps come back with it: undo reaches the paint on Temp and the mask through the
    // step that deleted the layer they were made on.
    CHECK(doc.undo());  // Temp's mask, put down
    CHECK(!doc.layer(1).mask.has_value());
    CHECK(doc.undo());  // the bottom's paint
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), make_pattern(Extent{16, 16}).checksum_hex());
    CHECK(doc.undo());  // Temp's paint
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK(doc.undo());  // Temp's arrival
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK(!doc.can_undo());

    // And forward again: the layer returns with its picture, then its mask, then the deletion.
    CHECK(doc.redo());
    CHECK_EQ(doc.layers().size(), 2u);
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(),
             Image::filled(Extent{16, 16}, 0, 0, 0, 0).checksum_hex());
    CHECK(doc.redo());  // Temp's paint
    CHECK_EQ(doc.layer(1).pixels.checksum_hex(), temp_pixels);
    CHECK(doc.redo());  // the bottom's paint, on the other layer
    CHECK_EQ(doc.layer(0).pixels.checksum_hex(), bottom_pixels);
    CHECK(doc.redo());  // Temp's mask
    REQUIRE(doc.layer(1).mask.has_value());
    CHECK_EQ(doc.layer(1).mask->covered(), 0u);
    CHECK(doc.redo());  // and the deletion
    CHECK_EQ(doc.layers().size(), 1u);
    CHECK(!doc.can_redo());
}

// A history naming a layer that went missing without a recorded departure is a lie about where bytes
// go: it is dropped whole rather than written somewhere plausible. Nothing in the editor can do this on
// purpose now that every departure is a step - it is the fence under the timeline, not a workflow.
// An entry names a plane as well as a rectangle: coverage goes back to the mask and not to the
// pixels, and the plane it did not name is left byte for byte as it stands.
void test_a_coverage_step_restores_the_mask_and_not_the_pixels() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    Layer& layer = layers.front();
    const std::uint64_t only = layer.id;
    const std::string pixels_before = layer.pixels.checksum_hex();
    History history(1u << 20);
    std::optional<Mask> unmarked;

    // The way a paint door uses it: record what was there, then change it.
    history.record(only, LayerPlane::mask, Rect{2, 2, 4, 4},
                   std::vector<std::uint8_t>(16, kMaskIn));
    layer.mask->op(Rect{2, 2, 4, 4}, MaskOp::subtract);
    CHECK_EQ(differing_coverage(*layer.mask, *one_layer_masked(kMaskIn).front().mask), 16u);

    const History::Applied undone = history.undo(layers, unmarked).value();
    CHECK_EQ(undone.layer, only);
    CHECK_EQ(undone.rect.w * undone.rect.h, 16);
    CHECK_EQ(differing_coverage(*layer.mask, *one_layer_masked(kMaskIn).front().mask), 0u);
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);

    // And redo puts the hidden square back, still without touching a pixel.
    history.redo(layers, unmarked);
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskOut);
    CHECK_EQ(layer.mask->coverage(1, 1), kMaskIn);
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);
}

// A mask plane arriving or going is one entry that swaps the plane, so the coverage a layer was hidden
// by comes back rather than a fresh one - and the coverage written into the mask before it went stays
// undoable, because undoing the swap hands that step its mask back first.
void test_a_mask_plane_comes_back_with_the_coverage_it_had() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    Layer& layer = layers.front();
    const std::uint64_t only = layer.id;
    const std::string pixels_before = layer.pixels.checksum_hex();
    const Mask pristine = *layer.mask;
    History history(1u << 20);
    std::optional<Mask> unmarked;

    // Coverage into the mask, the mask taken off, a fresh one put down in its place: three steps, and
    // the last two are the plane's rather than pixels'.
    history.record(only, LayerPlane::mask, Rect{2, 2, 4, 4}, std::vector<std::uint8_t>(16, kMaskIn));
    layer.mask->op(Rect{2, 2, 4, 4}, MaskOp::subtract);
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskOut);
    history.record_layer_mask(only, layer.mask);
    layer.mask = std::nullopt;
    Mask fresh(layer.pixels.size());
    fresh.fill(kMaskIn);
    history.record_layer_mask(only, std::nullopt);
    layer.mask = fresh;
    CHECK_EQ(history.stats().undo_depth, 3);
    // A swap trades payload with the document, so what the timeline costs cannot change by walking it:
    // pricing the wrong side of the swap underflows `bytes_`, and the budget then drops every step (see
    // the flatten note). The byte count says why the depth below would go wrong.
    const std::size_t priced = history.stats().bytes;

    const History::Applied took_off = history.undo(layers, unmarked).value();
    CHECK(!layer.mask.has_value());
    CHECK(took_off.structure);
    CHECK(!took_off.coverage);
    // The load-bearing byte: what comes back is the mask that was hidden by, not a new one.
    const History::Applied brought_back = history.undo(layers, unmarked).value();
    CHECK(layer.mask.has_value());
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskOut);
    CHECK_EQ(differing_coverage(*layer.mask, pristine), 16u);
    CHECK(brought_back.structure);
    // And the step written into it before it went is still there to be undone.
    history.undo(layers, unmarked);
    CHECK_EQ(differing_coverage(*layer.mask, pristine), 0u);
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);

    CHECK(history.redo(layers, unmarked).has_value());
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskOut);
    CHECK(history.redo(layers, unmarked).has_value());
    CHECK(!layer.mask.has_value());
    CHECK(history.redo(layers, unmarked).has_value());
    CHECK_EQ(layer.mask->coverage(2, 2), kMaskIn);  // the fresh one, put back as it was made
    CHECK_EQ(layer.pixels.checksum_hex(), pixels_before);
    CHECK_EQ(history.stats().bytes, priced);

    history.undo(layers, unmarked);
    history.undo(layers, unmarked);
    history.undo(layers, unmarked);
    CHECK_EQ(history.stats().bytes, priced);
}

// A plane of another canvas would hide pixels that are not there, so the step is refused and the
// timeline goes rather than a layer ending up wearing a mask that does not fit it.
void test_a_mask_plane_that_no_longer_fits_drops_the_timeline() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    const std::uint64_t only = layers.front().id;
    History history(1u << 20);
    std::optional<Mask> unmarked;

    Mask wrong_size(Extent{4, 4});
    wrong_size.fill(kMaskIn);
    history.record_layer_mask(only, std::move(wrong_size));
    CHECK(!history.undo(layers, unmarked).has_value());
    CHECK_EQ(history.stats().undo_depth, 0);
    CHECK_EQ(history.stats().redo_depth, 0);
    CHECK_EQ(history.stats().bytes, 0u);
}

// A coverage entry whose rectangle no longer fits is the same class of lie as a resized layer: the
// whole timeline goes rather than half restoring a document.
void test_a_coverage_step_that_no_longer_fits_drops_the_timeline() {
    std::vector<Layer> layers = one_layer_masked(kMaskIn);
    const std::uint64_t only = layers.front().id;
    History history(1u << 20);
    std::optional<Mask> unmarked;
    history.record(only, LayerPlane::mask, Rect{4, 4, 2, 2},
                   std::vector<std::uint8_t>(4, kMaskIn));
    layers.front().mask = Mask(Extent{4, 4});   // a mask for a smaller canvas
    CHECK(!history.undo(layers, unmarked).has_value());
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

// An answer arrives as a new layer, and `Ctrl+Z` has to take the whole thing away: leaving an empty
// layer named after a job behind is the half-undo this exists to prevent.
void test_an_added_layer_and_its_pixels_come_back_as_one_step() {
    Document doc;
    doc.reset(one_layer(Image::filled(Extent{8, 8}, 10, 20, 30, 255)), std::nullopt, "");
    const std::size_t layers_before = doc.layers().size();
    const int depth_before = doc.history().undo_depth;
    const Rect bounds{2, 2, 4, 4};

    const int index = doc.add_layer_written("Remove j0001", bounds, [bounds](Image& image) {
        paint_rect(image, bounds, 200);
        return true;
    });
    CHECK_EQ(index, 1);
    CHECK_EQ(doc.layers().size(), layers_before + 1);
    // One step, not two: the layer and what landed on it are the single edit the user made.
    CHECK_EQ(doc.history().undo_depth, depth_before + 1);

    CHECK(doc.undo());
    CHECK_EQ(doc.layers().size(), layers_before);
    CHECK_EQ(doc.active(), 0);  // not left pointing past the stack

    CHECK(doc.redo());
    REQUIRE(doc.layers().size() == layers_before + 1);
    CHECK_EQ(doc.layer(1).name, std::string("Remove j0001"));
    // The pixels come back with the layer. An empty re-inserted layer would be an answer that
    // silently vanished on redo.
    CHECK_EQ(doc.layer(1).pixels.at(3, 3)[0], 200);
    CHECK_EQ(doc.layer(1).pixels.at(7, 7)[3], 0);
    CHECK_EQ(doc.layer(0).pixels.at(3, 3)[0], 10);  // the layer under it never moved
}

void test_a_refused_write_adds_no_layer_and_records_nothing() {
    Document doc;
    doc.reset(one_layer(Image::filled(Extent{8, 8}, 10, 20, 30, 255)), std::nullopt, "");
    CHECK(!doc.can_undo());

    const int index = doc.add_layer_written("Nothing", Rect{0, 0, 4, 4}, [](Image&) { return false; });
    CHECK_EQ(index, -1);
    CHECK_EQ(doc.layers().size(), std::size_t(1));
    CHECK(!doc.can_undo());
}

void test_undoing_an_answer_leaves_the_layer_under_it_byte_for_byte() {
    Document doc;
    doc.reset(one_layer_masked(255), std::nullopt, "");
    fill(doc, Rect{1, 1, 3, 3}, 90);
    const Image base = doc.layer(0).pixels;
    const std::uint64_t base_id = id_of(doc, 0);

    doc.add_layer_written("Fill j0002", Rect{0, 0, 8, 8}, [](Image& image) {
        paint_rect(image, Rect{0, 0, 8, 8}, 123);
        return true;
    });
    CHECK(doc.undo());
    REQUIRE(doc.layers().size() == 1u);
    CHECK_EQ(id_of(doc, 0), base_id);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            CHECK_EQ(std::memcmp(base.at(x, y), doc.layer(0).pixels.at(x, y), 4), 0);
        }
    }
}

// A replaced stack is the most the history can hold, and it holds it on whichever side of the swap is
// not current. Get either half wrong - the order the layers go back in, or the bytes they cost - and
// nothing complains: the stack returns shuffled, or `bytes_` underflows and the budget "recovers" by
// dropping every step, leaving the undo key dead in a document that looks fine.
void test_a_replaced_stack_walks_back_and_forward_unchanged() {
    Document doc;
    std::vector<Layer> stack;
    for (int at = 0; at < 4; ++at) {
        Layer layer;
        layer.name = "layer" + std::to_string(at);
        layer.pixels = Image::filled(Extent{64, 48}, 40 * at, 10, 200 - 30 * at, 255);
        stack.push_back(std::move(layer));
    }
    doc.reset(std::move(stack), std::nullopt, "stack.noise");
    const std::string ids_original = ids_of_layers(doc);
    const std::string whole = doc.checksum();

    REQUIRE(doc.flatten_layers());
    const std::size_t holding_the_stack = doc.history().bytes;
    // The entry holds every layer of the stack it replaced, and that is nearly all of it.
    CHECK(holding_the_stack >= 4u * 64u * 48u * 4u);

    for (int round = 0; round < 3; ++round) {
        REQUIRE(doc.undo());
        CHECK_EQ(ids_of_layers(doc), ids_original);
        CHECK_EQ(doc.checksum(), whole);
        // Holding the one flattened layer rather than the four it swapped out is strictly cheaper.
        CHECK(doc.history().bytes < holding_the_stack);
        REQUIRE(doc.redo());
        CHECK_EQ(doc.layers().size(), 1u);
        // The price lands back on the same number each time: an entry that gained or lost bytes per
        // application walks the budget into the floor, and the floor drops every step.
        CHECK_EQ(doc.history().bytes, holding_the_stack);
        CHECK_EQ(doc.history().undo_depth, 1);
        CHECK(doc.can_undo());
    }
}

// Two removals in one step is not a step. Each index is read off the stack as it stands, so the second is
// recorded against a stack the first already shrank, and applying the pair back restores a shuffle - which
// is what flattening used to do before it recorded the replaced stack instead. A record that cannot be part
// of an exactly-reversible step is refused loudly: a silently missing entry is a person finding their undo
// key dead with no reason attached to it.
void test_two_removals_in_one_step_are_not_a_step() {
    auto three = [] {
        std::vector<Layer> layers;
        for (int at = 0; at < 3; ++at) {
            Layer layer;
            layer.name = "layer" + std::to_string(at);
            layer.pixels = Image::filled(Extent{4, 4}, 9, 90, 200 - 40 * at, 255);
            layers.push_back(std::move(layer));
        }
        return layers;
    };

    std::vector<Layer> recordable = three();
    History alone{kHistoryBudgetBytes};
    alone.begin_step();
    alone.record_layer_removed(std::move(recordable[2]), 2);
    alone.end_step();
    // The contrast: one removal per step is exactly what merge-down does, and it is kept.
    CHECK(alone.can_undo());

    recordable = three();
    History pair{kHistoryBudgetBytes};
    pair.begin_step();
    pair.record_layer_removed(std::move(recordable[2]), 2);
    pair.record_layer_removed(std::move(recordable[1]), 1);
    pair.end_step();
    // Not half a step either: the whole thing goes, because undoing the one entry it kept would leave the
    // other layer gone with nothing recorded about it.
    CHECK(!pair.can_undo());
    std::vector<Layer> live = three();
    std::optional<Mask> unmarked;
    CHECK(!pair.undo(live, unmarked).has_value());
    CHECK_EQ(live.size(), 3u);  // and a refused record applies nothing to the caller's stack
}

// The marked area is the document's own coverage: the entry holds it and applying it swaps the two,
// so the two directions are one operation, the plane is priced where it lands, and no layer's coming
// or going has anything to do with it.
void test_a_marked_coverage_step_swaps_and_survives_the_stack() {
    std::vector<Layer> layers = one_layer(Image::filled(Extent{4, 4}, 0, 0, 0, 0));
    History history(1u << 20);
    std::optional<Mask> marked;

    Mask box(Extent{4, 4});
    box.op(Rect{1, 1, 2, 2}, MaskOp::replace);

    // The step holds what the change replaced, which here was no marked area at all - and a step that
    // holds nothing costs nothing.
    history.record_coverage(marked);
    marked = box;
    CHECK_EQ(history.stats().bytes, 0u);
    CHECK_EQ(history.stats().undo_depth, 1);

    const History::Applied undone = history.undo(layers, marked).value();
    CHECK(!marked.has_value());
    CHECK(undone.coverage);
    CHECK(!undone.structure);  // no layer moved, so the canvas has nothing to repaint
    CHECK(undone.rect.empty());
    // The entry left holding the coverage the document gave up, and it is that plane the budget now
    // pays for - priced where it landed, which is the rule a swapped payload makes unforgiving.
    CHECK_EQ(history.stats().bytes, 16u);
    CHECK_EQ(history.stats().redo_depth, 1);

    history.redo(layers, marked);
    REQUIRE(marked.has_value());
    CHECK_EQ(marked->covered(), 4u);
    CHECK_EQ(history.stats().bytes, 0u);

    // A second mark, holding the first. Neither entry names a layer, so nothing a stack does - a layer
    // arriving, going, being reordered - can make either one inapplicable.
    history.record_coverage(marked);
    Mask strip(Extent{4, 4});
    strip.op(Rect{0, 0, 4, 1}, MaskOp::replace);
    marked = strip;
    CHECK_EQ(history.stats().undo_depth, 2);
    history.undo(layers, marked);
    REQUIRE(marked.has_value());
    CHECK_EQ(marked->covered(), 4u);

    // A marked area of another canvas is not a claim about these pixels: the step is refused and the
    // timeline goes rather than a document being left with coverage reaching past its own edge.
    layers.front().pixels = Image::filled(Extent{6, 6}, 0, 0, 0, 0);
    CHECK(!history.redo(layers, marked).has_value());
    CHECK(!history.can_undo());
    CHECK(!history.can_redo());
}

int main() {
    test_an_added_layer_and_its_pixels_come_back_as_one_step();
    test_a_refused_write_adds_no_layer_and_records_nothing();
    test_undoing_an_answer_leaves_the_layer_under_it_byte_for_byte();
    test_undo_after_many_mutations_restores_every_byte();
    test_undo_touches_only_the_rectangle_it_recorded();
    test_a_mutation_after_an_undo_forks_the_timeline();
    test_opening_a_document_starts_a_new_timeline();
    test_a_change_outside_the_pixels_is_not_a_step();
    test_the_revision_moves_with_every_undoable_change();
    test_the_budget_drops_whole_steps_oldest_first();
    test_a_coverage_step_restores_the_mask_and_not_the_pixels();
    test_a_mask_plane_comes_back_with_the_coverage_it_had();
    test_a_mask_plane_that_no_longer_fits_drops_the_timeline();
    test_a_coverage_step_that_no_longer_fits_drops_the_timeline();
    test_undo_restores_one_layer_and_leaves_the_others_alone();
    test_each_layer_undoes_independently_and_in_order();
    test_deleting_a_layer_is_one_step_that_gives_it_back_whole();
    test_a_history_that_no_longer_fits_its_layers_is_dropped();
    test_a_replaced_stack_walks_back_and_forward_unchanged();
    test_two_removals_in_one_step_are_not_a_step();
    test_a_marked_coverage_step_swaps_and_survives_the_stack();
    return noise::test::summary("history");
}
