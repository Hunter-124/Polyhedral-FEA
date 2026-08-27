// SPDX-License-Identifier: BSD-3-Clause
#include "widgets.hpp"

#include "theme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace polymesh::gui::iw {
namespace {

ImU32 u32(const ImVec4& c) { return ImGui::GetColorU32(c); }

// Shared building blocks, all read from the palette (theme.hpp).
void draw_box(ImDrawList* dl, const ImVec2& min, const ImVec2& max, bool hovered) {
    const float rounding = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(min, max, u32(hovered ? palette.frame_bg_hovered : palette.frame_bg),
                      rounding);
    dl->AddRect(min, max, u32(palette.border), rounding, ImDrawFlags_RoundCornersAll,
                ui_px(1.0f));
}

void draw_accent_fill(ImDrawList* dl, const ImVec2& min, const ImVec2& max) {
    const float rounding = ImGui::GetStyle().FrameRounding;
    // One rounded fill, then a top specular. The old inset multi-colour quad
    // could not inherit the outer rect's rounded clip, so it presented as a
    // bright square stripe down the left edge of every selected button.
    dl->AddRectFilled(min, max, u32(palette.accent), rounding);
    const float inset = std::max(rounding, ui_px(2.0f));
    if (max.x - min.x > 2.0f * inset) {
        const ImVec4 shine(palette.accent_soft_top.x, palette.accent_soft_top.y,
                           palette.accent_soft_top.z, 0.58f);
        dl->AddLine(ImVec2(min.x + inset, min.y + ui_px(1.0f)),
                    ImVec2(max.x - inset, min.y + ui_px(1.0f)), u32(shine), ui_px(1.0f));
    }
}

// ---- auto-sized group box ----
// The parent window already supplies the panel plane, so group boxes paint
// only their header strip and border after measuring content.

float group_header() { return ui_px(22.0f); }
float group_pad() { return ui_px(10.0f); }
float group_gap() { return ui_px(10.0f); }

struct GroupBoxFrame {
    ImVec2 start{};
    float width = 0.0f;
    const char* title = nullptr;
    /// When > 0, content child uses this fixed height instead of AutoResizeY.
    float fixed_content_h = 0.0f;
};

std::vector<GroupBoxFrame> g_group_stack;

// Horizontal padding inside custom buttons so labels clear the border.
float button_pad_x() { return ui_px(12.0f); }
float button_pad_y() { return ui_px(6.0f); }

/// Glyph box and glyph-to-text gap shared by every icon-bearing control, so a
/// selector cell, a button and a stat row all reserve the same 18 dp.
float icon_box() { return ui_px(12.0f); }
float icon_gap() { return ui_px(6.0f); }

/// Draw an optional glyph plus label as ONE centered unit, clipped so neither
/// spills the box. Centering the pair (not the text alone) is what keeps an
/// icon button from looking hung off its left edge.
void draw_centered_label(ImDrawList* dl, const ImVec2& min, const ImVec2& max,
                         const char* text, ImU32 col, Icon icon = Icon::kNone) {
    const ImVec2 tsize = ImGui::CalcTextSize(text);
    const float glyph = icon == Icon::kNone ? 0.0f : icon_box();
    const float gap = icon == Icon::kNone ? 0.0f : icon_gap();
    const float box_w = max.x - min.x;
    const float box_h = max.y - min.y;
    float x = min.x + 0.5f * (box_w - (glyph + gap + tsize.x));
    dl->PushClipRect(min, max, true);
    if (icon != Icon::kNone) {
        draw_icon(dl, ImVec2(x + 0.5f * glyph, min.y + 0.5f * box_h), glyph, icon, col);
        x += glyph + gap;
    }
    dl->AddText(ImVec2(x, min.y + 0.5f * (box_h - tsize.y)), col, text);
    dl->PopClipRect();
}

/// True when every option's text + glyph + padding fits a `cols`-wide row.
bool selector_columns_fit(const char* const* options, int count, float avail, float gap,
                          float pad_x, float icon_extra, int cols) {
    const float cell = (avail - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
    for (int i = 0; i < count; ++i) {
        if (ImGui::CalcTextSize(options[i]).x + icon_extra + 2.0f * pad_x > cell + 0.5f) {
            return false;
        }
    }
    return true;
}

/// Choose how many columns fit so every option's text + padding is readable.
/// `icon_extra` is the per-option glyph reservation (0 when the selector has
/// no icons) — without it an iconised selector picks a column count that then
/// clips every label it measured as fitting.
///
/// Among the counts that fit, prefer one whose final row holds at least two
/// options. The widest fit is not the best fit: four options in three columns
/// strands the fourth alone on a full-width row (the "Error η" / "Manual"
/// defect), while 2x2 reads as a block. The plain widest fit stays the
/// fallback so a genuinely narrow rail never collapses to one column just to
/// satisfy the balance rule.
int fit_selector_columns(const char* const* options, int count, float avail, float gap,
                         float pad_x, float icon_extra) {
    int widest = 1;
    for (int cols = count; cols >= 2; --cols) {
        if (selector_columns_fit(options, count, avail, gap, pad_x, icon_extra, cols)) {
            widest = cols;
            break;
        }
    }
    for (int cols = widest; cols >= 2; --cols) {
        const int last_row = count % cols;
        if ((last_row == 0 || last_row >= 2) &&
            selector_columns_fit(options, count, avail, gap, pad_x, icon_extra, cols)) {
            return cols;
        }
    }
    return widest;
}

} // namespace

const char* fit_text(char* buffer, size_t capacity, const char* text, float max_width,
                     ImFont* font, float font_size) {
    static constexpr char kEllipsis[] = "...";
    static constexpr size_t kEllipsisBytes = sizeof(kEllipsis) - 1;
    if (text == nullptr || font == nullptr || capacity <= kEllipsisBytes) {
        return "";
    }
    constexpr float kUnbounded = 1.0e9f;
    if (text[0] == '\0' ||
        font->CalcTextSizeA(font_size, kUnbounded, 0.0f, text).x <= max_width) {
        return text;
    }
    const float ellipsis_width = font->CalcTextSizeA(font_size, kUnbounded, 0.0f, kEllipsis).x;
    const char* cut = text;
    if (max_width > ellipsis_width) {
        // The font decides where the budget runs out, so the cut always lands on
        // a glyph boundary instead of halfway through a UTF-8 sequence.
        font->CalcTextSizeA(font_size, max_width - ellipsis_width, 0.0f, text, nullptr, &cut);
        if (cut == nullptr) {
            cut = text;
        }
    }
    size_t kept = static_cast<size_t>(cut - text);
    if (kept > capacity - kEllipsisBytes - 1) {
        kept = capacity - kEllipsisBytes - 1;
        while (kept > 0 && (static_cast<unsigned char>(text[kept]) & 0xC0U) == 0x80U) {
            --kept;
        }
    }
    std::memcpy(buffer, text, kept);
    std::memcpy(buffer + kept, kEllipsis, kEllipsisBytes + 1);
    return buffer;
}

namespace {

constexpr float kIconTau = 6.28318530718f;

/// Icon stroke weight: heavy enough to read at 12 dp, light enough that a
/// glyph never out-weighs the label beside it.
float icon_stroke() { return ui_px(1.6f); }

/// Square outline subdivided `divisions` times each way — the mesh/wireframe
/// family.
void icon_grid(ImDrawList* dl, ImVec2 c, float h, ImU32 col, int divisions) {
    const float stroke = icon_stroke();
    dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 0.0f,
                ImDrawFlags_None, stroke);
    for (int i = 1; i < divisions; ++i) {
        const float offset =
            -h + 2.0f * h * static_cast<float>(i) / static_cast<float>(divisions);
        dl->AddLine(ImVec2(c.x + offset, c.y - h), ImVec2(c.x + offset, c.y + h), col, stroke);
        dl->AddLine(ImVec2(c.x - h, c.y + offset), ImVec2(c.x + h, c.y + offset), col, stroke);
    }
}

/// Isometric cube: hexagon silhouette plus the three interior edges meeting at
/// the near corner, which is what reads as "three visible faces".
void icon_cube(ImDrawList* dl, ImVec2 c, float h, ImU32 col) {
    const float stroke = icon_stroke();
    const float w = h * 0.88f;
    const ImVec2 hexagon[6] = {
        ImVec2(c.x, c.y - h), ImVec2(c.x + w, c.y - h * 0.5f), ImVec2(c.x + w, c.y + h * 0.5f),
        ImVec2(c.x, c.y + h), ImVec2(c.x - w, c.y + h * 0.5f), ImVec2(c.x - w, c.y - h * 0.5f),
    };
    dl->AddPolyline(hexagon, 6, col, ImDrawFlags_Closed, stroke);
    dl->AddLine(c, hexagon[0], col, stroke);
    dl->AddLine(c, hexagon[2], col, stroke);
    dl->AddLine(c, hexagon[4], col, stroke);
}

/// Solid arrowhead at `tip` pointing along `dir` (need not be normalised).
void icon_arrow_head(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float length, ImU32 col) {
    const float norm = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (norm <= 0.0f) {
        return;
    }
    const ImVec2 unit(dir.x / norm, dir.y / norm);
    const ImVec2 base(tip.x - unit.x * length, tip.y - unit.y * length);
    const float wing = length * 0.58f;
    dl->AddTriangleFilled(tip, ImVec2(base.x - unit.y * wing, base.y + unit.x * wing),
                          ImVec2(base.x + unit.y * wing, base.y - unit.x * wing), col);
}

/// `count` right-pointing chevrons — the mesh-fidelity speed family.
void icon_chevrons(ImDrawList* dl, ImVec2 c, float h, ImU32 col, int count) {
    const float stroke = icon_stroke();
    const float pitch = h * 0.62f;
    const float x0 = c.x - pitch * (static_cast<float>(count) - 1.0f) * 0.5f - h * 0.28f;
    for (int i = 0; i < count; ++i) {
        const float x = x0 + pitch * static_cast<float>(i);
        dl->AddLine(ImVec2(x, c.y - h * 0.72f), ImVec2(x + h * 0.52f, c.y), col, stroke);
        dl->AddLine(ImVec2(x + h * 0.52f, c.y), ImVec2(x, c.y + h * 0.72f), col, stroke);
    }
}

/// Rectangle with ONE edge drawn heavy: the face a standard view looks at.
/// `edge` 0 = bottom (front), 1 = right, 2 = top.
void icon_face_rect(ImDrawList* dl, ImVec2 c, float h, ImU32 col, int edge) {
    const float stroke = icon_stroke();
    const ImVec2 mn(c.x - h, c.y - h);
    const ImVec2 mx(c.x + h, c.y + h);
    dl->AddRect(mn, mx, col, 0.0f, ImDrawFlags_None, stroke);
    const float heavy = stroke * 2.3f;
    if (edge == 0) {
        dl->AddLine(ImVec2(mn.x, mx.y), mx, col, heavy);
    } else if (edge == 1) {
        dl->AddLine(ImVec2(mx.x, mn.y), mx, col, heavy);
    } else {
        dl->AddLine(mn, ImVec2(mx.x, mn.y), col, heavy);
    }
}

} // namespace

void draw_icon(ImDrawList* dl, ImVec2 center, float size, Icon icon, ImU32 color) {
    if (dl == nullptr || icon == Icon::kNone || size <= 0.0f) {
        return;
    }
    const float h = size * 0.5f;
    const float stroke = icon_stroke();
    switch (icon) {
    case Icon::kCad:
        icon_cube(dl, center, h, color);
        break;
    case Icon::kIso:
        icon_cube(dl, center, h * 0.92f, color);
        break;
    case Icon::kMesh:
        icon_grid(dl, center, h * 0.92f, color, 2);
        break;
    case Icon::kWire:
        icon_grid(dl, center, h * 0.92f, color, 3);
        break;
    case Icon::kStress: {
        // Three stacked bars: the colormap read as a legend strip. It is chrome,
        // not a plotted value — no bar length encodes a number.
        const float bar_h = size * 0.18f;
        const float pitch = size * 0.29f;
        float y = center.y - pitch - bar_h * 0.5f;
        for (int i = 0; i < 3; ++i) {
            dl->AddRectFilled(ImVec2(center.x - h * 0.92f, y),
                              ImVec2(center.x + h * 0.92f, y + bar_h), color);
            y += pitch;
        }
        break;
    }
    case Icon::kDeflection: {
        const ImVec2 heel(center.x - h, center.y + h * 0.78f);
        const ImVec2 knee(center.x - h * 0.15f, center.y + h * 0.78f);
        const ImVec2 tip(center.x + h * 0.72f, center.y - h * 0.86f);
        dl->AddLine(heel, knee, color, stroke);
        dl->AddLine(knee, tip, color, stroke);
        icon_arrow_head(dl, tip, ImVec2(tip.x - knee.x, tip.y - knee.y), h * 0.62f, color);
        break;
    }
    case Icon::kError:
        dl->AddTriangle(ImVec2(center.x, center.y - h * 0.94f),
                        ImVec2(center.x + h * 0.96f, center.y + h * 0.74f),
                        ImVec2(center.x - h * 0.96f, center.y + h * 0.74f), color, stroke);
        dl->AddLine(ImVec2(center.x, center.y - h * 0.20f),
                    ImVec2(center.x, center.y + h * 0.36f), color, stroke);
        break;
    case Icon::kAuto:
        dl->AddLine(ImVec2(center.x - h * 0.85f, center.y),
                    ImVec2(center.x + h * 0.85f, center.y), color, stroke);
        icon_arrow_head(dl, ImVec2(center.x + h, center.y), ImVec2(1.0f, 0.0f), h * 0.60f,
                        color);
        icon_arrow_head(dl, ImVec2(center.x - h, center.y), ImVec2(-1.0f, 0.0f), h * 0.60f,
                        color);
        break;
    case Icon::kTrueScale: {
        // Ruler: the scale is exactly 1, so the glyph is a measure, not an arrow.
        const float baseline = center.y + h * 0.58f;
        dl->AddLine(ImVec2(center.x - h, baseline), ImVec2(center.x + h, baseline), color,
                    stroke);
        for (int i = 0; i < 3; ++i) {
            const float x = center.x - h + h * static_cast<float>(i);
            const float length = i == 1 ? h * 0.98f : h * 0.62f;
            dl->AddLine(ImVec2(x, baseline), ImVec2(x, baseline - length), color, stroke);
        }
        break;
    }
    case Icon::kCustom:
    case Icon::kManual:
        dl->AddLine(ImVec2(center.x - h, center.y), ImVec2(center.x + h, center.y), color,
                    stroke);
        dl->AddLine(ImVec2(center.x + h * 0.34f, center.y - h * 0.66f),
                    ImVec2(center.x + h * 0.34f, center.y + h * 0.66f), color, stroke * 1.4f);
        break;
    case Icon::kFront:
        icon_face_rect(dl, center, h * 0.86f, color, 0);
        break;
    case Icon::kRight:
        icon_face_rect(dl, center, h * 0.86f, color, 1);
        break;
    case Icon::kTop:
        icon_face_rect(dl, center, h * 0.86f, color, 2);
        break;
    case Icon::kFit: {
        const float arm = h * 0.56f;
        const ImVec2 corner[4] = {
            ImVec2(center.x - h, center.y - h),
            ImVec2(center.x + h, center.y - h),
            ImVec2(center.x + h, center.y + h),
            ImVec2(center.x - h, center.y + h),
        };
        const float step_x[4] = {1.0f, -1.0f, -1.0f, 1.0f};
        const float step_y[4] = {1.0f, 1.0f, -1.0f, -1.0f};
        for (int i = 0; i < 4; ++i) {
            dl->AddLine(corner[i], ImVec2(corner[i].x + step_x[i] * arm, corner[i].y), color,
                        stroke);
            dl->AddLine(corner[i], ImVec2(corner[i].x, corner[i].y + step_y[i] * arm), color,
                        stroke);
        }
        break;
    }
    case Icon::kCamera: {
        const ImVec2 mn(center.x - h, center.y - h * 0.52f);
        const ImVec2 mx(center.x + h, center.y + h * 0.78f);
        dl->AddRect(mn, mx, color, ui_px(2.0f), ImDrawFlags_RoundCornersAll, stroke);
        dl->AddLine(ImVec2(center.x - h * 0.42f, mn.y - stroke),
                    ImVec2(center.x + h * 0.02f, mn.y - stroke), color, stroke * 1.6f);
        dl->AddCircle(ImVec2(center.x, center.y + h * 0.14f), h * 0.34f, color, 0, stroke);
        break;
    }
    case Icon::kSolve:
        dl->AddTriangleFilled(ImVec2(center.x - h * 0.62f, center.y - h * 0.94f),
                              ImVec2(center.x + h * 0.94f, center.y),
                              ImVec2(center.x - h * 0.62f, center.y + h * 0.94f), color);
        break;
    case Icon::kMeshOnly: {
        ImVec2 hexagon[6];
        for (int i = 0; i < 6; ++i) {
            const float angle = kIconTau * (static_cast<float>(i) / 6.0f - 0.25f);
            hexagon[i] =
                ImVec2(center.x + h * std::cos(angle), center.y + h * std::sin(angle));
        }
        dl->AddPolyline(hexagon, 6, color, ImDrawFlags_Closed, stroke);
        break;
    }
    case Icon::kExport: {
        const float lip = center.y + h * 0.08f;
        dl->AddLine(ImVec2(center.x - h, lip), ImVec2(center.x - h, center.y + h), color,
                    stroke);
        dl->AddLine(ImVec2(center.x - h, center.y + h), ImVec2(center.x + h, center.y + h),
                    color, stroke);
        dl->AddLine(ImVec2(center.x + h, lip), ImVec2(center.x + h, center.y + h), color,
                    stroke);
        dl->AddLine(ImVec2(center.x, center.y + h * 0.46f),
                    ImVec2(center.x, center.y - h * 0.46f), color, stroke);
        icon_arrow_head(dl, ImVec2(center.x, center.y - h), ImVec2(0.0f, -1.0f), h * 0.58f,
                        color);
        break;
    }
    case Icon::kSave: {
        const ImVec2 mn(center.x - h, center.y - h);
        const ImVec2 mx(center.x + h, center.y + h);
        dl->AddRect(mn, mx, color, ui_px(2.0f), ImDrawFlags_RoundCornersAll, stroke);
        dl->AddRectFilled(ImVec2(center.x - h * 0.40f, mn.y + stroke),
                          ImVec2(center.x + h * 0.40f, center.y - h * 0.26f), color);
        dl->AddRect(ImVec2(center.x - h * 0.56f, center.y + h * 0.18f),
                    ImVec2(center.x + h * 0.56f, mx.y - stroke), color, 0.0f, ImDrawFlags_None,
                    stroke);
        break;
    }
    case Icon::kFast:
        icon_chevrons(dl, center, h, color, 1);
        break;
    case Icon::kStandard:
        icon_chevrons(dl, center, h, color, 2);
        break;
    case Icon::kFine:
        icon_chevrons(dl, center, h, color, 3);
        break;
    case Icon::kNodes:
        dl->AddCircleFilled(center, h * 0.52f, color);
        break;
    case Icon::kElements:
        dl->AddTriangle(ImVec2(center.x, center.y - h * 0.94f),
                        ImVec2(center.x + h * 0.92f, center.y + h * 0.72f),
                        ImVec2(center.x - h * 0.92f, center.y + h * 0.72f), color, stroke);
        break;
    case Icon::kDof:
        for (int i = -1; i <= 1; ++i) {
            dl->AddCircleFilled(ImVec2(center.x + static_cast<float>(i) * h * 0.68f, center.y),
                                h * 0.26f, color);
        }
        break;
    case Icon::kSolver:
        dl->AddLine(ImVec2(center.x - h * 0.88f, center.y - h * 0.34f),
                    ImVec2(center.x + h * 0.88f, center.y - h * 0.34f), color, stroke);
        dl->AddLine(ImVec2(center.x - h * 0.88f, center.y + h * 0.34f),
                    ImVec2(center.x + h * 0.88f, center.y + h * 0.34f), color, stroke);
        break;
    case Icon::kUndeformed:
        dl->AddRect(ImVec2(center.x - h, center.y - h),
                    ImVec2(center.x + h * 0.28f, center.y + h * 0.28f), color, 0.0f,
                    ImDrawFlags_None, stroke);
        dl->AddRectFilled(ImVec2(center.x - h * 0.28f, center.y - h * 0.28f),
                          ImVec2(center.x + h, center.y + h), color);
        break;
    case Icon::kNone:
        break;
    }
}

void begin_group_box(const char* title) {
    GroupBoxFrame frame;
    frame.start = ImGui::GetCursorScreenPos();
    frame.width = ImGui::GetContentRegionAvail().x;
    frame.title = title;
    frame.fixed_content_h = 0.0f;

    // Outer group claims the full box width; header is reserved with a Dummy so
    // layout height is correct before content is measured.
    ImGui::BeginGroup();
    const float header = group_header();
    const float pad = group_pad();
    ImGui::Dummy(ImVec2(frame.width, header));

    // Content child with BOTH left and right padding so GetContentRegionAvail()
    // already reserves padding on the right.
    const float content_w = std::max(1.0f, frame.width - 2.0f * pad);
    ImGui::SetCursorScreenPos(ImVec2(frame.start.x + pad, frame.start.y + header + pad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(title, ImVec2(content_w, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PushItemWidth(content_w);

    g_group_stack.push_back(frame);
}

void begin_group_box_fill(const char* title, float outer_height) {
    GroupBoxFrame frame;
    frame.start = ImGui::GetCursorScreenPos();
    frame.width = ImGui::GetContentRegionAvail().x;
    frame.title = title;
    // Header + top/bottom pad; content fills the rest.
    const float header = group_header();
    const float pad = group_pad();
    const float chrome = header + 2.0f * pad;
    frame.fixed_content_h = std::max(1.0f, outer_height - chrome);

    ImGui::BeginGroup();
    // Fixed outer height so chrome bounds match the fill region.
    ImGui::Dummy(ImVec2(frame.width, std::max(outer_height, chrome)));
    // Rewind to place content under the header.
    ImGui::SetCursorScreenPos(ImVec2(frame.start.x + pad, frame.start.y + header + pad));

    const float content_w = std::max(1.0f, frame.width - 2.0f * pad);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(title, ImVec2(content_w, frame.fixed_content_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    ImGui::PushItemWidth(content_w);

    g_group_stack.push_back(frame);
}

void end_group_box() {
    IM_ASSERT(!g_group_stack.empty());
    const GroupBoxFrame frame = g_group_stack.back();
    g_group_stack.pop_back();

    ImGui::PopItemWidth();
    ImGui::EndChild(); // content

    const float header = group_header();
    const float pad = group_pad();
    if (frame.fixed_content_h > 0.0f) {
        // Fixed-fill: outer Dummy already reserved full height; place cursor
        // after the box for the trailing gap.
        ImGui::SetCursorScreenPos(ImVec2(frame.start.x, frame.start.y + header + 2.0f * pad +
                                                            frame.fixed_content_h));
        ImGui::Dummy(ImVec2(frame.width, 0.0f));
    } else {
        // Auto-height: bottom padding inside the border.
        ImGui::SetCursorScreenPos(ImVec2(
            frame.start.x, std::max(ImGui::GetItemRectMax().y, frame.start.y + header) + pad));
        ImGui::Dummy(ImVec2(frame.width, 0.0f));
    }
    ImGui::EndGroup(); // outer chrome bounds

    const ImVec2 box_min = ImGui::GetItemRectMin();
    const ImVec2 box_max = ImGui::GetItemRectMax();

    // Draw chrome on the parent draw list (child is nested; outer group is parent).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Header strip (covers the reserved Dummy only — content sits below it).
    const float rounding = ImGui::GetStyle().ChildRounding;
    dl->AddRectFilled(box_min, ImVec2(box_max.x, box_min.y + header), u32(palette.header_bg),
                      rounding, ImDrawFlags_RoundCornersTop);
    dl->AddRect(box_min, box_max, u32(palette.border), rounding, ImDrawFlags_RoundCornersAll,
                ui_px(1.0f));
    const float inset = ui_px(1.0f);
    dl->AddRectFilled(ImVec2(box_min.x + inset, box_min.y + inset),
                      ImVec2(box_min.x + ui_px(3.0f), box_min.y + header),
                      u32(palette.accent));
    dl->PushClipRect(box_min, ImVec2(box_max.x - ui_px(4.0f), box_min.y + header), true);
    dl->AddText(ImVec2(box_min.x + ui_px(12.0f), box_min.y + ui_px(3.0f)), u32(palette.text),
                frame.title);
    dl->PopClipRect();

    ImGui::Dummy(ImVec2(0.0f, group_gap()));
}

namespace {

/// Preferred fill width for custom controls: respects PushItemWidth / group box
/// content width. Falls back to remaining content region if no item width set.
float fill_width() {
    const float item = ImGui::CalcItemWidth();
    const float avail = ImGui::GetContentRegionAvail().x;
    // CalcItemWidth can exceed avail when cursor is mid-row; never overflow.
    return std::max(1.0f, std::min(item, avail));
}

} // namespace

bool checkbox(const char* label, bool* value, Icon icon) {
    const float box = ui_px(15.0f);
    ImGui::PushID(label);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    const float glyph = icon == Icon::kNone ? 0.0f : icon_box() + icon_gap();
    const float hit_w = std::min(fill_width(), box + ui_px(8.0f) + glyph + label_size.x);
    const bool pressed = ImGui::InvisibleButton("##cb", ImVec2(hit_w, box + ui_px(2.0f)));
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 box_min(pos.x, pos.y + ui_px(1.0f));
    const ImVec2 box_max(pos.x + box, pos.y + ui_px(1.0f) + box);
    if (*value) {
        draw_accent_fill(dl, box_min, box_max);
        dl->AddLine(ImVec2(box_min.x + 0.25f * box, box_min.y + 0.55f * box),
                    ImVec2(box_min.x + 0.42f * box, box_min.y + 0.72f * box),
                    u32(palette.text), ui_px(1.6f));
        dl->AddLine(ImVec2(box_min.x + 0.42f * box, box_min.y + 0.72f * box),
                    ImVec2(box_min.x + 0.76f * box, box_min.y + 0.30f * box),
                    u32(palette.text), ui_px(1.6f));
    } else {
        draw_box(dl, box_min, box_max, hovered);
    }
    const float glyph_x = box_max.x + ui_px(8.0f);
    const float text_x = glyph_x + glyph;
    const ImU32 label_color = u32(hovered ? palette.text : palette.text_dim);
    if (icon != Icon::kNone) {
        draw_icon(dl, ImVec2(glyph_x + 0.5f * icon_box(), pos.y + ui_px(1.0f) + 0.5f * box),
                  icon_box(), icon, label_color);
    }
    dl->PushClipRect(ImVec2(text_x, pos.y), ImVec2(pos.x + hit_w, pos.y + box + ui_px(2.0f)),
                     true);
    dl->AddText(ImVec2(text_x, pos.y + ui_px(1.0f)), label_color, label);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed;
}

bool slider_double(const char* label, double* value, double min, double max,
                   const char* format) {
    const float bar = ui_px(10.0f);
    ImGui::PushID(label);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float width = fill_width();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    char value_text[64];
    std::snprintf(value_text, sizeof(value_text), format, *value);
    const ImVec2 vsize = ImGui::CalcTextSize(value_text);
    const ImVec2 lsize = ImGui::CalcTextSize(label);
    // Clip label so it never runs into the value on the right.
    dl->PushClipRect(
        pos, ImVec2(pos.x + width - vsize.x - ui_px(6.0f), pos.y + lsize.y + ui_px(2.0f)),
        true);
    dl->AddText(pos, u32(palette.text_dim), label);
    dl->PopClipRect();
    dl->AddText(ImVec2(pos.x + width - vsize.x, pos.y), u32(palette.text_dim), value_text);
    ImGui::Dummy(ImVec2(width, lsize.y + ui_px(2.0f)));

    pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##slider", ImVec2(width, bar + ui_px(4.0f)));
    const bool active = ImGui::IsItemActive();
    if (active) {
        const float mouse_t =
            std::clamp((ImGui::GetIO().MousePos.x - pos.x) / width, 0.0f, 1.0f);
        *value = min + (max - min) * static_cast<double>(mouse_t);
    }
    const ImVec2 bar_min(pos.x, pos.y + ui_px(2.0f));
    const ImVec2 bar_max(pos.x + width, pos.y + ui_px(2.0f) + bar);
    draw_box(dl, bar_min, bar_max, ImGui::IsItemHovered());
    const float t = max > min ? static_cast<float>((*value - min) / (max - min)) : 0.0f;
    if (t > 0.0f) {
        draw_accent_fill(dl, bar_min,
                         ImVec2(bar_min.x + std::max(ui_px(3.0f), t * width), bar_max.y));
    }
    ImGui::PopID();
    return active;
}

bool button(const char* label, const ImVec2& size, bool primary, const char* help, Icon icon) {
    ImGui::PushID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label);
    // size.x < 0 → fill item width (respects group-box PushItemWidth);
    // size.x == 0 → hug label; size.x > 0 → explicit.
    const float avail = fill_width();
    const float hug = label_size.x + 2.0f * button_pad_x() +
                      (icon == Icon::kNone ? 0.0f : icon_box() + icon_gap());
    const float width = size.x < 0.0f ? avail : size.x > 0.0f ? size.x : std::min(avail, hug);
    const float height = size.y > 0.0f ? size.y : label_size.y + 2.0f * button_pad_y();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##btn", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    if (primary) {
        draw_accent_fill(dl, pos, max);
        if (hovered) {
            dl->AddRect(pos, max, u32(palette.text), ImGui::GetStyle().FrameRounding,
                        ImDrawFlags_RoundCornersAll, ui_px(1.0f));
        }
    } else {
        draw_box(dl, pos, max, hovered);
    }
    draw_centered_label(dl, pos, max, label, u32(palette.text), icon);
    if (help != nullptr && help[0] != '\0') {
        tooltip(help);
    }
    ImGui::PopID();
    return pressed;
}

namespace {

/// Place a dim label, then prepare width for a full-width field control.
/// Stacks the field under the label when the row would overflow.
void begin_field(const char* label) {
    const float avail = fill_width();
    const float text_w = ImGui::CalcTextSize(label).x;
    const float min_field = ui_px(96.0f);
    const float gap = ui_px(10.0f);

    if (text_w + gap + min_field > avail) {
        ImGui::TextColored(palette.text_dim, "%s", label);
        ImGui::SetNextItemWidth(fill_width());
        return;
    }

    // Side-by-side relative to this row's start (not window origin — group boxes
    // indent via SetCursorScreenPos, so SameLine(offset) would misalign).
    const float row_start = ImGui::GetCursorPosX();
    const float label_col = std::clamp(text_w + gap, avail * 0.38f, avail - min_field);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(palette.text_dim, "%s", label);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(row_start + label_col);
    ImGui::SetNextItemWidth(std::max(1.0f, avail - label_col));
}

} // namespace

bool input_double(const char* label, double* value, const char* format) {
    ImGui::PushID(label);
    begin_field(label);
    const bool changed = ImGui::InputDouble("##v", value, 0.0, 0.0, format);
    ImGui::PopID();
    return changed;
}

bool input_float3(const char* label, float value[3]) {
    ImGui::PushID(label);
    begin_field(label);
    const bool changed = ImGui::InputFloat3("##v", value, "%.1f");
    ImGui::PopID();
    return changed;
}

bool input_text(const char* label, char* buffer, size_t buffer_size, const char* hint) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(fill_width());
    const bool changed = ImGui::InputTextWithHint("##t", hint, buffer, buffer_size);
    ImGui::PopID();
    return changed;
}

bool selector(const char* label, int* index, const char* const* options, int count,
              const char* const* help, const Icon* icons) {
    ImGui::PushID(label);
    bool changed = false;
    if (count <= 0) {
        ImGui::PopID();
        return false;
    }
    // One heading treatment across the whole app: uppercase, letter-spaced,
    // dim. A plain TextColored caption here was the reason "Field" read
    // title-case beside an all-caps "CAMERA" in the same rail.
    field_label(label);

    const float gap = ui_px(4.0f);
    const float pad_x = ui_px(8.0f);
    // Symmetric left/right: width comes from PushItemWidth / content child.
    const float avail = fill_width();
    const float height = ImGui::GetTextLineHeight() + 2.0f * button_pad_y();
    const float icon_extra = icons == nullptr ? 0.0f : icon_box() + icon_gap();
    const int cols = fit_selector_columns(options, count, avail, gap, pad_x, icon_extra);

    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        // The final row is usually incomplete (five fields in two columns). Share
        // the whole row between the options it actually holds instead of leaving
        // a hole beside the last one.
        const int row_count = std::min(cols, count - (i - col));
        const float cell_w =
            (avail - gap * static_cast<float>(row_count - 1)) / static_cast<float>(row_count);
        if (col > 0) {
            ImGui::SameLine(0.0f, gap);
        }

        ImGui::PushID(i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        // Last column absorbs leftover pixels so the row fills avail exactly
        // (symmetric padding comes from the group-box content width, not cells).
        float width = cell_w;
        if (col == row_count - 1) {
            width = avail - (cell_w + gap) * static_cast<float>(row_count - 1);
        }
        width = std::max(width, 1.0f);

        const bool pressed = ImGui::InvisibleButton("##opt", ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        if (help != nullptr && help[i] != nullptr && help[i][0] != '\0') {
            tooltip(help[i]);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 max(pos.x + width, pos.y + height);
        if (*index == i) {
            draw_accent_fill(dl, pos, max);
        } else {
            draw_box(dl, pos, max, hovered);
        }
        draw_centered_label(dl, pos, max, options[i],
                            u32(*index == i ? palette.text : palette.text_dim),
                            icons != nullptr ? icons[i] : Icon::kNone);
        if (pressed && *index != i) {
            *index = i;
            changed = true;
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

namespace {

struct StepFrame {
    float body_pad = 0.0f;
};

std::vector<StepFrame> g_step_stack;

ImFont* find_font(const char* name_fragment) {
    static int cached_frame = -1;
    static ImFont* cached_font = nullptr;
    const int frame = ImGui::GetFrameCount();
    if (cached_frame == frame) {
        return cached_font;
    }

    cached_frame = frame;
    cached_font = nullptr;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (atlas == nullptr) {
        return nullptr;
    }
    for (ImFont* font : atlas->Fonts) {
        if (font != nullptr && std::strstr(font->GetDebugName(), name_fragment) != nullptr) {
            cached_font = font;
            break;
        }
    }
    return cached_font;
}

void draw_step_chip(ImDrawList* dl, const ImVec2& min, float size, int index, bool done) {
    const ImVec2 max(min.x + size, min.y + size);
    const float radius = size * 0.5f;
    dl->AddRectFilled(min, max, u32(done ? palette.accent : palette.accent_soft), radius);
    dl->AddRect(min, max, u32(palette.accent), radius, ImDrawFlags_RoundCornersAll,
                ui_px(1.0f));

    if (done) {
        dl->AddLine(ImVec2(min.x + size * 0.27f, min.y + size * 0.53f),
                    ImVec2(min.x + size * 0.44f, min.y + size * 0.69f), u32(palette.text),
                    ui_px(1.8f));
        dl->AddLine(ImVec2(min.x + size * 0.44f, min.y + size * 0.69f),
                    ImVec2(min.x + size * 0.75f, min.y + size * 0.32f), u32(palette.text),
                    ui_px(1.8f));
        return;
    }

    char number[16];
    std::snprintf(number, sizeof(number), "%d", index);
    const ImVec2 text_size = ImGui::CalcTextSize(number);
    dl->AddText(
        ImVec2(min.x + (size - text_size.x) * 0.5f, min.y + (size - text_size.y) * 0.5f),
        u32(palette.accent), number);
}

const ImVec4& chip_tone_color(int tone) {
    switch (tone) {
    case 1:
        return palette.status_ok;
    case 2:
        return palette.status_warn;
    case 3:
        return palette.status_err;
    case 4:
        return palette.accent;
    default:
        return palette.text_dim;
    }
}

void draw_disclosure_arrow(ImDrawList* dl, ImVec2 center, bool open, const ImVec4& color) {
    const float size = ui_px(5.0f);
    const float thickness = ui_px(1.5f);
    if (open) {
        dl->AddLine(ImVec2(center.x - size, center.y - size * 0.5f),
                    ImVec2(center.x, center.y + size * 0.5f), u32(color), thickness);
        dl->AddLine(ImVec2(center.x, center.y + size * 0.5f),
                    ImVec2(center.x + size, center.y - size * 0.5f), u32(color), thickness);
        return;
    }
    dl->AddLine(ImVec2(center.x - size * 0.5f, center.y - size),
                ImVec2(center.x + size * 0.5f, center.y), u32(color), thickness);
    dl->AddLine(ImVec2(center.x + size * 0.5f, center.y),
                ImVec2(center.x - size * 0.5f, center.y + size), u32(color), thickness);
}

} // namespace

bool begin_step(int index, const char* title, const char* subtitle, bool done, bool* open) {
    IM_ASSERT(open != nullptr);
    if (open == nullptr) {
        return false;
    }

    const char* title_text = title != nullptr ? title : "";
    const char* subtitle_text = subtitle != nullptr ? subtitle : "";
    const float width = std::max(ui_px(1.0f), ImGui::GetContentRegionAvail().x);
    const ImVec2 start = ImGui::GetCursorScreenPos();

    ImGui::PushID(index);
    ImGui::PushID(title_text);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette.surface_hi);
    ImGui::PushStyleColor(ImGuiCol_Border, palette.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(0.0f), ui_px(0.0f)));
    ImGui::BeginChild("##workflow_step", ImVec2(width, ui_px(0.0f)),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    const float header_height = ui_px(62.0f);
    const float horizontal_pad = ui_px(14.0f);
    const float chip_size = ui_px(28.0f);
    const bool pressed = ImGui::InvisibleButton(
        "##header", ImVec2(ImGui::GetContentRegionAvail().x, header_height));
    const bool hovered = ImGui::IsItemHovered();
    if (pressed) {
        *open = !*open;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 header_min = ImGui::GetItemRectMin();
    const ImVec2 header_max = ImGui::GetItemRectMax();
    if (hovered) {
        dl->AddRectFilled(header_min, header_max, u32(palette.hover),
                          ImGui::GetStyle().ChildRounding, ImDrawFlags_RoundCornersTop);
    }

    const ImVec2 chip_min(header_min.x + horizontal_pad,
                          header_min.y + (header_height - chip_size) * 0.5f);
    draw_step_chip(dl, chip_min, chip_size, index, done);

    const float text_x = chip_min.x + chip_size + ui_px(12.0f);
    const float arrow_x = header_max.x - horizontal_pad;
    const float text_max_x = arrow_x - ui_px(13.0f);
    ImFont* medium = find_font("Rubik-Medium");
    if (medium != nullptr) {
        ImGui::PushFont(medium);
    }
    const float title_height = ImGui::GetTextLineHeight();
    const float title_y = subtitle_text[0] == '\0'
                              ? header_min.y + (header_height - title_height) * 0.5f
                              : header_min.y + ui_px(12.0f);
    // Both rows are ellipsized rather than hard-clipped: a clip rect alone cut
    // "plate+hole.step · 268 triangles · 7 faces" mid-word (and mid-glyph) with
    // no sign that anything was dropped. The clip rect stays as the backstop.
    const float text_budget = std::max(0.0f, text_max_x - text_x);
    char fitted[192]{};
    dl->PushClipRect(ImVec2(text_x, header_min.y), ImVec2(text_max_x, header_max.y), true);
    dl->AddText(ImVec2(text_x, title_y), u32(palette.text),
                fit_text(fitted, sizeof(fitted), title_text, text_budget, ImGui::GetFont(),
                         ImGui::GetFontSize()));
    dl->PopClipRect();
    if (medium != nullptr) {
        ImGui::PopFont();
    }

    if (subtitle_text[0] != '\0') {
        const float subtitle_y = title_y + title_height + ui_px(3.0f);
        dl->PushClipRect(ImVec2(text_x, header_min.y), ImVec2(text_max_x, header_max.y), true);
        dl->AddText(ImVec2(text_x, subtitle_y), u32(palette.text_dim),
                    fit_text(fitted, sizeof(fitted), subtitle_text, text_budget,
                             ImGui::GetFont(), ImGui::GetFontSize()));
        dl->PopClipRect();
    }
    draw_disclosure_arrow(dl, ImVec2(arrow_x, header_min.y + header_height * 0.5f), *open,
                          hovered ? palette.accent : palette.text_dim);

    if (!*open) {
        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopID();
        ImGui::Dummy(ImVec2(ui_px(0.0f), ui_px(5.0f)));
        return false;
    }

    const float body_pad = ui_px(14.0f);
    const float body_width = std::max(ui_px(1.0f), width - 2.0f * body_pad);
    ImGui::SetCursorScreenPos(ImVec2(start.x, header_max.y + ui_px(6.0f)));
    ImGui::Indent(body_pad);
    ImGui::PushItemWidth(body_width);
    g_step_stack.push_back({body_pad});
    return true;
}

void end_step() {
    IM_ASSERT(!g_step_stack.empty());
    if (g_step_stack.empty()) {
        return;
    }
    const StepFrame frame = g_step_stack.back();
    g_step_stack.pop_back();

    ImGui::PopItemWidth();
    ImGui::Unindent(frame.body_pad);
    ImGui::Dummy(ImVec2(ui_px(0.0f), ui_px(8.0f)));
    ImGui::EndChild();

    const ImVec2 card_min = ImGui::GetItemRectMin();
    const ImVec2 card_max = ImGui::GetItemRectMax();
    const float inset = ui_px(1.0f);
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(card_min.x + inset, card_min.y + ImGui::GetStyle().ChildRounding),
        ImVec2(card_min.x + ui_px(3.0f), card_max.y - ImGui::GetStyle().ChildRounding),
        u32(palette.accent), ui_px(1.0f));

    ImGui::PopID();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(ui_px(0.0f), ui_px(5.0f)));
}

void stat_row(const char* label, const char* value, ImFont* mono, Icon icon) {
    const char* label_text = label != nullptr ? label : "";
    const char* value_text = value != nullptr ? value : "";
    const float width = fill_width();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 label_size = ImGui::CalcTextSize(label_text);

    if (mono != nullptr) {
        ImGui::PushFont(mono);
    }
    const ImVec2 value_size = ImGui::CalcTextSize(value_text);
    const float value_font_size = ImGui::GetFontSize();
    if (mono != nullptr) {
        ImGui::PopFont();
    }

    const float height = std::max(label_size.y, value_size.y) + ui_px(2.0f);
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float glyph = icon == Icon::kNone ? 0.0f : icon_box();
    const float label_x = pos.x + (icon == Icon::kNone ? 0.0f : glyph + icon_gap());
    const float value_x = pos.x + width - value_size.x;
    const float label_max_x = std::max(label_x, value_x - ui_px(8.0f));
    if (icon != Icon::kNone) {
        draw_icon(dl, ImVec2(pos.x + 0.5f * glyph, pos.y + 0.5f * height), glyph, icon,
                  u32(palette.text_dim));
    }
    dl->PushClipRect(ImVec2(label_x, pos.y), ImVec2(label_max_x, pos.y + height), true);
    dl->AddText(ImVec2(label_x, pos.y), u32(palette.text_dim), label_text);
    dl->PopClipRect();
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + height), true);
    if (mono != nullptr) {
        dl->AddText(mono, value_font_size, ImVec2(value_x, pos.y), u32(palette.text),
                    value_text);
    } else {
        dl->AddText(ImVec2(value_x, pos.y), u32(palette.text), value_text);
    }
    dl->PopClipRect();
}

void chip(const char* text, int tone) {
    const char* chip_text = text != nullptr ? text : "";
    const ImVec2 text_size = ImGui::CalcTextSize(chip_text);
    const float pad_x = ui_px(8.0f);
    const float pad_y = ui_px(3.0f);
    const ImVec2 size(text_size.x + 2.0f * pad_x, text_size.y + 2.0f * pad_y);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);

    const ImVec4& tone_color = chip_tone_color(tone);
    const ImVec4& fill = tone == 4 ? palette.accent_soft : palette.surface_hi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + size.x, pos.y + size.y);
    const float rounding = size.y * 0.5f;
    dl->AddRectFilled(pos, max, u32(fill), rounding);
    dl->AddRect(pos, max, u32(tone_color), rounding, ImDrawFlags_RoundCornersAll, ui_px(1.0f));
    dl->AddText(ImVec2(pos.x + pad_x, pos.y + pad_y), u32(tone_color), chip_text);
}

void field_label(const char* text, Icon icon) {
    const char* source = text != nullptr ? text : "";
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float max_x = pos.x + fill_width();
    const float spacing = ui_px(1.5f);
    const float word_gap = ui_px(5.0f);
    const float line = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = pos.x;
    if (icon != Icon::kNone) {
        const float glyph = icon_box();
        draw_icon(dl, ImVec2(x + 0.5f * glyph, pos.y + 0.5f * line), glyph, icon,
                  u32(palette.text_dim));
        x += glyph + icon_gap();
    }
    dl->PushClipRect(pos, ImVec2(max_x, pos.y + line), true);
    for (const char* cursor = source; *cursor != '\0'; ++cursor) {
        const unsigned char ch = static_cast<unsigned char>(*cursor);
        if (ch == ' ') {
            x += word_gap;
            continue;
        }
        char glyph[2] = {static_cast<char>(std::toupper(ch)), '\0'};
        dl->AddText(ImVec2(x, pos.y), u32(palette.text_dim), glyph);
        x += ImGui::CalcTextSize(glyph).x + spacing;
    }
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(std::min(max_x - pos.x, x - pos.x), line));
}

bool disclosure(const char* label, bool* open) {
    IM_ASSERT(open != nullptr);
    if (open == nullptr) {
        return false;
    }

    const char* label_text = label != nullptr ? label : "";
    const float width = fill_width();
    const float height = ImGui::GetTextLineHeight() + ui_px(6.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label_text, ImVec2(width, height));
    if (pressed) {
        *open = !*open;
    }

    const bool hovered = ImGui::IsItemHovered();
    const ImVec4& color = hovered ? palette.text : palette.text_dim;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(pos.x, pos.y + ui_px(3.0f)), u32(color), label_text);
    const ImVec2 arrow_center(pos.x + width - ui_px(8.0f), pos.y + height * 0.5f);
    draw_disclosure_arrow(dl, arrow_center, *open, color);
    return *open;
}

void progress(const char* caption, float fraction, const char* value) {
    const char* caption_text = caption != nullptr ? caption : "";
    const char* value_text = value != nullptr ? value : "";
    const float width = fill_width();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 value_size = ImGui::CalcTextSize(value_text);
    const float text_height = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float value_x = pos.x + width - value_size.x;
    const float caption_max_x = std::max(pos.x, value_x - ui_px(8.0f));
    dl->PushClipRect(pos, ImVec2(caption_max_x, pos.y + text_height), true);
    dl->AddText(pos, u32(palette.text_dim), caption_text);
    dl->PopClipRect();
    dl->PushClipRect(pos, ImVec2(pos.x + width, pos.y + text_height), true);
    dl->AddText(ImVec2(value_x, pos.y), u32(palette.text), value_text);
    dl->PopClipRect();

    const float gap = ui_px(6.0f);
    const float bar_height = ui_px(8.0f);
    const ImVec2 bar_min(pos.x, pos.y + text_height + gap);
    const ImVec2 bar_max(pos.x + width, bar_min.y + bar_height);
    draw_box(dl, bar_min, bar_max, false);

    if (fraction < 0.0f) {
        const float sweep_width = width * 0.30f;
        const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 0.65f, 1.0f);
        const float sweep_x = bar_min.x - sweep_width + (width + sweep_width) * phase;
        dl->PushClipRect(bar_min, bar_max, true);
        draw_accent_fill(dl, ImVec2(sweep_x, bar_min.y),
                         ImVec2(sweep_x + sweep_width, bar_max.y));
        dl->PopClipRect();
    } else {
        const float amount = std::isfinite(fraction) ? std::clamp(fraction, 0.0f, 1.0f) : 0.0f;
        if (amount > 0.0f) {
            draw_accent_fill(dl, bar_min, ImVec2(bar_min.x + width * amount, bar_max.y));
        }
    }

    ImGui::Dummy(ImVec2(width, text_height + gap + bar_height));
}

void tooltip(const char* text) {
    if (text == nullptr || text[0] == '\0' ||
        !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                              ImGuiHoveredFlags_NoSharedDelay)) {
        return;
    }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ui_px(380.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
} // namespace polymesh::gui::iw
