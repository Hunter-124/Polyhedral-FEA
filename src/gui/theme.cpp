// SPDX-License-Identifier: AGPL-3.0-or-later
#include "theme.hpp"

namespace polymesh::gui {

Palette palette;

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    const Palette& p = palette;

    s.WindowRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.FrameRounding = 3.0f;
    s.PopupRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.TabRounding = 3.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.WindowPadding = {10, 10};
    s.FramePadding = {8, 4};
    s.ItemSpacing = {8, 6};
    s.ScrollbarSize = 12.0f;

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
