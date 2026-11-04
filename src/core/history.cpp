// SPDX-License-Identifier: Apache-2.0
#include "core/history.h"

#include "core/log.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace noise {

namespace {

void paste_rect(Image& image, Rect rect, const std::uint8_t* rgba) {
    const std::size_t row = static_cast<std::size_t>(rect.w) * 4;
    for (int y = 0; y < rect.h; ++y) {
        std::memcpy(image.at(rect.x, rect.y + y), rgba + static_cast<std::size_t>(y) * row, row);
    }
}

std::vector<std::uint8_t> copy_of(const Image& image) {
    const std::span<const std::uint8_t> bytes = image.pixels();
    return {bytes.begin(), bytes.end()};
}

// The coverage equivalent of `paste_rect`: one byte per pixel, rows addressed through the mask's own
// accessors so the plane's derived facts are dropped along with the write.
void paste_coverage(Mask& mask, Rect rect, const std::uint8_t* bytes) {
    const std::size_t row = static_cast<std::size_t>(rect.w);
    for (int y = 0; y < rect.h; ++y) {
        std::copy_n(bytes + static_cast<std::size_t>(y) * row, row,
                    mask.row(rect.y + y).begin() + rect.x);
    }
}

void exchange_masks(std::optional<Mask>& live, std::optional<Mask>& stored) {
    if (live && stored) {
        Mask held{};
        held = std::move(*live);
        *live = std::move(*stored);
        *stored = std::move(held);
    } else if (live) {
        stored.emplace(std::move(*live));
        live.reset();
    } else if (stored) {
        live.emplace(std::move(*stored));
        stored.reset();
    }
}

}  // namespace

History::History(std::size_t budget_bytes) : budget_bytes_(budget_bytes) {}

void History::record(std::uint64_t layer_id, LayerPlane plane, Rect rect,
                     std::vector<std::uint8_t> before) {
    redo_stack_.clear();
    if (rect.empty()) {
        return;
    }
    // One record is one step unless a `begin_step` has a step open; entries sharing a number - which an
    // operation touching several rectangles or the layer stack can produce - are undone, redone and
    // dropped together.
    const int step = open_step_ != 0 ? open_step_ : ++next_step_;
    bytes_ += before.size();
    undo_stack_.push_back(Stacked{.layer = layer_id,
                                  .rect = rect,
                                  .bytes = std::move(before),
                                  .step = step,
                                  .kind = Kind::pixels,
                                  .plane = plane,
                                  .name = {},
                                  .index = 0,
                                  .size = {},
                                  .order = {},
                                  .whole = {},
                                   .stack = {},
                                   .marked = {},
                                   .mask = {}});
    drop_oldest_until_within_budget();
}

void History::begin_step() { open_step_ = ++next_step_; }

void History::end_step() { open_step_ = 0; }

void History::record_layer_created(std::uint64_t layer_id, int index, Extent size) {
    redo_stack_.clear();
    const int step = open_step_ != 0 ? open_step_ : ++next_step_;
    // No `bytes_`: the layer's own pixels are held by the pixel entries recorded alongside this one, and
    // counting them twice would make the budget lie about what the history costs.
    undo_stack_.push_back(Stacked{.layer = layer_id,
                                  .rect = {},
                                  .bytes = {},
                                  .step = step,
                                  .kind = Kind::layer_created,
                                  .plane = LayerPlane::pixels,
                                  .name = {},
                                  .index = index,
                                  .size = size,
                                  .order = {},
                                  .whole = {},
                                   .stack = {},
                                   .marked = {},
                                   .mask = {}});
    drop_oldest_until_within_budget();
}

std::size_t History::held(const Stacked& entry) {
    // The name a `layer_renamed` entry is the whole of, and a small part of a layer that came whole.
    std::size_t bytes = entry.bytes.size() + entry.name.size();
    bytes += entry.whole.pixels.bytes();
    if (entry.whole.mask) {
        bytes += entry.whole.mask->plane().size();
    }
    // A `stack_replaced` entry carries its whole layers in `stack`, and they are nearly all of what the
    // step costs.
    for (const Layer& layer : entry.stack) {
        bytes += layer.pixels.bytes();
        if (layer.mask) {
            bytes += layer.mask->plane().size();
        }
    }
    if (entry.marked) {
        bytes += entry.marked->plane().size();
    }
    if (entry.mask) {
        bytes += entry.mask->plane().size();
    }
    return bytes;
}

// True for the entries that put a layer into the stack when applied, and so have to run before the
// pixel entries of the same step look for it.
bool History::inserts(const Stacked& entry) { return entry.kind == Kind::layer_absent; }

void History::record_coverage(std::optional<Mask> marked) {
    redo_stack_.clear();
    Stacked entry;
    entry.kind = Kind::coverage;
    entry.step = open_step_ != 0 ? open_step_ : ++next_step_;
    entry.marked = std::move(marked);
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

void History::record_layer_mask(std::uint64_t layer_id, std::optional<Mask> mask) {
    redo_stack_.clear();
    Stacked entry;
    entry.kind = Kind::mask_replaced;
    entry.step = open_step_ != 0 ? open_step_ : ++next_step_;
    entry.layer = layer_id;
    entry.plane = LayerPlane::mask;
    entry.mask = std::move(mask);
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

void History::record_layer_renamed(std::uint64_t layer_id, std::string previous_name) {
    redo_stack_.clear();
    Stacked entry;
    entry.layer = layer_id;
    entry.kind = Kind::layer_renamed;
    entry.step = open_step_ != 0 ? open_step_ : ++next_step_;
    entry.name = std::move(previous_name);
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

void History::record_stack_replaced(std::vector<Layer> stack) {
    redo_stack_.clear();
    Stacked entry;
    entry.kind = Kind::stack_replaced;
    entry.step = open_step_ != 0 ? open_step_ : ++next_step_;
    entry.stack = std::move(stack);
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

void History::record_canvas_replaced(std::vector<Layer> stack, std::optional<Mask> marked) {
    redo_stack_.clear();
    Stacked entry;
    entry.kind = Kind::canvas_replaced;
    entry.step = open_step_ != 0 ? open_step_ : ++next_step_;
    entry.stack = std::move(stack);
    entry.marked = std::move(marked);
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

void History::record_layer_order(std::vector<std::uint64_t> order) {
    redo_stack_.clear();
    const int step = open_step_ != 0 ? open_step_ : ++next_step_;
    // No bytes: an order is a list of ids, and the layers it names hold their own.
    undo_stack_.push_back(Stacked{.layer = 0,
                                  .rect = {},
                                  .bytes = {},
                                  .step = step,
                                  .kind = Kind::layer_order,
                                  .plane = LayerPlane::pixels,
                                  .name = {},
                                  .index = 0,
                                  .size = {},
                                  .order = std::move(order),
                                  .whole = {},
                                   .stack = {},
                                   .marked = {},
                                   .mask = {}});
    drop_oldest_until_within_budget();
}

void History::record_layer_removed(Layer layer, int index) {
    // One removal per step, checked rather than trusted: a second one in the same step undoes as a
    // shuffle, because each index was read off the stack as it stood and the step puts both back into one
    // that has since shrunk. A step taking more than one layer away replaces the stack, which is
    // `record_stack_replaced` - and a step that can be applied but not put back goes, rather than being
    // kept to half-restore.
    const int opening = open_step_ != 0 ? open_step_ : next_step_ + 1;
    for (const Stacked& entry : undo_stack_) {
        if (entry.step == opening && entry.kind == Kind::layer_absent) {
            log::error("a step cannot remove two layers: '" + layer.name +
                       "' joins a removal already recorded - the history goes rather than undoing to a "
                       "shuffled stack");
            clear();
            return;
        }
    }
    redo_stack_.clear();
    const int step = open_step_ != 0 ? open_step_ : ++next_step_;
    Stacked entry{.layer = layer.id,
                  .rect = {},
                  .bytes = {},
                  .step = step,
                  .kind = Kind::layer_absent,
                  .plane = LayerPlane::pixels,
                  .name = {},
                  .index = index,
                  .size = layer.pixels.size(),
                  .order = {},
                  .whole = std::move(layer),
                  .stack = {},
                  .marked = {},
                  .mask = {}};
    bytes_ += held(entry);
    undo_stack_.push_back(std::move(entry));
    drop_oldest_until_within_budget();
}

bool History::can_undo() const { return !undo_stack_.empty(); }
bool History::can_redo() const { return !redo_stack_.empty(); }

std::optional<History::Applied> History::undo(std::vector<Layer>& layers,
                                             std::optional<Mask>& marked) {
    return step_back(undo_stack_, redo_stack_, layers, marked);
}

std::optional<History::Applied> History::redo(std::vector<Layer>& layers,
                                             std::optional<Mask>& marked) {
    return step_back(redo_stack_, undo_stack_, layers, marked);
}

void History::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
    bytes_ = 0;
}

HistoryStats History::stats() const {
    HistoryStats out;
    out.undo_depth = steps_in(undo_stack_);
    out.redo_depth = steps_in(redo_stack_);
    out.bytes = bytes_;
    return out;
}

Layer* History::layer_of(std::vector<Layer>& layers, std::uint64_t id) {
    for (Layer& layer : layers) {
        if (layer.id == id) {
            return &layer;
        }
    }
    return nullptr;
}

bool History::fits(const Layer* layer, const Stacked& entry) {
    switch (entry.kind) {
        case Kind::pixels:
            if (layer == nullptr) {
                return false;
            }
            return entry.plane == LayerPlane::pixels
                       ? entry.rect.inside(layer->pixels.size())
                       : layer->mask.has_value() && entry.rect.inside(layer->mask->size());
        case Kind::layer_created:
            // Stepping back removes it, so it has to be there, and still the size it was created at.
            return layer != nullptr && layer->pixels.size() == entry.size;
        case Kind::layer_absent:
            // Stepping forward re-inserts it, so it must not already be there. A document always keeps
            // at least the layer the answer was made on, so re-inserting can never overflow a stack and
            // removing can never empty one.
            return layer == nullptr;
        case Kind::layer_order:
            // Checked against the whole stack by `fits_stack`.
            return true;
        case Kind::layer_renamed:
            // The name belongs to a layer, so the layer has to be there. The name itself always fits.
            return layer != nullptr;
        case Kind::stack_replaced:
        case Kind::canvas_replaced:
            return true;
        case Kind::coverage:
            // Checked against the canvas by `fits_coverage`; it reads no layer.
            return true;
        case Kind::mask_replaced:
            // The swap needs the layer and needs the plane to be the size of its picture - a mask of
            // another canvas would hide pixels that are not there. Whether a mask is already present is
            // no problem: the entry holds whichever side the layer is on.
            return layer != nullptr &&
                   (!entry.mask || entry.mask->size() == layer->pixels.size());
    }
    return false;
}

bool History::fits_coverage(const std::vector<Layer>& layers, const Stacked& entry) {
    if (entry.kind != Kind::coverage || !entry.marked) {
        return true;
    }
    if (layers.empty()) {
        return false;
    }
    return entry.marked->size() == layers.front().pixels.size();
}

bool History::fits_stack(const std::vector<Layer>& layers, const Stacked& entry) {
    if (entry.kind != Kind::layer_order) {
        return true;
    }
    // The entry has to name exactly the layers that are there: one missing or one invented and the
    // order it would impose is not the order of anything, so the step is refused rather than a stack
    // quietly losing a layer.
    if (entry.order.size() != layers.size()) {
        return false;
    }
    for (const std::uint64_t id : entry.order) {
        const bool present = std::any_of(layers.begin(), layers.end(),
                                         [id](const Layer& layer) { return layer.id == id; });
        if (!present) {
            return false;
        }
    }
    return true;
}

void History::step_structure(Stacked& entry, std::vector<Stacked>& into,
                             std::vector<Layer>& layers, std::optional<Mask>& marked, int step) {
    if (entry.kind == Kind::coverage) {
        // The entry leaves holding the coverage the document had, which is what the other direction
        // needs: the two directions are the same swap, and `nullopt` - no marked area - travels
        // through it exactly like a plane does.
        exchange_masks(marked, entry.marked);
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    if (entry.kind == Kind::mask_replaced) {
        // The layer leaves holding the plane the entry arrived with, and the entry leaves holding the
        // one the layer had: the same swap in both directions, so a mask taken off comes back with the
        // coverage it was hidden by rather than as a fresh one.
        if (Layer* target = layer_of(layers, entry.layer)) {
            exchange_masks(target->mask, entry.mask);
        }
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    if (entry.kind == Kind::canvas_replaced) {
        std::swap(layers, entry.stack);
        exchange_masks(marked, entry.marked);
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    if (entry.kind == Kind::stack_replaced) {
        // The entry leaves holding the stack that is current now, which is what the other direction
        // needs: undo and redo are the same swap, so a layer cannot come back in the wrong place.
        std::swap(layers, entry.stack);
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    if (entry.kind == Kind::layer_renamed) {
        // One field holds the name that is not current, so this is the whole of both directions. The
        // layer is there because the pass above checked it.
        std::swap(layer_of(layers, entry.layer)->name, entry.name);
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    if (entry.kind == Kind::layer_created) {
        for (std::size_t at = 0; at < layers.size(); ++at) {
            if (layers[at].id != entry.layer) {
                continue;
            }
            Layer gone = std::move(layers[at]);
            layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(at));
            entry.index = static_cast<int>(at);
            entry.size = gone.pixels.size();
            entry.whole = std::move(gone);
            break;
        }
        entry.kind = Kind::layer_absent;
        entry.step = step;  // the pair is one step: an old number here would split it in two
        into.push_back(entry);
        return;
    }
    if (entry.kind == Kind::layer_order) {
        std::vector<std::uint64_t> displaced;
        displaced.reserve(layers.size());
        for (const Layer& layer : layers) {
            displaced.push_back(layer.id);
        }
        std::vector<Layer> wanted;
        wanted.reserve(entry.order.size());
        for (const std::uint64_t id : entry.order) {
            if (Layer* layer = layer_of(layers, id)) {
                wanted.push_back(std::move(*layer));
            }
        }
        entry.order = std::move(displaced);
        layers = std::move(wanted);
        entry.step = step;
        into.push_back(std::move(entry));
        return;
    }
    // A layer comes back as the whole thing it was: pixels, mask, opacity, blend mode, and the id a
    // panel card is keyed to. Both roads reach here carrying it - `record_layer_removed` files the layer
    // it took, and undoing a creation moves the live layer into the entry - so an empty stand-in would
    // not be a fallback but a lost edit, and the fill a layer arrived wearing needs no entry of its own.
    Layer added = std::move(entry.whole);
    const std::size_t at = entry.index < 0 ? 0
                                           : std::min(static_cast<std::size_t>(entry.index),
                                                      layers.size());
    layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(at), std::move(added));
    entry.kind = Kind::layer_created;
    entry.step = step;
    into.push_back(entry);
}

std::optional<History::Applied> History::step_back(std::vector<Stacked>& from,
                                                   std::vector<Stacked>& into,
                                                   std::vector<Layer>& layers,
                                                   std::optional<Mask>& marked) {
    if (from.empty()) {
        return std::nullopt;
    }
    std::vector<Stacked> group;
    pop_step(from, group);


    // The order a step goes back in. Pixel entries are restored newest-first, as they came. A structural
    // entry goes **first when it re-inserts a layer** - the pixels of the same step need somewhere to
    // land - and **last when it removes one**, because those pixels have to be put back before the layer
    // that held them is taken away. Getting this backwards does not fail loudly: the pixel entry finds no
    // layer at all.
    // Three passes, in the only order that can work: a layer the step puts back first, so the pixel
    // entries of the same step have somewhere to land; then the pixels; then whatever takes a layer
    // away, after its pixels have been put back. An order entry moves layers without touching a pixel, so
    // it belongs in the last pass with the rest of the structure.
    std::vector<std::size_t> order;
    order.reserve(group.size());
    for (const int pass : {0, 1, 2}) {
        for (std::size_t index = group.size(); index-- > 0;) {
            const Stacked& entry = group[index];
            const bool mine = pass == 0
                                  ? inserts(entry)
                                  : (pass == 1 ? entry.kind == Kind::pixels
                                               : entry.kind != Kind::pixels && !inserts(entry));
            if (mine) {
                order.push_back(index);
            }
        }
    }

    // Validated in that same order, tracking the layers the step itself puts back: a check against the
    // state the step *starts* in cannot see a layer the step re-inserts, and would refuse a step that is
    // perfectly applicable. Anything that still fails is the caller having lost track - the same class of
    // lie as a resized layer - and the timeline goes rather than half a step being applied.
    std::optional<Extent> reinserted;
    std::uint64_t reinserted_id = 0;
    for (const std::size_t index : order) {
        const Stacked& entry = group[index];
        Layer* found = layer_of(layers, entry.layer);
        if (entry.kind == Kind::layer_order) {
            if (!fits_stack(layers, entry)) {
                clear();
                return std::nullopt;
            }
            continue;
        }
        if (entry.kind == Kind::stack_replaced || entry.kind == Kind::canvas_replaced) {
            continue;
        }
        if (entry.kind == Kind::coverage) {
            if (!fits_coverage(layers, entry)) {
                clear();
                return std::nullopt;
            }
            continue;
        }
        if (entry.kind == Kind::layer_absent) {
            if (!fits(found, entry)) {
                clear();
                return std::nullopt;
            }
            reinserted = entry.size;
            reinserted_id = entry.layer;
            continue;
        }
        if (entry.kind == Kind::layer_created) {
            if (!fits(found, entry)) {
                clear();
                return std::nullopt;
            }
            if (entry.layer == reinserted_id) {
                reinserted.reset();
                reinserted_id = 0;
            }
            continue;
        }
        const bool writable =
            found ? fits(found, entry)
                  : reinserted.has_value() && entry.layer == reinserted_id &&
                        entry.plane == LayerPlane::pixels && entry.rect.inside(*reinserted);
        if (!writable) {
            clear();
            return std::nullopt;
        }
    }

    const int step = ++next_step_;
    Applied touched;
    for (const std::size_t index : order) {
        Stacked entry = std::move(group[index]);
        touched.layer = entry.layer;
        touched.rect = unite(touched.rect, entry.rect);
        if (entry.kind != Kind::pixels) {
            const std::size_t carried = held(entry);
            const bool paints_nothing =
                entry.kind == Kind::coverage || entry.kind == Kind::layer_renamed;
            touched.canvas_changed = touched.canvas_changed || entry.kind == Kind::canvas_replaced;
            step_structure(entry, into, layers, marked, step);
            // A structural entry trades content with the stack, so what it costs the budget changes with
            // the trade: a step that takes a layer away starts paying for it, and one that gives it back
            // stops. The entry is moved by now, so the price of what stays is read where it landed - a
            // `stack_replaced` entry leaves holding the stack the document just gave up, and pricing that
            // as nothing underflows `bytes_` and drops the whole timeline.
            const std::size_t replaced = held(into.back());
            if (replaced >= carried) {
                bytes_ += replaced - carried;
            } else {
                bytes_ -= carried - replaced;
            }
            // Trading coverage or a name put back no pixel and moved no layer, so it is not
            // `structure`: telling the document it was would repaint the whole canvas to show a mark
            // moving or a card relabelling itself.
            touched.structure = touched.structure || !paints_nothing;
            continue;
        }
        Layer& layer = *layer_of(layers, entry.layer);
        bytes_ -= held(entry);
        // The bytes this step displaces are what makes the step repeatable, and they come from the
        // same plane the entry was recorded against.
        if (entry.plane == LayerPlane::pixels) {
            const Image displaced = layer.pixels.region(entry.rect);
            into.push_back(Stacked{.layer = entry.layer,
                                   .rect = entry.rect,
                                   .bytes = copy_of(displaced),
                                   .step = step,
                                   .kind = entry.kind,
                                   .plane = entry.plane,
                                    .name = {},
                                    .index = 0,
                                   .size = {},
                                   .order = {},
                                   .whole = {},
                                   .stack = {},
                                   .marked = {},
                                   .mask = {}});
        } else {
            const Mask displaced = layer.mask->region(entry.rect);
            into.push_back(Stacked{.layer = entry.layer,
                                   .rect = entry.rect,
                                   .bytes = displaced.plane(),
                                   .step = step,
                                   .kind = entry.kind,
                                   .plane = entry.plane,
                                    .name = {},
                                    .index = 0,
                                   .size = {},
                                   .order = {},
                                   .whole = {},
                                   .stack = {},
                                   .marked = {},
                                   .mask = {}});
        }
        bytes_ += into.back().bytes.size();
        if (entry.plane == LayerPlane::pixels) {
            paste_rect(layer.pixels, entry.rect, entry.bytes.data());
        } else {
            paste_coverage(*layer.mask, entry.rect, entry.bytes.data());
        }
    }
    return touched;
}

void History::pop_step(std::vector<Stacked>& stack, std::vector<Stacked>& moved) {
    const int step = stack.back().step;
    while (!stack.empty() && stack.back().step == step) {
        moved.push_back(std::move(stack.back()));
        stack.pop_back();
    }
}

int History::steps_in(const std::vector<Stacked>& stack) {
    int steps = 0;
    int previous = 0;
    for (const Stacked& entry : stack) {
        steps += entry.step != previous ? 1 : 0;
        previous = entry.step;
    }
    return steps;
}

void History::drop_oldest_until_within_budget() {
    // Never drop the last step: a geometry edit must remain undoable even when one
    // old full-canvas snapshot alone exceeds the nominal history budget.
    while (bytes_ > budget_bytes_ && steps_in(undo_stack_) > 1) {
        const int step = undo_stack_.front().step;
        while (!undo_stack_.empty() && undo_stack_.front().step == step) {
            bytes_ -= held(undo_stack_.front());
            undo_stack_.erase(undo_stack_.begin());
        }
    }
}

}  // namespace noise
