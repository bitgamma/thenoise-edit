// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/history.h"
#include "core/image.h"

namespace noise {

// Budgeted in stored bytes rather than step count: one region operation on a large document
// is worth a great many dabs, and dropping whole steps is what keeps the budget predictable.
inline constexpr std::size_t kHistoryBudgetBytes = 128 * 1024 * 1024;

// What the one v0 layer is called until there is a layer list to name (M2).
inline constexpr const char* kDefaultLayerName = "Layer 1";

// The open document: one RGBA layer plus the history of what changed in it. Pixels are only
// changed through `paint`, so every visible change can be put back exactly.
class Document {
public:
    Document();

    // A new or freshly opened document starts a new timeline: `path` empty means "never
    // saved", which is what makes doc.save report itself instead of guessing a filename.
    void reset(Image contents, std::string path);

    const std::string& path() const;
    void set_path(std::string path);

    // The v0 layer's name, as the container holds it.
    const std::string& layer_name() const;
    void set_layer_name(std::string name);

    // Unsaved changes, conservative: undo does not clear it, because "back to what is on disk"
    // would mean holding the saved checksum as a second fact about the file.
    bool dirty() const;
    void mark_saved();

    bool empty() const;
    int width() const;
    int height() const;
    Extent size() const;
    const Image& pixels() const;
    std::string checksum() const;

    // Bumped by anything a step could undo, and by a reset: the renderer re-uploads on it.
    std::uint64_t revision() const;

    // Applies `edit` and records the pixels `bounds` held beforehand. An edit that writes
    // outside `bounds` will not be fully undone, which is why bounds is not inferred.
    void paint(Rect bounds, const std::function<void(Image&)>& edit);

    // A gesture that is still going - a pointer stroke - shows its result before it ends.
    // The pixels under `area` are held aside, every repaint starts from them, and `commit`
    // records them as one undo step. Without this a live stroke would either compound its own
    // opacity once per frame or push an undo step per frame. Calling `begin_provisional` again
    // widens the held-aside area, restoring the current one first so the new snapshot is of
    // pristine pixels.
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
        Rect rect;
        Image pixels;
    };

    std::optional<Provisional> provisional_;
    void paste(Rect rect, const Image& snapshot);

    std::string path_;
    std::string layer_name_ = kDefaultLayerName;
    bool dirty_ = false;
    Image image_;
    std::uint64_t revision_ = 0;
    History history_;
};

}  // namespace noise
