// SPDX-License-Identifier: Apache-2.0
#include "app/reference_thumbs.h"

#include <algorithm>

namespace noise::app {

const gfx::Texture& ReferenceThumbs::sync(const AiReference& reference) {
    Entry& entry = entries_[reference.id];
    if (entry.revision != reference.revision) {
        entry.texture.sync(reference.thumbnail, reference.revision);
        entry.revision = reference.revision;
        ++rebuilds;
    }
    return entry.texture;
}

void ReferenceThumbs::keep(std::span<const AiReference> references) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const bool present = std::any_of(references.begin(), references.end(),
                                         [&entry](const AiReference& reference) {
                                             return reference.id == entry->first;
                                         });
        if (present) {
            ++entry;
            continue;
        }
        entry->second.texture.discard();
        entry = entries_.erase(entry);
    }
}

}  // namespace noise::app
