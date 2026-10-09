// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "core/image.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace noise::io {

// A decoder is handed a file it did not choose, and dimensions come out of the file: an upper bound
// on a side is what keeps a hostile header from asking for a multi-gigabyte allocation. Twice the
// document ceiling, so anything this app could hold it can also read.
inline constexpr int kMaxDecodedSide = 16384;

inline constexpr int kMinQuality = 1;
inline constexpr int kMaxQuality = 100;
// The quality an export uses when the call does not name one. Defaults live in a table, not in a
// parameter, so there is one place to ask what "a good export" meant.
inline constexpr int kDefaultExportQuality = 90;

// What a format with no alpha channel shows underneath the source, and what `encode_jpeg` therefore
// composites against.
inline constexpr std::uint8_t kNoAlphaBackdropRgb[3] = {255, 255, 255};

int clamp_quality(int requested);

// The flat formats this app reads and writes. Reading decides by the bytes; writing by the
// extension, because a file being created has no bytes yet. Adding a format is a row in the table
// in flat.cpp plus its codec: the sniff, the accepted-extension message and the alpha question all
// read that table.
// Decodes whichever flat format the bytes turn out to be. A file that is none of them is refused
// naming the formats read and the first bytes of what was found instead.
Image load_flat_image(const std::string& path, std::string& error);

// Writes `path` in the format its extension names. An extension that is not a format this app
// writes is refused with the list.
bool save_flat_image(const std::string& path, const Image& image, int quality, std::string& error);

// True when `extension` names a format this app writes at all.
bool flat_format_exists(const std::string& extension);

// True when the named export format can hold transparency. JPEG cannot, so an export to it is a
// composite against `kNoAlphaBackdropRgb` and the caller says so.
bool flat_format_has_alpha(const std::string& extension);

// True when `quality` means something for the named format. PNG is lossless, so sending it a
// quality is a field with no effect, and the command refuses rather than ignoring it.
bool flat_format_has_quality(const std::string& extension);

// The extensions `save_flat_image` writes, for an error message: "png, jpg, jpeg, webp".
std::string flat_export_extensions();

}  // namespace noise::io
