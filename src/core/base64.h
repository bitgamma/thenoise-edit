// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace noise {

std::string base64_encode(std::span<const std::uint8_t> bytes);

}
