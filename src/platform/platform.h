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
void sleep_ms(int ms);

std::int64_t pid();

}
