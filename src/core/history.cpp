// SPDX-License-Identifier: Apache-2.0
#include "core/history.h"

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
                                  .size = {}});
    drop_oldest_until_within_budget();
}

void History::begin_step() { open_step_ = ++next_step_; }

void History::end_step() { open_step_ = 0; }

void History::record_layer_created(std::uint64_t layer_id, std::string name, int index,
                                   Extent size) {
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
                                  .name = std::move(name),
                                  .index = index,
                                  .size = size});
    drop_oldest_until_within_budget();
}

bool History::can_undo() const { return !undo_stack_.empty(); }
bool History::can_redo() const { return !redo_stack_.empty(); }

std::optional<History::Applied> History::undo(std::vector<Layer>& layers) {
    return step_back(undo_stack_, redo_stack_, layers);
}

std::optional<History::Applied> History::redo(std::vector<Layer>& layers) {
    return step_back(redo_stack_, undo_stack_, layers);
}

void History::drop_layer(std::uint64_t layer_id) {
    drop_entries([layer_id](const Stacked& entry) { return entry.layer == layer_id; });
}

void History::drop_mask(std::uint64_t layer_id) {
    drop_entries([layer_id](const Stacked& entry) {
        return entry.layer == layer_id && entry.plane == LayerPlane::mask;
    });
}

void History::drop_entries(const std::function<bool(const Stacked&)>& names) {
    for (std::vector<Stacked>* stack : {&undo_stack_, &redo_stack_}) {
        for (std::size_t index = stack->size(); index-- > 0;) {
            if (names((*stack)[index])) {
                bytes_ -= (*stack)[index].bytes.size();
                stack->erase(stack->begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
    }
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
    }
    return false;
}

void History::step_structure(Stacked& entry, std::vector<Stacked>& into,
                             std::vector<Layer>& layers, int step) {
    if (entry.kind == Kind::layer_created) {
        for (std::size_t at = 0; at < layers.size(); ++at) {
            if (layers[at].id != entry.layer) {
                continue;
            }
            Layer gone = std::move(layers[at]);
            layers.erase(layers.begin() + static_cast<std::ptrdiff_t>(at));
            entry.index = static_cast<int>(at);
            entry.size = gone.pixels.size();
            entry.name = std::move(gone.name);
            break;
        }
        entry.kind = Kind::layer_absent;
        entry.step = step;  // the pair is one step: an old number here would split it in two
        into.push_back(entry);
        return;
    }
    Layer added;
    added.id = entry.layer;
    added.name = entry.name;
    added.pixels = Image::filled(entry.size, 0, 0, 0, 0);
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
                                                   std::vector<Layer>& layers) {
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
    const bool structure_first = std::any_of(group.begin(), group.end(),
                                             [](const Stacked& entry) {
                                                 return entry.kind == Kind::layer_absent;
                                             });
    std::vector<std::size_t> order;
    order.reserve(group.size());
    for (const bool structural : {structure_first, !structure_first}) {
        for (std::size_t index = group.size(); index-- > 0;) {
            if ((group[index].kind != Kind::pixels) == structural) {
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
            step_structure(entry, into, layers, step);
            touched.structure = true;
            continue;
        }
        Layer& layer = *layer_of(layers, entry.layer);
        bytes_ -= entry.bytes.size();
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
                                   .size = {}});
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
                                   .size = {}});
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
    while (bytes_ > budget_bytes_ && !undo_stack_.empty()) {
        const int step = undo_stack_.front().step;
        while (!undo_stack_.empty() && undo_stack_.front().step == step) {
            bytes_ -= undo_stack_.front().bytes.size();
            undo_stack_.erase(undo_stack_.begin());
        }
    }
}

}  // namespace noise
