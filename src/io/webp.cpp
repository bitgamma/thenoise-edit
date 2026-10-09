// SPDX-License-Identifier: Apache-2.0
#include "io/webp.h"

#include "io/file.h"
#include "io/flat.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstddef>
#include <vector>

namespace noise::io {

Image decode_webp(std::span<const std::uint8_t> bytes, std::string& error) {
    int width = 0;
    int height = 0;
    uint8_t* decoded = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height);
    if (decoded == nullptr) {
        error = "webp decode: not a decodable WebP image";
        return {};
    }
    if (width > kMaxDecodedSide || height > kMaxDecodedSide) {
        error = "webp decode: " + std::to_string(width) + "x" + std::to_string(height) +
                " is past the " + std::to_string(kMaxDecodedSide) + " px side this app decodes";
        WebPFree(decoded);
        return {};
    }
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    std::vector<std::uint8_t> rgba(decoded, decoded + count);
    WebPFree(decoded);
    return Image::from_rgba(Extent{width, height}, std::move(rgba));
}

Image load_webp(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) {
        return {};
    }
    Image image = decode_webp(std::span<const std::uint8_t>(bytes), error);
    if (image.empty() && error.empty()) {
        error = "webp decode: no image in " + path;
    }
    return image;
}

std::vector<std::uint8_t> encode_webp(const Image& image, int quality) {
    if (image.empty()) {
        return {};
    }
    uint8_t* encoded = nullptr;
    const std::size_t written = WebPEncodeRGBA(
        image.data(), image.size().w, image.size().h, static_cast<int>(stride_of(image.size())),
        static_cast<float>(clamp_quality(quality)), &encoded);
    if (encoded == nullptr || written == 0) {
        if (encoded != nullptr) {
            WebPFree(encoded);
        }
        return {};
    }
    std::vector<std::uint8_t> out(encoded, encoded + written);
    WebPFree(encoded);
    return out;
}

bool save_webp(const std::string& path, const Image& image, int quality, std::string& error) {
    const std::vector<std::uint8_t> bytes = encode_webp(image, quality);
    if (bytes.empty()) {
        error = "webp encode failed: " + path;
        return false;
    }
    return write_file_atomic(path, bytes, error);
}

}  // namespace noise::io
