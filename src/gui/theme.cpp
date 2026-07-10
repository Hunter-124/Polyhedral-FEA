// SPDX-License-Identifier: AGPL-3.0-or-later
#include "theme.hpp"

namespace polymesh::gui {

Palette palette;

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    const Palette& p = palette;

    // Interwebz-v2 geometry: tight 2px rounding, thin frames, compact rows.
    s.WindowRounding = 2.0f;
    s.ChildRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.PopupRounding = 2.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = {12, 12};
    s.FramePadding = {7, 4};
    s.ItemSpacing = {8, 7};
    s.ScrollbarSize = 10.0f;
    s.WindowMinSize = {320, 240};

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = p.panel_bg;
    c[ImGuiCol_ChildBg] = p.panel_bg;
    c[ImGuiCol_PopupBg] = p.popup_bg;
    c[ImGuiCol_Border] = p.border;
    c[ImGuiCol_Text] = p.text;
    c[ImGuiCol_TextDisabled] = p.text_disabled;
    c[ImGuiCol_FrameBg] = p.frame_bg;
    c[ImGuiCol_FrameBgHovered] = p.frame_bg_hovered;
    c[ImGuiCol_FrameBgActive] = p.frame_bg_active;
    c[ImGuiCol_TitleBg] = p.header_bg;
    c[ImGuiCol_TitleBgActive] = p.header_bg;
    c[ImGuiCol_TitleBgCollapsed] = p.header_bg;
    c[ImGuiCol_MenuBarBg] = p.header_bg;
    c[ImGuiCol_ScrollbarBg] = p.window_bg;
    c[ImGuiCol_ScrollbarGrab] = p.button;
    c[ImGuiCol_ScrollbarGrabHovered] = p.button_hovered;
    c[ImGuiCol_ScrollbarGrabActive] = p.button_active;
    c[ImGuiCol_CheckMark] = p.accent;
    c[ImGuiCol_SliderGrab] = p.accent_dim;
    c[ImGuiCol_SliderGrabActive] = p.accent;
    c[ImGuiCol_Button] = p.button;
    c[ImGuiCol_ButtonHovered] = p.button_hovered;
    c[ImGuiCol_ButtonActive] = p.button_active;
    c[ImGuiCol_Header] = p.accent_soft;
    c[ImGuiCol_HeaderHovered] = p.accent_mid;
    c[ImGuiCol_HeaderActive] = p.accent_mid;
    c[ImGuiCol_Separator] = p.border;
    c[ImGuiCol_ResizeGrip] = p.accent_soft;
    c[ImGuiCol_ResizeGripHovered] = p.accent_mid;
    c[ImGuiCol_ResizeGripActive] = p.accent;
    c[ImGuiCol_Tab] = p.header_bg;
    c[ImGuiCol_TabHovered] = p.accent_mid;
    c[ImGuiCol_TabSelected] = p.panel_bg;
    c[ImGuiCol_DockingPreview] = p.accent_soft;
    c[ImGuiCol_PlotHistogram] = p.accent;
    c[ImGuiCol_TextSelectedBg] = p.accent_soft;
    c[ImGuiCol_NavCursor] = p.accent;
}

} // namespace polymesh::gui
