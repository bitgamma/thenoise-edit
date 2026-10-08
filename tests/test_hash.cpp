// SPDX-License-Identifier: Apache-2.0
#include "test.h"

#include "core/base64.h"
#include "core/hash.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace noise;

namespace {

std::span<const std::uint8_t> bytes_of(const std::string& s) {
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void test_sha256_vectors() {
    CHECK_EQ(sha256_hex(std::string("")),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex(std::string("abc")),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(sha256_hex(std::string("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
             std::string("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

void test_sha256_padding_boundaries() {
    // Padding differs on either side of the 56-byte and 64-byte marks; a wrong
    // branch changes the digest of exactly these lengths.
    const std::size_t lengths[] = {0, 1, 54, 55, 56, 57, 62, 63, 64, 65, 119, 120, 127, 128, 129};
    for (const std::size_t length : lengths) {
        std::string data(length, 'a');
        const std::string first = sha256_hex(bytes_of(data));
        CHECK_EQ(first.size(), std::string::size_type(64));
        if (length > 0) {
            data[length - 1] = 'b';
            CHECK(sha256_hex(bytes_of(data)) != first);
        }
    }
    CHECK_EQ(sha256_hex(std::string(1000000, 'a')),
             std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

void test_base64_rfc_vectors() {
    CHECK_EQ(base64_encode(bytes_of("")), std::string(""));
    CHECK_EQ(base64_encode(bytes_of("f")), std::string("Zg=="));
    CHECK_EQ(base64_encode(bytes_of("fo")), std::string("Zm8="));
    CHECK_EQ(base64_encode(bytes_of("foo")), std::string("Zm9v"));
    CHECK_EQ(base64_encode(bytes_of("foob")), std::string("Zm9vYg=="));
    CHECK_EQ(base64_encode(bytes_of("fooba")), std::string("Zm9vYmE="));
    CHECK_EQ(base64_encode(bytes_of("foobar")), std::string("Zm9vYmFy"));
}

void test_base64_all_byte_values() {
    std::vector<std::uint8_t> all(256);
    for (int i = 0; i < 256; ++i) {
        all[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    const std::string encoded = base64_encode(std::span<const std::uint8_t>(all));
    CHECK_EQ(encoded.size(), std::string::size_type(344));
    CHECK(encoded.find_first_not_of(
              "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
          std::string::npos);
    // Every length residue class must pad correctly.
    for (std::size_t len = 0; len < 12; ++len) {
        const std::string out = base64_encode(std::span<const std::uint8_t>(all.data(), len));
        const std::size_t expected = ((len + 2u) / 3u) * 4u;
        CHECK_EQ(out.size(), expected);
    }
}

}

int main() {
    test_sha256_vectors();
    test_sha256_padding_boundaries();
    test_base64_rfc_vectors();
    test_base64_all_byte_values();
    return test::summary("hash+base64");
}
