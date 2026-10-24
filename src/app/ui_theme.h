// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "io/config.h"

#include "imgui.h"

namespace noise::app::ui_theme {

struct Palette {
    ImVec4 text;
    ImVec4 text_secondary;
    ImVec4 text_disabled;
    ImVec4 base;
    ImVec4 panel;
    ImVec4 surface;
    ImVec4 raised;
    ImVec4 border;
    ImVec4 border_strong;
    ImVec4 accent;
    ImVec4 accent_hover;
    ImVec4 accent_soft;
    ImVec4 accent_strong;
    ImVec4 accent_ink;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 danger;
    ImVec4 info;
    ImVec4 canvas_backdrop;
    ImVec4 canvas_border;
    ImVec4 card;
    ImVec4 card_selected;
    ImVec4 card_border;
    ImVec4 card_selected_border;
    ImVec4 floating;
    ImVec4 floating_border;
    ImVec4 modal_dim;
};

const Palette& palette(io::Theme theme);
void apply(io::Theme theme);

}
