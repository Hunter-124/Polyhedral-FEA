// SPDX-License-Identifier: AGPL-3.0-or-later

// PolyMesh desktop app: import geometry, click faces to assign fixtures and
// loads, tune mesher/solver settings, solve, and inspect stress/deflection
// results — all in an AI-CAD styled shell.

#include "scene.hpp"
#include "theme.hpp"
#include "viewport.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <format>
#include <optional>

namespace polymesh::gui {
namespace {

struct App {
    std::optional<Model> model;
    SimSetup setup;
    SolveJob job;
    std::optional<SolveResult> result;
    Viewport viewport;
    DisplayMode mode = DisplayMode::kSetup;
    int selected_region = -1;
    int hovered_region = -1;
    float deform_scale = 1.0f;
    bool overlays_dirty = false;
    char open_path[512] = "";
    std::string status = "open an STL to begin";
    // Pending load edit buffer (N).
    float load_force[3] = {0.0f, 0.0f, -1000.0f};
};

void load_model(App& app, const std::string& path) {
    try {
        app.model = Model::load(path);
        app.setup = SimSetup{};
        app.result.reset();
        app.mode = DisplayMode::kSetup;
        app.selected_region = -1;
        app.viewport.set_model(*app.model);
        app.viewport.camera.fit(app.model->bbox_min, app.model->bbox_max);
        app.overlays_dirty = true;
        app.status = std::format("{}: {} triangles, {} faces", app.model->name,
                                 app.model->surface.triangles.size(), app.model->region_count);
    } catch (const std::exception& e) {
        app.status = std::format("import failed: {}", e.what());
    }
}

void draw_study_panel(App& app) {
    ImGui::Begin("Study");

    if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::InputTextWithHint("##path", "path/to/part.stl", app.open_path,
                                 sizeof(app.open_path));
        ImGui::SameLine();
        if (ImGui::Button("Open") && app.open_path[0] != '\0') {
            load_model(app, app.open_path);
        }
        if (app.model) {
            ImGui::TextColored(palette.text_dim, "%s — %d faces", app.model->name.c_str(),
                               app.model->region_count);
        }
    }

    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
        double e_gpa = app.setup.youngs_modulus / 1e9;
        if (ImGui::InputDouble("E (GPa)", &e_gpa, 0.0, 0.0, "%.1f")) {
            app.setup.youngs_modulus = e_gpa * 1e9;
        }
        ImGui::InputDouble("Poisson", &app.setup.poissons_ratio, 0.0, 0.0, "%.3f");
    }

    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        double h_mm = app.setup.mesh_size * 1e3;
        if (ImGui::InputDouble("size (mm, 0=auto)", &h_mm, 0.0, 0.0, "%.2f")) {
            app.setup.mesh_size = h_mm / 1e3;
        }
        ImGui::TextColored(palette.status_warn,
                           "draft voxel mesher v0 — adaptive mesher lands in P2/P3");
    }

    if (ImGui::CollapsingHeader("Fixtures & Loads", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (app.selected_region >= 0) {
            ImGui::Text("selected face: %d", app.selected_region);
            const bool fixed = app.setup.fixtures.contains(app.selected_region);
            if (ImGui::Button(fixed ? "Remove fixture" : "Fix face (all DOFs)")) {
                if (fixed) {
                    app.setup.fixtures.erase(app.selected_region);
                } else {
                    app.setup.fixtures.insert(app.selected_region);
                    app.setup.loads.erase(app.selected_region);
                }
                app.overlays_dirty = true;
            }
            ImGui::InputFloat3("force N", app.load_force);
            const bool loaded = app.setup.loads.contains(app.selected_region);
            if (ImGui::Button(loaded ? "Update load" : "Apply load to face")) {
                app.setup.loads[app.selected_region].force =
                    Eigen::Vector3d(app.load_force[0], app.load_force[1], app.load_force[2]);
                app.setup.fixtures.erase(app.selected_region);
                app.overlays_dirty = true;
            }
            if (loaded) {
                ImGui::SameLine();
                if (ImGui::Button("Remove load")) {
                    app.setup.loads.erase(app.selected_region);
                    app.overlays_dirty = true;
                }
            }
        } else {
            ImGui::TextColored(palette.text_dim, "click a face in the viewport");
        }
        ImGui::Separator();
        ImGui::TextColored(palette.sim_fixture, "fixtures: %zu", app.setup.fixtures.size());
        ImGui::TextColored(palette.sim_load, "loads: %zu", app.setup.loads.size());
    }

    if (ImGui::CollapsingHeader("Solve", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto state = app.job.state();
        const bool busy =
            state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
        ImGui::BeginDisabled(!app.model || busy);
        if (ImGui::Button("Solve", ImVec2(-1, 0))) {
            app.job.start(*app.model, app.setup);
        }
        ImGui::EndDisabled();
        if (busy) {
            ImGui::TextColored(palette.status_warn, "%s", app.job.status_text().c_str());
        } else if (state == SolveJob::State::kFailed) {
            ImGui::TextColored(palette.status_err, "%s", app.job.status_text().c_str());
        } else if (app.result) {
            ImGui::TextColored(palette.status_ok, "%s", app.job.status_text().c_str());
        }
    }

    if (app.result && ImGui::CollapsingHeader("Results", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = app.mode == DisplayMode::kSetup             ? 0
                   : app.mode == DisplayMode::kResultsVonMises ? 1
                                                               : 2;
        const bool changed = ImGui::RadioButton("setup", &mode, 0);
        ImGui::SameLine();
        const bool changed2 = ImGui::RadioButton("von Mises", &mode, 1);
        ImGui::SameLine();
        const bool changed3 = ImGui::RadioButton("deflection", &mode, 2);
        if (changed || changed2 || changed3) {
            app.mode = mode == 0   ? DisplayMode::kSetup
                       : mode == 1 ? DisplayMode::kResultsVonMises
                                   : DisplayMode::kResultsDisplacement;
        }
        ImGui::SliderFloat("deform x", &app.deform_scale, 0.0f, 100.0f, "%.0f");
        ImGui::TextColored(palette.text_dim, "%s", app.result->mesh_note.c_str());
        ImGui::Text("max von Mises: %.4g MPa", app.result->max_von_mises / 1e6);
        ImGui::Text("max deflection: %.4g mm", app.result->max_displacement * 1e3);
    }

    ImGui::End();
}

void draw_viewport_window(App& app) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const int width = static_cast<int>(size.x);
    const int height = static_cast<int>(size.y);

    if (app.overlays_dirty && app.model) {
        app.viewport.update_overlays(*app.model, app.setup, app.selected_region,
                                     app.hovered_region);
        app.overlays_dirty = false;
    }

    const float result_max = app.result
                                 ? (app.mode == DisplayMode::kResultsVonMises
                                        ? static_cast<float>(app.result->max_von_mises)
                                        : static_cast<float>(app.result->max_displacement))
                                 : 1.0f;
    app.viewport.render(width, height, app.mode, app.deform_scale, result_max);

    ImGui::Image(static_cast<ImTextureID>(app.viewport.texture()), size, ImVec2(0, 1),
                 ImVec2(1, 0));

    // Camera + picking interaction on the image.
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
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift &&
                !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                app.selected_region = hovered;
                app.overlays_dirty = true;
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

int run() {
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
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    app.viewport.init();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Full-window dockspace with an initial left/main split.
        const ImGuiID dock_id = ImGui::DockSpaceOverViewport(
            0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);
        static bool first = true;
        if (first) {
            first = false;
            // Default layout is created by ImGui's docking .ini on first run;
            // fall back to floating windows if the user rearranges.
            (void)dock_id;
        }

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Quit")) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
                ImGui::EndMenu();
            }
            ImGui::TextColored(palette.text_dim, "  %s", app.status.c_str());
            ImGui::EndMainMenuBar();
        }

        if (auto result = app.job.take_result()) {
            app.result = std::move(result);
            app.viewport.set_result(*app.result);
            app.mode = DisplayMode::kResultsVonMises;
        }

        draw_study_panel(app);
        draw_viewport_window(app);

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
    (void)argc;
    (void)argv;
    return polymesh::gui::run();
}
