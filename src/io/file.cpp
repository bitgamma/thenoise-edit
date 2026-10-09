// SPDX-License-Identifier: Apache-2.0
#include "io/file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace noise::io {

bool read_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open file: " + path;
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        error = "cannot read file: " + path;
        return false;
    }
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
