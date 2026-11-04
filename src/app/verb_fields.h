// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

namespace noise::app {

// One phrase box per asking verb, shared by every door that types into it - the panel row and the bar's
// popup - so the two cannot hold different text for the same intent. The truth is still `ai.intent`:
// these are edit buffers, refilled from it whenever nothing holds them.
std::map<std::string, std::string>& verb_field_boxes();

}  // namespace noise::app
