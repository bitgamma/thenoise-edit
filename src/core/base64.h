// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noise {

std::string base64_encode(std::span<const std::uint8_t> bytes);

// The server answers with base64 PNGs, so a decode is as much a part of the wire as the encode.
// Whitespace between lines is skipped; a character outside the alphabet, a length that is not a whole
// number of quads, or padding in the wrong place is a refusal with the offset named - a truncated
// image that decodes would reach the compositor as pixels.
std::optional<std::vector<std::uint8_t>> base64_decode(const std::string& text, std::string& error);

}
