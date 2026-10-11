// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace noise::ai {

// Result blobs by request key, on disk, across restarts. The key is a hash the request builder computes
// over everything the server saw, so a replay is exact and a changed prompt is a different file.
//
// This is not the deferred "blob store": it holds answers keyed by the request that produced them, and
// nothing can reference a blob except by asking for that request again. History and variants that
// outlive a session would need identity of their own, which is the thing still deliberately unbuilt.
class BlobCache {
public:
    explicit BlobCache(std::string directory);

    const std::string& directory() const { return directory_; }

    // The path a key would live at, or empty when the key is not a blob id. A key arrives from a hash,
    // but the API takes a string, so the shape is checked rather than trusted: anything that could walk
    // out of the cache directory is refused - and the refusing is counted, so this takes the lock.
    std::string blob_path(const std::string& key) const;

    // True when the blob is there and read. `misses` and `hits` count the calls, so `cache.stats` says
    // what the app did this run as well as what is on disk.
    bool lookup(const std::string& key, std::vector<std::uint8_t>& png, std::string& error);
    bool store(const std::string& key, const std::vector<std::uint8_t>& png, std::string& error);

    struct Stats {
        std::size_t entries = 0;
        std::size_t bytes = 0;
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t refused_keys = 0;
        std::string directory;
    };
    Stats stats() const;
    nlohmann::json json() const;

private:
    // For callers that already hold `mutex_`, since taking it twice would deadlock.
    std::string blob_path_locked(const std::string& key) const;

    std::string directory_;
    // Counters of calls, not of contents: mutable because asking what is on disk is a const question
    // and refusing a bad key is something a const path does. The worker writes them (`lookup`, `store`)
    // while the thread that draws reads `cache.stats`, so they sit behind a lock the type owns: tearing
    // counters would not corrupt an answer, but they would report a run that never happened.
    mutable std::mutex mutex_;
    mutable std::size_t hits_ = 0;
    mutable std::size_t misses_ = 0;
    mutable std::size_t refused_keys_ = 0;
};

}  // namespace noise::ai
