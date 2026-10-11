// SPDX-License-Identifier: Apache-2.0
#include "ai/cache.h"

#include "io/file.h"

#include <filesystem>

namespace noise::ai {

namespace {

constexpr std::size_t kBlobIdLength = 64;

bool is_blob_id(const std::string& key) {
    if (key.size() != kBlobIdLength) {
        return false;
    }
    for (const char c : key) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

}  // namespace

BlobCache::BlobCache(std::string directory) : directory_(std::move(directory)) {}

std::string BlobCache::blob_path(const std::string& key) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return blob_path_locked(key);
}

std::string BlobCache::blob_path_locked(const std::string& key) const {
    if (!is_blob_id(key)) {
        ++refused_keys_;
        return {};
    }
    return directory_ + "/" + key.substr(0, 2) + "-" + key + ".png";
}

bool BlobCache::lookup(const std::string& key, std::vector<std::uint8_t>& png, std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string path = blob_path_locked(key);
    if (path.empty()) {
        error = "cache: '" + key + "' is not a 64-character lowercase hex blob id";
        return false;
    }
    if (!std::filesystem::exists(std::filesystem::path(path))) {
        ++misses_;
        return false;
    }
    if (!io::read_file(path, png, error)) {
        ++misses_;
        return false;
    }
    ++hits_;
    return true;
}

bool BlobCache::store(const std::string& key, const std::vector<std::uint8_t>& png,
                      std::string& error) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string path = blob_path_locked(key);
    if (path.empty()) {
        error = "cache: '" + key + "' is not a 64-character lowercase hex blob id";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(directory_), ec);
    if (ec) {
        error = "cache: cannot create '" + directory_ + "': " + ec.message();
        return false;
    }
    // A half-written cache entry is worse than none: it decodes to nothing and reads as a hit.
    if (!io::write_file_atomic(path, png, error)) {
        return false;
    }
    return true;
}

BlobCache::Stats BlobCache::stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    Stats stats;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.refused_keys = refused_keys_;
    stats.directory = directory_;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) {
        return stats;
    }
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory_, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") {
            continue;
        }
        ++stats.entries;
        stats.bytes += static_cast<std::size_t>(entry.file_size(ec));
    }
    return stats;
}

nlohmann::json BlobCache::json() const {
    const Stats stats = this->stats();
    return nlohmann::json{{"entries", stats.entries},
                          {"bytes", stats.bytes},
                          {"hits", stats.hits},
                          {"misses", stats.misses},
                          {"refused_keys", stats.refused_keys},
                          {"directory", stats.directory}};
}

}  // namespace noise::ai
