// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "imgui.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace noise::app::ui_icons {

enum class Icon : std::uint16_t {
    Move, SquareDashed, CircleDashed, Lasso, Highlighter, Plus, Minus,
    Paintbrush, Eraser, Layers, LayersPlus, Trash, ArrowUp, ArrowDown,
    Eye, EyeOff, Merge, Layers2, Contrast, Blend, ImageUpscale,
    WandSparkles, PaintBucket, RefreshCw, Sparkles, FilePlus, FolderOpen,
    Save, FileOutput, ImagePlus, Undo2, Redo2, Scaling, Expand, Crop,
    RotateCcw, RotateCw, Ratio, Palette, Images, X, Plug, LoaderCircle,
    CircleCheck, ScrollText, Settings2, SquareDashedX, Image, Feather,
    Check, MarkAdd, MarkRemove, MaskAdd, MaskRemove, SaveAs,
};

void draw(Icon icon, ImVec2 top_left, float size, ImU32 color);
bool button(Icon icon, const char* id, ImVec2 size, const char* tooltip);
float labeled_button_width(const char* text);
bool labeled_button(Icon icon, const char* text, ImVec2 size);
std::optional<Icon> for_tool(std::string_view tool);
Icon for_verb(std::string_view verb);

} // namespace noise::app::ui_icons
