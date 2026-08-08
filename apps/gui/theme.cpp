// SPDX-License-Identifier: BSD-3-Clause
#include "theme.hpp"

namespace polymesh::gui {

Palette palette;
ThemeId active_theme = ThemeId::kStudio;

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

/// Studio: graphite chrome (#0E1116 / #161B22 / #1C2330), cyan accent #4CC2FF,
/// dark viewport gradient. Default theme and the palette every showcase render
/// is captured in — keep it in sync with docs/gui/theme-layout.md.
Palette make_studio_palette() {
    Palette p;
    p.window_bg = {0.055f, 0.067f, 0.086f, 1};   // #0E1116 chrome
    p.panel_bg = {0.086f, 0.106f, 0.133f, 1};    // #161B22
    p.header_bg = {0.110f, 0.137f, 0.188f, 1};   // #1C2330
    p.popup_bg = {0.102f, 0.125f, 0.165f, 1};    // #1A2029
    p.border = {0.165f, 0.196f, 0.251f, 1};      // #2A3240
    p.status_bg = {0.039f, 0.051f, 0.071f, 1};   // #0A0D12
    p.text = {0.902f, 0.918f, 0.941f, 1};        // #E6EAF0
    p.text_dim = {0.541f, 0.576f, 0.639f, 1};    // #8A93A3
    p.text_disabled = {0.353f, 0.384f, 0.447f, 1}; // #5A6272
    p.accent = {0.298f, 0.761f, 1.000f, 1};      // #4CC2FF
    p.accent_soft_top = {0.498f, 0.831f, 1.000f, 1}; // #7FD4FF (gradient top)
    p.accent_dim = {0.165f, 0.431f, 0.588f, 1};  // #2A6E96
    p.accent_soft = {0.298f, 0.761f, 1.000f, 0.20f};
    p.accent_mid = {0.298f, 0.761f, 1.000f, 0.38f};
    p.button = {0.102f, 0.129f, 0.169f, 1};
    p.button_hovered = {0.133f, 0.169f, 0.220f, 1};
    p.button_active = {0.169f, 0.212f, 0.275f, 1};
    p.frame_bg = {0.063f, 0.082f, 0.110f, 1}; // inputs sit below the panel plane
    p.frame_bg_hovered = {0.086f, 0.114f, 0.149f, 1};
    p.frame_bg_active = {0.114f, 0.145f, 0.192f, 1};
    p.viewport_top = {0.106f, 0.125f, 0.157f, 1};    // #1B2028
    p.viewport_mid = {0.078f, 0.098f, 0.133f, 1};    // #141922
    p.viewport_bottom = {0.059f, 0.075f, 0.102f, 1}; // #0F131A
    p.part_default = {0.545f, 0.584f, 0.647f, 1};    // #8B95A5
    p.sim_fixture = {0.180f, 0.800f, 0.443f, 0.65f}; // #2ECC71
    p.sim_load = {0.941f, 0.263f, 0.227f, 0.65f};    // #F0433A
    p.selection = {0.298f, 0.761f, 1.000f, 0.60f};
    p.hover = {0.298f, 0.761f, 1.000f, 0.30f};
    p.axis_x = {0.961f, 0.427f, 0.404f, 1};
    p.axis_y = {0.400f, 0.855f, 0.478f, 1};
    p.axis_z = {0.298f, 0.761f, 1.000f, 1};
    p.status_ok = {0.176f, 0.831f, 0.749f, 1};  // #2DD4BF
    p.status_warn = {0.961f, 0.773f, 0.259f, 1}; // #F5C542
    p.status_err = {0.961f, 0.529f, 0.416f, 1};  // #F5876C
    return p;
}

void apply_theme(ThemeId id) {
    active_theme = id;
    switch (id) {
    case ThemeId::kInterwebz:
        palette = make_interwebz_palette();
        break;
    case ThemeId::kSlate:
        palette = make_slate_palette();
        break;
    case ThemeId::kStudio:
    default: // out-of-range persisted value falls back to the default theme
        palette = make_studio_palette();
        break;
    }

    ImGuiStyle& s = ImGui::GetStyle();
    const Palette& p = palette;

    // Studio breathes a little more than the older two (softer rounding, taller
    // frames) — the rest of the metrics are shared.
    const bool studio = id == ThemeId::kStudio;
    const float rounding = studio ? 4.0f : 2.0f;
    s.WindowRounding = rounding;
    s.ChildRounding = rounding;
    s.FrameRounding = rounding;
    s.PopupRounding = rounding;
    s.GrabRounding = rounding;
    s.TabRounding = rounding;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = {12, 12};
    // Enough room for frame labels without clipping.
    s.FramePadding = studio ? ImVec2{9, 6} : ImVec2{8, 5};
    s.ItemSpacing = studio ? ImVec2{8, 7} : ImVec2{8, 8};
    s.ItemInnerSpacing = {6, 4};
    s.ScrollbarSize = studio ? 13.0f : 12.0f;
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
