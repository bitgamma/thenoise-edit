// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace noise::io {

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error);

// Up to `want` bytes from the start of the file, for telling one format from another before any of
// it is decoded. Same refusal as `read_file` for anything that is not a regular file.
bool read_head(const std::string& path, std::vector<std::uint8_t>& out, std::size_t want,
               std::string& error);

// Writes to `<path>.noise-tmp` and renames over the target, so an interrupted save cannot
// destroy the file it was replacing. `fsync` is deliberately skipped: a torn rename is the
// failure this guards, and a power cut in the middle of a save is not.
bool write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes,
                       std::string& error);

// Lowercase extension without the dot, "" when there is none.
std::string extension_of(const std::string& path);

}  // namespace noise::io
