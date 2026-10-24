// SPDX-License-Identifier: Apache-2.0
#include "gfx/renderer.h"

#include "core/log.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <sokol_glue.h>
#include <sokol_log.h>

#include <cstring>
#include <vector>

namespace noise::gfx {

void init() {
    sg_desc desc{};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    sg_setup(&desc);
}

void shutdown() {
    sg_shutdown();
}

Image read_framebuffer(int width, int height) {
    if (width <= 0 || height <= 0) {
        return {};
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    std::vector<std::uint8_t> rows(stride * static_cast<std::size_t>(height));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());

    Image out = Image::filled(Extent{width, height}, 0, 0, 0, 0);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * stride,
                    rows.data() + static_cast<std::size_t>(height - 1 - y) * stride, stride);
    }
    return out;
}

void Texture::sync(const Image& pixels, std::uint64_t revision) {
    if (pixels.empty()) {
        discard();
        return;
    }
    if (valid() && pixels.size() == size && has_content && revision == uploaded_revision) {
        return;
    }
    discard();

    sg_image_desc img_desc{};
    img_desc.width = pixels.size().w;
    img_desc.height = pixels.size().h;
    img_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    img_desc.label = "noise-canvas";
    img_desc.data.mip_levels[0].ptr = pixels.data();
    img_desc.data.mip_levels[0].size = pixels.bytes();
    image = sg_make_image(&img_desc);

    sg_view_desc view_desc{};
    view_desc.texture.image = image;
    view = sg_make_view(&view_desc);

    size = pixels.size();
    uploaded_revision = revision;
    has_content = true;
}

void Texture::discard() {
    if (view.id != SG_INVALID_ID) {
        sg_destroy_view(view);
        view = sg_view{SG_INVALID_ID};
    }
    if (image.id != SG_INVALID_ID) {
        sg_destroy_image(image);
        image = sg_image{SG_INVALID_ID};
    }
    size = Extent{0, 0};
    has_content = false;
}

}
