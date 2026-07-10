// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh desktop app: import geometry, click faces to assign fixtures and
// loads, tune mesher/solver settings, solve, and inspect stress/deflection
// results. Interwebz-v2-styled chrome with a fixed, constrained layout:
// a study sidebar (splitter-resizable) and the viewport filling the rest —
// windows cannot be dragged out of the frame, collapsed, or lost.

#include "fea/backend.hpp"
#include "fea/vtu.hpp"
#include "pipeline/scene.hpp"
#include "theme.hpp"
#include "viewport.hpp"
#include "widgets.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace polymesh::gui {

namespace fea = polymesh::fea;

// Core types live in pipeline (headless). GUI only presents them.
using pipeline::Model;
using pipeline::RegionLoad;
using pipeline::SimSetup;
using pipeline::SolveJob;
using pipeline::SolveResult;
using pipeline::VolumeMesher;
using pipeline::VolumeMeshOutput;

namespace {

constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoScrollWithMouse;

struct App {
    std::optional<Model> model;
    SimSetup setup = [] {
        // Product defaults: graded tet + light adaptive loop (η-target stop).
        SimSetup s;
        s.mesher = VolumeMesher::kGradedTet;
        s.adapt_passes = 3;
        s.eta_target = 0.12;
        s.adapt_leb_waves = 2;
        s.use_feature_grading = true; // curvature/thin-wall → h/4 near features
        s.skin_layers = 3;            // thicker fine boundary band
        s.p_elevate = true;           // geo-hp bulk quadratic + ZZ smooth elev
        return s;
    }();
    SolveJob job;
    std::optional<SolveResult> result;
    std::optional<VolumeMeshOutput> mesh_preview;
    Viewport viewport;
    DisplayMode mode = DisplayMode::kSetup;
    int selected_region = -1;
    int hovered_region = -1;
    /// Multiplier on true displacement for viewport exaggeration.
    /// After solve we set this so max |u| maps to ~12% of model diagonal
    /// (true-scale FEA deflection is often invisible). Slider re-scales from there.
    double deform_scale = 1.0;
    double deform_auto = 1.0; // last auto scale (1× true when max|u| is large)
    bool overlays_dirty = false;
    bool show_wireframe = true;
    bool show_undeformed = false;
    bool deform_true_scale = false;
    float sidebar_width = 360.0f;
    char open_path[512] = "";
    std::string status = "drop an STL/STEP on the window, or type a path below";
    std::string mesh_status;
    std::string mesh_note; // mesher note (and DOF line) after mesh/solve
    std::size_t dof_count = 0;
    float load_force[3] = {0.0f, 0.0f, -1000.0f};
    // Paths dropped via GLFW (processed on the main thread next frame).
    std::vector<std::string> pending_drops;
    // Click-vs-orbit: accumulate LMB drag so a pure click selects a face.
    float lmb_drag_px = 0.0f;
    bool pick_faces = true; // when true, LMB click assigns selection (CAD pick)
};

bool is_geometry_path(const std::string& path) {
    auto lower = path;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const auto dot = lower.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    const auto ext = lower.substr(dot);
    return ext == ".stl" || ext == ".step" || ext == ".stp";
}

void set_mesh_info(App& app, const std::string& note, std::size_t nnodes, std::size_t nelems) {
    app.dof_count = 3 * nnodes;
    app.mesh_note = note;
    app.mesh_status =
        std::format("{} | nodes {}  elems {}  DOF {}", note, nnodes, nelems, app.dof_count);
    app.status =
        std::format("mesh: {} elems, {} nodes, {} DOF", nelems, nnodes, app.dof_count);
}

void load_model(App& app, const std::string& path) {
    try {
        app.model = Model::load(path);
        // Keep mesher / adapt / material settings; only clear BCs tied to the old part.
        app.setup.fixtures.clear();
        app.setup.loads.clear();
        app.result.reset();
        app.mesh_preview.reset();
        app.mesh_status.clear();
        app.mesh_note.clear();
        app.dof_count = 0;
        app.mode = DisplayMode::kSetup;
        app.selected_region = -1;
        app.viewport.set_model(*app.model);
        app.viewport.camera.fit(app.model->bbox_min, app.model->bbox_max);
        app.overlays_dirty = true;
        std::snprintf(app.open_path, sizeof(app.open_path), "%s", path.c_str());
        app.status = std::format("{}: {} triangles, {} faces", app.model->name,
                                 app.model->surface.triangles.size(), app.model->region_count);
    } catch (const std::exception& e) {
        app.status = std::format("import failed: {}", e.what());
    }
}

void drop_callback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app == nullptr || paths == nullptr) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (paths[i] != nullptr && paths[i][0] != '\0') {
            app->pending_drops.emplace_back(paths[i]);
        }
    }
}

void draw_colorbar(const char* title, float vmin, float vmax, const char* unit) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = 18.0f;
    const float h = 140.0f;
    for (int i = 0; i < 32; ++i) {
        const float t0 = static_cast<float>(i) / 32.0f;
        const float t1 = static_cast<float>(i + 1) / 32.0f;
        // Match fea_colormap: blue→cyan→green→yellow→red
        auto col = [](float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            ImVec4 c;
            if (t < 0.25f) {
                const float u = t / 0.25f;
                c = ImVec4(0, u, 1, 1);
            } else if (t < 0.5f) {
                const float u = (t - 0.25f) / 0.25f;
                c = ImVec4(0, 1, 1 - u, 1);
            } else if (t < 0.75f) {
                const float u = (t - 0.5f) / 0.25f;
                c = ImVec4(u, 1, 0, 1);
            } else {
                const float u = (t - 0.75f) / 0.25f;
                c = ImVec4(1, 1 - u, 0, 1);
            }
            return ImGui::ColorConvertFloat4ToU32(c);
        };
        dl->AddRectFilled(ImVec2(p0.x, p0.y + h * (1.0f - t1)),
                          ImVec2(p0.x + w, p0.y + h * (1.0f - t0)), col(0.5f * (t0 + t1)));
    }
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(255, 255, 255, 80));
    ImGui::Dummy(ImVec2(w + 8, h));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", title);
    ImGui::Text("%.3g %s", static_cast<double>(vmax), unit);
    ImGui::Dummy(ImVec2(0, h - 48));
    ImGui::Text("%.3g", static_cast<double>(vmin));
    ImGui::EndGroup();
}

void draw_study_panel(App& app) {
    iw::begin_group_box("model");
    ImGui::TextColored(palette.text_dim, "drop .stl/.step/.stp on window");
    iw::input_text("path", app.open_path, sizeof(app.open_path), "path/to/part.stl|.step");
    if (iw::button("open", ImVec2(-1, 0)) && app.open_path[0] != '\0') {
        load_model(app, app.open_path);
    }
    iw::end_group_box();

    iw::begin_group_box("material");
    double e_gpa = app.setup.youngs_modulus / 1e9;
    if (iw::input_double("young's modulus (GPa)", &e_gpa, "%.1f")) {
        app.setup.youngs_modulus = e_gpa * 1e9;
    }
    iw::input_double("poisson's ratio", &app.setup.poissons_ratio, "%.3f");
    iw::end_group_box();

    iw::begin_group_box("mesh");
    double h_mm = app.setup.mesh_size * 1e3;
    if (iw::input_double("element size (mm, 0=auto)", &h_mm, "%.2f")) {
        app.setup.mesh_size = h_mm / 1e3;
    }
    {
        int m = static_cast<int>(app.setup.mesher);
        static const char* kMeshers[] = {"tet fill",   "hex fill",    "hex VEM",
                                         "graded tet", "hex+pyramid", "prism sweep"};
        if (iw::selector("mesher", &m, kMeshers, 6)) {
            app.setup.mesher = static_cast<VolumeMesher>(m);
        }
    }
    {
        // Stack label above full-width slider so ImGui's trailing label never
        // overflows the group box (PushItemWidth only sizes the frame).
        int ap = app.setup.adapt_passes;
        ImGui::TextColored(palette.text_dim, "adapt passes (0=off)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##adapt_passes", &ap, 0, 8)) {
            app.setup.adapt_passes = ap;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Max ZZ→LEB/seed-remesh loops after the first solve. Stops early if η "
                "target is met. Prefer graded tet for a posteriori seed balls.");
        }
        double eta_t = app.setup.eta_target;
        if (iw::input_double("η target (0=off)", &eta_t, "%.4g")) {
            app.setup.eta_target = eta_t < 0.0 ? 0.0 : eta_t;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Stop adapting when global ZZ η ≤ this value (energy-norm style). "
                "0 disables early stop and runs all adapt passes.");
        }
        bool fg = app.setup.use_feature_grading;
        if (iw::checkbox("feature grading", &fg)) {
            app.setup.use_feature_grading = fg;
        }
        bool pe = app.setup.p_elevate;
        if (iw::checkbox("p-elevate smooth", &pe)) {
            app.setup.p_elevate = pe;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Promote low-η tet4/hex8 → tet10/hex20 (auto when adapt>0)");
        }
        int skin = app.setup.skin_layers;
        ImGui::TextColored(palette.text_dim, "skin layers");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##skin_layers", &skin, 1, 4)) {
            app.setup.skin_layers = skin;
        }
    }
    iw::end_group_box();

    iw::begin_group_box("fixtures & loads");
    if (!app.model) {
        ImGui::TextColored(palette.text_dim, "open a model first");
    } else {
        // Face picking only works on the CAD surface (setup mode). Mesh/results
        // modes hide region colors — auto-switch when the user wants BCs.
        if (app.mode != DisplayMode::kSetup) {
            ImGui::TextColored(palette.status_warn, "switch to setup (CAD) to pick faces");
            if (iw::button("show CAD + pick faces", ImVec2(-1, 0), /*primary=*/true)) {
                app.mode = DisplayMode::kSetup;
                app.pick_faces = true;
                app.overlays_dirty = true;
            }
        } else {
            ImGui::TextColored(palette.text_dim,
                               "click a face (no drag). shift+lmb pan, wheel zoom");
            if (iw::checkbox("click-to-select faces", &app.pick_faces)) {
                /* toggle only */
            }
        }

        // Face list: works even when viewport pick is awkward (small faces).
        ImGui::TextColored(palette.text_dim, "faces (%d) — click to select",
                           app.model->region_count);
        const float list_h =
            std::clamp(18.0f * static_cast<float>(std::min(app.model->region_count, 8)) + 8.0f,
                       56.0f, 160.0f);
        if (ImGui::BeginChild("##face_list", ImVec2(-FLT_MIN, list_h),
                              ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            for (int r = 0; r < app.model->region_count; ++r) {
                const bool is_fix = app.setup.fixtures.contains(r);
                const bool is_load = app.setup.loads.contains(r);
                const char* tag = is_fix ? " [fixture]" : (is_load ? " [load]" : "");
                const bool selected = (app.selected_region == r);
                if (is_fix) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_fixture);
                } else if (is_load) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_load);
                }
                if (ImGui::Selectable(std::format("face {}{}", r, tag).c_str(), selected)) {
                    app.selected_region = r;
                    app.mode = DisplayMode::kSetup;
                    app.overlays_dirty = true;
                    if (is_load) {
                        const auto& f = app.setup.loads[r].force;
                        app.load_force[0] = static_cast<float>(f[0]);
                        app.load_force[1] = static_cast<float>(f[1]);
                        app.load_force[2] = static_cast<float>(f[2]);
                    }
                }
                if (is_fix || is_load) {
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndChild();

        if (app.selected_region >= 0) {
            ImGui::Text("selected face: %d", app.selected_region);
            const bool fixed = app.setup.fixtures.contains(app.selected_region);
            if (iw::button(fixed ? "remove fixture" : "fix face (all DOFs)", ImVec2(-1, 0))) {
                if (fixed) {
                    app.setup.fixtures.erase(app.selected_region);
                } else {
                    app.setup.fixtures.insert(app.selected_region);
                    app.setup.loads.erase(app.selected_region);
                }
                app.overlays_dirty = true;
            }
            iw::input_float3("force (N)", app.load_force);
            const bool loaded = app.setup.loads.contains(app.selected_region);
            if (iw::button(loaded ? "update load" : "apply load", ImVec2(-1, 0))) {
                app.setup.loads[app.selected_region].force =
                    Eigen::Vector3d(app.load_force[0], app.load_force[1], app.load_force[2]);
                app.setup.fixtures.erase(app.selected_region);
                app.overlays_dirty = true;
            }
            if (loaded && iw::button("remove load", ImVec2(-1, 0))) {
                app.setup.loads.erase(app.selected_region);
                app.overlays_dirty = true;
            }
        } else {
            ImGui::TextColored(palette.text_dim, "no face selected");
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(palette.sim_fixture, "fixtures: %zu", app.setup.fixtures.size());
    {
        const std::string loads_txt = std::format("loads: {}", app.setup.loads.size());
        if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(loads_txt.c_str()).x + 18.0f) {
            ImGui::SameLine(0, 18);
        }
    }
    ImGui::TextColored(palette.sim_load, "loads: %zu", app.setup.loads.size());
    if (!app.setup.fixtures.empty() || !app.setup.loads.empty()) {
        if (iw::button("clear all BCs", ImVec2(-1, 0))) {
            app.setup.fixtures.clear();
            app.setup.loads.clear();
            app.overlays_dirty = true;
        }
    }
    iw::end_group_box();

    iw::begin_group_box("mesh & solve");
    const auto state = app.job.state();
    const bool busy = state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    // Live status while worker runs (job.status_text updates from the thread).
    if (busy) {
        ImGui::TextColored(palette.status_warn, "%s", app.job.status_text().c_str());
        app.status = app.job.status_text();
    }
    ImGui::BeginDisabled(!app.model || busy);
    if (iw::button("mesh only", ImVec2(-1, 0))) {
        app.status = "meshing…";
        app.job.start_mesh(*app.model, app.setup);
    }
    if (iw::button(busy ? "working…" : "solve", ImVec2(-1, 0), /*primary=*/true)) {
        app.status = "solving…";
        app.job.start(*app.model, app.setup);
    }
    ImGui::EndDisabled();
    if (state == SolveJob::State::kFailed) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.status_err);
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        ImGui::PopStyleColor();
        if (iw::button("dismiss error", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (!busy &&
               (state != SolveJob::State::kIdle || app.result || !app.mesh_status.empty())) {
        ImGui::TextColored(palette.status_ok, "%s", app.job.status_text().c_str());
    }
    if (app.dof_count > 0) {
        ImGui::Text("DOF: %zu  (3 × nodes)", app.dof_count);
    }
    if (!app.mesh_note.empty()) {
        ImGui::TextWrapped("%s", app.mesh_note.c_str());
    } else if (!app.mesh_status.empty()) {
        ImGui::TextWrapped("%s", app.mesh_status.c_str());
    }
    iw::end_group_box();

    if (app.mesh_preview || app.result) {
        iw::begin_group_box("display");
        static const char* kModes[] = {"setup (CAD)", "mesh", "von mises", "deflection",
                                       "error η"};
        int mode = static_cast<int>(app.mode);
        if (mode < 0 || mode > 4) {
            mode = 0;
        }
        if (iw::selector("mode", &mode, kModes, 5)) {
            app.mode = static_cast<DisplayMode>(mode);
            if (app.mode == DisplayMode::kMeshPreview && !app.viewport.has_mesh_preview()) {
                app.mode = DisplayMode::kSetup;
            }
            if ((app.mode == DisplayMode::kResultsVonMises ||
                 app.mode == DisplayMode::kResultsDisplacement ||
                 app.mode == DisplayMode::kResultsError) &&
                !app.result) {
                app.mode = app.viewport.has_mesh_preview() ? DisplayMode::kMeshPreview
                                                           : DisplayMode::kSetup;
            }
        }
        iw::checkbox("wireframe edges", &app.show_wireframe);
        if (app.result) {
            iw::checkbox("undeformed outline", &app.show_undeformed);
            if (iw::checkbox("true-scale deflection", &app.deform_true_scale)) {
                app.deform_scale = app.deform_true_scale ? 1.0 : app.deform_auto;
            }
            // Range: true-scale (1) up through auto and beyond — tiny |u| needs huge ×.
            const double scale_max =
                std::max({100.0, app.deform_auto * 20.0, app.deform_scale * 2.0, 10.0});
            iw::slider_double("deformation scale", &app.deform_scale, 0.0, scale_max, "%.3gx");
            if (app.result->max_displacement > 0.0 && app.model) {
                const double diag =
                    (app.model->bbox_max - app.model->bbox_min).norm();
                const double tip_frac =
                    (app.deform_scale * app.result->max_displacement) / std::max(diag, 1e-30);
                ImGui::TextColored(palette.text_dim, "auto %.3gx → tip ~%.1f%% of model",
                                   app.deform_auto, 100.0 * tip_frac);
            }
            ImGui::Text("max von mises: %.4g MPa", app.result->max_von_mises / 1e6);
            ImGui::Text("max deflection: %.4g mm", app.result->max_displacement * 1e3);
            ImGui::Text("ZZ η global: %.4g  max nodal: %.4g", app.result->global_eta,
                        app.result->max_nodal_eta);
            ImGui::Text("nodes %zu  DOF %zu", app.result->volume_mesh.nodes.size(),
                        3 * app.result->volume_mesh.nodes.size());
            ImGui::TextWrapped("%s", app.result->mesh_note.c_str());
            if (iw::button("export VTU", ImVec2(-1, 0))) {
                try {
                    std::vector<fea::VtuPointData> pdata;
                    pdata.push_back({.name = "von_Mises",
                                     .scalars = app.result->von_mises,
                                     .vectors = {}});
                    pdata.push_back({.name = "displacement",
                                     .scalars = {},
                                     .vectors = app.result->displacement});
                    if (!app.result->nodal_eta.empty()) {
                        pdata.push_back({.name = "ZZ_eta",
                                         .scalars = app.result->nodal_eta,
                                         .vectors = {}});
                    }
                    std::vector<fea::VtuCellData> cdata;
                    cdata.push_back(
                        {.name = "quality",
                         .scalars = fea::tet4_cell_quality(app.result->volume_mesh)});
                    const std::string out =
                        app.model ? (app.model->name + "_result.vtu") : "result.vtu";
                    fea::write_vtu(out, app.result->volume_mesh, pdata, cdata);
                    app.status = std::format("wrote {}", out);
                } catch (const std::exception& e) {
                    app.status = std::format("export failed: {}", e.what());
                }
            }
        } else if (app.mesh_preview) {
            ImGui::Text("nodes %zu  elems %zu  DOF %zu", app.mesh_preview->mesh.nodes.size(),
                        app.mesh_preview->mesh.elements.size(),
                        3 * app.mesh_preview->mesh.nodes.size());
            ImGui::TextWrapped("%s", app.mesh_preview->mesher_note.c_str());
        }
        iw::end_group_box();
    }
}

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

    // Camera works whenever the cursor is over the 3D image (all display modes).
    const ImGuiIO& io = ImGui::GetIO();
    const bool mouse_over_view = io.MousePos.x >= item_min.x && io.MousePos.x <= item_max.x &&
                                 io.MousePos.y >= item_min.y && io.MousePos.y <= item_max.y;
    if (viewport_hovered || mouse_over_view) {
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

/// Fixed, constrained layout: menu bar on top, a single workspace (study |
/// splitter | viewport) filling the content band, status strip bottom. One
/// window tiles left/right so there is no multi-window gap / purple strip
/// between the tools panel and the 3D view.
void draw_frame(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    float menu_height = 0.0f;
    if (ImGui::BeginMainMenuBar()) {
        menu_height = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("file")) {
            if (ImGui::MenuItem("quit")) {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("view")) {
            if (ImGui::MenuItem("theme: interwebz", nullptr,
                                active_theme == ThemeId::kInterwebz)) {
                apply_theme(ThemeId::kInterwebz);
            }
            if (ImGui::MenuItem("theme: slate", nullptr, active_theme == ThemeId::kSlate)) {
                apply_theme(ThemeId::kSlate);
            }
            ImGui::MenuItem("wireframe edges", nullptr, &app.show_wireframe);
            if (app.result) {
                ImGui::MenuItem("undeformed outline", nullptr, &app.show_undeformed);
            }
            ImGui::EndMenu();
        }
        ImGui::TextColored(palette.text_dim, "  %s", app.status.c_str());
        ImGui::EndMainMenuBar();
    }

    constexpr float kStatusHeight = 28.0f;
    constexpr float kSplitter = 6.0f;
    // Floor positions so subpixel seams never expose glClear window_bg.
    const float content_y = std::floor(vp->Pos.y + menu_height);
    const float content_h =
        std::floor(vp->Pos.y + vp->Size.y - kStatusHeight) - content_y;
    const float content_w = std::floor(vp->Size.x);

    // Cap sidebar so the viewport is never squeezed by a huge empty-looking panel.
    const float sidebar_max = std::min(420.0f, content_w * 0.38f);
    app.sidebar_width = std::clamp(app.sidebar_width, 300.0f, std::max(300.0f, sidebar_max));
    app.sidebar_width = std::floor(app.sidebar_width);

    // Single fullscreen content window — children abut with zero gap.
    ImGui::SetNextWindowPos(ImVec2(std::floor(vp->Pos.x), content_y));
    ImGui::SetNextWindowSize(ImVec2(content_w, content_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##workspace", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float row_h = ImGui::GetContentRegionAvail().y;

    // Left: study tools (padding restored for controls).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::BeginChild("study", ImVec2(app.sidebar_width, row_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_None);
    draw_study_panel(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);

    // Drag splitter (thin hit target; accent fill only while hovered/dragged).
    ImGui::InvisibleButton("##splitter", ImVec2(kSplitter, row_h));
    if (ImGui::IsItemActive()) {
        app.sidebar_width += ImGui::GetIO().MouseDelta.x;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                                  ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(palette.accent_mid));
    }

    ImGui::SameLine(0.0f, 0.0f);

    // Right: 3D viewport fills remaining width.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("viewport", ImVec2(0.0f, row_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_viewport_content(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2); // outer padding + border

    // Status strip.
    ImGui::SetNextWindowPos(
        ImVec2(std::floor(vp->Pos.x), std::floor(vp->Pos.y + vp->Size.y - kStatusHeight)));
    ImGui::SetNextWindowSize(ImVec2(content_w, kStatusHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.status_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
    ImGui::Begin("##status", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        std::string line;
        if (app.dof_count > 0) {
            line = std::format(
                "polymesh — {} | DOF {} | lmb orbit (all modes), shift+lmb pan, wheel zoom",
                app.status, app.dof_count);
        } else {
            line = std::format(
                "polymesh — {} | drop .stl/.step or type path | lmb pick/orbit, "
                "shift+lmb pan, wheel zoom",
                app.status);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, palette.text_dim);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int run(int argc, char** argv) {
    // OpenMP + Eigen multi-thread (double-only; no fast-math).
    fea::init_runtime_performance();

    glfwSetErrorCallback([](int code, const char* text) {
        std::fprintf(stderr, "glfw error %d: %s\n", code, text);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "polymesh-gui: failed to initialize GLFW (no display?)\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(1600, 1000, "PolyMesh", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 960, 640, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // fixed layout — nothing to persist
    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, drop_callback);
    app.viewport.init();
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        load_model(app, argv[1]);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Process drag-and-drop on the main thread (paths queued by callback).
        if (!app.pending_drops.empty()) {
            std::string chosen;
            for (const auto& p : app.pending_drops) {
                if (is_geometry_path(p)) {
                    chosen = p;
                    break;
                }
            }
            if (chosen.empty()) {
                app.status = std::format("drop ignored (want .stl/.step/.stp): {}",
                                         app.pending_drops.front());
            } else {
                load_model(app, chosen);
            }
            app.pending_drops.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (auto mesh = app.job.take_mesh()) {
            app.mesh_preview = std::move(mesh);
            app.viewport.set_mesh(*app.mesh_preview);
            set_mesh_info(app, app.mesh_preview->mesher_note,
                          app.mesh_preview->mesh.nodes.size(),
                          app.mesh_preview->mesh.elements.size());
            app.mode = DisplayMode::kMeshPreview;
        }
        if (auto result = app.job.take_result()) {
            app.result = std::move(result);
            app.viewport.set_result(*app.result);
            set_mesh_info(app, app.result->mesh_note, app.result->volume_mesh.nodes.size(),
                          app.result->volume_mesh.elements.size());
            // Auto-exaggeration: map max |u| to ~12% of model diagonal so the
            // deformed shape is visible without cranking a tiny true-scale slider.
            if (app.model && app.result->max_displacement > 1e-30) {
                const double diag =
                    (app.model->bbox_max - app.model->bbox_min).norm();
                app.deform_auto = (0.12 * diag) / app.result->max_displacement;
                app.deform_auto = std::clamp(app.deform_auto, 1.0, 1e9);
            } else {
                app.deform_auto = 1.0;
            }
            app.deform_true_scale = false;
            app.deform_scale = app.deform_auto;
            app.mode = DisplayMode::kResultsVonMises;
            app.status = std::format("solved: {} elems, {} DOF, max σ_vm {:.4g} MPa",
                                     app.result->volume_mesh.elements.size(), app.dof_count,
                                     app.result->max_von_mises / 1e6);
        }

        draw_frame(app);

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(palette.window_bg.x, palette.window_bg.y, palette.window_bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

} // namespace
} // namespace polymesh::gui

int main(int argc, char** argv) { return polymesh::gui::run(argc, argv); }
