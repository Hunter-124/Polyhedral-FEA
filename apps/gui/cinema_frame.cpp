// SPDX-License-Identifier: BSD-3-Clause

// Studio side of the activation cinema: the measured HUD snapshot, the
// fullscreen layout, mechanics glyphs over the viewport and the frame itself.

#include "app_state.hpp"
#include "cinema.hpp"
#include "theme.hpp"
#include "viewport.hpp"

#include "imgui.h"

#include <Eigen/Core>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

namespace polymesh::gui {

namespace {

/// Snapshot of what the app measured, for the cinema HUD. Every field is read
/// straight off the app state the studio's own panels report, so the two can
/// never disagree — in particular the mesher/h/order line is the SimSetup that
/// is actually meshing, not the advisor decision that asked for it.
CinemaHud make_cinema_hud(const App& app) {
    CinemaHud hud;
    if (app.model) {
        hud.part = app.model->name;
    }
    hud.mesher = std::string(pipeline::mesher_name(app.setup.mesher));
    hud.mesh_size = app.setup.mesh_size;
    if (app.mesh_preview) {
        hud.geometry_h = app.mesh_preview->geometry_h;
    }
    // One p-elevation step exists in the solve path, so the executed order is
    // 2 when it runs and 1 when it does not.
    hud.order = app.setup.p_elevate ? 2 : 1;
    hud.adapt_passes = app.setup.adapt_passes;
    hud.eta_target = app.setup.eta_target;
    hud.youngs_modulus = app.setup.youngs_modulus;
    hud.poissons_ratio = app.setup.poissons_ratio;
    if (app.model) {
        hud.model_diagonal = (app.model->bbox_max - app.model->bbox_min).norm();
    }
    // Vector sum, not a sum of magnitudes: two opposed face loads resultant to
    // zero and the load factor has to say so.
    Eigen::Vector3d total_force = Eigen::Vector3d::Zero();
    for (const auto& [face, load] : app.setup.loads) {
        total_force += load.force;
    }
    hud.load_newtons = total_force.norm();
    if (app.result) {
        hud.has_result = true;
        hud.nodes = app.result->volume_mesh.nodes.size();
        hud.elements = app.result->volume_mesh.elements.size();
        hud.max_von_mises = app.result->max_von_mises;
        hud.max_displacement = app.result->max_displacement;
        hud.global_eta = app.result->global_eta;
    } else if (app.mesh_preview) {
        hud.nodes = app.mesh_preview->mesh.nodes.size();
        hud.elements = app.mesh_preview->mesh.elements.size();
    }
    hud.dof = app.dof_count;
    hud.deform_scale = app.deform_scale;
    hud.cinema_elements = app.viewport.cinema_element_count();
    hud.cinema_skipped_elements = app.viewport.cinema_skipped_element_count();
    hud.unchanged_elements = app.viewport.cinema_unchanged_element_count();
    hud.removed_elements = app.viewport.cinema_removed_element_count();
    hud.added_elements = app.viewport.cinema_added_element_count();
    hud.stamp = app.cinema_stamp;
    return hud;
}

std::optional<ImVec2> project_cinema_point(const Camera& camera, const Eigen::Vector3d& world,
                                           const ImVec2& image_min, const ImVec2& image_size) {
    const float aspect = image_size.x / std::max(image_size.y, 1.0f);
    const Eigen::Vector4f point(static_cast<float>(world.x()), static_cast<float>(world.y()),
                                static_cast<float>(world.z()), 1.0f);
    const Eigen::Vector4f clip = camera.projection(aspect) * camera.view() * point;
    if (!(clip.w() > 1.0e-6f)) {
        return std::nullopt;
    }
    const Eigen::Vector3f ndc = clip.head<3>() / clip.w();
    return ImVec2(image_min.x + (0.5f * ndc.x() + 0.5f) * image_size.x,
                  image_min.y + (0.5f - 0.5f * ndc.y()) * image_size.y);
}

Eigen::Vector3d displayed_marker_position(const CinemaState& state,
                                          const CinemaMechanicsMarker& marker,
                                          const CinemaRender& render) {
    if (state.solve_stages.empty()) {
        return marker.position;
    }
    // `result_node` was resolved against the authoritative final result. Use
    // that surface node from frame zero onward; only its displacement scale
    // changes during the final load ramp, so the glyph never teleports from the
    // cylindrical region's area centroid (which lies in the bore void).
    const auto& result = state.solve_stages.back().result;
    if (marker.result_node >= result.volume_mesh.nodes.size() ||
        result.displacement.size() !=
            3 * static_cast<Eigen::Index>(result.volume_mesh.nodes.size())) {
        return marker.position;
    }
    const Eigen::Index base = 3 * static_cast<Eigen::Index>(marker.result_node);
    return result.volume_mesh.nodes[marker.result_node] +
           static_cast<double>(render.deform_scale) * result.displacement.segment<3>(base);
}

void draw_cinema_mechanics(App& app, const CinemaCue& cue, const CinemaRender& render,
                           const CinemaType& type, const ImVec2& image_min,
                           const ImVec2& image_size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    const auto color = [](ImVec4 c, float alpha) {
        c.w *= alpha;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    const auto label_position = [&](const std::string& text, float size, ImVec2 desired,
                                    float anchor_x) {
        const float width = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
        const float left = image_min.x + 8.0f;
        const float right = image_min.x + image_size.x - 8.0f;
        if (desired.x + width > right) {
            desired.x = anchor_x - width - 20.0f;
        }
        desired.x = std::clamp(desired.x, left, std::max(left, right - width));
        desired.y =
            std::clamp(desired.y, image_min.y + 8.0f,
                       std::max(image_min.y + 8.0f, image_min.y + image_size.y - size - 8.0f));
        return desired;
    };
    const float entering = cue.act == CinemaAct::kSkeleton
                               ? std::clamp(static_cast<float>(cue.act_t / 1.0), 0.0f, 1.0f)
                               : 1.0f;
    const float mechanics_alpha = 0.88f * entering;
    const float mechanics_pulse = 0.5f + 0.5f * std::sin(static_cast<float>(cue.act_t) * 5.2f);

    for (std::size_t i = 0; i < app.cinema.support_markers.size(); ++i) {
        const auto& marker = app.cinema.support_markers[i];
        const Eigen::Vector3d world = displayed_marker_position(app.cinema, marker, render);
        const auto projected =
            project_cinema_point(app.viewport.camera, world, image_min, image_size);
        if (!projected) {
            continue;
        }
        const ImVec2 p = *projected;
        const ImVec4 c = palette.sim_fixture;
        dl->AddCircleFilled(p, 7.0f, color(c, mechanics_alpha));
        dl->AddCircle(p, 15.0f + 3.0f * mechanics_pulse,
                      color(c, (0.32f + 0.20f * mechanics_pulse) * mechanics_alpha), 0, 2.0f);
        dl->AddLine(ImVec2(p.x - 14.0f, p.y + 13.0f), ImVec2(p.x + 14.0f, p.y + 13.0f),
                    color(c, mechanics_alpha), 3.0f);
        for (int hatch = -2; hatch <= 2; ++hatch) {
            const float x = p.x + static_cast<float>(hatch) * 6.0f;
            dl->AddLine(ImVec2(x - 4.0f, p.y + 20.0f), ImVec2(x + 3.0f, p.y + 13.0f),
                        color(c, 0.72f * mechanics_alpha), 1.5f);
        }
        const std::string label = std::format("FIXED SUPPORT {}", static_cast<char>('A' + i));
        const float label_w = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, label.c_str()).x;
        const ImVec2 desired(i == 0 ? p.x - label_w - 34.0f : p.x + 24.0f,
                             p.y - type.legend * (i == 0 ? 3.2f : 2.1f));
        const ImVec2 label_at = label_position(label, type.legend, desired, p.x);
        dl->AddLine(p,
                    ImVec2(i == 0 ? label_at.x + label_w : label_at.x,
                           label_at.y + type.legend * 0.55f),
                    color(c, 0.48f * mechanics_alpha), 1.0f);
        dl->AddText(font, type.legend, label_at, color(c, mechanics_alpha), label.c_str());
    }

    for (const auto& marker : app.cinema.load_markers) {
        if (!(marker.vector.norm() > 0.0)) {
            continue;
        }
        const double force_scale = cue.solve_phase == SolvePhase::kLoadRamp
                                       ? std::clamp(cue.load_factor, 0.0, 1.0)
                                       : 1.0;
        if (!(force_scale > 1.0e-4)) {
            continue;
        }
        const Eigen::Vector3d centre = displayed_marker_position(app.cinema, marker, render);
        const Eigen::Vector3d direction = marker.vector.normalized();
        const double length = 0.17 * force_scale * std::max(app.cinema.model_diagonal, 1.0e-6);
        const auto tail = project_cinema_point(
            app.viewport.camera, centre - 0.50 * length * direction, image_min, image_size);
        const auto head = project_cinema_point(
            app.viewport.camera, centre + 0.50 * length * direction, image_min, image_size);
        if (!tail || !head) {
            continue;
        }
        const ImVec4 c = palette.sim_load;
        const ImVec2 delta(head->x - tail->x, head->y - tail->y);
        const float n = std::hypot(delta.x, delta.y);
        if (!(n > 2.0f)) {
            continue;
        }
        const ImVec2 unit(delta.x / n, delta.y / n);
        const ImVec2 normal(-unit.y, unit.x);
        dl->AddLine(*tail, *head,
                    color(c, (0.12f + 0.14f * mechanics_pulse) * mechanics_alpha),
                    9.0f + 2.0f * mechanics_pulse);
        dl->AddLine(*tail, *head, color(c, mechanics_alpha), 3.5f);
        const ImVec2 wing_a(head->x - 18.0f * unit.x + 9.0f * normal.x,
                            head->y - 18.0f * unit.y + 9.0f * normal.y);
        const ImVec2 wing_b(head->x - 18.0f * unit.x - 9.0f * normal.x,
                            head->y - 18.0f * unit.y - 9.0f * normal.y);
        dl->AddTriangleFilled(*head, wing_a, wing_b, color(c, mechanics_alpha));
        const std::string label =
            std::format("{:.3g} kN APPLIED FORCE", force_scale * marker.vector.norm() / 1e3);
        const ImVec2 label_at = label_position(
            label, type.label, ImVec2(head->x + 18.0f, head->y + type.label * 0.65f), head->x);
        dl->AddLine(*head, ImVec2(label_at.x - 5.0f, label_at.y + type.label * 0.45f),
                    color(c, 0.45f * mechanics_alpha), 1.0f);
        dl->AddText(font, type.label, label_at, color(c, mechanics_alpha), label.c_str());
    }

    if (cue.action_bridge_alpha > 0.0f && app.cinema.advisor_ran) {
        const auto target = project_cinema_point(
            app.viewport.camera, app.cinema.subject_center, image_min, image_size);
        if (target) {
            const ImVec2 start(image_min.x + 3.0f, image_min.y + image_size.y * 0.78f);
            const ImVec2 c1(image_min.x + image_size.x * 0.13f, start.y);
            const ImVec2 c2(target->x - image_size.x * 0.18f, target->y);
            const float a = cue.action_bridge_alpha;
            const ImVec4 flow =
                app.cinema.decision_applied ? palette.accent : palette.status_warn;
            dl->AddBezierCubic(start, c1, c2, *target, color(flow, 0.11f * a), 7.0f);
            dl->AddBezierCubic(start, c1, c2, *target, color(flow, 0.84f * a), 2.2f);
            const auto bezier = [&](float t) {
                const float q = 1.0f - t;
                return ImVec2(q * q * q * start.x + 3.0f * q * q * t * c1.x +
                                  3.0f * q * t * t * c2.x + t * t * t * target->x,
                              q * q * q * start.y + 3.0f * q * q * t * c1.y +
                                  3.0f * q * t * t * c2.y + t * t * t * target->y);
            };
            for (int i = 0; i < 5; ++i) {
                const float t = std::fmod(static_cast<float>(cue.activation_wave) +
                                              0.19f * static_cast<float>(i),
                                          1.0f);
                dl->AddCircleFilled(bezier(t), 3.2f, color(flow, a));
            }
            const float target_pulse =
                0.5f + 0.5f * std::sin(static_cast<float>(cue.act_t) * 6.0f);
            dl->AddCircle(*target, 9.0f + 4.0f * target_pulse,
                          color(flow, (0.28f + 0.34f * target_pulse) * a), 0, 1.6f);
            const char* bridge = app.cinema.decision_applied ? "CHOSEN ACTION → RECORDED CELLS"
                                 : app.cinema.decision_vetoed
                                     ? "ADVISOR ABSTAINED → VERIFIED BASELINE CELLS"
                                     : "UNRECOGNISED ACTION → STUDIO SETUP";
            const ImVec2 text_at(start.x + 10.0f, start.y - type.legend * 1.5f);
            const ImVec2 text_size = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, bridge);
            dl->AddRectFilled(
                ImVec2(text_at.x - 8.0f, text_at.y - 5.0f),
                ImVec2(text_at.x + text_size.x + 8.0f, text_at.y + text_size.y + 5.0f),
                color(palette.panel_bg, 0.78f * a), 6.0f);
            dl->AddRect(ImVec2(text_at.x - 8.0f, text_at.y - 5.0f),
                        ImVec2(text_at.x + text_size.x + 8.0f, text_at.y + text_size.y + 5.0f),
                        color(flow, 0.42f * a), 6.0f, 0, 1.0f);
            dl->AddText(
                font, type.legend, text_at,
                color(app.cinema.decision_applied ? palette.accent : palette.status_warn, a),
                bridge);
        }
    }
}

/// The cinema's viewport: the offscreen render as a full-bleed image. No
/// colorbar, no frame button, no face picking — nothing that would have to be
/// cropped out of a recorded frame. Camera drag still works so a take can be
/// composed interactively; a recording never touches the mouse.
///
/// The mode, the exaggeration and the colour-scale maximum all come from
/// `cinema_render`, not from the studio's own sliders.
void draw_cinema_viewport(App& app, const CinemaRender& render, const CinemaCue& cue,
                          const CinemaType& type) {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 1 || size.y < 1) {
        return;
    }
    app.viewport.render(static_cast<int>(size.x), static_cast<int>(size.y), render.mode,
                        render.deform_scale, render.result_max, app.show_wireframe, false);
    ImGui::Image(static_cast<ImTextureID>(app.viewport.texture()), size, ImVec2(0, 1),
                 ImVec2(1, 0));
    draw_cinema_mechanics(app, cue, render, type, ImGui::GetItemRectMin(), size);
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsItemHovered()) {
        return;
    }
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && io.KeyShift)) {
        app.viewport.camera.pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
               ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        app.viewport.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (io.MouseWheel != 0.0f) {
        app.viewport.camera.dolly(io.MouseWheel);
    }
}

} // namespace

CinemaLayout cinema_layout(const App& app, const ImGuiViewport& vp) {
    CinemaLayout out;
    out.content_w = std::floor(vp.Size.x);
    out.type = cinema_type(app.cinema_font, std::floor(vp.Size.y));
    out.strip_h =
        std::max(4.0f * ImGui::GetTextLineHeightWithSpacing(), cinema_strip_height(out.type));
    out.content_h = std::max(1.0f, std::floor(vp.Size.y) - out.strip_h);
    out.panel_w = std::floor(out.content_w * kCinemaPanelWidthFraction);
    out.settled_view_aspect = std::max(1.0e-6f, (out.content_w - out.panel_w) / out.content_h);
    return out;
}

/// The split is not fixed: the viewport holds the whole window through the
/// opening act and the panel slides into its share as it fades up.
void draw_cinema_frame(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const CinemaCue cue = cinema_cue(app.cinema);

    // The cue owns what is shown; the app only carries it into the viewport.
    const CinemaRender render = cinema_render(app.cinema, cue, app.deform_scale);
    app.mode = render.mode;
    sync_cinema_viewport(app.cinema, cue, render, app.viewport);
    const CinemaHud hud = make_cinema_hud(app);

    const CinemaLayout layout = cinema_layout(app, *vp);
    const float content_w = layout.content_w;
    const CinemaType& type = layout.type;
    const float strip_h = layout.strip_h;
    const float content_h = layout.content_h;
    const float panel_w = layout.panel_w;
    // The panel opens exactly once, during the opening act, sliding in from the
    // left while laid out at its FINAL width, so wrapping never changes
    // mid-slide. The projection fixes the vertical field and the pane height is
    // constant, so the part keeps its pixel scale and only pans with the split.
    const float open = std::clamp(cue.panel_open, 0.0f, 1.0f);
    const float panel_slide = std::floor(panel_w * (1.0f - open));

    ImGui::SetNextWindowPos(ImVec2(std::floor(vp->Pos.x), std::floor(vp->Pos.y)));
    ImGui::SetNextWindowSize(ImVec2(content_w, content_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##cinema", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const float row_h = ImGui::GetContentRegionAvail().y;

    if (open > 0.0f) {
        // Translated left by the un-opened remainder, so the panel's right edge
        // sits at `panel_w * open` and the viewport starts exactly there. The
        // part of the child that hangs off the left is clipped by the host
        // window; nothing else in the composition knows the difference.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - panel_slide);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 14.0f));
        ImGui::BeginChild("cinema_panel", ImVec2(panel_w, row_h),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        draw_cinema_panel(app.cinema, cue, type, hud);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, 0.0f);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("cinema_view", ImVec2(0.0f, row_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_cinema_viewport(app, render, cue, type);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::SetNextWindowPos(ImVec2(std::floor(vp->Pos.x), std::floor(vp->Pos.y) + content_h));
    ImGui::SetNextWindowSize(ImVec2(content_w, strip_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.status_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kStripPadX, kStripPadY));
    ImGui::Begin("##cinema_strip", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    draw_cinema_strip(app.cinema, cue, hud, type);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace polymesh::gui
