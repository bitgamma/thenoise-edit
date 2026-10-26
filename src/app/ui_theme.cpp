// SPDX-License-Identifier: Apache-2.0
#include "app/ui_theme.h"

#include <cstdlib>
#include <optional>

namespace noise::app::ui_theme {
namespace {

constexpr Palette kDark{
    {0.949f, 0.937f, 0.898f, 1.0f},
    {0.780f, 0.761f, 0.710f, 1.0f},
    {0.659f, 0.639f, 0.604f, 1.0f},
    {0.055f, 0.055f, 0.043f, 1.0f},
    {0.102f, 0.094f, 0.075f, 1.0f},
    {0.122f, 0.114f, 0.090f, 1.0f},
    {0.176f, 0.161f, 0.133f, 1.0f},
    {1.0f, 1.0f, 1.0f, 0.10f},
    {0.988f, 0.847f, 0.275f, 0.24f},
    {0.471f, 0.667f, 1.0f, 1.0f},
    {0.59f, 0.73f, 1.0f, 1.0f},
    {0.28f, 0.42f, 0.62f, 1.0f},
    {0.23f, 0.39f, 0.66f, 1.0f},
    {0.949f, 0.949f, 0.95f, 1.0f},
    {0.471f, 0.659f, 0.420f, 1.0f},
    {0.949f, 0.725f, 0.310f, 1.0f},
    {0.937f, 0.553f, 0.498f, 1.0f},
    {0.533f, 0.741f, 0.914f, 1.0f},
    {0.149f, 0.149f, 0.165f, 1.0f},
    {0.353f, 0.353f, 0.392f, 1.0f},
    {0.125f, 0.125f, 0.149f, 1.0f},
    {0.165f, 0.180f, 0.235f, 1.0f},
    {0.275f, 0.275f, 0.322f, 1.0f},
    {0.471f, 0.667f, 1.0f, 1.0f},
    {0.086f, 0.086f, 0.102f, 0.95f},
    {0.471f, 0.471f, 0.518f, 0.78f},
    {0.055f, 0.055f, 0.043f, 0.78f},
};

constexpr Palette kLight{
    {0.102f, 0.094f, 0.075f, 1.0f},
    {0.176f, 0.184f, 0.184f, 1.0f},
    {0.420f, 0.400f, 0.365f, 1.0f},
    {0.992f, 0.984f, 0.965f, 1.0f},
    {0.937f, 0.922f, 0.882f, 1.0f},
    {0.961f, 0.949f, 0.925f, 1.0f},
    {1.0f, 0.996f, 0.980f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.15f},
    {0.361f, 0.294f, 0.0f, 0.30f},
    {0.22f, 0.42f, 0.78f, 1.0f},
    {0.30f, 0.50f, 0.88f, 1.0f},
    {0.75f, 0.82f, 0.96f, 1.0f},
    {0.55f, 0.69f, 0.91f, 1.0f},
    {0.1f, 0.1f, 0.16f, 1.0f},
    {0.235f, 0.510f, 0.235f, 1.0f},
    {0.580f, 0.357f, 0.0f, 1.0f},
    {0.690f, 0.196f, 0.153f, 1.0f},
    {0.078f, 0.420f, 0.690f, 1.0f},
    {0.149f, 0.149f, 0.165f, 1.0f},
    {0.353f, 0.353f, 0.392f, 1.0f},
    {0.92f, 0.92f, 0.94f, 1.0f},
    {0.82f, 0.86f, 0.96f, 1.0f},
    {0.55f, 0.55f, 0.60f, 1.0f},
    {0.25f, 0.47f, 0.81f, 1.0f},
    {0.94f, 0.94f, 0.96f, 0.95f},
    {0.45f, 0.45f, 0.50f, 0.78f},
    {0.102f, 0.094f, 0.075f, 0.38f},
};

std::optional<io::Theme> applied_theme;

}  // namespace

const Palette& palette(io::Theme theme) {
    switch (theme) {
        case io::Theme::dark: return kDark;
        case io::Theme::light: return kLight;
    }
    std::abort();
}

void apply(io::Theme theme) {
    if (applied_theme == theme) return;
    switch (theme) {
        case io::Theme::dark: ImGui::StyleColorsDark(); break;
        case io::Theme::light: ImGui::StyleColorsLight(); break;
    }
    applied_theme = theme;
}

}  // namespace noise::app::ui_theme
