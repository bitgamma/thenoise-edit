// SPDX-License-Identifier: Apache-2.0
#include "core/log.h"

#include <cstdio>
#include <deque>
#include <mutex>

namespace noise::log {

namespace {

constexpr std::size_t kRingCapacity = 4096;

std::mutex g_mutex;
std::deque<std::string> g_ring;
Level g_level = Level::Info;

}

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "UNKNOWN";
}

void set_level(Level level) {
    std::lock_guard lock(g_mutex);
    g_level = level;
}

Level level() {
    std::lock_guard lock(g_mutex);
    return g_level;
}

void record(Level level, const std::string& message) {
    bool echo = false;
    {
        std::lock_guard lock(g_mutex);
        echo = static_cast<int>(level) >= static_cast<int>(g_level);
        g_ring.push_back(std::string(level_name(level)) + " " + message);
        if (g_ring.size() > kRingCapacity) {
            g_ring.pop_front();
        }
    }
    if (echo) {
        std::fprintf(stderr, "[%s] %s\n", level_name(level), message.c_str());
    }
}

std::vector<std::string> tail(std::size_t count) {
    std::lock_guard lock(g_mutex);
    const std::size_t begin = g_ring.size() > count ? g_ring.size() - count : 0;
    return std::vector<std::string>(g_ring.begin() + static_cast<std::ptrdiff_t>(begin), g_ring.end());
}

void debug(const std::string& message) { record(Level::Debug, message); }
void info(const std::string& message) { record(Level::Info, message); }
void warn(const std::string& message) { record(Level::Warn, message); }
void error(const std::string& message) { record(Level::Error, message); }

}
