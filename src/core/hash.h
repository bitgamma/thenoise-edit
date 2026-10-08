// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace noise {

// FIPS 180-4 SHA-256. Blob identity, capture checksums and cache keys all rest
// on this being exact, so it is tested against the published vectors.
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);
std::string sha256_hex(std::span<const std::uint8_t> data);
std::string sha256_hex(const std::string& text);

}
