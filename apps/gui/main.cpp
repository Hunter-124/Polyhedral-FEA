// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh Studio (polymesh-gui): window, event loop and the fixed
// Test Lab | Sim Setup | viewport | Results layout. Test Lab talks to the
// harness only via docs/dag/interfaces.md file formats (no apps/testlab link).
// Usage: polymesh-gui [part.step|.brep] [--auto "<verb args>; ..."] (see
// tick_auto in automation.cpp). Env: POLYMESH_GUI_SHOT, POLYMESH_GUI_SIZE,
// POLYMESH_GUI_FONT, POLYMESH_CINEMA_STAMP.

#include "app_state.hpp"
#include "fea/backend.hpp"
#include "testlab_panel.hpp"
#include "theme.hpp"
#include "viewport.hpp"
#include "widgets.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

// On Windows, glad owns GL symbols — keep GLFW from including system gl.h.
#if defined(_WIN32)
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <glad/glad.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace polymesh::gui {

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
        app.viewport.frame_content(DisplayMode::kSetup);
        app.overlays_dirty = true;
        std::snprintf(app.open_path, sizeof(app.open_path), "%s", path.c_str());
        app.status = std::format("{}: {} triangles, {} faces", app.model->name,
                                 app.model->surface.triangles.size(), app.model->region_count);
    } catch (const std::exception& e) {
        app.status = std::format("import failed: {}", e.what());
    }
}

std::filesystem::path executable_dir;

/// Keeps `app.mode` on something that actually has geometry behind it. A stale
/// mode (results selected before a solve was cleared, mesh preview dropped by a
/// re-import, …) otherwise renders the bare background gradient over content
/// the status strip claims is loaded.
void sanitize_display_mode(App& app) {
    const bool has_result = app.result.has_value();
    const bool has_mesh = app.viewport.has_mesh_preview();
    const bool results_mode = app.mode == DisplayMode::kResultsVonMises ||
                              app.mode == DisplayMode::kResultsDisplacement ||
                              app.mode == DisplayMode::kResultsError ||
                              app.mode == DisplayMode::kResultsGradient;
    if (results_mode && !has_result) {
        app.mode = has_mesh ? DisplayMode::kMeshPreview : DisplayMode::kSetup;
    } else if (app.mode == DisplayMode::kMeshPreview && !has_mesh) {
        app.mode = DisplayMode::kSetup;
    } else if (app.mode == DisplayMode::kSetup && !app.model) {
        if (has_result) {
            app.mode = DisplayMode::kResultsVonMises;
        } else if (has_mesh) {
            app.mode = DisplayMode::kMeshPreview;
        }
    }
}

namespace {

/// Presentation-only exaggeration target. The solve stays in true SI units;
/// the viewport maps the authoritative final max |u| to exactly this fraction
/// of the undeformed model diagonal and reports both values in the film.
constexpr double kAutoDeformationFraction = 0.04;

float requested_ui_scale = 1.0f;
float ui_scale_override = 0.0f;

float window_content_scale(GLFWwindow* window) {
    if (ui_scale_override > 0.0f) {
        return ui_scale_override;
    }
    float x = 1.0f;
    float y = 1.0f;
    glfwGetWindowContentScale(window, &x, &y);
    return std::clamp(std::max(x, y), 0.75f, 3.0f);
}

void content_scale_callback(GLFWwindow* window, float, float) {
    requested_ui_scale = window_content_scale(window);
}

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
    return ext == ".step" || ext == ".stp" || ext == ".brep" || ext == ".brp" || ext == ".stl";
}

void set_mesh_info(App& app, const std::string& note, std::size_t nnodes, std::size_t nelems) {
    app.dof_count = 3 * nnodes;
    app.mesh_note = note;
    app.mesh_status =
        std::format("{} | nodes {}  elems {}  DOF {}", note, nnodes, nelems, app.dof_count);
    app.status =
        std::format("mesh: {} elems, {} nodes, {} DOF", nelems, nnodes, app.dof_count);
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

/// Guided product layout: workflow rail | canvas | optional results rail.
/// The developer Test Lab is a separate, explicitly selected surface and never
/// adds columns to the ordinary study.
void draw_frame(App& app) {
    if (app.cinema.active) {
        draw_cinema_frame(app);
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const auto job_state = app.job.state();
    const bool worker_busy =
        job_state == SolveJob::State::kMeshing || job_state == SolveJob::State::kSolving;

    float menu_height = 0.0f;
    if (ImGui::BeginMainMenuBar()) {
        menu_height = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open model…")) {
                app.workspace = WorkspaceMode::kStudy;
                app.expanded_step = 0;
                app.status = "enter a STEP, BRep, or STL path, or drop the file";
            }
            ImGui::BeginDisabled(!app.result);
            if (ImGui::MenuItem("Export result · VTU")) {
                const std::string output =
                    app.model ? (app.model->name + "_result.vtu") : "result.vtu";
                std::string error_text;
                app.status = export_result_vtu(app, output, error_text)
                                 ? std::format("wrote {}", output)
                                 : std::format("export failed: {}", error_text);
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Save screenshot", "F12")) {
                app.shot_countdown = 1;
            }
            ImGui::Separator();
            ImGui::BeginDisabled(worker_busy || app.live.active() || app.cinema.active);
            const bool developer = app.workspace == WorkspaceMode::kDeveloper;
            if (ImGui::MenuItem(developer ? "Return to Study" : "Developer Test Lab")) {
                app.workspace = developer ? WorkspaceMode::kStudy : WorkspaceMode::kDeveloper;
            }
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("How to read a study")) {
                ImGui::OpenPopup("How to read a study");
            }
            if (ImGui::MenuItem("About PolyMesh")) {
                ImGui::OpenPopup("About PolyMesh");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (ImGui::BeginPopupModal("About PolyMesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(palette.accent, "PolyMesh");
        ImGui::Text("Version %s", POLYMESH_VERSION);
        ImGui::TextUnformatted("Adaptive polyhedral finite element analysis");
        ImGui::Separator();
        ImGui::TextColored(palette.text_dim,
                           "STEP/BRep · geometry-aware meshing · linear elasticity");
        ImGui::TextColored(palette.text_dim, "A Chudware product · BSD-3-Clause");
        if (iw::button("Close", ImVec2(-1, 0), true)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(ui_px(560.0f), 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("How to read a study", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(palette.accent, "Reading PolyMesh without hiding the engineering");
        ImGui::Separator();
        iw::field_label("Fields");
        ImGui::BulletText("Stress");
        ImGui::SameLine();
        ImGui::TextWrapped("is von Mises equivalent stress: a yield-oriented scalar, not "
                           "maximum principal stress.");
        ImGui::BulletText("Deflection");
        ImGui::SameLine();
        ImGui::TextWrapped("is displacement magnitude |u|. Auto deformation magnifies the "
                           "shape only; every reported value remains physical.");
        ImGui::BulletText("Error η");
        ImGui::SameLine();
        ImGui::TextWrapped("is the Zienkiewicz–Zhu recovery estimator that drives mesh "
                           "adaptivity. It is not a probability or confidence score.");
        iw::field_label("What happens when you press Solve");
        ImGui::TextWrapped("The optional ONNX advisor scores real candidate actions under "
                           "your DOF budget. It may abstain. PolyMesh then constructs the "
                           "volume mesh, assembles one FE/VEM stiffness system, solves linear "
                           "elastostatics, recovers stress, estimates error, and refines when "
                           "requested.");
        iw::field_label("3D controls");
        ImGui::TextWrapped("Right-drag orbit · Shift+left-drag pan · wheel zoom · F frame · "
                           "F12 screenshot. In CAD mode, left-click picks a face.");
        if (iw::button("Close guide", ImVec2(-1, 0), true)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
            app.shot_countdown = 0;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            app.viewport.frame_content(app.mode);
        }
    }
    if (app.shot_msg_ttl > 0.0f) {
        app.shot_msg_ttl -= ImGui::GetIO().DeltaTime;
    }

    const float status_height =
        std::floor(std::max(ui_px(30.0f), ImGui::GetTextLineHeight() + ui_px(12.0f)));
    const float content_y = std::floor(viewport->Pos.y + menu_height);
    const float content_height =
        std::floor(viewport->Pos.y + viewport->Size.y - status_height) - content_y;
    const float content_width = std::floor(viewport->Size.x);
    const bool developer = app.workspace == WorkspaceMode::kDeveloper;

    ImGui::SetNextWindowPos(ImVec2(std::floor(viewport->Pos.x), content_y));
    ImGui::SetNextWindowSize(ImVec2(content_width, content_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##workspace", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const float row_height = ImGui::GetContentRegionAvail().y;
    const ImVec2 panel_padding(ui_px(14.0f), ui_px(14.0f));
    const float gap = ui_px(1.0f);

    if (developer) {
        const float left = std::floor((content_width - gap) * 0.46f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
        ImGui::BeginChild("testlab", ImVec2(left, row_height),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_testlab_panel(app.testlab);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, gap);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
        ImGui::BeginChild("testlab_results", ImVec2(0.0f, row_height),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_results_panel(app.testlab);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
        const bool show_results = app.mesh_preview.has_value() || app.result.has_value() ||
                                  app.live.has_advisor_content() ||
                                  app.live.has_convergence_content();
        const float left =
            std::floor(std::clamp(content_width * 0.235f, ui_px(292.0f), ui_px(372.0f)));
        // The results rail carries a five-way Field selector, a four-way camera
        // row and right-aligned statistics, so it needs more width than the
        // study rail: at 388 dp the Field row could only wrap 2+2+1 and stranded
        // "Error η" alone on a full-width row.
        float right =
            show_results
                ? std::floor(std::clamp(content_width * 0.28f, ui_px(340.0f), ui_px(460.0f)))
                : 0.0f;
        const float minimum_canvas = ui_px(300.0f);
        const float overflow =
            left + right + (show_results ? 2.0f : 1.0f) * gap + minimum_canvas - content_width;
        if (overflow > 0.0f && show_results) {
            right = std::max(ui_px(232.0f), right - overflow);
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
        ImGui::BeginChild("study", ImVec2(left, row_height),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_study_panel(app);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, gap);

        const float view_width =
            std::max(1.0f, content_width - left - (show_results ? right + 2.0f * gap : gap));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("viewport", ImVec2(view_width, row_height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        draw_viewport_content(app);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        if (show_results) {
            ImGui::SameLine(0.0f, gap);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
            // Pass the computed width instead of 0: the rail's own layout math
            // (and every full-width control in it) has to match the child it is
            // actually drawn into.
            ImGui::BeginChild("results", ImVec2(right, row_height),
                              ImGuiChildFlags_AlwaysUseWindowPadding);
            draw_analysis_panel(app);
            ImGui::EndChild();
            ImGui::PopStyleVar();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);

    ImGui::SetNextWindowPos(
        ImVec2(std::floor(viewport->Pos.x),
               std::floor(viewport->Pos.y + viewport->Size.y - status_height)));
    ImGui::SetNextWindowSize(ImVec2(content_width, status_height));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.status_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui_px(10.0f), ui_px(5.0f)));
    ImGui::Begin("##status", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    std::string status_text;
    if (app.shot_msg_ttl > 0.0f && !app.shot_msg.empty()) {
        status_text = app.shot_msg;
    } else if (developer) {
        const std::string head =
            app.testlab.git_head.empty() ? "unknown" : app.testlab.git_head;
        status_text = std::format("Developer Test Lab · {} · {}", head, app.testlab.status);
    } else if (!app.model) {
        status_text = app.status;
    } else {
        std::size_t nodes = 0;
        std::size_t elements = 0;
        if (app.result) {
            nodes = app.result->volume_mesh.nodes.size();
            elements = app.result->volume_mesh.elements.size();
        } else if (app.mesh_preview) {
            nodes = app.mesh_preview->mesh.nodes.size();
            elements = app.mesh_preview->mesh.elements.size();
        } else {
            const auto progress = app.job.progress();
            nodes = progress.n_nodes;
            elements = progress.n_elems;
        }
        status_text = app.model->name;
        if (nodes > 0 || elements > 0) {
            status_text +=
                std::format(" · {} elements · {} nodes · {} DOF", elements, nodes, nodes * 3);
        }
        if (app.live.active() && app.live.caption()[0] != '\0') {
            status_text += std::format(" · {}", app.live.caption());
        }
        const auto traces = pass_trace_snapshot(app);
        if (!traces.empty() && !traces.back().solve_method.empty()) {
            status_text += std::format(" · {}", traces.back().solve_method);
        }
    }

    const char* hint = "F frame · F12 screenshot · right-drag orbit · wheel zoom";
    const float available = ImGui::GetContentRegionAvail().x;
    const float status_width = ImGui::CalcTextSize(status_text.c_str()).x;
    const float hint_width = ImGui::CalcTextSize(hint).x;
    const ImVec2 line_origin = ImGui::GetCursorScreenPos();
    ImGui::TextColored(app.shot_msg_ttl > 0.0f && !app.shot_msg_ok ? palette.status_err
                                                                   : palette.text,
                       "%s", status_text.c_str());
    if (status_width + hint_width + ui_px(28.0f) < available) {
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(line_origin.x + available - hint_width, line_origin.y),
            ImGui::GetColorU32(palette.text_dim), hint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int run(int argc, char** argv) {

    // either order. Parsed before any GL/ImGui state exists so a bad command
    // line exits without a window to tear down.
    std::string part_path;
    std::string auto_spec;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg == nullptr || arg[0] == '\0') {
            continue;
        }
        if (std::strcmp(arg, "--auto") == 0) {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::fprintf(stderr, "polymesh-gui: --auto wants a spec argument\n");
                return 1;
            }
            auto_spec = argv[++i];
        } else if (part_path.empty()) {
            part_path = arg;
        }
    }
    {
        std::error_code ec;
        std::filesystem::path exe;
#if defined(__linux__)
        exe = std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
        if (exe.empty()) {
            ec.clear();
            exe = argc > 0 && argv[0] != nullptr ? std::filesystem::path{argv[0]}
                                                 : std::filesystem::path{};
            if (exe.is_relative()) {
                exe = std::filesystem::current_path(ec) / exe;
            }
        }
        const auto resolved = std::filesystem::weakly_canonical(exe, ec);
        executable_dir = (ec ? exe : resolved).parent_path();
    }

    int window_w = kDefaultWindowW;
    int window_h = kDefaultWindowH;
    if (const char* size_env = std::getenv("POLYMESH_GUI_SIZE");
        size_env != nullptr && size_env[0] != '\0') {
        if (!parse_window_size(size_env, window_w, window_h)) {
            std::fprintf(stderr,
                         "polymesh-gui: POLYMESH_GUI_SIZE=\"%s\" is not <width>x<height> in "
                         "960..16384 by 640..16384\n",
                         size_env);
            return 1;
        }
    }

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
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    const std::string window_title =
        std::format("PolyMesh {} — Adaptive Polyhedral FEA", POLYMESH_VERSION);
    GLFWwindow* window =
        glfwCreateWindow(window_w, window_h, window_title.c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 960, 640, GLFW_DONT_CARE, GLFW_DONT_CARE);
    set_window_icon(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
#if defined(_WIN32)
    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        std::fprintf(stderr, "glad: failed to load OpenGL\n");
        return 1;
    }
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // constrained layout — nothing to persist
    if (const char* scale_env = std::getenv("POLYMESH_GUI_SCALE");
        scale_env != nullptr && scale_env[0] != '\0') {
        char* end = nullptr;
        const float parsed = std::strtof(scale_env, &end);
        if (end == scale_env || *end != '\0' || !std::isfinite(parsed) || parsed < 0.75f ||
            parsed > 3.0f) {
            std::fprintf(stderr,
                         "polymesh-gui: POLYMESH_GUI_SCALE=\"%s\" is not in 0.75..3.0\n",
                         scale_env);
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }
        ui_scale_override = parsed;
    }
    requested_ui_scale = window_content_scale(window);
    set_ui_scale(requested_ui_scale);
    ImGui::GetIO().FontGlobalScale = 1.0f / ui_scale;
    apply_theme();
    ImFont* cinema_font = nullptr;
    ImFont* mono_font = nullptr;
    const bool ttf_loaded = load_ui_font(ui_scale, &cinema_font, &mono_font);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, drop_callback);
    glfwSetWindowContentScaleCallback(window, content_scale_callback);
    app.custom_font = ttf_loaded;
    app.cinema_font = cinema_font;
    app.mono_font = mono_font;
    if (const char* advisor_env = std::getenv("POLYMESH_ADVISOR_DIR");
        advisor_env != nullptr && advisor_env[0] != '\0') {
        app.advisor_dir = advisor_env;
    } else {
        std::error_code ec;
        const auto source_root =
            std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
        const auto cwd = std::filesystem::current_path(ec);
        std::vector<std::filesystem::path> advisor_roots{
            executable_dir / ".." / "share" / "polymesh" / "advisor",
            source_root / "bench" / "advisor",
        };
        if (!ec) {
            advisor_roots.push_back(cwd / "bench" / "advisor");
        }
        for (const auto& root : advisor_roots) {
            std::error_code model_ec;
            if (std::filesystem::is_regular_file(root / "model.onnx", model_ec)) {
                app.advisor_dir = root.string();
                break;
            }
        }
    }
    if (const char* shot_env = std::getenv("POLYMESH_GUI_SHOT");
        shot_env != nullptr && shot_env[0] != '\0') {
        app.shot_env_path = shot_env;
    }
    if (const char* stamp_env = std::getenv("POLYMESH_CINEMA_STAMP");
        stamp_env != nullptr && stamp_env[0] != '\0') {
        app.cinema_stamp = stamp_env;
    }
    app.viewport.init();
    app.testlab.cache_git_head(); // once at startup
    app.testlab.sync_buffers_from_settings();
    app.testlab.force_refresh = true;
    app.testlab.tick(0.0f);
    if (!part_path.empty()) {
        load_model(app, part_path);
    }
    AutoRunner auto_run;
    if (!auto_spec.empty()) {
        auto_run.actions = parse_auto_spec(auto_spec);
        if (auto_run.actions.empty()) {
            std::fprintf(stderr, "polymesh-gui: --auto spec has no actions\n");
            auto_run.failed = true;
            glfwSetWindowShouldClose(window, 1);
        }
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        rebuild_ui_fonts(app, requested_ui_scale);

        // Scripted step, one per frame (see AutoRunner). No-op unless --auto.
        tick_auto(auto_run, app, window);

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
                app.status = std::format("drop ignored (want .step/.stp/.brep/.brp/.stl): {}",
                                         app.pending_drops.front());
            } else {
                load_model(app, chosen);
            }
            app.pending_drops.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Poll harness process + refresh campaign files (chrono-gated inside).
        app.testlab.tick(ImGui::GetIO().DeltaTime);

        // Intermediate mesh from interactive SolveJob (after mesh / adapt remesh).
        if (auto live = app.job.poll_live_mesh(app.live_mesh_seen_gen)) {
            const bool was_mesh = app.mode == DisplayMode::kMeshPreview;
            app.mesh_preview = std::move(live);
            app.viewport.set_mesh(*app.mesh_preview);
            set_mesh_info(app, app.mesh_preview->mesher_note,
                          app.mesh_preview->mesh.nodes.size(),
                          app.mesh_preview->mesh.elements.size());
            // Show each live mesh while a replacement job is active. The
            // previous completed result remains retained and is restored if
            // this run is cancelled.
            const auto live_state = app.job.state();
            if (live_state == SolveJob::State::kMeshing ||
                live_state == SolveJob::State::kSolving || app.mode == DisplayMode::kSetup ||
                app.mode == DisplayMode::kMeshPreview) {
                app.mode = DisplayMode::kMeshPreview;
            }
            // Frame the first mesh of a run only: later adapt passes remesh the
            // same part, and refitting then would yank the user's zoom away.
            // A cinema take has already framed its own composition, and moving
            // the camera mid-recording would put a cut in the middle of a
            // continuous shot.
            if (!was_mesh && !app.cinema.active) {
                app.viewport.frame_content(DisplayMode::kMeshPreview);
            }
        }

        // Campaign harness mesh_preview.pmp (Test Lab runs). Skipped while an
        // interactive SolveJob owns the viewport. A stale campaign artifact must
        // never replace a part the user opened: only a live harness run takes
        // over, and each .pmp rewrite re-dirties this for the live preview.
        if (app.testlab.campaign_mesh_dirty && app.testlab.campaign_mesh) {
            app.testlab.campaign_mesh_dirty = false;
            const auto st = app.job.state();
            const bool job_busy =
                st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving;
            const bool may_take_view = !app.model || app.testlab.runner.is_running();
            if (!job_busy && may_take_view) {
                const auto& prev = *app.testlab.campaign_mesh;
                VolumeMeshOutput vol;
                vol.mesh.nodes.reserve(prev.nodes.size());
                for (const auto& p : prev.nodes) {
                    vol.mesh.nodes.emplace_back(p[0], p[1], p[2]);
                }
                vol.boundary_quads = prev.quads;
                vol.mesher_note = prev.note;
                const bool was_mesh = app.mode == DisplayMode::kMeshPreview;
                app.mesh_preview = std::move(vol);
                app.viewport.set_mesh(*app.mesh_preview);
                set_mesh_info(app, app.mesh_preview->mesher_note,
                              app.mesh_preview->mesh.nodes.size(), prev.n_elems);
                if (!app.result || app.mode == DisplayMode::kSetup ||
                    app.mode == DisplayMode::kMeshPreview) {
                    app.mode = DisplayMode::kMeshPreview;
                }
                // A campaign part is unrelated to anything else on screen, so
                // the camera has to move with it the first time it appears.
                if (!was_mesh) {
                    app.viewport.frame_content(DisplayMode::kMeshPreview);
                }
            }
        }

        if (auto mesh = app.job.take_mesh()) {
            detach_live_callbacks(app);
            const bool was_mesh = app.mode == DisplayMode::kMeshPreview;
            app.mesh_preview = std::move(mesh);
            app.viewport.set_mesh(*app.mesh_preview);
            set_mesh_info(app, app.mesh_preview->mesher_note,
                          app.mesh_preview->mesh.nodes.size(),
                          app.mesh_preview->mesh.elements.size());
            app.mode = DisplayMode::kMeshPreview;
            if (!was_mesh && !app.cinema.active) {
                app.viewport.frame_content(DisplayMode::kMeshPreview);
            }
        }
        if (auto result = app.job.take_result()) {
            detach_live_callbacks(app);
            if (app.cinema.active) {
                app.cinema.drain_stages();
                app.cinema.drain_solve_stages();
                app.cinema.adopt_final_result(*result);
            }
            app.result = std::move(result);
            app.viewport.set_result(*app.result);
            set_mesh_info(app, app.result->mesh_note, app.result->volume_mesh.nodes.size(),
                          app.result->volume_mesh.elements.size());
            // Auto-exaggeration is computed only after the cinema has adopted
            // this same authoritative result. The resulting screen geometry is
            // exactly x + scale*u; true and shown displacement are both reported.
            if (app.model && app.result->max_displacement > 1e-30) {
                const double diag = (app.model->bbox_max - app.model->bbox_min).norm();
                app.deform_auto =
                    (kAutoDeformationFraction * diag) / app.result->max_displacement;
                app.deform_auto = std::clamp(app.deform_auto, 1.0, 1e9);
            } else {
                app.deform_auto = 1.0;
            }
            app.deformation_view = DeformationView::kAuto;
            app.deform_scale = app.deform_auto;
            app.mode = DisplayMode::kResultsVonMises;
            app.status = std::format("solved: {} elems, {} DOF, max σ_vm {:.4g} MPa",
                                     app.result->volume_mesh.elements.size(), app.dof_count,
                                     app.result->max_von_mises / 1e6);
        }

        const auto current_job_state = app.job.state();
        if (current_job_state == SolveJob::State::kCancelled &&
            app.observed_job_state != SolveJob::State::kCancelled && app.result) {
            // A cancelled adapt/re-solve must not strand the viewport on its
            // last intermediate mesh. The last successfully taken solution is
            // still valid and remains the authoritative result.
            app.viewport.set_result(*app.result);
            set_mesh_info(app, app.result->mesh_note, app.result->volume_mesh.nodes.size(),
                          app.result->volume_mesh.elements.size());
            app.mode = DisplayMode::kResultsVonMises;
            app.status = "cancelled — showing retained solve result";
        }
        app.observed_job_state = current_job_state;

        if (app.cinema.active) {
            // Construction stages and completed solve passes both arrive on the
            // SolveJob worker thread; these are the single main-thread hand-off,
            // so the GL uploads and the draw never race the mesher or the solver.
            app.cinema.drain_stages();
            app.cinema.drain_solve_stages();
            if (app.cinema.recording()) {
                // A recording is defined by its frame COUNT, so the clock is a
                // pure function of the frame index. ImGui::GetIO().DeltaTime is
                // deliberately not consulted here: it would make the recorded
                // composition depend on how long each frame took to draw.
                app.cinema.seek_frame(app.cinema.record_next);
            } else {
                app.cinema.advance(ImGui::GetIO().DeltaTime);
            }
        } else {
            // DisplayMode::kCinema means nothing outside the cinema layout, and
            // sanitize_display_mode does not know it — so it only runs here.
            sanitize_display_mode(app);
        }
        draw_frame(app);

        ImGui::Render();
        int display_w = 0, display_h = 0;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(palette.window_bg.x, palette.window_bg.y, palette.window_bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Capture after every draw call, before the swap discards the back buffer.
        service_screenshot(app, window);
        service_auto_shot(auto_run, window);
        service_cinema_record(auto_run, app, window);
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return auto_run.failed ? 1 : 0;
}

} // namespace
} // namespace polymesh::gui

int main(int argc, char** argv) { return polymesh::gui::run(argc, argv); }
