// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace noise::app {

// The keys that nudge a brush field by one step, as rows. `paint_commands.cpp` registers every row
// as a command (and the command table is what binds the key), and the Brush panel finds a field's
// two commands here so the hint beside a slider is never a second copy of the binding.
//
// `field` is a row of the brush-field table in `paint_commands.cpp`, which owns the range the step
// clamps to. `shortcut` is canonical text (core/shortcut.h) and empty for a field with no key.
struct BrushStep {
    const char* id;
    const char* field;
    int direction;  // -1 lowers the field, +1 raises it
    const char* shortcut;
    const char* title;
};

// The MVP binds `,`/`.` to "hardness/opacity"; four actions on two keys means the modifier takes
// the second pair, which is what the shortcut grammar is for. Flow and spacing get no keys: the
// sliders and `tool.set` reach them, and a binding nobody asked for is a key someone else cannot
// use.
inline constexpr BrushStep kBrushSteps[]{
    {"brush.hardness_down", "hardness", -1, ",", "Soften the brush edge one step"},
    {"brush.hardness_up", "hardness", 1, ".", "Harden the brush edge one step"},
    {"brush.opacity_down", "opacity", -1, "Shift+,", "Make the stroke fainter one step"},
    {"brush.opacity_up", "opacity", 1, "Shift+.", "Make the stroke more opaque one step"},
};

}  // namespace noise::app
