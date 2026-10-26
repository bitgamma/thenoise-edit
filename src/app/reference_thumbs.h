// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/ai_references.h"
#include "gfx/renderer.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace noise::app {

struct ReferenceThumbs {
    struct Entry {
        std::uint64_t revision = 0;
        gfx::Texture texture{};
    };

    const gfx::Texture& sync(const AiReference& reference);
    void keep(std::span<const AiReference> references);
    std::uint64_t rebuilds = 0;

private:
    std::map<std::string, Entry> entries_;
};

}  // namespace noise::app
