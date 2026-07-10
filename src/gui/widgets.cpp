// SPDX-License-Identifier: AGPL-3.0-or-later
#include "widgets.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cstdio>

namespace polymesh::gui::iw {
namespace {

ImU32 u32(const ImVec4& c) { return ImGui::GetColorU32(c); }

// Interwebz-look building blocks, all read from the palette (theme.hpp).
void draw_box(ImDrawList* dl, const ImVec2& min, const ImVec2& max, bool hovered) {
    dl->AddRectFilled(min, max, u32(hovered ? palette.frame_bg_hovered : palette.frame_bg),
                      2.0f);
    dl->AddRect(min, max, u32(palette.border), 2.0f);
}

void draw_accent_fill(ImDrawList* dl, const ImVec2& min, const ImVec2& max) {
    dl->AddRectFilled(min, max, u32(palette.accent), 2.0f);
    dl->AddRectFilledMultiColor(ImVec2(min.x + 1, min.y + 1), ImVec2(max.x - 1, max.y - 1),
                                u32(palette.accent_soft_top), u32(palette.accent),
                                u32(palette.accent), u32(palette.accent_soft_top));
}

} // namespace

void begin_group_box(const char* title, float content_height) {
    constexpr float kHeader = 20.0f;
    constexpr float kPad = 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 size(width, content_height + kHeader + 2 * kPad);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), u32(palette.panel_bg),
                      3.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + kHeader), u32(palette.header_bg),
                      3.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), u32(palette.border), 3.0f);
    dl->AddText(ImVec2(pos.x + 12, pos.y + 3), u32(palette.text), title);

    ImGui::SetCursorScreenPos(ImVec2(pos.x + kPad, pos.y + kHeader + kPad));
    ImGui::BeginChild(title, ImVec2(size.x - 2 * kPad, content_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
}

void end_group_box() {
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 14)); // outer padding + spacing to the next box
}

bool checkbox(const char* label, bool* value) {
    constexpr float kBox = 15.0f;
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const bool pressed =
        ImGui::InvisibleButton("##cb", ImVec2(kBox + 8 + label_size.x, kBox + 2));
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 box_min(pos.x, pos.y + 1);
    const ImVec2 box_max(pos.x + kBox, pos.y + 1 + kBox);
    if (*value) {
        draw_accent_fill(dl, box_min, box_max);
        // Check mark: two strokes, white.
        const float s = kBox;
        dl->AddLine(ImVec2(box_min.x + 0.25f * s, box_min.y + 0.55f * s),
                    ImVec2(box_min.x + 0.42f * s, box_min.y + 0.72f * s), u32(palette.text),
                    1.6f);
        dl->AddLine(ImVec2(box_min.x + 0.42f * s, box_min.y + 0.72f * s),
                    ImVec2(box_min.x + 0.76f * s, box_min.y + 0.30f * s), u32(palette.text),
                    1.6f);
    } else {
        draw_box(dl, box_min, box_max, hovered);
    }
    dl->AddText(ImVec2(box_max.x + 8, pos.y + 1),
                u32(hovered ? palette.text : palette.text_dim), label);
    ImGui::PopID();
    return pressed;
}

bool slider_double(const char* label, double* value, double min, double max,
                   const char* format) {
    constexpr float kBar = 10.0f;
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Label row: name left, value right, lavender.
    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), format, *value);
    dl->AddText(pos, u32(palette.text_dim), label);
    const ImVec2 vsize = ImGui::CalcTextSize(value_text);
    dl->AddText(ImVec2(pos.x + width - vsize.x, pos.y), u32(palette.text_dim), value_text);
    ImGui::Dummy(ImVec2(width, vsize.y + 2));

    pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slider", ImVec2(width, kBar + 4));
    const bool active = ImGui::IsItemActive();
    if (active) {
        const float mouse_t =
            std::clamp((ImGui::GetIO().MousePos.x - pos.x) / width, 0.0f, 1.0f);
        *value = min + (max - min) * static_cast<double>(mouse_t);
    }
    const ImVec2 bar_min(pos.x, pos.y + 2);
    const ImVec2 bar_max(pos.x + width, pos.y + 2 + kBar);
    draw_box(dl, bar_min, bar_max, ImGui::IsItemHovered());
    const float t = max > min ? static_cast<float>((*value - min) / (max - min)) : 0.0f;
    if (t > 0.0f) {
        draw_accent_fill(dl, bar_min,
                         ImVec2(bar_min.x + std::max(3.0f, t * width), bar_max.y));
    }
    ImGui::PopID();
    return active;
}

bool button(const char* label, const ImVec2& size, bool primary) {
    ImGui::PushID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const float width = size.x != 0 ? (size.x < 0 ? ImGui::GetContentRegionAvail().x : size.x)
                                    : label_size.x + 24;
    const float height = size.y != 0 ? size.y : label_size.y + 10;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##btn", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    if (primary) {
        draw_accent_fill(dl, pos, max);
        if (hovered) {
            dl->AddRect(pos, max, u32(palette.text), 2.0f);
        }
    } else {
        draw_box(dl, pos, max, hovered);
    }
    dl->AddText(
        ImVec2(pos.x + 0.5f * (width - label_size.x), pos.y + 0.5f * (height - label_size.y)),
        u32(palette.text), label);
    ImGui::PopID();
    return pressed;
}

namespace {

void field_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette.text_dim, "%s", label);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
    ImGui::SetNextItemWidth(-1);
}

} // namespace

bool input_double(const char* label, double* value, const char* format) {
    ImGui::PushID(label);
    field_label(label);
    const bool changed = ImGui::InputDouble("##v", value, 0.0, 0.0, format);
    ImGui::PopID();
    return changed;
}

bool input_float3(const char* label, float value[3]) {
    ImGui::PushID(label);
    field_label(label);
    const bool changed = ImGui::InputFloat3("##v", value, "%.1f");
    ImGui::PopID();
    return changed;
}

bool input_text(const char* label, char* buffer, size_t buffer_size, const char* hint) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1);
    const bool changed = ImGui::InputTextWithHint("##t", hint, buffer, buffer_size);
    ImGui::PopID();
    return changed;
}

bool selector(const char* label, int* index, const char* const* options, int count) {
    ImGui::PushID(label);
    bool changed = false;
    const float width =
        (ImGui::GetContentRegionAvail().x - 4.0f * static_cast<float>(count - 1)) /
        static_cast<float>(count);
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            ImGui::SameLine(0, 4);
        }
        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float height = ImGui::GetTextLineHeight() + 8;
        const bool pressed = ImGui::InvisibleButton("##opt", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + width, pos.y + height);
        if (*index == i) {
            draw_accent_fill(dl, pos, max);
        } else {
            draw_box(dl, pos, max, hovered);
        }
        const ImVec2 tsize = ImGui::CalcTextSize(options[i]);
        dl->AddText(ImVec2(pos.x + 0.5f * (width - tsize.x), pos.y + 4),
                    u32(*index == i ? palette.text : palette.text_dim), options[i]);
        if (pressed && *index != i) {
            *index = i;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

} // namespace polymesh::gui::iw
