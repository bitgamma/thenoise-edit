// SPDX-License-Identifier: Apache-2.0
#include "io/png.h"

#include "core/image.h"

#include <png.h>

#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace noise::io {

Image decode_png(std::span<const std::uint8_t> bytes, std::string& error) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) {
        error = std::string("png decode: ") + png.message;
        return {};
    }
    png.format = PNG_FORMAT_RGBA;
    const Extent size{static_cast<int>(png.width), static_cast<int>(png.height)};
    std::vector<std::uint8_t> px(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, px.data(),
                               static_cast<png_int_32>(stride_of(size)), nullptr)) {
        error = std::string("png decode: ") + png.message;
        return {};
    }
    return Image::from_rgba(size, std::move(px));
}

Image load_png(const std::string& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open file: " + path;
        return {};
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
    return decode_png(std::span<const std::uint8_t>(bytes), error);
}

std::vector<std::uint8_t> encode_png(const Image& image) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = static_cast<png_uint_32>(image.size().w);
    png.height = static_cast<png_uint_32>(image.size().h);
    png.format = PNG_FORMAT_RGBA;

    png_alloc_size_t needed = 0;
    if (!png_image_write_to_memory(&png, nullptr, &needed, 0, image.data(),
                                   static_cast<png_int_32>(stride_of(image.size())), nullptr)) {
        return {};
    }
    std::vector<std::uint8_t> out(needed);
    if (!png_image_write_to_memory(&png, out.data(), &needed, 0, image.data(),
                                   static_cast<png_int_32>(stride_of(image.size())), nullptr)) {
        return {};
    }
    out.resize(needed);
    return out;
}

bool save_png(const std::string& path, const Image& image, std::string& error) {
    const std::vector<std::uint8_t> bytes = encode_png(image);
    if (bytes.empty()) {
        error = "png encode failed";
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot write file: " + path;
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "short write: " + path;
        return false;
    }
    return true;
}

}  // namespace noise::io
