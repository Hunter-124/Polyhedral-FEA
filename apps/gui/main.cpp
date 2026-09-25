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

namespace {

/// Presentation-only exaggeration target. The solve stays in true SI units;
/// the viewport maps the authoritative final max |u| to exactly this fraction
/// of the undeformed model diagonal and reports both values in the film.
constexpr double kAutoDeformationFraction = 0.12;

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
    return ext == ".step" || ext == ".stp" || ext == ".brep" || ext == ".brp";
}

void set_mesh_info(App& app, const std::string& note, std::size_t nnodes, std::size_t nelems) {
    app.dof_count = 3 * nnodes;
    app.mesh_note = note;
    app.mesh_status =
        std::format("{} | nodes {}  elems {}  DOF {}", note, nnodes, nelems, app.dof_count);
    app.status =
        std::format("mesh: {} elems, {} nodes, {} DOF", nelems, nnodes, app.dof_count);
}

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

/// Drag splitter between columns. Mutates `*width` by mouse delta * `sign`
/// (+1 grows left column to the right; -1 grows right column to the left).
void draw_column_splitter(const char* id, float row_h, float* width, float sign = 1.0f) {
    constexpr float kSplitter = 6.0f;
    ImGui::InvisibleButton(id, ImVec2(kSplitter, row_h));
    if (ImGui::IsItemActive()) {
        *width += sign * ImGui::GetIO().MouseDelta.x;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(),
                                                  ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(palette.accent_mid));
    }
}

/// Fixed, constrained layout: menu bar on top; workspace columns
/// Test Lab | Sim Setup | viewport | Results; status strip bottom.
/// One host window tiles children with zero gap so chrome never leaks.
///
/// `cinema on` replaces the whole thing with the cinema composition.
void draw_frame(App& app) {
    if (app.cinema.active) {
        draw_cinema_frame(app);
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    auto& gs = app.testlab.settings;

    // Theme swap must drop palette-baked GL vertex colors, or setup overlays
    // keep the previous theme's greens/reds until the next selection change.
    auto pick_theme = [&app, &gs](ThemeId id) {
        if (active_theme == id) {
            return;
        }
        apply_theme(id);
        gs.theme = id;
        app.viewport.invalidate_colors();
        app.overlays_dirty = true;
    };

    float menu_height = 0.0f;
    if (ImGui::BeginMainMenuBar()) {
        menu_height = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("file")) {
            if (ImGui::MenuItem("save screenshot (F12)")) {
                app.shot_countdown = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("quit")) {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("view")) {
            if (ImGui::MenuItem("theme: studio", nullptr, active_theme == ThemeId::kStudio)) {
                pick_theme(ThemeId::kStudio);
            }
            if (ImGui::MenuItem("theme: interwebz", nullptr,
                                active_theme == ThemeId::kInterwebz)) {
                pick_theme(ThemeId::kInterwebz);
            }
            if (ImGui::MenuItem("theme: slate", nullptr, active_theme == ThemeId::kSlate)) {
                pick_theme(ThemeId::kSlate);
            }
            ImGui::Separator();
            ImGui::MenuItem("wireframe edges", nullptr, &app.show_wireframe);
            if (app.result) {
                ImGui::MenuItem("undeformed outline", nullptr, &app.show_undeformed);
            }
            ImGui::EndMenu();
        }
        // Status text lives in the status strip — the menu bar stays file/view.
        ImGui::EndMainMenuBar();
    }

    // F12 (capture) and F (frame content) anywhere, except while a text field
    // owns the keyboard.
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

    // Tall enough for a 16 px TTF face plus the 5 px vertical window padding.
    const float status_h = std::floor(std::max(28.0f, ImGui::GetTextLineHeight() + 12.0f));
    constexpr float kSplitter = 6.0f;
    // Floor positions so subpixel seams never expose glClear window_bg.
    const float content_y = std::floor(vp->Pos.y + menu_height);
    const float content_h = std::floor(vp->Pos.y + vp->Size.y - status_h) - content_y;
    const float content_w = std::floor(vp->Size.x);

    // Clamp panel widths so the viewport keeps a usable center band.
    const float min_view = 280.0f;
    const float max_side = std::max(200.0f, (content_w - min_view - 3.0f * kSplitter) * 0.4f);
    gs.testlab_width = std::floor(std::clamp(gs.testlab_width, 200.0f, max_side));
    gs.sim_width = std::floor(std::clamp(gs.sim_width, 240.0f, max_side));
    gs.results_width = std::floor(std::clamp(gs.results_width, 200.0f, max_side));
    // If panels still overflow, shrink results then testlab then sim.
    float panels = gs.testlab_width + gs.sim_width + gs.results_width + 3.0f * kSplitter;
    if (panels + min_view > content_w) {
        const float overflow = panels + min_view - content_w;
        gs.results_width = std::max(180.0f, gs.results_width - overflow);
        panels = gs.testlab_width + gs.sim_width + gs.results_width + 3.0f * kSplitter;
        if (panels + min_view > content_w) {
            const float o2 = panels + min_view - content_w;
            gs.testlab_width = std::max(180.0f, gs.testlab_width - o2);
        }
    }

    // Single fullscreen content window — children abut with zero gap.
    ImGui::SetNextWindowPos(ImVec2(std::floor(vp->Pos.x), content_y));
    ImGui::SetNextWindowSize(ImVec2(content_w, content_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##workspace", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);

    const float row_h = ImGui::GetContentRegionAvail().y;

    // Col 1: Test Lab
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::BeginChild("testlab", ImVec2(gs.testlab_width, row_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    draw_testlab_panel(app.testlab);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);
    draw_column_splitter("##split_tl_sim", row_h, &gs.testlab_width, +1.0f);
    ImGui::SameLine(0.0f, 0.0f);

    // Col 2: Sim Setup (existing study tools)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::BeginChild("study", ImVec2(gs.sim_width, row_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    draw_study_panel(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);
    draw_column_splitter("##split_sim_vp", row_h, &gs.sim_width, +1.0f);
    ImGui::SameLine(0.0f, 0.0f);

    // Col 3: 3D viewport fills remaining width (minus results + splitter).
    const float results_band = gs.results_width + kSplitter;
    const float view_w = std::max(1.0f, ImGui::GetContentRegionAvail().x - results_band);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("viewport", ImVec2(view_w, row_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_viewport_content(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);
    // Dragging this splitter left grows the results panel.
    draw_column_splitter("##split_vp_res", row_h, &gs.results_width, -1.0f);
    ImGui::SameLine(0.0f, 0.0f);

    // Col 4: Results
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    ImGui::BeginChild("results", ImVec2(0.0f, row_h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_None);
    draw_results_panel(app.testlab);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(2); // outer padding + border

    // Status strip.
    ImGui::SetNextWindowPos(
        ImVec2(std::floor(vp->Pos.x), std::floor(vp->Pos.y + vp->Size.y - status_h)));
    ImGui::SetNextWindowSize(ImVec2(content_w, status_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.status_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
    ImGui::Begin("##status", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    {
        // One shared table (pipeline::mesher_name) so the strip, the cinema
        // HUD, the CLI and testlab cannot drift into four spellings of the
        // same enumerator.
        const std::string_view mesher = pipeline::mesher_name(app.setup.mesher);

        // Last campaign result health when results are loaded (newest row).
        std::string health_bit;
        if (!app.testlab.results.empty()) {
            const auto& last = app.testlab.results.back();
            if (last.health.present) {
                health_bit = last.health.ok ? "health ok" : "health fail";
            } else if (!last.status.empty()) {
                health_bit = last.status;
            }
        }

        const char* tl = app.testlab.status.c_str();
        const char* head =
            app.testlab.git_head.empty() ? "unknown" : app.testlab.git_head.c_str();

        // Status segments, joined with " · ".
        std::string info =
            std::format("polymesh @ {} · {} · mesher {}", head, app.status, mesher);
        if (!health_bit.empty()) {
            info += " · campaign: " + health_bit;
        }
        info += std::format(" · testlab: {}", tl);
        info += app.dof_count > 0 ? std::format(" · DOF {}", app.dof_count)
                                  : std::string(" · drop .step/.brep");
        const char* hint =
            app.dof_count > 0 ? "lmb orbit · shift+lmb pan · wheel zoom · F12 screenshot"
                              : "lmb pick/orbit · shift+lmb pan · wheel zoom · F12 screenshot";

        // Transient capture toast leads the line while it lives.
        if (app.shot_msg_ttl > 0.0f && !app.shot_msg.empty()) {
            ImGui::TextColored(app.shot_msg_ok ? palette.status_ok : palette.status_err, "%s",
                               app.shot_msg.c_str());
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(palette.text_dim, " · ");
            ImGui::SameLine(0.0f, 0.0f);
        }
        ImGui::TextColored(palette.text, "%s", info.c_str());
        ImGui::SameLine(0.0f, 0.0f);
        // Control hints are reference material — dimmed so the state reads first.
        ImGui::TextColored(palette.text_dim, " · %s", hint);
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int run(int argc, char** argv) {
    // argv: an optional positional part path, an optional --auto "<spec>", in
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
    GLFWwindow* window = glfwCreateWindow(
        window_w, window_h, "PolyMesh Studio — Adaptive Polyhedral FEA", nullptr, nullptr);
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
    ImGui::GetIO().IniFilename = nullptr; // fixed layout — nothing to persist
    apply_theme();
    ImFont* cinema_font = nullptr;
    const bool ttf_loaded = load_ui_font(&cinema_font);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, drop_callback);
    app.custom_font = ttf_loaded;
    app.cinema_font = cinema_font;
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
                app.status = std::format("drop ignored (want .step/.stp/.brep/.brp): {}",
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
            app.deform_true_scale = false;
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
