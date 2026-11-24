// SPDX-License-Identifier: Apache-2.0
#include "app/ui_icons.h"

#include "app/ui_lucide_data.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace noise::app::ui_icons {
namespace {

constexpr float kLucideGrid = 24.0f;
constexpr float kIconSize = 18.0f;
constexpr float kTextIconSize = 15.0f;
constexpr float kLabeledButtonGap = 6.0f;
static_assert(data::glyph_count == static_cast<std::size_t>(Icon::MarkAdd));

void draw_outline(Icon icon, ImVec2 pos, float size, ImU32 color, float min_stroke = 0.0f) {
    const auto index = static_cast<std::size_t>(icon);
    if (index >= data::glyph_count) return;
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float scale = size / kLucideGrid;
    const float stroke = std::max(2.0f * scale, min_stroke);
    const auto point = [&](float x, float y) {
        return ImVec2(pos.x + x * scale, pos.y + y * scale);
    };
    const data::Glyph glyph = data::glyphs[index];
    for (std::size_t path_index = glyph.first; path_index < glyph.first + glyph.count; ++path_index) {
        const data::Path path = data::paths[path_index];
        ImVec2 first{};
        ImVec2 last{};
        bool closed = false;
        for (std::size_t i = path.first; i < path.first + path.count; ++i) {
            const data::Op& op = data::ops[i];
            switch (op.type) {
                case 'M':
                    first = last = point(op.a, op.b);
                    list->PathLineTo(first);
                    break;
                case 'L':
                    last = point(op.a, op.b);
                    list->PathLineTo(last);
                    break;
                case 'C':
                    last = point(op.e, op.f);
                    list->PathBezierCubicCurveTo(point(op.a, op.b), point(op.c, op.d), last);
                    break;
                case 'Z':
                    closed = true;
                    break;
            }
        }
        list->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, stroke);
        if (!closed) {
            list->AddCircleFilled(first, stroke * 0.5f, color, 8);
            list->AddCircleFilled(last, stroke * 0.5f, color, 8);
        }
    }
}

ImU32 text_color() {
    return ImGui::GetColorU32(ImGuiCol_Text);
}

void icon_on_last_item(Icon icon, float size) {
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (icon == Icon::MarkAdd || icon == Icon::MarkRemove) {
        constexpr float kMarker = 20.0f;
        constexpr float kSign = 16.0f;
        constexpr float kGap = 3.0f;
        const float width = kMarker + kGap + kSign;
        const ImVec2 pos(min.x + (max.x - min.x - width) * 0.5f,
                         min.y + (max.y - min.y - kMarker) * 0.5f);
        draw_outline(Icon::Highlighter, pos, kMarker, text_color());
        const ImVec2 sign_pos(pos.x + kMarker + kGap, pos.y + (kMarker - kSign) * 0.5f);
        draw_outline(icon == Icon::MarkAdd ? Icon::Plus : Icon::Minus,
                     sign_pos, kSign, text_color(), 2.1f);
        return;
    }
    const ImVec2 pos(min.x + (max.x - min.x - size) * 0.5f,
               min.y + (max.y - min.y - size) * 0.5f);
    draw(icon, pos, size, text_color());
}

} // namespace

void draw(Icon icon, ImVec2 top_left, float size, ImU32 color) {
    const bool mark_plus = icon == Icon::MarkAdd;
    const bool mark_minus = icon == Icon::MarkRemove;
    const bool mask_plus = icon == Icon::MaskAdd;
    const bool mask_minus = icon == Icon::MaskRemove;
    const bool save_as = icon == Icon::SaveAs;
    if (mark_plus || mark_minus || mask_plus || mask_minus || save_as) {
        const Icon base = mark_plus || mark_minus ? Icon::Highlighter :
                          mask_plus || mask_minus ? Icon::Contrast : Icon::Save;
        draw_outline(base, top_left, size, color);
        const Icon badge = mark_minus ? Icon::Minus : mask_minus ? Icon::X : Icon::Plus;
        const float badge_size = size * 0.55f;
        const ImVec2 badge_pos(top_left.x + size * 0.50f, top_left.y + size * 0.51f);
        draw_outline(badge, badge_pos, badge_size, color);
        return;
    }
    draw_outline(icon, top_left, size, color);
}

bool button(Icon icon, const char* id, ImVec2 size, const char* tooltip) {
    ImGui::PushID(id);
    const bool pressed = ImGui::Button("##icon", size);
    icon_on_last_item(icon, kIconSize);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::PopID();
    return pressed;
}

float labeled_button_width(const char* text) {
    const float text_width = ImGui::CalcTextSize(text).x;
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    return text_width + kTextIconSize + kLabeledButtonGap + padding.x * 2.0f;
}

bool labeled_button(Icon icon, const char* text, ImVec2 size) {
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    const float width = size.x > 0.0f ? size.x : labeled_button_width(text);
    const float height = size.y > 0.0f ? size.y : ImGui::GetFrameHeight();
    ImGui::PushID(text);
    ImGui::PushID(static_cast<int>(icon));
    const bool pressed = ImGui::Button("##labeled_button", ImVec2(width, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 pos(min.x + padding.x,
                     min.y + (max.y - min.y - kTextIconSize) * 0.5f);
    draw(icon, pos, kTextIconSize, text_color());
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(pos.x + kTextIconSize + kLabeledButtonGap,
               min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_Text), text);
    ImGui::PopID();
    ImGui::PopID();
    return pressed;
}

std::optional<Icon> for_tool(std::string_view tool) {
    if (tool == "move") return Icon::Move;
    if (tool == "select") return Icon::SquareDashed;
    if (tool == "select_ellipse") return Icon::CircleDashed;
    if (tool == "lasso") return Icon::Lasso;
    if (tool == "cover") return Icon::MarkAdd;
    if (tool == "un-cover") return Icon::MarkRemove;
    if (tool == "brush") return Icon::Paintbrush;
    if (tool == "eraser") return Icon::Eraser;
    return std::nullopt;
}

Icon for_verb(std::string_view verb) {
    if (verb == "heal") return Icon::WandSparkles;
    if (verb == "remove") return Icon::Eraser;
    if (verb == "fill") return Icon::PaintBucket;
    if (verb == "change") return Icon::RefreshCw;
    return Icon::Sparkles;
}

} // namespace noise::app::ui_icons
