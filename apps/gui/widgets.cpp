// SPDX-License-Identifier: BSD-3-Clause
#include "widgets.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

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

// ---- auto-sized group box ----
// WindowBg is already panel_bg, so we only paint the header strip + border after
// measuring content (no draw-list splitter / channel gymnastics).

constexpr float kGroupHeader = 22.0f;
constexpr float kGroupPad = 10.0f;
constexpr float kGroupGap = 10.0f; // space after each box

struct GroupBoxFrame {
    ImVec2 start{};
    float width = 0.0f;
    const char* title = nullptr;
};

std::vector<GroupBoxFrame> g_group_stack;

// Horizontal padding inside custom buttons so labels clear the border.
constexpr float kBtnPadX = 12.0f;
constexpr float kBtnPadY = 6.0f;

/// Draw label text centered in [min,max], clipped so it never spills the box.
void draw_centered_label(ImDrawList* dl, const ImVec2& min, const ImVec2& max, const char* text,
                         ImU32 col) {
    const ImVec2 tsize = ImGui::CalcTextSize(text);
    const float box_w = max.x - min.x;
    const float box_h = max.y - min.y;
    const ImVec2 tpos(min.x + 0.5f * (box_w - tsize.x), min.y + 0.5f * (box_h - tsize.y));
    dl->PushClipRect(min, max, true);
    dl->AddText(tpos, col, text);
    dl->PopClipRect();
}

/// Choose how many columns fit so every option's text + padding is readable.
int fit_selector_columns(const char* const* options, int count, float avail, float gap,
                         float pad_x) {
    for (int cols = count; cols >= 1; --cols) {
        const float cell = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        bool ok = true;
        for (int i = 0; i < count; ++i) {
            if (ImGui::CalcTextSize(options[i]).x + 2.0f * pad_x > cell + 0.5f) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return cols;
        }
    }
    return 1;
}

} // namespace

void begin_group_box(const char* title) {
    GroupBoxFrame frame;
    frame.start = ImGui::GetCursorScreenPos();
    frame.width = ImGui::GetContentRegionAvail().x;
    frame.title = title;

    // Outer group claims the full box width; header is reserved with a Dummy so
    // layout height is correct before content is measured.
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(frame.width, kGroupHeader));

    // Content child with BOTH left and right padding so GetContentRegionAvail()
    // already reserves kGroupPad on the right (PushItemWidth alone is not enough
    // for custom widgets that use avail width, or ImGui items with -FLT_MIN).
    const float content_w = std::max(1.0f, frame.width - 2.0f * kGroupPad);
    ImGui::SetCursorScreenPos(
        ImVec2(frame.start.x + kGroupPad, frame.start.y + kGroupHeader + kGroupPad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(title, ImVec2(content_w, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushItemWidth(content_w);

    g_group_stack.push_back(frame);
}

void end_group_box() {
    IM_ASSERT(!g_group_stack.empty());
    const GroupBoxFrame frame = g_group_stack.back();
    g_group_stack.pop_back();

    ImGui::PopItemWidth();
    ImGui::EndChild(); // content (auto-height)

    // Bottom padding inside the border; Dummy spans full frame width for chrome.
    ImGui::SetCursorScreenPos(
        ImVec2(frame.start.x, std::max(ImGui::GetItemRectMax().y, frame.start.y + kGroupHeader) +
                                  kGroupPad));
    ImGui::Dummy(ImVec2(frame.width, 0.0f));
    ImGui::EndGroup(); // outer chrome bounds

    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();

    // Draw chrome on the parent draw list (child is nested; outer group is parent).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header strip (covers the reserved Dummy only — content sits below it).
    dl->AddRectFilled(box_min, ImVec2(box_max.x, box_min.y + kGroupHeader), u32(palette.header_bg),
                      3.0f, ImDrawFlags_RoundCornersTop);
    dl->AddRect(box_min, box_max, u32(palette.border), 3.0f);
    dl->PushClipRect(box_min, ImVec2(box_max.x - 4.0f, box_min.y + kGroupHeader), true);
    dl->AddText(ImVec2(box_min.x + 12.0f, box_min.y + 3.0f), u32(palette.text), frame.title);
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(0.0f, kGroupGap));
}

namespace {

/// Preferred fill width for custom controls: respects PushItemWidth / group box
/// content width. Falls back to remaining content region if no item width set.
float fill_width() {
    const float item = ImGui::CalcItemWidth();
    const float avail = ImGui::GetContentRegionAvail().x;
    // CalcItemWidth can exceed avail when cursor is mid-row; never overflow.
    return std::max(1.0f, std::min(item, avail));
}

} // namespace

bool checkbox(const char* label, bool* value) {
    constexpr float kBox = 15.0f;
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const float hit_w = std::min(fill_width(), kBox + 8.0f + label_size.x);
    const bool pressed = ImGui::InvisibleButton("##cb", ImVec2(hit_w, kBox + 2.0f));
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 box_min(pos.x, pos.y + 1.0f);
    const ImVec2 box_max(pos.x + kBox, pos.y + 1.0f + kBox);
    if (*value) {
        draw_accent_fill(dl, box_min, box_max);
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
    const ImVec2 text_min(box_max.x + 8.0f, pos.y);
    const ImVec2 text_max(pos.x + hit_w, pos.y + kBox + 2.0f);
    dl->PushClipRect(text_min, text_max, true);
    dl->AddText(ImVec2(box_max.x + 8.0f, pos.y + 1.0f),
                u32(hovered ? palette.text : palette.text_dim), label);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed;
}

bool slider_double(const char* label, double* value, double min, double max,
                   const char* format) {
    constexpr float kBar = 10.0f;
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = fill_width();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), format, *value);
    const ImVec2 vsize = ImGui::CalcTextSize(value_text);
    const ImVec2 lsize = ImGui::CalcTextSize(label);
    // Clip label so it never runs into the value on the right.
    dl->PushClipRect(pos, ImVec2(pos.x + width - vsize.x - 6.0f, pos.y + lsize.y + 2.0f), true);
    dl->AddText(pos, u32(palette.text_dim), label);
    dl->PopClipRect();
    dl->AddText(ImVec2(pos.x + width - vsize.x, pos.y), u32(palette.text_dim), value_text);
    ImGui::Dummy(ImVec2(width, lsize.y + 2.0f));

    pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slider", ImVec2(width, kBar + 4.0f));
    const bool active = ImGui::IsItemActive();
    if (active) {
        const float mouse_t =
            std::clamp((ImGui::GetIO().MousePos.x - pos.x) / width, 0.0f, 1.0f);
        *value = min + (max - min) * static_cast<double>(mouse_t);
    }
    const ImVec2 bar_min(pos.x, pos.y + 2.0f);
    const ImVec2 bar_max(pos.x + width, pos.y + 2.0f + kBar);
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
    // size.x < 0 → fill item width (respects group-box PushItemWidth);
    // size.x == 0 → hug label; size.x > 0 → explicit.
    const float avail = fill_width();
    const float width = size.x < 0.0f ? avail
                        : size.x > 0.0f
                            ? size.x
                            : std::min(avail, label_size.x + 2.0f * kBtnPadX);
    const float height =
        size.y > 0.0f ? size.y : label_size.y + 2.0f * kBtnPadY;
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
    draw_centered_label(dl, pos, max, label, u32(palette.text));
    ImGui::PopID();
    return pressed;
}

namespace {

/// Place a dim label, then prepare width for a full-width field control.
/// Stacks the field under the label when the row would overflow.
void begin_field(const char* label) {
    const float avail = fill_width();
    const float text_w = ImGui::CalcTextSize(label).x;
    constexpr float kMinField = 96.0f;
    constexpr float kGap = 10.0f;

    if (text_w + kGap + kMinField > avail) {
        ImGui::TextColored(palette.text_dim, "%s", label);
        ImGui::SetNextItemWidth(fill_width());
        return;
    }

    // Side-by-side relative to this row's start (not window origin — group boxes
    // indent via SetCursorScreenPos, so SameLine(offset) would misalign).
    const float row_start = ImGui::GetCursorPosX();
    const float label_col =
        std::clamp(text_w + kGap, avail * 0.38f, avail - kMinField);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette.text_dim, "%s", label);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(row_start + label_col);
    ImGui::SetNextItemWidth(std::max(1.0f, avail - label_col));
}

} // namespace

bool input_double(const char* label, double* value, const char* format) {
    ImGui::PushID(label);
    begin_field(label);
    const bool changed = ImGui::InputDouble("##v", value, 0.0, 0.0, format);
    ImGui::PopID();
    return changed;
}

bool input_float3(const char* label, float value[3]) {
    ImGui::PushID(label);
    begin_field(label);
    const bool changed = ImGui::InputFloat3("##v", value, "%.1f");
    ImGui::PopID();
    return changed;
}

bool input_text(const char* label, char* buffer, size_t buffer_size, const char* hint) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(fill_width());
    const bool changed = ImGui::InputTextWithHint("##t", hint, buffer, buffer_size);
    ImGui::PopID();
    return changed;
}

bool selector(const char* label, int* index, const char* const* options, int count) {
    ImGui::PushID(label);
    bool changed = false;
    if (count <= 0) {
        ImGui::PopID();
        return false;
    }

    constexpr float kGap = 4.0f;
    constexpr float kPadX = 8.0f;
    // Symmetric left/right: width comes from PushItemWidth / content child.
    const float avail = fill_width();
    const float height = ImGui::GetTextLineHeight() + 2.0f * kBtnPadY;
    const int cols = fit_selector_columns(options, count, avail, kGap, kPadX);
    const float cell_w =
        (avail - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);

    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        if (col > 0) {
            ImGui::SameLine(0.0f, kGap);
        }

        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        // Last column absorbs leftover pixels so the row fills avail exactly
        // (symmetric padding comes from the group-box content width, not cells).
        float width = cell_w;
        if (col == cols - 1) {
            width = avail - (cell_w + kGap) * static_cast<float>(cols - 1);
        }
        width = std::max(width, 1.0f);

        const bool pressed = ImGui::InvisibleButton("##opt", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + width, pos.y + height);
        if (*index == i) {
            draw_accent_fill(dl, pos, max);
        } else {
            draw_box(dl, pos, max, hovered);
        }
        draw_centered_label(dl, pos, max, options[i],
                            u32(*index == i ? palette.text : palette.text_dim));
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
