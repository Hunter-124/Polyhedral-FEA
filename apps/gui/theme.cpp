// SPDX-License-Identifier: BSD-3-Clause
#include "theme.hpp"

namespace polymesh::gui {

Palette palette;
ThemeId active_theme = ThemeId::kInterwebz;

Palette make_interwebz_palette() {
    return Palette{}; // defaults in theme.hpp are Interwebz
}

Palette make_slate_palette() {
    Palette p;
    p.window_bg = {0.07f, 0.08f, 0.10f, 1};
    p.panel_bg = {0.12f, 0.13f, 0.16f, 1};
    p.header_bg = {0.16f, 0.18f, 0.22f, 1};
    p.popup_bg = {0.14f, 0.15f, 0.18f, 1};
    p.border = {0.28f, 0.30f, 0.36f, 1};
    p.status_bg = {0.05f, 0.06f, 0.08f, 1};
    p.text = {0.95f, 0.96f, 0.98f, 1};
    p.text_dim = {0.65f, 0.68f, 0.74f, 1};
    p.text_disabled = {0.45f, 0.48f, 0.52f, 1};
    p.accent = {0.30f, 0.62f, 0.90f, 1};
    p.accent_soft_top = {0.35f, 0.55f, 0.75f, 1};
    p.accent_dim = {0.20f, 0.42f, 0.65f, 1};
    p.accent_soft = {0.30f, 0.62f, 0.90f, 0.22f};
    p.accent_mid = {0.30f, 0.62f, 0.90f, 0.40f};
    p.button = {0.14f, 0.15f, 0.18f, 1};
    p.button_hovered = {0.18f, 0.20f, 0.24f, 1};
    p.button_active = {0.22f, 0.24f, 0.28f, 1};
    p.frame_bg = {0.14f, 0.15f, 0.18f, 1};
    p.frame_bg_hovered = {0.18f, 0.20f, 0.24f, 1};
    p.frame_bg_active = {0.22f, 0.24f, 0.28f, 1};
    p.sim_fixture = {0.20f, 0.75f, 0.45f, 0.65f};
    p.sim_load = {0.95f, 0.45f, 0.20f, 0.65f};
    p.selection = {0.30f, 0.62f, 0.90f, 0.55f};
    p.hover = {0.30f, 0.62f, 0.90f, 0.28f};
    p.status_ok = {0.30f, 0.85f, 0.55f, 1};
    p.status_warn = {0.95f, 0.75f, 0.30f, 1};
    p.status_err = {0.95f, 0.40f, 0.35f, 1};
    return p;
}

void apply_theme(ThemeId id) {
    active_theme = id;
    palette = (id == ThemeId::kSlate) ? make_slate_palette() : make_interwebz_palette();

    ImGuiStyle& s = ImGui::GetStyle();
    const Palette& p = palette;

    s.WindowRounding = 2.0f;
    s.ChildRounding = 2.0f;
    s.FrameRounding = 2.0f;
    s.PopupRounding = 2.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 2.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = {12, 12};
    s.FramePadding = {8, 5}; // enough room for frame labels without clipping
    s.ItemSpacing = {8, 8};
    s.ItemInnerSpacing = {6, 4};
    s.ScrollbarSize = 12.0f;
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
