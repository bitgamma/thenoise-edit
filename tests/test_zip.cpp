// SPDX-License-Identifier: Apache-2.0
#include "io/zip.h"

#include "test.h"

#include <zlib.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace noise;

namespace {

constexpr std::size_t kEndRecordBytes = 22;

std::vector<std::uint8_t> payload(std::size_t count, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 37 + seed) & 0xFF);
    }
    return bytes;
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 4; ++shift) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * shift)) & 0xFF));
    }
}

void put16_at(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xFF);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
}

void put32_at(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    for (std::size_t shift = 0; shift < 4; ++shift) {
        out[offset + shift] = static_cast<std::uint8_t>((value >> (8 * shift)) & 0xFF);
    }
}

std::vector<std::uint8_t> raw_deflate(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out(deflateBound(nullptr, static_cast<uLong>(in.size())) + 64, 0u);
    z_stream stream{};
    deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    stream.next_in = const_cast<Bytef*>(in.data());
    stream.avail_in = static_cast<uInt>(in.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

std::uint32_t crc_of(const std::vector<std::uint8_t>& bytes) {
    return static_cast<std::uint32_t>(
        ::crc32(0L, bytes.empty() ? nullptr : bytes.data(), static_cast<uInt>(bytes.size())));
}

// An archive with one deflated member, built by hand: the reader has to accept what other
// applications write, and nothing this app saves exercises that path.
std::vector<std::uint8_t> handmade_deflated_archive(const std::string& name,
                                                    const std::vector<std::uint8_t>& bytes) {
    const std::vector<std::uint8_t> packed = raw_deflate(bytes);
    const std::uint32_t crc = crc_of(bytes);
    std::vector<std::uint8_t> out;
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0);
    put16(out, 8);  // deflate
    put16(out, 0);
    put16(out, 0x21);
    put32(out, crc);
    put32(out, static_cast<std::uint32_t>(packed.size()));
    put32(out, static_cast<std::uint32_t>(bytes.size()));
    put16(out, static_cast<std::uint16_t>(name.size()));
    put16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), packed.begin(), packed.end());

    const std::uint32_t local_offset = 0;
    const std::size_t directory_start = out.size();
    put32(out, 0x02014b50);
    put16(out, 20);
    put16(out, 20);
    put16(out, 0);
    put16(out, 8);
    put16(out, 0);
    put16(out, 0x21);
    put32(out, crc);
    put32(out, static_cast<std::uint32_t>(packed.size()));
    put32(out, static_cast<std::uint32_t>(bytes.size()));
    put16(out, static_cast<std::uint16_t>(name.size()));
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put16(out, 0);
    put32(out, 0);
    put32(out, local_offset);
    out.insert(out.end(), name.begin(), name.end());

    const std::uint32_t directory_bytes = static_cast<std::uint32_t>(out.size() - directory_start);
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, 1);
    put16(out, 1);
    put32(out, directory_bytes);
    put32(out, static_cast<std::uint32_t>(directory_start));
    put16(out, 0);
    return out;
}

void test_members_survive_a_round_trip() {
    io::ZipWriter writer;
    writer.add("mimetype", std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>("application/x-noise+zip"),
                               24));
    writer.add("layers/0/layer.png", payload(1000, 3));
    writer.add("manifest.json", payload(1, 7));
    writer.add("empty.txt", std::span<const std::uint8_t>{});
    const std::vector<std::uint8_t> archive = writer.bytes();

    std::string error;
    const auto read = io::read_zip(std::span<const std::uint8_t>(archive), error);
    CHECK_EQ(error, std::string());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK_EQ(read->size(), 4u);
    if (read->size() != 4) {
        return;
    }
    CHECK_EQ((*read)[0].name, std::string("mimetype"));
    CHECK_EQ((*read)[0].data.size(), 24u);
    CHECK_EQ((*read)[1].data, payload(1000, 3));
    CHECK_EQ((*read)[2].data, payload(1, 7));
    CHECK_EQ((*read)[3].name, std::string("empty.txt"));
    CHECK_EQ((*read)[3].data.size(), 0u);
}

// Fixed timestamps are a claim about the format: the same content gives the same bytes, which
// is what lets a container be diffed.
void test_writing_the_same_members_gives_the_same_bytes() {
    io::ZipWriter a;
    io::ZipWriter b;
    for (int i = 0; i < 3; ++i) {
        const std::vector<std::uint8_t> bytes = payload(64, static_cast<std::uint8_t>(i));
        a.add("e" + std::to_string(i), std::span<const std::uint8_t>(bytes));
        b.add("e" + std::to_string(i), std::span<const std::uint8_t>(bytes));
    }
    CHECK_EQ(a.bytes(), b.bytes());
}

void test_a_deflated_member_from_elsewhere_is_read() {
    const std::vector<std::uint8_t> original = payload(5000, 11);
    const std::vector<std::uint8_t> archive = handmade_deflated_archive("layers/0/layer.png",
                                                                       original);
    std::string error;
    const auto read = io::read_zip(std::span<const std::uint8_t>(archive), error);
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK_EQ(read->size(), 1u);
    CHECK_EQ((*read)[0].data, original);
}

void test_an_end_record_that_misstates_the_directory_is_refused() {
    io::ZipWriter writer;
    writer.add("manifest.json", payload(20, 5));
    std::vector<std::uint8_t> archive = writer.bytes();
    const std::size_t end = archive.size() - kEndRecordBytes;
    const std::uint32_t directory_size = static_cast<std::uint32_t>(archive[end + 12] |
                                                                   (archive[end + 13] << 8) |
                                                                   (archive[end + 14] << 16) |
                                                                   (archive[end + 15] << 24));
    // A directory size off by one byte is a file another reader will walk out of position.
    archive[end + 12] = static_cast<std::uint8_t>((directory_size + 1) & 0xFF);
    archive[end + 13] = static_cast<std::uint8_t>(((directory_size + 1) >> 8) & 0xFF);
    std::string error;
    const auto read = io::read_zip(std::span<const std::uint8_t>(archive), error);
    CHECK(!read.has_value());
    CHECK(error.find("does not reach the end record") != std::string::npos);

    // And the end record must be the last thing in the file.
    archive = writer.bytes();
    archive.push_back(0);
    CHECK(!io::read_zip(std::span<const std::uint8_t>(archive), error).has_value());
}

void test_a_corrupt_member_names_it_instead_of_returning_garbage() {
    io::ZipWriter writer;
    writer.add("layers/0/layer.png", payload(200, 5));
    writer.add("manifest.json", payload(50, 9));
    std::vector<std::uint8_t> archive = writer.bytes();

    std::string error;
    CHECK(io::read_zip(std::span<const std::uint8_t>(archive), error).has_value());

    // A byte of payload changed: the crc says so, and the message says which member. The
    // first member's data starts after its local header (30) and name (18).
    archive[48] = static_cast<std::uint8_t>(archive[48] ^ 0xFF);
    CHECK(!io::read_zip(std::span<const std::uint8_t>(archive), error).has_value());
    CHECK(error.find("crc") != std::string::npos);
}

void test_truncated_and_foreign_files_are_refused() {
    io::ZipWriter writer;
    writer.add("a", payload(400, 1));
    const std::vector<std::uint8_t> archive = writer.bytes();

    const std::vector<std::size_t> cuts{archive.size() - 1, archive.size() / 2, 10};
    for (const std::size_t cut : cuts) {
        std::vector<std::uint8_t> truncated(
            archive.begin(), archive.begin() + static_cast<std::ptrdiff_t>(cut));
        std::string error;
        CHECK(!io::read_zip(std::span<const std::uint8_t>(truncated), error).has_value());
        CHECK(!error.empty());
    }

    const std::string text = "this is not a zip file at all, not even close to one";
    std::vector<std::uint8_t> foreign(text.begin(), text.end());
    std::string error;
    CHECK(!io::read_zip(std::span<const std::uint8_t>(foreign), error).has_value());
    CHECK(error.find("zip") != std::string::npos);
}

// A directory that lies about a member must not become a short read or an out-of-bounds copy.
void test_a_member_that_lies_about_its_size_is_refused() {
    io::ZipWriter writer;
    writer.add("a", payload(300, 2));
    std::vector<std::uint8_t> archive = writer.bytes();

    // The local header's name length (offset 26) is what decides where data starts, so a
    // huge value here used to be a way past the bounds checks.
    put16_at(archive, 26, 0xFFFF);
    std::string error;
    CHECK(!io::read_zip(std::span<const std::uint8_t>(archive), error).has_value());
    CHECK(!error.empty());

    archive = writer.bytes();
    // The central directory's "uncompressed size" for the single member.
    const std::size_t end = archive.size() - 22;
    const std::uint32_t directory_offset =
        archive[end + 16] | (static_cast<std::uint32_t>(archive[end + 17]) << 8) |
        (static_cast<std::uint32_t>(archive[end + 18]) << 16) |
        (static_cast<std::uint32_t>(archive[end + 19]) << 24);
    put32_at(archive, directory_offset + 24, 0xFFFFFFFFu);
    CHECK(!io::read_zip(std::span<const std::uint8_t>(archive), error).has_value());
    CHECK(!error.empty());
}

}  // namespace

int main() {
    test_members_survive_a_round_trip();
    test_writing_the_same_members_gives_the_same_bytes();
    test_an_end_record_that_misstates_the_directory_is_refused();
    test_a_deflated_member_from_elsewhere_is_read();
    test_a_corrupt_member_names_it_instead_of_returning_garbage();
    test_truncated_and_foreign_files_are_refused();
    test_a_member_that_lies_about_its_size_is_refused();
    return test::summary("zip");
}
