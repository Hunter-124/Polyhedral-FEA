// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Chudware desktop controls, clean-room implemented on public Dear ImGui APIs
// (InvisibleButton + DrawList): workflow cards, accent-filled controls,
// bordered buttons, compact status pills and read-only statistics. Every color
// comes from the active palette (theme.hpp); the widgets carry no literals.

#include "imgui.h"

namespace polymesh::gui::iw {

/// Geometric glyph set drawn straight into an ImDrawList — no icon font, no
/// atlas, no external asset. Every glyph is 12-14 dp of lines/triangles in a
/// palette color, so the icons scale with `ui_scale` and re-tint with the
/// theme. They label controls; they never encode a measurement.
enum class Icon : int {
    kNone,
    // display fields
    kCad,
    kMesh,
    kStress,
    kDeflection,
    kError,
    // deformation scaling
    kAuto,
    kTrueScale,
    kCustom,
    // camera
    kIso,
    kFront,
    kRight,
    kTop,
    kFit,
    kCamera,
    // actions
    kSolve,
    kMeshOnly,
    kExport,
    kSave,
    // mesh fidelity
    kFast,
    kStandard,
    kFine,
    kManual,
    // result statistics
    kNodes,
    kElements,
    kDof,
    kSolver,
    // overlays
    kWire,
    kUndeformed,
};

/// Draw `icon` centered on `center`, fitted to a `size` x `size` box (pass an
/// already-scaled pixel size, e.g. `ui_px(12.0f)`). `Icon::kNone` draws
/// nothing. `color` must come from the palette.
void draw_icon(ImDrawList* dl, ImVec2 center, float size, Icon icon, ImU32 color);

/// Group box with a header strip and floating title. Height auto-fits content —
/// always pair with end_group_box().
void begin_group_box(const char* title);
/// Same chrome as begin_group_box, but content region has a fixed outer height
/// (fills remaining panel space). Use for tables/lists that should grow.
/// `outer_height` is the full box height including header + padding.
void begin_group_box_fill(const char* title, float outer_height);
void end_group_box();

/// Accent-gradient checkbox with label to the right. `icon`, when not kNone,
/// sits between the box and the label.
bool checkbox(const char* label, bool* value, Icon icon = Icon::kNone);

/// Fill-style slider: label above-left, value above-right, gradient fill.
bool slider_double(const char* label, double* value, double min, double max,
                   const char* format);

/// Flat bordered button; `primary` gets the accent fill. `help`, when set,
/// appears after the normal hover delay. `icon`, when not kNone, is drawn to
/// the left of the label and the icon+label pair is recentered as one unit.
/// size.x <= 0 (or -1) fills available width; size.y <= 0 uses text + padding.
bool button(const char* label, const ImVec2& size = ImVec2(0, 0), bool primary = false,
            const char* help = nullptr, Icon icon = Icon::kNone);

/// Dim field label followed by a recessed input box (stacked if the label is
/// too wide for a single row).
bool input_double(const char* label, double* value, const char* format);
bool input_float3(const char* label, float value[3]);
bool input_text(const char* label, char* buffer, size_t buffer_size, const char* hint);

/// Horizontal/wrapping selector row (radio replacement): returns true on change.
/// Options wrap to extra rows when they would overflow the available width.
/// `help`, when non-null, has one hover explanation per option. `icons`, when
/// non-null, is a parallel array of one glyph per option; the column fit
/// measurement reserves the glyph and its gap.
bool selector(const char* label, int* index, const char* const* options, int count,
              const char* const* help = nullptr, const Icon* icons = nullptr);

/// Numbered, collapsible workflow step. `index` is the 1-based step number
/// shown in the accent chip; `done` draws the chip filled and a check;
/// `open` is in/out. Returns true when the body should be drawn (always pair
/// with end_step() when it returns true).
bool begin_step(int index, const char* title, const char* subtitle, bool done, bool* open);
void end_step();

/// Fit `text` into `max_width` at `font`/`font_size`, appending "..." when
/// characters have to be dropped. Returns `text` itself when it already fits,
/// otherwise a pointer into `buffer`. The cut is measured by the font, so a
/// multi-byte glyph is never split in half — the failure that renders a clipped
/// "η" as a stray "n". `buffer` must hold at least 8 bytes.
const char* fit_text(char* buffer, size_t capacity, const char* text, float max_width,
                     ImFont* font, float font_size);

/// Right-aligned label/value row: dim label left, value right in `mono` when
/// non-null. For read-only numerics ("Peak von Mises", "202.49 MPa"). `icon`,
/// when not kNone, draws a 12 dp glyph left of the dim label.
void stat_row(const char* label, const char* value, ImFont* mono = nullptr,
              Icon icon = Icon::kNone);

/// Small pill. `tone`: 0 = neutral (text_dim on surface_hi), 1 = ok,
/// 2 = warn, 3 = err, 4 = accent.
void chip(const char* text, int tone = 0);

/// Section label inside a step body: dim, uppercase, letter-spaced. `icon`,
/// when not kNone, identifies the group the label opens.
void field_label(const char* text, Icon icon = Icon::kNone);

/// Single-line disclosure toggle ("Advanced" / "Fewer options"). Returns the
/// new state; `open` is in/out.
bool disclosure(const char* label, bool* open);

/// Accent-tracked progress bar with a caption above and a right-aligned
/// value. `fraction` < 0 draws an indeterminate sweep.
void progress(const char* caption, float fraction, const char* value);

/// Explains the immediately preceding item after ImGui's normal hover delay.
/// Text wraps to a readable technical width; use plain language first and the
/// precise term/units second.
void tooltip(const char* text);

} // namespace polymesh::gui::iw
