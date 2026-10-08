// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace noise::log {

enum class Level : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

const char* level_name(Level level);

void set_level(Level level);
Level level();

// Records the message into the ring buffer and mirrors it to stderr when it
// passes the level filter. The ring is what the debug wire reads.
void record(Level level, const std::string& message);

// Newest last, each entry formatted "<LEVEL> <message>".
std::vector<std::string> tail(std::size_t count);

void debug(const std::string& message);
void info(const std::string& message);
void warn(const std::string& message);
void error(const std::string& message);

}
