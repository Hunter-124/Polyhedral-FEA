// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh desktop app: import geometry, click faces to assign fixtures and
// loads, tune mesher/solver settings, solve, and inspect stress/deflection
// results. Interwebz-v2-styled chrome with a fixed, constrained layout:
// a study sidebar (splitter-resizable) and the viewport filling the rest —
// windows cannot be dragged out of the frame, collapsed, or lost.

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
#include <cstdio>
#include <format>
#include <optional>

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
    SimSetup setup;
    SolveJob job;
    std::optional<SolveResult> result;
    std::optional<VolumeMeshOutput> mesh_preview;
    Viewport viewport;
    DisplayMode mode = DisplayMode::kSetup;
    int selected_region = -1;
    int hovered_region = -1;
    double deform_scale = 1.0;
    bool overlays_dirty = false;
    float sidebar_width = 360.0f;
    char open_path[512] = "";
    std::string status = "open an STL/STEP or pass a path: polymesh-gui part.stl";
    std::string mesh_status;
    float load_force[3] = {0.0f, 0.0f, -1000.0f};
};

void load_model(App& app, const std::string& path) {
    try {
        app.model = Model::load(path);
        app.setup = SimSetup{};
        app.result.reset();
        app.mesh_preview.reset();
        app.mesh_status.clear();
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
    iw::begin_group_box("model", 52);
    iw::input_text("path", app.open_path, sizeof(app.open_path), "path/to/part.stl|.step");
    if (iw::button("open", ImVec2(-1, 0)) && app.open_path[0] != '\0') {
        load_model(app, app.open_path);
    }
    iw::end_group_box();

    iw::begin_group_box("material", 56);
    double e_gpa = app.setup.youngs_modulus / 1e9;
    if (iw::input_double("young's modulus (GPa)", &e_gpa, "%.1f")) {
        app.setup.youngs_modulus = e_gpa * 1e9;
    }
    iw::input_double("poisson's ratio", &app.setup.poissons_ratio, "%.3f");
    iw::end_group_box();

    iw::begin_group_box("mesh", 165);
    double h_mm = app.setup.mesh_size * 1e3;
    if (iw::input_double("element size (mm, 0=auto)", &h_mm, "%.2f")) {
        app.setup.mesh_size = h_mm / 1e3;
    }
    {
        int m = static_cast<int>(app.setup.mesher);
        static const char* kMeshers[] = {"tet fill", "hex fill", "hex VEM", "graded tet",
                                         "hex+pyramid"};
        if (iw::selector("mesher", &m, kMeshers, 5)) {
            app.setup.mesher = static_cast<VolumeMesher>(m);
        }
    }
    {
        int ap = app.setup.adapt_passes;
        if (ImGui::SliderInt("adapt passes", &ap, 0, 3)) {
            app.setup.adapt_passes = ap;
        }
        bool fg = app.setup.use_feature_grading;
        if (ImGui::Checkbox("feature grading", &fg)) {
            app.setup.use_feature_grading = fg;
        }
        int skin = app.setup.skin_layers;
        if (ImGui::SliderInt("skin layers", &skin, 1, 4)) {
            app.setup.skin_layers = skin;
        }
    }
    iw::end_group_box();

    iw::begin_group_box("fixtures & loads", 150);
    if (app.model && app.selected_region >= 0) {
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
        ImGui::TextColored(palette.text_dim, "click a face in the viewport");
    }
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::TextColored(palette.sim_fixture, "fixtures: %zu", app.setup.fixtures.size());
    ImGui::SameLine(0, 18);
    ImGui::TextColored(palette.sim_load, "loads: %zu", app.setup.loads.size());
    iw::end_group_box();

    iw::begin_group_box("mesh & solve", 110);
    const auto state = app.job.state();
    const bool busy = state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    ImGui::BeginDisabled(!app.model || busy);
    if (iw::button("mesh only", ImVec2(-1, 0))) {
        app.job.start_mesh(*app.model, app.setup);
    }
    if (iw::button(busy ? "working…" : "solve", ImVec2(-1, 26), /*primary=*/true)) {
        app.job.start(*app.model, app.setup);
    }
    ImGui::EndDisabled();
    if (state == SolveJob::State::kFailed) {
        ImGui::TextColored(palette.status_err, "%s", app.job.status_text().c_str());
        if (iw::button("dismiss error", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (state != SolveJob::State::kIdle || app.result || !app.mesh_status.empty()) {
        const ImVec4 status_color = busy ? palette.status_warn : palette.status_ok;
        ImGui::TextColored(status_color, "%s", app.job.status_text().c_str());
    }
    if (!app.mesh_status.empty()) {
        ImGui::TextWrapped("%s", app.mesh_status.c_str());
    }
    iw::end_group_box();

    if (app.mesh_preview || app.result) {
        iw::begin_group_box("display", 200);
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
        if (app.result) {
            iw::slider_double("deformation scale", &app.deform_scale, 0.0, 100.0, "%.0fx");
            ImGui::Text("max von mises: %.4g MPa", app.result->max_von_mises / 1e6);
            ImGui::Text("max deflection: %.4g mm", app.result->max_displacement * 1e3);
            ImGui::Text("ZZ η global: %.4g  max nodal: %.4g", app.result->global_eta,
                        app.result->max_nodal_eta);
            ImGui::TextColored(palette.text_dim, "%s", app.result->mesh_note.c_str());
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
                    const std::string out =
                        app.model ? (app.model->name + "_result.vtu") : "result.vtu";
                    fea::write_vtu(out, app.result->volume_mesh, pdata);
                    app.status = std::format("wrote {}", out);
                } catch (const std::exception& e) {
                    app.status = std::format("export failed: {}", e.what());
                }
            }
        } else if (app.mesh_preview) {
            ImGui::TextColored(palette.text_dim, "%s", app.mesh_preview->mesher_note.c_str());
            ImGui::Text("nodes %zu  elems %zu", app.mesh_preview->mesh.nodes.size(),
                        app.mesh_preview->mesh.elements.size());
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
                        static_cast<float>(app.deform_scale), result_max);
    ImGui::Image(static_cast<ImTextureID>(app.viewport.texture()), size, ImVec2(0, 1),
                 ImVec2(1, 0));

    // Colorbar overlay (results modes only).
    if (app.result && (app.mode == DisplayMode::kResultsVonMises ||
                       app.mode == DisplayMode::kResultsDisplacement ||
                       app.mode == DisplayMode::kResultsError)) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMin().x + 12,
                                         ImGui::GetItemRectMin().y + 12));
        ImGui::BeginChild("##cbar", ImVec2(120, 170), false,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
        if (app.mode == DisplayMode::kResultsVonMises) {
            draw_colorbar("von Mises", 0.0f, result_max, "Pa");
        } else if (app.mode == DisplayMode::kResultsDisplacement) {
            draw_colorbar("|u|", 0.0f, result_max, "m");
        } else {
            draw_colorbar("ZZ η", 0.0f, result_max, "");
        }
        ImGui::EndChild();
    }

    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const float u = (io.MousePos.x - item_min.x) / size.x;
        const float v = (io.MousePos.y - item_min.y) / size.y;
        const float aspect = size.x / size.y;

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
        if (app.model && app.mode == DisplayMode::kSetup) {
            const auto hover = app.viewport.pick_region(*app.model, u, v, aspect);
            const int hovered = hover.value_or(-1);
            if (hovered != app.hovered_region) {
                app.hovered_region = hovered;
                app.overlays_dirty = true;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift) {
                app.selected_region = hovered;
                app.overlays_dirty = true;
            }
        }
    }
}

/// Fixed, constrained layout: menu bar on top, sidebar left (splitter),
/// viewport right, status strip bottom. All windows pinned — nothing can be
/// dragged out of frame or collapsed away.
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
            ImGui::EndMenu();
        }
        ImGui::TextColored(palette.text_dim, "  %s", app.status.c_str());
        ImGui::EndMainMenuBar();
    }

    constexpr float kStatusHeight = 24.0f;
    constexpr float kSplitter = 5.0f;
    const float content_height = vp->Size.y - menu_height - kStatusHeight;
    app.sidebar_width = std::clamp(app.sidebar_width, 300.0f, vp->Size.x * 0.5f);

    // Sidebar.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(app.sidebar_width, content_height));
    ImGui::Begin("study", nullptr, kPanelFlags & ~ImGuiWindowFlags_NoScrollWithMouse);
    draw_study_panel(app);
    ImGui::End();

    // Splitter.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + app.sidebar_width, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(kSplitter, content_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##splitter", nullptr, kPanelFlags);
    ImGui::InvisibleButton("##drag", ImVec2(kSplitter, content_height));
    if (ImGui::IsItemActive()) {
        app.sidebar_width += ImGui::GetIO().MouseDelta.x;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                                  ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(palette.accent_mid));
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // Viewport.
    const float viewport_x = app.sidebar_width + kSplitter;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + viewport_x, vp->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x - viewport_x, content_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("viewport", nullptr, kPanelFlags);
    draw_viewport_content(app);
    ImGui::End();
    ImGui::PopStyleVar();

    // Status strip.
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - kStatusHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kStatusHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.status_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    ImGui::Begin("##status", nullptr, kPanelFlags);
    ImGui::TextColored(palette.text_dim,
                       "polymesh — adaptive hybrid mesher + fea | lmb pick/orbit, "
                       "shift+lmb pan, wheel zoom");
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int run(int argc, char** argv) {
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
    app.viewport.init();
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        load_model(app, argv[1]);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (auto mesh = app.job.take_mesh()) {
            app.mesh_preview = std::move(mesh);
            app.viewport.set_mesh(*app.mesh_preview);
            app.mesh_status = app.mesh_preview->mesher_note;
            app.mode = DisplayMode::kMeshPreview;
            app.status = std::format("mesh: {} elems", app.mesh_preview->mesh.elements.size());
        }
        if (auto result = app.job.take_result()) {
            app.result = std::move(result);
            app.viewport.set_result(*app.result);
            app.mesh_status = app.result->mesh_note;
            app.mode = DisplayMode::kResultsVonMises;
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

int main(int argc, char** argv) {
    return polymesh::gui::run(argc, argv);
}
