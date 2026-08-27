// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// PolyMesh UI theme tokens — the Chudware desktop palette.
//
// PolyMesh ships as a Chudware product, so the chrome roles here are the same
// values the Chudware CAD desktop app installs (its `ui::Palette` dark theme):
// #0B0E11-family graphite panels, the #F28216 primary accent for what an
// operation PRODUCED, and the #0078D7 secondary accent for direction and
// annotation cues. Panels read ONLY these tokens; never hardcode color
// literals.
//
// One deliberate divergence from the CAD app: the 3D viewport gradient stays
// DARK here. Chudware CAD renders a light SolidWorks-style studio canvas
// because it draws solid bodies; PolyMesh's viewport is a field-visualisation
// surface — the FEA colormap, the wireframe density cue and the mesh-arrival
// glow all read against a dark canvas and wash out on a light one.

#include "imgui.h"

namespace polymesh::gui {

struct Palette {
    // ---- chrome ----
    ImVec4 window_bg{0.082f, 0.090f, 0.110f, 1};
    ImVec4 panel_bg{0.094f, 0.106f, 0.129f, 1};
    ImVec4 surface_hi{0.118f, 0.133f, 0.165f, 1}; // raised card inside a panel
    ImVec4 header_bg{0.110f, 0.125f, 0.157f, 1};
    ImVec4 popup_bg{0.106f, 0.118f, 0.145f, 1};
    ImVec4 border{0.165f, 0.184f, 0.227f, 0.85f};
    ImVec4 status_bg{0.075f, 0.086f, 0.108f, 1};
    // ---- text ----
    ImVec4 text{0.898f, 0.918f, 0.945f, 1};
    ImVec4 text_dim{0.620f, 0.660f, 0.720f, 1};
    ImVec4 text_disabled{0.471f, 0.502f, 0.549f, 1};
    // ---- accent: what an operation PRODUCED (#F28216) ----
    ImVec4 accent{0.949f, 0.510f, 0.086f, 1};
    ImVec4 accent_soft_top{0.976f, 0.639f, 0.286f, 1}; // gradient top for fills
    ImVec4 accent_dim{0.664f, 0.357f, 0.060f, 1};
    ImVec4 accent_soft{0.949f, 0.510f, 0.086f, 0.22f};
    ImVec4 accent_mid{0.949f, 0.510f, 0.086f, 0.40f};
    // ---- accent2: direction / annotation cue (#0078D7) ----
    ImVec4 accent2{0.000f, 0.471f, 0.843f, 1};
    ImVec4 accent2_dim{0.000f, 0.330f, 0.590f, 1};
    ImVec4 accent2_soft{0.000f, 0.471f, 0.843f, 0.22f};
    ImVec4 accent2_mid{0.000f, 0.471f, 0.843f, 0.40f};
    // ---- interactive ----
    ImVec4 button{0.149f, 0.176f, 0.220f, 1};
    ImVec4 button_hovered{0.184f, 0.224f, 0.275f, 1};
    ImVec4 button_active{0.216f, 0.259f, 0.318f, 1};
    ImVec4 frame_bg{0.129f, 0.149f, 0.184f, 1};
    ImVec4 frame_bg_hovered{0.165f, 0.192f, 0.235f, 1};
    ImVec4 frame_bg_active{0.192f, 0.224f, 0.271f, 1};
    // ---- viewport (dark field canvas — see the header note) ----
    ImVec4 viewport_top{0.106f, 0.118f, 0.145f, 1};
    ImVec4 viewport_mid{0.075f, 0.086f, 0.108f, 1};
    ImVec4 viewport_bottom{0.055f, 0.063f, 0.082f, 1};
    ImVec4 part_default{0.776f, 0.792f, 0.847f, 1};
    // ---- glass: floating overlays drawn over the viewport ----
    ImVec4 glass_bg{0.118f, 0.133f, 0.167f, 0.87f};
    ImVec4 glass_border{0.230f, 0.275f, 0.365f, 0.80f};
    ImVec4 glass_highlight{1.000f, 1.000f, 1.000f, 0.09f};
    ImVec4 glass_shadow{0.000f, 0.000f, 0.000f, 0.40f};
    // ---- simulation overlays ----
    ImVec4 sim_fixture{0.18f, 0.80f, 0.36f, 0.65f};
    ImVec4 sim_load{0.95f, 0.27f, 0.20f, 0.65f};
    ImVec4 sim_mesh{0.55f, 0.74f, 0.95f, 0.55f};
    ImVec4 selection{0.949f, 0.510f, 0.086f, 0.50f};
    ImVec4 hover{0.949f, 0.510f, 0.086f, 0.30f};
    // ---- orientation triad ----
    ImVec4 axis_x{0.910f, 0.380f, 0.380f, 1};
    ImVec4 axis_y{0.470f, 0.820f, 0.450f, 1};
    ImVec4 axis_z{0.420f, 0.620f, 0.960f, 1};
    // ---- status ----
    ImVec4 status_ok{0.459f, 0.859f, 0.549f, 1};
    ImVec4 status_warn{0.953f, 0.761f, 0.420f, 1};
    ImVec4 status_err{0.961f, 0.549f, 0.420f, 1};
};

/// Active chrome palette (read by widgets; never hardcode colors).
extern Palette palette;
/// UI density multiplier from GLFW content scale. All custom geometry uses
/// `ui_px()`; fonts are rebuilt at the same density.
extern float ui_scale;
void set_ui_scale(float scale);
inline float ui_px(float value) { return value * ui_scale; }

/// Load the palette into `palette` and push it into ImGuiStyle. There is one
/// theme — PolyMesh is a Chudware surface and looks like one.
void apply_theme();

/// Glassmorphism panel background for floating overlays drawn over the
/// viewport (live mesh HUD, advisor activation card, result chips). Layers back
/// to front: drop shadow, translucent base, inner vertical shine, border, top
/// specular line. `rounding` is a logical dp value scaled by `ui_scale`.
void glass_background(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float rounding = 8.0f,
                      float alpha = 1.0f);

} // namespace polymesh::gui
