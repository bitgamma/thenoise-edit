// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace noise::io {

// One member of an archive, with its path inside the archive.
struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> data;
};

// Writes archives whose members are stored (not deflated). Everything this app puts in a
// container is already PNG, so deflating would cost CPU and save nothing; readers still accept
// deflated members, because files from other applications are not ours to choose.
//
// Timestamps are fixed rather than "now": two saves of the same document produce the same
// bytes, which is what makes a container diffable in a test and in git.
class ZipWriter {
public:
    void add(const std::string& name, std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> bytes() const;

private:
    std::vector<ZipEntry> entries_;
};

// Reads the central directory and returns the members in archive order. Parsing is exact and
// hostile-input safe: a member that lies about its size, fails its CRC, or cannot be inflated
// is an error naming that member, not a truncated image.
std::optional<std::vector<ZipEntry>> read_zip(std::span<const std::uint8_t> bytes,
                                              std::string& error);

}  // namespace noise::io
