// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace noise::platform {

std::string config_dir();
std::string runtime_dir();
std::string executable_path();

std::optional<std::string> env(const std::string& name);

double now_seconds();
std::int64_t now_unix_ms();

// Entropy for one request. The only place noise enters the app, so that "no `rand()` in logic a test
// can reach" is a fact about one function rather than about every caller; a test replaces the seam in
// `AiSession` rather than this.
std::uint64_t random_seed();
void sleep_ms(int ms);

std::int64_t pid();

}
