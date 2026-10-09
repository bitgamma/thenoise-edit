// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noise::io {

// WebP holds alpha, and libwebp's RGBA entry points take and give straight (unpremultiplied) alpha,
// which is the store this app uses everywhere else: no conversion at this boundary.
//
// The format itself keeps premultiplied samples, so a nearly transparent pixel's colour is divided
// up and rounded away and does not survive an encode - measured, and why the lossy assertions bound
// colour only where the pixel is opaque, while alpha is bounded everywhere.
Image decode_webp(std::span<const std::uint8_t> bytes, std::string& error);
Image load_webp(const std::string& path, std::string& error);
std::vector<std::uint8_t> encode_webp(const Image& image, int quality);
bool save_webp(const std::string& path, const Image& image, int quality, std::string& error);

}  // namespace noise::io
