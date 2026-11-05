// SPDX-License-Identifier: Apache-2.0
#include "app/verb_fields.h"

namespace noise::app {

std::map<std::string, std::string>& verb_field_boxes() {
    static std::map<std::string, std::string> boxes;
    return boxes;
}

}  // namespace noise::app
