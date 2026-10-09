// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace noise::io {

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error);

// Writes to `<path>.noise-tmp` and renames over the target, so an interrupted save cannot
// destroy the file it was replacing. `fsync` is deliberately skipped: a torn rename is the
// failure this guards, and a power cut in the middle of a save is not.
bool write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes,
                       std::string& error);

// Lowercase extension without the dot, "" when there is none.
std::string extension_of(const std::string& path);

}  // namespace noise::io
