// SPDX-License-Identifier: Apache-2.0
#include "io/jpeg.h"

#include "io/file.h"
#include "io/flat.h"

#include <jpeglib.h>

#include <csetjmp>
#include <cstddef>
#include <vector>

namespace noise::io {
namespace {

// libjpeg reports by calling out and either warning or never returning. Both are captured: the
// message is kept for our own error string, and nothing is written to stderr, which is a channel a
// wire-driven app has no use for.
struct JpegError {
    jpeg_error_mgr pub;
    std::jmp_buf jump;
    std::string message;
};

void on_error_exit(j_common_ptr common) {
    JpegError* state = reinterpret_cast<JpegError*>(common->err);
    char text[JMSG_LENGTH_MAX] = {0};
    common->err->format_message(common, text);
    state->message = text;
    std::longjmp(state->jump, 1);
}

void on_emit_message(j_common_ptr common, int) {
    JpegError* state = reinterpret_cast<JpegError*>(common->err);
    char text[JMSG_LENGTH_MAX] = {0};
    common->err->format_message(common, text);
    if (state->message.empty()) {
        state->message = text;
    }
}

constexpr std::size_t kDestChunkBytes = 16384;

// Encoding goes into a vector rather than a file, so a failed encode leaves nothing on disk and a
// successful one goes through the same atomic write as every other file this app produces. It also
// sidesteps libjpeg's allocator, whose individual-object free libjpeg-turbo 3.2 removed.
struct VectorDest {
    jpeg_destination_mgr base;
    std::vector<std::uint8_t> bytes;
    std::vector<JOCTET> chunk;
};

// libjpeg-turbo 3.2 hands the compress object to these callbacks rather than the manager, so the
// vector is reached through `cinfo->dest`, which is `VectorDest` with its manager first.
VectorDest* self_of(j_compress_ptr cinfo) {
    return reinterpret_cast<VectorDest*>(cinfo->dest);
}

void dest_init(j_compress_ptr cinfo) {
    VectorDest* self = self_of(cinfo);
    self->chunk.resize(kDestChunkBytes);
    cinfo->dest->next_output_byte = self->chunk.data();
    cinfo->dest->free_in_buffer = kDestChunkBytes;
}

boolean dest_empty(j_compress_ptr cinfo) {
    VectorDest* self = self_of(cinfo);
    self->bytes.insert(self->bytes.end(), self->chunk.begin(), self->chunk.end());
    cinfo->dest->next_output_byte = self->chunk.data();
    cinfo->dest->free_in_buffer = kDestChunkBytes;
    return TRUE;
}

void dest_term(j_compress_ptr cinfo) {
    VectorDest* self = self_of(cinfo);
    const std::size_t filled = kDestChunkBytes - cinfo->dest->free_in_buffer;
    self->bytes.insert(self->bytes.end(), self->chunk.begin(),
                       self->chunk.begin() + static_cast<std::ptrdiff_t>(filled));
}

// The composite against `kNoAlphaBackdropRgb`, in integer arithmetic that rounds instead of
// truncating.
void flatten_onto_backdrop(const std::uint8_t* rgba, std::uint8_t* rgb, std::size_t pixels) {
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::size_t src = i * 4u;
        const std::size_t dst = i * 3u;
        const unsigned a = rgba[src + 3u];
        for (unsigned c = 0; c < 3u; ++c) {
            const unsigned over = rgba[src + c];
            rgb[dst + c] = static_cast<std::uint8_t>(
                (over * a + kNoAlphaBackdropRgb[c] * (255u - a) + 127u) / 255u);
        }
    }
}

}  // namespace

Image decode_jpeg(std::span<const std::uint8_t> bytes, std::string& error) {
    jpeg_decompress_struct info{};
    JpegError state;
    info.err = jpeg_std_error(&state.pub);
    state.pub.error_exit = &on_error_exit;
    state.pub.emit_message = &on_emit_message;
    if (setjmp(state.jump)) {
        error = "jpeg decode: " + (state.message.empty() ? std::string("failed") : state.message);
        jpeg_destroy_decompress(&info);
        return {};
    }
    jpeg_create_decompress(&info);
    jpeg_mem_src(&info, bytes.data(), static_cast<unsigned long>(bytes.size()));
    jpeg_read_header(&info, TRUE);
    const int width = static_cast<int>(info.image_width);
    const int height = static_cast<int>(info.image_height);
    if (width > kMaxDecodedSide || height > kMaxDecodedSide) {
        error = "jpeg decode: " + std::to_string(width) + "x" + std::to_string(height) +
                " is past the " + std::to_string(kMaxDecodedSide) + " px side this app decodes";
        jpeg_destroy_decompress(&info);
        return {};
    }
    info.out_color_space = JCS_RGB;
    jpeg_start_decompress(&info);
    const std::size_t width_bytes = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                       static_cast<std::size_t>(height) * 4u,
                                   255);
    std::vector<std::uint8_t> row(width_bytes * 3u);
    int y = 0;
    while (static_cast<int>(info.output_scanline) < height) {
        JSAMPLE* start = row.data();
        jpeg_read_scanlines(&info, &start, 1);
        std::uint8_t* at = rgba.data() + static_cast<std::size_t>(y) * width_bytes * 4u;
        for (std::size_t x = 0; x < width_bytes; ++x) {
            at[x * 4u + 0u] = row[x * 3u + 0u];
            at[x * 4u + 1u] = row[x * 3u + 1u];
            at[x * 4u + 2u] = row[x * 3u + 2u];
            at[x * 4u + 3u] = 255u;
        }
        ++y;
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return Image::from_rgba(Extent{width, height}, std::move(rgba));
}

Image load_jpeg(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) {
        return {};
    }
    Image image = decode_jpeg(std::span<const std::uint8_t>(bytes), error);
    if (image.empty() && error.empty()) {
        error = "jpeg decode: no image in " + path;
    }
    return image;
}

std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality) {
    if (image.empty()) {
        return {};
    }
    jpeg_compress_struct info{};
    JpegError error_state;
    VectorDest dest;
    info.err = jpeg_std_error(&error_state.pub);
    error_state.pub.error_exit = &on_error_exit;
    error_state.pub.emit_message = &on_emit_message;
    if (setjmp(error_state.jump)) {
        jpeg_destroy_compress(&info);
        return {};
    }
    jpeg_create_compress(&info);
    dest.base.init_destination = &dest_init;
    dest.base.empty_output_buffer = &dest_empty;
    dest.base.term_destination = &dest_term;
    info.dest = &dest.base;
    info.image_width = static_cast<JDIMENSION>(image.size().w);
    info.image_height = static_cast<JDIMENSION>(image.size().h);
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, clamp_quality(quality), TRUE);
    jpeg_start_compress(&info, TRUE);
    const std::size_t width_pixels = static_cast<std::size_t>(image.size().w);
    std::vector<std::uint8_t> row(width_pixels * 3u);
    for (int y = 0; y < image.size().h; ++y) {
        flatten_onto_backdrop(image.at(0, y), row.data(), width_pixels);
        JSAMPROW start = row.data();
        jpeg_write_scanlines(&info, &start, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    return dest.bytes;
}

bool save_jpeg(const std::string& path, const Image& image, int quality, std::string& error) {
    const std::vector<std::uint8_t> bytes = encode_jpeg(image, quality);
    if (bytes.empty()) {
        error = "jpeg encode failed: " + path;
        return false;
    }
    return write_file_atomic(path, bytes, error);
}

}  // namespace noise::io
