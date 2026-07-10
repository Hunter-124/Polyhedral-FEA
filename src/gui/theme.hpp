// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// PolyMesh UI theme — token-for-token sibling of the AI-CAD dark palette
// (teal #2DD4BF accent on dark chrome, light SolidWorks-style studio
// viewport). Panels read ONLY these tokens; never hardcode color literals.

#include "imgui.h"

namespace polymesh::gui {

struct Palette {
    // chrome
    ImVec4 window_bg{0.082f, 0.090f, 0.110f, 1};
    ImVec4 panel_bg{0.094f, 0.106f, 0.129f, 1};
    ImVec4 header_bg{0.110f, 0.125f, 0.157f, 1};
    ImVec4 popup_bg{0.106f, 0.118f, 0.145f, 1};
    ImVec4 border{0.165f, 0.184f, 0.227f, 0.85f};
    ImVec4 status_bg{0.075f, 0.086f, 0.108f, 1};
    // text
    ImVec4 text{0.898f, 0.918f, 0.945f, 1};
    ImVec4 text_dim{0.620f, 0.660f, 0.720f, 1};
    ImVec4 text_disabled{0.471f, 0.502f, 0.549f, 1};
    // accent (#2DD4BF)
    ImVec4 accent{0.176f, 0.831f, 0.749f, 1};
    ImVec4 accent_dim{0.118f, 0.553f, 0.498f, 1};
    ImVec4 accent_soft{0.176f, 0.831f, 0.749f, 0.22f};
    ImVec4 accent_mid{0.176f, 0.831f, 0.749f, 0.40f};
    // interactive
    ImVec4 button{0.149f, 0.176f, 0.220f, 1};
    ImVec4 button_hovered{0.184f, 0.224f, 0.275f, 1};
    ImVec4 button_active{0.216f, 0.259f, 0.318f, 1};
    ImVec4 frame_bg{0.129f, 0.149f, 0.184f, 1};
    ImVec4 frame_bg_hovered{0.165f, 0.192f, 0.235f, 1};
    ImVec4 frame_bg_active{0.192f, 0.224f, 0.271f, 1};
    // viewport (light studio gradient, SolidWorks look)
    ImVec4 viewport_top{0.957f, 0.965f, 0.976f, 1};
    ImVec4 viewport_mid{0.851f, 0.867f, 0.894f, 1};
    ImVec4 viewport_bottom{0.933f, 0.941f, 0.953f, 1};
    ImVec4 part_default{0.776f, 0.792f, 0.847f, 1};
    // simulation overlays
    ImVec4 sim_fixture{0.18f, 0.80f, 0.36f, 0.65f};
    ImVec4 sim_load{0.95f, 0.27f, 0.20f, 0.65f};
    ImVec4 selection{0.176f, 0.831f, 0.749f, 0.55f};
    ImVec4 hover{0.176f, 0.831f, 0.749f, 0.30f};
    // orientation triad
    ImVec4 axis_x{0.910f, 0.380f, 0.380f, 1};
    ImVec4 axis_y{0.470f, 0.820f, 0.450f, 1};
    ImVec4 axis_z{0.420f, 0.620f, 0.960f, 1};
    // status
    ImVec4 status_ok{0.176f, 0.831f, 0.749f, 1};
    ImVec4 status_warn{0.953f, 0.761f, 0.420f, 1};
    ImVec4 status_err{0.961f, 0.549f, 0.420f, 1};
};

extern Palette palette;

/// Populates ImGuiStyle from the palette. Call once after ImGui context init.
void apply_theme();

} // namespace polymesh::gui
