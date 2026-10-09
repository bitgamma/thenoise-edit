// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noise::io {

// JPEG is the one flat format here with no alpha: decoding produces an opaque RGBA8 image, and
// encoding composites the source against `kNoAlphaBackdropRgb`, because an image that cannot hold
// transparency still has to be shown against something. A caller that cares can see whether the
// source had anything to lose — `flat_format_has_alpha` says which formats do.
//
// It is also lossy, so `encode_jpeg` does not come back as the bytes that went in: a round trip is
// checked against dimensions, opacity and mean error, never against a checksum.
Image decode_jpeg(std::span<const std::uint8_t> bytes, std::string& error);
Image load_jpeg(const std::string& path, std::string& error);
std::vector<std::uint8_t> encode_jpeg(const Image& image, int quality);
bool save_jpeg(const std::string& path, const Image& image, int quality, std::string& error);

}  // namespace noise::io
