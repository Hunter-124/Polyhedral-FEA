// SPDX-License-Identifier: BSD-3-Clause

// Studio viewport column: the offscreen render as an image, the results
// colorbar overlay, the frame button, camera input and CAD face picking.

#include "app_state.hpp"
#include "colormap.hpp"
#include "theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>

namespace polymesh::gui {

namespace {

void draw_colorbar(const char* title, float vmin, float vmax, const char* unit) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = 18.0f;
    const float h = 140.0f;
    for (int i = 0; i < 32; ++i) {
        const float t0 = static_cast<float>(i) / 32.0f;
        const float t1 = static_cast<float>(i + 1) / 32.0f;
        // Same ramp the viewport bakes into result vertex colors (colormap.hpp)
        // — one source of truth so the legend can never drift from the render.
        const auto rgb = fea_colormap(0.5f * (t0 + t1));
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f));
        dl->AddRectFilled(ImVec2(p0.x, p0.y + h * (1.0f - t1)),
                          ImVec2(p0.x + w, p0.y + h * (1.0f - t0)), col);
    }
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), palette.colorbar_border);
    ImGui::Dummy(ImVec2(w + 8, h));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", title);
    ImGui::Text("%.3g %s", static_cast<double>(vmax), unit);
    ImGui::Dummy(ImVec2(0, h - 48));
    ImGui::Text("%.3g", static_cast<double>(vmin));
    ImGui::EndGroup();
}

} // namespace

void draw_viewport_content(App& app) {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1 || size.y < 1) {
        return;
    }

    if (app.overlays_dirty && app.model) {
        app.viewport.update_overlays(*app.model, app.setup, app.selected_region,
                                     app.hovered_region);
        app.overlays_dirty = false;
    }
    float result_max = 1.0f;
    if (app.result) {
        if (app.mode == DisplayMode::kResultsVonMises) {
            result_max = static_cast<float>(app.result->max_von_mises);
        } else if (app.mode == DisplayMode::kResultsDisplacement) {
            result_max = static_cast<float>(app.result->max_displacement);
        } else if (app.mode == DisplayMode::kResultsError) {
            result_max = static_cast<float>(std::max(app.result->max_nodal_eta, 1e-30));
        }
    }
    app.viewport.render(static_cast<int>(size.x), static_cast<int>(size.y), app.mode,
                        static_cast<float>(app.deform_scale), result_max, app.show_wireframe,
                        app.show_undeformed);
    ImGui::Image(static_cast<ImTextureID>(app.viewport.texture()), size, ImVec2(0, 1),
                 ImVec2(1, 0));

    // Capture Image hover/rect *before* the colorbar child — otherwise
    // IsItemHovered() latches onto the colorbar and pan/orbit die in results modes.
    const bool viewport_hovered = ImGui::IsItemHovered();
    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();

    // Colorbar overlay (results modes only). NoInputs so it never steals camera.
    if (app.result && (app.mode == DisplayMode::kResultsVonMises ||
                       app.mode == DisplayMode::kResultsDisplacement ||
                       app.mode == DisplayMode::kResultsError)) {
        ImGui::SetCursorScreenPos(ImVec2(item_min.x + 12, item_min.y + 12));
        ImGui::BeginChild("##cbar", ImVec2(148, 170), false,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs |
                              ImGuiWindowFlags_NoScrollbar);
        if (app.mode == DisplayMode::kResultsVonMises) {
            draw_colorbar("von Mises", 0.0f, result_max, "Pa");
        } else if (app.mode == DisplayMode::kResultsDisplacement) {
            draw_colorbar("|u|", 0.0f, result_max, "m");
        } else {
            draw_colorbar("ZZ η", 0.0f, result_max, "");
        }
        ImGui::EndChild();
    }

    // "frame" affordance, top-right of the 3D image (the F key does the same
    // from anywhere). Its own rect is excluded from the camera/pick handling
    // below so pressing it never orbits or deselects a face.
    constexpr float kFrameBtnW = 84.0f;
    ImGui::SetCursorScreenPos(ImVec2(item_max.x - kFrameBtnW - 12.0f, item_min.y + 12.0f));
    if (ImGui::Button("frame (F)", ImVec2(kFrameBtnW, 0))) {
        app.viewport.frame_content(app.mode);
    }
    const bool over_frame_button = ImGui::IsItemHovered();

    // Camera works whenever the cursor is over the 3D image (all display modes).
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_over_view = io.MousePos.x >= item_min.x && io.MousePos.x <= item_max.x &&
                                 io.MousePos.y >= item_min.y && io.MousePos.y <= item_max.y;
    if ((viewport_hovered || mouse_over_view) && !over_frame_button) {
        const float u = (io.MousePos.x - item_min.x) / size.x;
        const float v = (io.MousePos.y - item_min.y) / size.y;
        const float aspect = size.x / size.y;

        // Track LMB travel so a pure click selects and a drag orbits.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            app.lmb_drag_px = 0.0f;
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            app.lmb_drag_px += std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
            (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyShift)) {
            app.viewport.camera.pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
                   (ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                    (app.mode != DisplayMode::kSetup || !app.pick_faces ||
                     app.lmb_drag_px > 4.0f))) {
            // Orbit: RMB always; LMB after drag (or always outside face-pick setup).
            app.viewport.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
        }
        if (io.MouseWheel != 0.0f) {
            app.viewport.camera.dolly(io.MouseWheel);
        }

        // Face hover/select only in CAD setup mode (region colors are meaningful).
        if (app.model && app.mode == DisplayMode::kSetup && app.pick_faces) {
            const auto hover = app.viewport.pick_region(*app.model, u, v, aspect);
            const int hovered = hover.value_or(-1);
            if (hovered != app.hovered_region) {
                app.hovered_region = hovered;
                app.overlays_dirty = true;
            }
            // Select on mouse release with little travel — avoids fight with orbit.
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.KeyShift &&
                app.lmb_drag_px < 5.0f) {
                app.selected_region = hovered;
                app.overlays_dirty = true;
                if (hovered >= 0) {
                    app.status = std::format("selected face {}", hovered);
                }
            }
        } else if (app.hovered_region >= 0) {
            app.hovered_region = -1;
            app.overlays_dirty = true;
        }
    }
}

} // namespace polymesh::gui
