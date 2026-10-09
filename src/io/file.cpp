// SPDX-License-Identifier: Apache-2.0
#include "io/file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace noise::io {
namespace {

// Opening a directory with an ifstream succeeds on Linux and then throws from libstdc++'s
// `basic_filebuf::underflow` on the first read, so what kind of path this is gets checked before
// anything is streamed from it.
bool open_regular(const std::string& path, std::ifstream& file, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = ec ? "cannot open file: " + path + " (" + ec.message() + ")"
                   : "not a regular file: " + path;
        return false;
    }
    file.open(path, std::ios::binary);
    if (!file) {
        error = "cannot open file: " + path;
        return false;
    }
    return true;
}

}  // namespace

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream file;
    if (!open_regular(path, file, error)) {
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        error = "cannot read file: " + path;
        return false;
    }
    return true;
}

bool read_head(const std::string& path, std::vector<std::uint8_t>& out, std::size_t want,
               std::string& error) {
    std::ifstream file;
    if (!open_regular(path, file, error)) {
        return false;
    }
    out.resize(want);
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(want));
    const std::streamsize got = file.gcount();
    out.resize(static_cast<std::size_t>(got < 0 ? 0 : got));
    return true;
}

bool write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& bytes,
                       std::string& error) {
    const std::filesystem::path target(path);
    const std::filesystem::path temp = target.parent_path() /
                                       (target.filename().string() + ".noise-tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot write file: " + temp.string();
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            error = "short write: " + temp.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        error = "cannot replace file: " + path + " (" + ec.message() + ")";
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

std::string extension_of(const std::string& path) {
    const std::string extension = std::filesystem::path(path).extension().string();
    std::string lowered = extension.rfind('.', 0) == 0 ? extension.substr(1) : extension;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered;
}

}  // namespace noise::io
