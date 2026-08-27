// SPDX-License-Identifier: BSD-3-Clause
#include "theme.hpp"

#include <algorithm>

namespace polymesh::gui {
namespace {

ImVec4 with_alpha(const ImVec4& color, float alpha) {
    ImVec4 result = color;
    result.w *= alpha;
    return result;
}

ImU32 color_u32(const ImVec4& color, float alpha) {
    return ImGui::GetColorU32(with_alpha(color, alpha));
}

} // namespace

Palette palette;
float ui_scale = 1.0f;

void set_ui_scale(float scale) { ui_scale = std::clamp(scale, 0.75f, 3.0f); }

void apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    const Palette& p = palette;

    style.WindowRounding = ui_px(9.0f);
    style.ChildRounding = ui_px(9.0f);
    style.FrameRounding = ui_px(7.0f);
    style.PopupRounding = ui_px(8.0f);
    style.ScrollbarRounding = ui_px(10.0f);
    style.GrabRounding = ui_px(7.0f);
    style.TabRounding = ui_px(8.0f);
    style.WindowPadding = {ui_px(15.0f), ui_px(13.0f)};
    style.FramePadding = {ui_px(12.0f), ui_px(7.0f)};
    style.ItemSpacing = {ui_px(10.0f), ui_px(9.0f)};
    style.CellPadding = {ui_px(8.0f), ui_px(6.0f)};
    style.ScrollbarSize = ui_px(12.0f);
    style.WindowBorderSize = ui_px(1.0f);
    style.FrameBorderSize = ui_px(1.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = p.text;
    colors[ImGuiCol_TextDisabled] = p.text_disabled;
    colors[ImGuiCol_WindowBg] = p.window_bg;
    colors[ImGuiCol_ChildBg] = p.panel_bg;
    colors[ImGuiCol_PopupBg] = p.popup_bg;
    colors[ImGuiCol_Border] = p.border;
    colors[ImGuiCol_BorderShadow] = p.glass_shadow;
    colors[ImGuiCol_FrameBg] = p.frame_bg;
    colors[ImGuiCol_FrameBgHovered] = p.frame_bg_hovered;
    colors[ImGuiCol_FrameBgActive] = p.frame_bg_active;
    colors[ImGuiCol_TitleBg] = p.header_bg;
    colors[ImGuiCol_TitleBgActive] = p.header_bg;
    colors[ImGuiCol_TitleBgCollapsed] = p.header_bg;
    colors[ImGuiCol_MenuBarBg] = p.header_bg;
    colors[ImGuiCol_ScrollbarBg] = p.window_bg;
    colors[ImGuiCol_ScrollbarGrab] = p.button;
    colors[ImGuiCol_ScrollbarGrabHovered] = p.button_hovered;
    colors[ImGuiCol_ScrollbarGrabActive] = p.button_active;
    colors[ImGuiCol_CheckMark] = p.accent;
    colors[ImGuiCol_SliderGrab] = p.accent_dim;
    colors[ImGuiCol_SliderGrabActive] = p.accent;
    colors[ImGuiCol_Button] = p.button;
    colors[ImGuiCol_ButtonHovered] = p.button_hovered;
    colors[ImGuiCol_ButtonActive] = p.button_active;
    colors[ImGuiCol_Header] = p.accent_soft;
    colors[ImGuiCol_HeaderHovered] = p.accent_mid;
    colors[ImGuiCol_HeaderActive] = p.accent_mid;
    colors[ImGuiCol_Separator] = p.border;
    colors[ImGuiCol_SeparatorHovered] = p.accent_mid;
    colors[ImGuiCol_SeparatorActive] = p.accent;
    colors[ImGuiCol_ResizeGrip] = p.accent_soft;
    colors[ImGuiCol_ResizeGripHovered] = p.accent_mid;
    colors[ImGuiCol_ResizeGripActive] = p.accent;
    colors[ImGuiCol_TabHovered] = p.accent_mid;
    colors[ImGuiCol_Tab] = p.header_bg;
    colors[ImGuiCol_TabSelected] = p.panel_bg;
    colors[ImGuiCol_TabSelectedOverline] = p.accent;
    colors[ImGuiCol_TabDimmed] = p.window_bg;
    colors[ImGuiCol_TabDimmedSelected] = p.panel_bg;
    colors[ImGuiCol_TabDimmedSelectedOverline] = p.accent_dim;
    colors[ImGuiCol_DockingPreview] = p.accent_soft;
    colors[ImGuiCol_DockingEmptyBg] = p.window_bg;
    colors[ImGuiCol_PlotLines] = p.accent2;
    colors[ImGuiCol_PlotLinesHovered] = p.accent;
    colors[ImGuiCol_PlotHistogram] = p.accent;
    colors[ImGuiCol_PlotHistogramHovered] = p.accent_soft_top;
    colors[ImGuiCol_TableHeaderBg] = p.header_bg;
    colors[ImGuiCol_TableBorderStrong] = p.border;
    colors[ImGuiCol_TableBorderLight] = p.border;
    colors[ImGuiCol_TableRowBg] = p.panel_bg;
    colors[ImGuiCol_TableRowBgAlt] = p.surface_hi;
    colors[ImGuiCol_TextLink] = p.accent2;
    colors[ImGuiCol_TextSelectedBg] = p.accent_soft;
    colors[ImGuiCol_DragDropTarget] = p.accent;
    colors[ImGuiCol_NavCursor] = p.accent;
    colors[ImGuiCol_NavWindowingHighlight] = p.accent_mid;
    colors[ImGuiCol_NavWindowingDimBg] = p.glass_shadow;
    colors[ImGuiCol_ModalWindowDimBg] = p.glass_shadow;
}

void glass_background(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding, float alpha) {
    if (dl == nullptr || mx.x <= mn.x || mx.y <= mn.y) {
        return;
    }

    const float opacity = std::clamp(alpha, 0.0f, 1.0f);
    if (opacity <= 0.0f) {
        return;
    }

    const float radius = ui_px(rounding);
    const float border_width = ui_px(1.0f);

    const float shadow_expand_outer = ui_px(5.0f);
    const float shadow_expand_middle = ui_px(3.0f);
    const float shadow_expand_inner = ui_px(1.5f);
    dl->AddRectFilled(
        ImVec2(mn.x - shadow_expand_outer, mn.y - shadow_expand_outer + ui_px(4.0f)),
        ImVec2(mx.x + shadow_expand_outer, mx.y + shadow_expand_outer + ui_px(4.0f)),
        color_u32(palette.glass_shadow, opacity * 0.16f), radius + shadow_expand_outer);
    dl->AddRectFilled(
        ImVec2(mn.x - shadow_expand_middle, mn.y - shadow_expand_middle + ui_px(3.0f)),
        ImVec2(mx.x + shadow_expand_middle, mx.y + shadow_expand_middle + ui_px(3.0f)),
        color_u32(palette.glass_shadow, opacity * 0.24f), radius + shadow_expand_middle);
    dl->AddRectFilled(
        ImVec2(mn.x - shadow_expand_inner, mn.y - shadow_expand_inner + ui_px(2.0f)),
        ImVec2(mx.x + shadow_expand_inner, mx.y + shadow_expand_inner + ui_px(2.0f)),
        color_u32(palette.glass_shadow, opacity * 0.34f), radius + shadow_expand_inner);

    dl->AddRectFilled(mn, mx, color_u32(palette.glass_bg, opacity), radius);

    const float shine_inset = std::max(border_width, radius * 0.35f);
    const float shine_bottom = mn.y + (mx.y - mn.y) * 0.40f;
    dl->AddRectFilledMultiColor(
        ImVec2(mn.x + shine_inset, mn.y + border_width),
        ImVec2(mx.x - shine_inset, shine_bottom), color_u32(palette.glass_highlight, opacity),
        color_u32(palette.glass_highlight, opacity), color_u32(palette.glass_highlight, 0.0f),
        color_u32(palette.glass_highlight, 0.0f));

    dl->AddRect(mn, mx, color_u32(palette.glass_border, opacity), radius,
                ImDrawFlags_RoundCornersAll, border_width);

    const float specular_inset = std::max(radius, ui_px(5.0f));
    const float specular_y = mn.y + border_width;
    dl->AddLine(ImVec2(mn.x + specular_inset, specular_y),
                ImVec2(mx.x - specular_inset, specular_y),
                color_u32(palette.glass_highlight, opacity * 1.5f), border_width);
}

} // namespace polymesh::gui
