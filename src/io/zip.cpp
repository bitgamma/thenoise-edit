// SPDX-License-Identifier: Apache-2.0
#include "io/zip.h"

#include "io/file.h"

#include <zlib.h>

#include <cstring>
#include <limits>

namespace noise::io {
namespace {

constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::uint16_t kVersion = 20;  // 2.0: deflate and stored, anything older is moot
constexpr std::uint16_t kTimeFixed = 0;
constexpr std::uint16_t kDateFixed = 0x0021;  // 1980-01-01, the epoch DOS can express
constexpr std::size_t kLocalHeaderBytes = 30;
constexpr std::size_t kEndHeaderBytes = 22;
constexpr std::size_t kMaxCommentBytes = 0xFFFF;
constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodDeflate = 8;

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint32_t>(p[1]) << 8));
}

std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t crc_of(const std::vector<std::uint8_t>& data) {
    return static_cast<std::uint32_t>(
        ::crc32(0L, data.empty() ? nullptr : data.data(),
                static_cast<uInt>(data.size())));
}

// The raw deflate stream a ZIP member holds (no zlib or gzip wrapper).
bool raw_inflate(std::span<const std::uint8_t> in, std::size_t expect,
                 std::vector<std::uint8_t>& out, std::string& error) {
    out.assign(expect, 0u);
    z_stream stream{};
    if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        error = "inflate init failed";
        return false;
    }
    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    const int status = ::inflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    ::inflateEnd(&stream);
    if (status != Z_STREAM_END) {
        error = "inflate failed";
        return false;
    }
    if (written != expect) {
        error = "inflated to " + std::to_string(written) + " bytes, directory promised " +
                std::to_string(expect);
        return false;
    }
    return true;
}

// A cursor that refuses to read past the end of the buffer: a container from elsewhere is
// untrusted input, and `data[n]` on a truncated file is a crash rather than an error.
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    bool done() const { return offset_ >= size_; }
    std::size_t offset() const { return offset_; }

    bool skip(std::size_t count) {
        if (count > size_ - offset_) {
            return false;
        }
        offset_ += count;
        return true;
    }

    bool u16(std::uint16_t& out_value) {
        if (!remaining_at_least(2)) {
            return false;
        }
        out_value = get16(data_ + offset_);
        offset_ += 2;
        return true;
    }

    bool u32(std::uint32_t& out_value) {
        if (!remaining_at_least(4)) {
            return false;
        }
        out_value = get32(data_ + offset_);
        offset_ += 4;
        return true;
    }

    bool text(std::string& out_text, std::size_t count) {
        if (!skip(count)) {
            return false;
        }
        out_text.assign(reinterpret_cast<const char*>(data_ + offset_ - count), count);
        return true;
    }

    bool bytes(std::vector<std::uint8_t>& out_bytes, std::size_t count) {
        if (!skip(count)) {
            return false;
        }
        out_bytes.assign(data_ + offset_ - count, data_ + offset_);
        return true;
    }

    std::size_t remaining() const { return size_ - offset_; }

private:
    bool remaining_at_least(std::size_t count) const { return count <= size_ - offset_; }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

// Where a member's bytes start: the local header repeats the name and extra field lengths,
// and they are allowed to differ from the central directory's, so the local one decides.
bool member_start(const std::uint8_t* data, std::size_t size, std::uint32_t offset,
                  const std::string& name, std::size_t& out_start, std::string& error) {
    if (offset > size || size - offset < kLocalHeaderBytes) {
        error = "member '" + name + "' points outside the archive";
        return false;
    }
    const std::uint8_t* head = data + offset;
    if (get32(head) != kLocalSignature) {
        error = "member '" + name + "' has no local header where the directory says";
        return false;
    }
    const std::size_t header_bytes = kLocalHeaderBytes + get16(head + 26) + get16(head + 28);
    if (header_bytes > size - offset) {
        error = "member '" + name + "' has a header that runs past the archive";
        return false;
    }
    out_start = offset + header_bytes;
    return true;
}

std::optional<std::size_t> find_end_record(const std::uint8_t* data, std::size_t size) {
    if (size < kEndHeaderBytes) {
        return std::nullopt;
    }
    const std::size_t first = size - std::min(size, kEndHeaderBytes + kMaxCommentBytes);
    for (std::size_t offset = size - kEndHeaderBytes + 1; offset-- > first;) {
        if (get32(data + offset) == kEndSignature) {
            return offset;
        }
    }
    return std::nullopt;
}

}  // namespace

void ZipWriter::add(const std::string& name, std::span<const std::uint8_t> data) {
    entries_.push_back(
        ZipEntry{name, std::vector<std::uint8_t>(data.begin(), data.end())});
}

std::vector<std::uint8_t> ZipWriter::bytes() const {
    std::vector<std::uint8_t> out;
    struct Placement {
        std::uint32_t offset;
        std::uint32_t crc;
    };
    std::vector<Placement> placements;
    placements.reserve(entries_.size());

    for (const ZipEntry& entry : entries_) {
        const std::uint32_t crc = crc_of(entry.data);
        placements.push_back(Placement{static_cast<std::uint32_t>(out.size()), crc});
        put32(out, kLocalSignature);
        put16(out, kVersion);
        put16(out, 0);  // general flags: no data descriptor, names are not encrypted
        put16(out, kMethodStore);
        put16(out, kTimeFixed);
        put16(out, kDateFixed);
        put32(out, crc);
        put32(out, static_cast<std::uint32_t>(entry.data.size()));
        put32(out, static_cast<std::uint32_t>(entry.data.size()));
        put16(out, static_cast<std::uint16_t>(entry.name.size()));
        put16(out, 0);  // extra field
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), entry.data.begin(), entry.data.end());
    }

    const std::size_t directory_start = out.size();
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const ZipEntry& entry = entries_[index];
        put32(out, kCentralSignature);
        put16(out, kVersion);
        put16(out, kVersion);
        put16(out, 0);
        put16(out, kMethodStore);
        put16(out, kTimeFixed);
        put16(out, kDateFixed);
        put32(out, placements[index].crc);
        put32(out, static_cast<std::uint32_t>(entry.data.size()));
        put32(out, static_cast<std::uint32_t>(entry.data.size()));
        put16(out, static_cast<std::uint16_t>(entry.name.size()));
        put16(out, 0);  // extra
        put16(out, 0);  // comment
        put16(out, 0);  // disk number
        put16(out, 0);  // internal attributes
        put32(out, 0);  // external attributes
        put32(out, placements[index].offset);
        out.insert(out.end(), entry.name.begin(), entry.name.end());
    }

    // Measured before the end record is appended: a size taken after it would count the end
    // record itself, which `unzip` forgives and other readers reject.
    const std::uint32_t directory_bytes = static_cast<std::uint32_t>(out.size() - directory_start);
    put32(out, kEndSignature);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint16_t>(entries_.size()));
    put16(out, static_cast<std::uint16_t>(entries_.size()));
    put32(out, directory_bytes);
    put32(out, static_cast<std::uint32_t>(directory_start));
    put16(out, 0);  // comment length
    return out;
}

bool zip_container_at(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> head;
    if (!read_head(path, head, 4, error)) {
        return false;
    }
    if (head.size() < 4 || head[0] != 'P' || head[1] != 'K') {
        return false;
    }
    // A stored local header, or the end record of an archive with nothing in it.
    return (head[2] == 3 && head[3] == 4) || (head[2] == 5 && head[3] == 6);
}

std::optional<std::vector<ZipEntry>> read_zip(std::span<const std::uint8_t> bytes,
                                              std::string& error) {
    const std::uint8_t* data = bytes.data();
    const std::size_t size = bytes.size();
    const std::optional<std::size_t> end = find_end_record(data, size);
    if (!end) {
        error = "not a zip archive: no end-of-directory record";
        return std::nullopt;
    }

    Cursor tail(data + *end, kEndHeaderBytes);
    std::uint16_t disk = 0;
    std::uint16_t directory_disk = 0;
    std::uint16_t on_disk = 0;
    std::uint16_t total = 0;
    std::uint32_t directory_size = 0;
    std::uint32_t directory_offset = 0;
    std::uint16_t comment_length = 0;
    if (!tail.skip(4) || !tail.u16(disk) || !tail.u16(directory_disk) || !tail.u16(on_disk) ||
        !tail.u16(total) || !tail.u32(directory_size) || !tail.u32(directory_offset) ||
        !tail.u16(comment_length)) {
        error = "truncated end-of-directory record";
        return std::nullopt;
    }
    if (total == std::numeric_limits<std::uint16_t>::max() ||
        directory_offset == std::numeric_limits<std::uint32_t>::max()) {
        error = "zip64 archives are not read yet";
        return std::nullopt;
    }
    if (disk != 0 || directory_disk != 0 || on_disk != total ||
            *end + kEndHeaderBytes + comment_length != size) {
        error = "multi-disk archives and archives with trailing bytes are not read";
        return std::nullopt;
    }
    if (directory_offset > size || directory_size > size - directory_offset) {
        error = "the central directory points outside the file";
        return std::nullopt;
    }
    if (directory_offset + directory_size != *end) {
        error = "the central directory is " + std::to_string(directory_size) +
                " bytes at " + std::to_string(directory_offset) +
                ", which does not reach the end record at " + std::to_string(*end);
        return std::nullopt;
    }

    std::vector<ZipEntry> entries;
    entries.reserve(total);
    Cursor cursor(data + directory_offset, directory_size);
    for (std::uint16_t index = 0; index < total; ++index) {
        std::uint32_t signature = 0;
        std::uint16_t made_by = 0;
        std::uint16_t version_needed = 0;
        std::uint16_t flags = 0;
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint32_t packed = 0;
        std::uint32_t unpacked = 0;
        std::uint16_t name_length = 0;
        std::uint16_t extra_length = 0;
        std::uint16_t entry_comment_length = 0;
        std::uint32_t local_offset = 0;
        std::string name;
        // 46 bytes: signature, the two version fields, flags, method, time and date, crc,
        // the two sizes, three lengths, disk and attributes, local offset - then the name.
        if (!cursor.u32(signature) || signature != kCentralSignature || !cursor.u16(made_by) ||
            !cursor.u16(version_needed) || !cursor.u16(flags) || !cursor.u16(method) ||
            !cursor.skip(4) || !cursor.u32(crc) || !cursor.u32(packed) ||
            !cursor.u32(unpacked) || !cursor.u16(name_length) || !cursor.u16(extra_length) ||
            !cursor.u16(entry_comment_length) || !cursor.skip(2) || !cursor.skip(2) ||
            !cursor.skip(4) || !cursor.u32(local_offset) || !cursor.text(name, name_length) ||
            !cursor.skip(extra_length + entry_comment_length)) {
            error = "truncated or corrupt central directory at entry " + std::to_string(index);
            return std::nullopt;
        }
        if (flags & 0x1) {
            error = "encrypted member '" + name + "' is not read";
            return std::nullopt;
        }
        if (method != kMethodStore && method != kMethodDeflate) {
            error = "member '" + name + "' uses compression method " + std::to_string(method) +
                    ", which is not read";
            return std::nullopt;
        }

        std::size_t start = 0;
        if (!member_start(data, size, local_offset, name, start, error)) {
            return std::nullopt;
        }
        if (packed > size - start) {
            error = "member '" + name + "' claims " + std::to_string(packed) +
                    " bytes and the archive does not hold them";
            return std::nullopt;
        }

        std::vector<std::uint8_t> payload;
        if (method == kMethodStore) {
            if (packed != unpacked) {
                error = "member '" + name + "' is stored with two different sizes";
                return std::nullopt;
            }
            payload.assign(data + start, data + start + packed);
        } else {
            std::string inflate_error;
            if (!raw_inflate(std::span<const std::uint8_t>(data + start, packed), unpacked,
                             payload, inflate_error)) {
                error = "member '" + name + "': " + inflate_error;
                return std::nullopt;
            }
        }
        if (crc_of(payload) != crc) {
            error = "member '" + name + "' does not match the crc in its directory entry";
            return std::nullopt;
        }
        entries.push_back(ZipEntry{name, std::move(payload)});
    }
    return entries;
}

}
