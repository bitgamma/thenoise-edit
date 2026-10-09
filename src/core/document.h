// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/history.h"
#include "core/image.h"
#include "core/layer.h"

namespace noise {

// Budgeted in stored bytes rather than step count: one region operation on a large document
// is worth a great many dabs, and dropping whole steps is what keeps the budget predictable.
inline constexpr std::size_t kHistoryBudgetBytes = 128 * 1024 * 1024;

// A document with no layer has no canvas, and every command that edits pixels needs one, so
// the last layer cannot be removed.
inline constexpr std::size_t kMinLayers = 1;

// The open document: a stack of layers, and the history of what changed in them. Pixels change
// only through `paint`, a provisional gesture or undo/redo, so every visible change can be put
// back exactly - within one layer, which is what a step is recorded against.
class Document {
public:
    Document();

    // A new or freshly opened document starts a new timeline: `path` empty means "never
    // saved", which is what makes doc.save report itself instead of guessing a filename.
    // Ids are assigned here, so a document always owns the identity its history names and a
    // file cannot name a layer this document does not have.
    void reset(std::vector<Layer> layers, std::string path);

    const std::string& path() const;
    void set_path(std::string path);

    // Unsaved changes, conservative: undo does not clear it, because "back to what is on disk"
    // would mean holding the saved checksum as a second fact about the file.
    bool dirty() const;
    void mark_saved();

    bool empty() const;
    int width() const;
    int height() const;
    Extent size() const;

    // Bottom-first. `active` is the layer `paint` and a gesture edit.
    std::span<const Layer> layers() const;
    int active() const;
    const Layer& layer(std::size_t index) const;

    // An empty `name` gets the next generated one. The new layer goes directly above the active
    // layer, which is where a painter expects it, and becomes active. Returns its index, or -1
    // when there is no canvas to size it from.
    int add_layer(std::string name, Rgba8 fill);
    // False when `index` is out of range or it is the last layer standing.
    bool remove_layer(int index);
    // False when `index` is out of range. Selecting changes no pixels, so it bumps no revision.
    bool select_layer(int index);

    // Every layer's pixels, bottom-first, with the layer count in front: two documents with
    // different stacks differ here even when the flattened result happens to match.
    std::string checksum() const;

    // The flattened stack: what the renderer uploads, what a flat export writes, and what the
    // container stores as `mergedimage.png`. Built on demand and repaired only over the
    // rectangles the layers changed since it was last asked for, because a stack that is
    // re-blended whole puts a canvas-sized pass on every frame of a live stroke.
    const Image& composite() const;

    // Bumped by anything a step could undo and by anything that changes what the canvas shows;
    // the renderer re-uploads on it.
    std::uint64_t revision() const;

    // Applies `edit` to the active layer and records the pixels `bounds` held beforehand. An
    // edit that writes outside `bounds` will not be fully undone, which is why bounds is not
    // inferred.
    void paint(Rect bounds, const std::function<void(Image&)>& edit);

    // A gesture that is still going - a pointer stroke - shows its result before it ends, on
    // the layer that was active when it began. The pixels under `area` are held aside, every
    // repaint starts from them, and `commit` records them as one undo step. Without this a live
    // stroke would either compound its own opacity once per frame or push an undo step per
    // frame. Calling `begin_provisional` again widens the held-aside area, restoring the current
    // one first so the new snapshot is of pristine pixels.
    void begin_provisional(Rect area);
    void repaint_provisional(const std::function<void(Image&)>& paint);
    void commit_provisional();
    void discard_provisional();
    bool in_provisional() const;
    Rect provisional_area() const;

    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
    HistoryStats history() const;

private:
    struct Provisional {
        std::uint64_t layer;
        Rect rect;
        Image pixels;
    };

    Image* layer_pixels(std::uint64_t id);
    void paste(Image& into, Rect rect, const Image& snapshot);

    // What the layers changed since `composite_` was last repaired. Accumulated by every pixel
    // change, emptied by the repair; structural changes widen it to the whole canvas.
    mutable Rect pending_;

    std::uint64_t next_layer_id_ = 1;
    std::vector<Layer> layers_;
    int active_ = 0;
    std::optional<Provisional> provisional_;
    std::string path_;
    bool dirty_ = false;
    std::uint64_t revision_ = 0;
    History history_;
    mutable Image composite_;
};

}  // namespace noise
