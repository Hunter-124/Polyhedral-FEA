// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// PolyMesh UI theme — Interwebz-v2-inspired plum/rose chrome (owner pivot,
// 2026-07-10): deep purple panels, header-strip group boxes, rose gradient
// accents, lavender labels. The 3D viewport keeps the light studio gradient
// (SolidWorks-style canvas). Panels read ONLY these tokens; never hardcode
// color literals.

#include "imgui.h"

namespace polymesh::gui {

struct Palette {
    // chrome (plum)
    ImVec4 window_bg{0.051f, 0.031f, 0.078f, 1}; // rgb(13, 8, 20)
    ImVec4 panel_bg{0.102f, 0.067f, 0.149f, 1};  // rgb(26, 17, 38)
    ImVec4 header_bg{0.161f, 0.110f, 0.212f, 1}; // rgb(41, 28, 54)
    ImVec4 popup_bg{0.118f, 0.075f, 0.173f, 1};  // rgb(30, 19, 44)
    ImVec4 border{0.259f, 0.216f, 0.322f, 1};    // rgb(66, 55, 82)
    ImVec4 status_bg{0.043f, 0.027f, 0.067f, 1};
    // text
    ImVec4 text{1.0f, 1.0f, 1.0f, 1};
    ImVec4 text_dim{0.612f, 0.569f, 0.667f, 1}; // lavender rgb(156, 145, 170)
    ImVec4 text_disabled{0.431f, 0.392f, 0.490f, 1};
    // accent (rose gradient family)
    ImVec4 accent{0.757f, 0.259f, 0.392f, 1};          // rgb(193, 66, 100)
    ImVec4 accent_soft_top{0.675f, 0.400f, 0.416f, 1}; // rgb(172, 102, 106)
    ImVec4 accent_dim{0.549f, 0.188f, 0.290f, 1};
    ImVec4 accent_soft{0.757f, 0.259f, 0.392f, 0.22f};
    ImVec4 accent_mid{0.757f, 0.259f, 0.392f, 0.40f};
    // interactive
    ImVec4 button{0.118f, 0.075f, 0.173f, 1};         // rgb(30, 19, 44)
    ImVec4 button_hovered{0.157f, 0.110f, 0.212f, 1}; // rgb(40, 28, 54)
    ImVec4 button_active{0.188f, 0.133f, 0.251f, 1};
    ImVec4 frame_bg{0.118f, 0.075f, 0.173f, 1};
    ImVec4 frame_bg_hovered{0.157f, 0.110f, 0.212f, 1};
    ImVec4 frame_bg_active{0.188f, 0.133f, 0.251f, 1};
    // viewport (light studio gradient, SolidWorks look)
    ImVec4 viewport_top{0.957f, 0.965f, 0.976f, 1};
    ImVec4 viewport_mid{0.851f, 0.867f, 0.894f, 1};
    ImVec4 viewport_bottom{0.933f, 0.941f, 0.953f, 1};
    ImVec4 part_default{0.776f, 0.792f, 0.847f, 1};
    // simulation overlays
    ImVec4 sim_fixture{0.18f, 0.80f, 0.36f, 0.65f};
    ImVec4 sim_load{0.95f, 0.27f, 0.20f, 0.65f};
    ImVec4 selection{0.757f, 0.259f, 0.392f, 0.60f};
    ImVec4 hover{0.757f, 0.259f, 0.392f, 0.30f};
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
