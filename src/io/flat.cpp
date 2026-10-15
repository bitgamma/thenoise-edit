// SPDX-License-Identifier: Apache-2.0
#include "io/flat.h"

#include "io/file.h"
#include "io/jpeg.h"
#include "io/png.h"
#include "io/webp.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace noise::io {
namespace {

struct FlatFormat {
    const char* extension;
    bool alpha;
    bool quality_used;
    bool (*save)(const std::string& path, const Image& image, int quality,
                 std::string& error);
};

// PNG is lossless: the row takes the quality parameter the table hands every codec and has no use
// for it. `flat_format_has_quality` is how a caller learns that before sending one.
bool save_png_row(const std::string& path, const Image& image, int, std::string& error) {
    return save_png(path, image, error);
}

// The one list of what can be written. `jpg` and `jpeg` are separate rows because both spellings
// are in use and the app refuses to guess which one a caller meant to write.
constexpr FlatFormat kFlatFormats[]{
    {"png", true, false, &save_png_row},
    {"jpg", false, true, &save_jpeg},
    {"jpeg", false, true, &save_jpeg},
    {"webp", true, true, &save_webp},
};

constexpr std::size_t kSniffBytes = 16;

std::string hex_head(const std::vector<std::uint8_t>& bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    const std::size_t take = std::min<std::size_t>(bytes.size(), 4);
    for (std::size_t i = 0; i < take; ++i) {
        if (!out.empty()) {
            out += ' ';
        }
        out += kDigits[bytes[i] >> 4];
        out += kDigits[bytes[i] & 0x0f];
    }
    return out;
}

bool is_png(const std::vector<std::uint8_t>& head) {
    return head.size() >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' &&
           head[3] == 'G';
}

bool is_jpeg(const std::vector<std::uint8_t>& head) {
    return head.size() >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
}

// `RIFF....WEBP`: the container is chunked, so the format name is four bytes past the size field.
bool is_webp(const std::vector<std::uint8_t>& head) {
    if (head.size() < 12) {
        return false;
    }
    const std::string start(head.begin(), head.begin() + 4);
    const std::string tag(head.begin() + 8, head.begin() + 12);
    return start == "RIFF" && tag == "WEBP";
}

const FlatFormat* format_for(const std::string& extension) {
    for (const FlatFormat& format : kFlatFormats) {
        if (extension == format.extension) {
            return &format;
        }
    }
    return nullptr;
}

}  // namespace

int clamp_quality(int requested) {
    return std::clamp(requested, kMinQuality, kMaxQuality);
}

Image load_flat_image(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(path, bytes, error)) {
        return {};
    }
    const std::vector<std::uint8_t> head(bytes.begin(),
                                         bytes.begin() +
                                             static_cast<std::ptrdiff_t>(
                                                 std::min<std::size_t>(bytes.size(), kSniffBytes)));
    if (is_png(head)) {
        return decode_png(std::span<const std::uint8_t>(bytes), error);
    }
    if (is_jpeg(head)) {
        return decode_jpeg(std::span<const std::uint8_t>(bytes), error);
    }
    if (is_webp(head)) {
        return decode_webp(std::span<const std::uint8_t>(bytes), error);
    }
    error = "not an image format this app reads (PNG, JPEG, WebP): " + path + " starts " +
            hex_head(head);
    return {};
}

bool save_flat_image(const std::string& path, const Image& image, int quality,
                     std::string& error) {
    const FlatFormat* format = format_for(extension_of(path));
    if (format == nullptr) {
        error = "unsupported export extension in '" + path + "'; this app writes " +
                flat_export_extensions();
        return false;
    }
    return format->save(path, image, clamp_quality(quality), error);
}

bool flat_format_exists(const std::string& extension) {
    return format_for(extension) != nullptr;
}

bool flat_format_has_alpha(const std::string& extension) {
    const FlatFormat* format = format_for(extension);
    return format != nullptr && format->alpha;
}

bool flat_format_has_quality(const std::string& extension) {
    const FlatFormat* format = format_for(extension);
    return format != nullptr && format->quality_used;
}

std::vector<std::string> flat_export_extension_list() {
    std::vector<std::string> out;
    for (const FlatFormat& format : kFlatFormats) {
        out.push_back(format.extension);
    }
    return out;
}

std::string flat_export_extensions() {
    std::string out;
    for (const std::string& extension : flat_export_extension_list()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += extension;
    }
    return out;
}

}  // namespace noise::io
