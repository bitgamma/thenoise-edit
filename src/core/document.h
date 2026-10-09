// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/history.h"
#include "core/image.h"

namespace noise {

// Budgeted in stored bytes rather than step count: one region operation on a large document
// is worth a great many dabs, and dropping whole steps is what keeps the budget predictable.
inline constexpr std::size_t kHistoryBudgetBytes = 128 * 1024 * 1024;

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

    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;
    HistoryStats history() const;

private:
    std::string path_;
    Image image_;
    std::uint64_t revision_ = 0;
    History history_;
};

}  // namespace noise
