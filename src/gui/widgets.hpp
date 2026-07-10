// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Interwebz-v2-styled interactive controls, clean-room implemented on public
// Dear ImGui APIs (InvisibleButton + DrawList): plum group boxes with header
// strips and floating titles, rose gradient-filled checkboxes and sliders,
// bordered flat buttons, lavender labels. Visual reference: the Interwebz v2
// menu; no third-party code is copied (that source carries no license).

#include "imgui.h"

namespace polymesh::gui::iw {

/// Group box with a header strip and floating title. Pass the content height
/// (the box frames it plus header/padding). Always pair with end_group_box().
void begin_group_box(const char* title, float content_height);
void end_group_box();

/// Rose-gradient checkbox with label to the right.
bool checkbox(const char* label, bool* value);

/// Fill-style slider: label above-left, value above-right, gradient fill.
bool slider_double(const char* label, double* value, double min, double max,
                   const char* format);

/// Flat bordered button; `primary` gets the rose gradient fill.
bool button(const char* label, const ImVec2& size = ImVec2(0, 0), bool primary = false);

/// Lavender field label followed by a dark input box on the same row grid.
bool input_double(const char* label, double* value, const char* format);
bool input_float3(const char* label, float value[3]);
bool input_text(const char* label, char* buffer, size_t buffer_size, const char* hint);

/// Horizontal selector row (radio replacement): returns true on change.
bool selector(const char* label, int* index, const char* const* options, int count);

} // namespace polymesh::gui::iw
