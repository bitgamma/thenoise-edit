// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"

#include "sokol_gfx.h"

namespace noise::gfx {

void init();
void shutdown();

// Reads the current framebuffer (bottom-up in GL, flipped here). Only valid
// inside a pass on the GL backend; the debug wire capture path is the caller.
Image read_framebuffer(int width, int height);

// RGBA8 CPU pixels mirrored onto the GPU, plus the view ImGui binds. The whole
// texture is recreated when the content revision moves: sokol's incremental
// image-write API is mid-rewrite and a full upload is honest for M0, where
// pixels change only on load.
struct Texture {
    sg_image image{SG_INVALID_ID};
    sg_view view{SG_INVALID_ID};
    Extent size{0, 0};
    std::uint64_t uploaded_revision = 0;
    bool has_content = false;

    void sync(const Image& pixels, std::uint64_t revision);
    void discard();
    bool valid() const { return image.id != SG_INVALID_ID; }
};

}
