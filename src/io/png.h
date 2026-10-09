// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noise::io {

// PNG is the flat exchange format here: RGBA8 straight alpha in, RGBA8 straight alpha out.
// Converting palettes and other bit depths is libpng's job; a file that cannot be read at all
// is an error that names the file, never a silently different image.
Image decode_png(std::span<const std::uint8_t> bytes, std::string& error);
Image load_png(const std::string& path, std::string& error);
std::vector<std::uint8_t> encode_png(const Image& image);
bool save_png(const std::string& path, const Image& image, std::string& error);

}  // namespace noise::io
