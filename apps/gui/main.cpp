// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh Studio desktop app: import geometry, click faces to assign fixtures
// and loads, tune mesher/solver settings, solve, and inspect stress/deflection
// results. The default constrained workspace is Study setup | viewport | FEA
// Results; repository campaign/Test Lab panels are available only in the
// Developer workspace. Panels cannot be dragged out, collapsed, or lost.
// Test Lab talks to the harness only via docs/dag/interfaces.md file formats.
// F12 / File menu / POLYMESH_GUI_SHOT capture the window to a PNG.
// --auto "load p.step; h 6; fix 5; solve; wire off; shot out.png; quit" scripts
// the app end-to-end without pointer input (doc captures, agent operation).
// `savevtu out.vtu` additionally exports the solved nodal fields exactly as
// the viewport sees them, for headless GUI-vs-CLI cross-checks.
// The advisor cinema adds `spectral on|off`, `cinema on|off`,
// `cinema advisor <model dir>`, and `record <dir> <frames>`.
// `record` renders exactly <frames> frames at a fixed 1/60 s virtual timestep
// into <dir>/frame_%05d.png, so the take is independent of the real frame rate
// and reproducible headlessly. POLYMESH_CINEMA_STAMP is drawn verbatim in the
// cinema footer as the provenance line.
// The take is a fixed sequence: exact-CAD/spectral analysis, measured advisor
// passes, the mesher's stage snapshots and cell microscope, then the real
// solve/estimate/refine loop from `pipeline::SolveJob::on_solve_stage`.
// Both worker-thread sinks are installed by `cinema on` and removed by `cinema off`,
// so a studio session that never enters the cinema pays nothing for either.
// POLYMESH_GUI_SIZE=<w>x<h> sets the window (default 1600x1000).
// POLYMESH_GUI_SCALE=0.75..3 overrides GLFW monitor scale for deterministic capture.

#include "cinema.hpp"
#include "colormap.hpp"
#include "fea/backend.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/vtu.hpp"
#include "pipeline/scene.hpp"
#include "png_writer.hpp"
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
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h> // WIFEXITED / WEXITSTATUS for std::system's result
#endif

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

/// `App::improve_exit` when no self-improve run has finished this session. Not
/// 0, which is a real exit status meaning the run succeeded.
constexpr int kImproveNoRun = -1;
/// Presentation-only exaggeration target. The solve stays in true SI units;
/// the viewport maps the authoritative final max |u| to exactly this fraction
/// of the undeformed model diagonal and reports both values in the film.
constexpr double kAutoDeformationFraction = 0.04;
enum class WorkspaceMode { kStudy, kDeveloper };
enum class MeshPreset { kFast, kBalanced, kRefined, kCustom };
enum class DeformationView { kAuto, kTrueScale, kCustom };

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

/// `std::system`'s result as the command's own exit code. POSIX returns a wait
/// status, where exit 1 reads as 256; Windows returns the code directly.
int exit_code_of(int system_result) {
#ifdef _WIN32
    return system_result;
#else
    return WIFEXITED(system_result) ? WEXITSTATUS(system_result) : system_result;
#endif
}

struct App {
    std::optional<Model> model;
    SimSetup setup = [] {
        // Product defaults: graded tet + light adaptive loop (η-target stop).
        // Graded multi-level LEB: L0 bulk / L1 features / L2 high-κ. Thin parts
        // skip free-surface flood when feature grading is on; curved solve geometry is
        // default.
        SimSetup s;
        s.mesher = VolumeMesher::kGradedTet;
        s.adapt_passes = 2;
        s.eta_target = 0.12;
        s.adapt_leb_waves = 2;
        s.use_feature_grading = true; // curvature/thin-wall → L1/L2 near features
        s.skin_layers = 1;            // free-surface depth (0 on thin+feature path)
        s.p_elevate = true;           // authoritative projected quadratic CAD geometry
        return s;
    }();
    SolveJob job;
    std::optional<SolveResult> result;
    std::optional<VolumeMeshOutput> mesh_preview;
    Viewport viewport;
    DisplayMode mode = DisplayMode::kSetup;
    WorkspaceMode workspace = WorkspaceMode::kStudy;
    MeshPreset mesh_preset = MeshPreset::kBalanced;
    DeformationView deformation_view = DeformationView::kAuto;
    bool advanced_setup = false;
    int selected_region = -1;
    int hovered_region = -1;
    /// Multiplier on true displacement for viewport exaggeration.
    /// After solve we set this so max |u| maps to ~12% of model diagonal
    /// (true-scale FEA deflection is often invisible). Slider re-scales from there.
    double deform_scale = 1.0;
    double deform_auto = 1.0; // last auto scale (1× true when max|u| is large)
    bool overlays_dirty = false;
    bool show_wireframe = false;
    bool show_undeformed = false;
    char open_path[512] = "";
    std::string status = "drop a .step / .brep CAD part on the window, or type a path below";
    std::string mesh_status;
    std::string mesh_note; // mesher note (and DOF line) after mesh/solve
    std::size_t dof_count = 0;
    float load_force[3] = {0.0f, 0.0f, -1000.0f};
    // Paths dropped via GLFW (processed on the main thread next frame).
    std::vector<std::string> pending_drops;
    // Click-vs-orbit: accumulate LMB drag so a pure click selects a face.
    float lmb_drag_px = 0.0f;
    bool pick_faces = true; // when true, LMB click assigns selection (CAD pick)
    TestLabState testlab;
    /// Generation last uploaded from SolveJob::poll_live_mesh.
    std::uint64_t live_mesh_seen_gen = 0;
    /// Last worker state observed by the UI, used to restore a retained result
    /// exactly once when a replacement solve is cancelled.
    SolveJob::State observed_job_state = SolveJob::State::kIdle;
    /// True while a background self-improve (LLM) run is in flight.
    std::atomic<bool> improve_running{false};
    /// Exit status of the last finished self-improve run, or `kImproveNoRun`
    /// when none has finished this session. The script shells out, so its
    /// failure is a number nobody was reading: the launcher used to discard
    /// `std::system`'s result, which -O0 let pass and an optimised build --
    /// where _FORTIFY_SOURCE marks `system` warn_unused_result -- rejected. The
    /// worker thread cannot touch `status`, so it parks the code here and the UI
    /// reports it on a later frame.
    std::atomic<int> improve_exit{kImproveNoRun};
    /// Screenshot plumbing (png_writer.hpp). Frames still to wait before the
    /// capture, -1 = idle. F12 asks for 0 (this frame); the File menu asks for
    /// 1 so the still-drawn popup stays out of the shot. Serviced at the end of
    /// the frame, after render, before the swap.
    int shot_countdown = -1;
    /// POLYMESH_GUI_SHOT target — rewritten at most once a second while set,
    /// so a headless Xvfb run can grab a frame and then kill the app.
    std::string shot_env_path;
    double shot_env_last = -1.0e9;
    /// Transient capture toast shown in the status strip.
    std::string shot_msg;
    float shot_msg_ttl = 0.0f;
    bool shot_msg_ok = true;
    /// True when a TTF UI face loaded (else ImGui's stock bitmap font).
    bool custom_font = false;
    /// The same face again at `kCinemaAtlasSize`, for the film. ImGui
    /// rasterises one size per `ImFont` and scales the rest, so drawing a 40 px
    /// headline from the 16 px UI atlas is a 2.5x upscale of a bitmap — soft
    /// enough to read as blur in a recorded frame. Null when no TTF loaded at
    /// all, in which case the film falls back to the UI face and is legible
    /// rather than crisp.
    ImFont* cinema_font = nullptr;
    /// The activation cinema (cinema.hpp). Inert until an `--auto cinema on`:
    /// the stage sink is not installed, the clock does not run, and the studio
    /// draws exactly what it drew before this feature existed.
    CinemaState cinema;
    /// $POLYMESH_CINEMA_STAMP, read once at startup and drawn verbatim in the
    /// cinema footer. The render script supplies the git revision and the model
    /// sha256; the app never computes a provenance line of its own.
    std::string cinema_stamp;
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

// ---- screenshots ----------------------------------------------------------
// Frame capture with no new dependencies (png_writer.hpp).

/// UTC-stamped capture name, written into the process CWD.
std::string timestamped_shot_name() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "polymesh_shot_%Y%m%dT%H%M%SZ.png", &utc) == 0) {
        return "polymesh_shot.png";
    }
    return std::string(buf);
}

/// Reads the default framebuffer and writes it as an RGBA PNG. MUST run after
/// every draw call of the frame and before glfwSwapBuffers: the back buffer
/// still holds the finished image there, and the offscreen viewport FBO is
/// already unbound (Viewport::render restores 0).
bool capture_screenshot(GLFWwindow* window, const std::string& path) {
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    if (fb_w <= 0 || fb_h <= 0 || path.empty()) {
        return false;
    }
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fb_w) *
                                      static_cast<std::size_t>(fb_h) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // glReadPixels hands back rows bottom-up; the writer flips them.
    return png::write_png_rgba(path.c_str(), fb_w, fb_h, pixels.data());
}

/// Services a pending capture and records the toast text.
void service_screenshot(App& app, GLFWwindow* window) {
    if (app.shot_countdown > 0) {
        --app.shot_countdown;
    } else if (app.shot_countdown == 0) {
        app.shot_countdown = -1;
        const std::string name = timestamped_shot_name();
        app.shot_msg_ok = capture_screenshot(window, name);
        app.shot_msg = app.shot_msg_ok ? std::format("saved {}", name)
                                       : std::format("screenshot failed: {}", name);
        app.shot_msg_ttl = 4.0f;
    }
    if (!app.shot_env_path.empty()) {
        const double now = glfwGetTime();
        if (now - app.shot_env_last >= 1.0) {
            app.shot_env_last = now;
            capture_screenshot(window, app.shot_env_path);
        }
    }
}

// ---- headless automation (--auto) -----------------------------------------
// Scripted driving of the same code paths the buttons call, for documentation
// captures and agent operation where there is no pointer to click with.
//
// Exactly one action is executed per frame. The render loop has to keep
// turning between steps: `shot` reads the default framebuffer, so the new
// state must have been drawn (and the viewport FBO resolved) at least one
// full frame before the capture, and ImGui itself needs a frame to lay the
// status strip out again.

struct AutoAction {
    std::string verb;
    std::vector<std::string> args;
};

struct AutoRunner {
    std::vector<AutoAction> actions;
    std::size_t next = 0;
    /// `mesh` / `solve` hold the queue until the job settles. take_mesh() /
    /// take_result() runs later in the frame that first observes kDone, so one
    /// extra frame is burned before app.status is read back for the outcome.
    bool awaiting_solve = false;
    const char* awaiting_action = "solve";
    int settle_frames = 0;
    /// `shot` is deferred to the end of its frame: glReadPixels only sees the
    /// finished image between the last draw call and glfwSwapBuffers.
    std::string pending_shot;
    bool failed = false;

    bool enabled() const { return !actions.empty(); }
};

/// Splits "load p.step; h 6; solve" into one action per ';', whitespace-split
/// into verb + args. Empty segments are dropped, so a trailing ';' and any
/// amount of padding are harmless.
std::vector<AutoAction> parse_auto_spec(const std::string& spec) {
    std::vector<AutoAction> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t sep = spec.find(';', pos);
        const std::string seg =
            spec.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        std::vector<std::string> tok;
        for (std::size_t i = 0; i < seg.size();) {
            while (i < seg.size() && std::isspace(static_cast<unsigned char>(seg[i]))) {
                ++i;
            }
            const std::size_t start = i;
            while (i < seg.size() && !std::isspace(static_cast<unsigned char>(seg[i]))) {
                ++i;
            }
            if (i > start) {
                tok.emplace_back(seg.substr(start, i - start));
            }
        }
        if (!tok.empty()) {
            AutoAction action;
            action.verb = tok.front();
            action.args.assign(tok.begin() + 1, tok.end());
            out.push_back(std::move(action));
        }
        if (sep == std::string::npos) {
            break;
        }
        pos = sep + 1;
    }
    return out;
}

/// strtod / strtol with the whole-token check they normally lack: a typo'd
/// number in a script must fail the run, not silently mesh at h = 0.
bool parse_auto_double(const std::string& text, double& out) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
        return false;
    }
    out = v;
    return true;
}

bool parse_auto_int(const std::string& text, int& out) {
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max()) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

std::string auto_action_text(const AutoAction& action) {
    std::string text = action.verb;
    for (const auto& arg : action.args) {
        text += ' ';
        text += arg;
    }
    return text;
}

/// Writes the current solve result as VTU. Shared by the Results-panel
/// "export VTU" button and the --auto `savevtu` verb, so a headless run can
/// capture exactly the nodal field the viewport is showing.
bool export_result_vtu(const App& app, const std::string& path, std::string& err) {
    if (!app.result) {
        err = "no solve result";
        return false;
    }
    try {
        std::vector<fea::VtuPointData> pdata;
        pdata.push_back(
            {.name = "von_Mises", .scalars = app.result->von_mises, .vectors = {}});
        pdata.push_back(
            {.name = "displacement", .scalars = {}, .vectors = app.result->displacement});
        if (!app.result->nodal_eta.empty()) {
            pdata.push_back(
                {.name = "ZZ_eta", .scalars = app.result->nodal_eta, .vectors = {}});
        }
        std::vector<fea::VtuCellData> cdata;
        cdata.push_back(
            {.name = "quality", .scalars = fea::tet4_cell_quality(app.result->volume_mesh)});
        fea::write_vtu(path, app.result->volume_mesh, pdata, cdata);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

/// Rejects an `--auto` face id that is not a region of the loaded model.
///
/// `fix 99` and `loadface 99 ...` used to be accepted for any non-negative id:
/// the selection then matched nothing, the solve ran on a part with no fixture
/// or no load, and the run still exited 0. For a documentation capture — and
/// especially for a recorded video — that means publishing a picture of a load
/// case that was never applied. `region_count` is the same face set the Sim
/// Setup panel's face list iterates, so there is no second source of truth.
std::optional<std::string> bad_face_id(const App& app, const char* verb, int face) {
    if (!app.model) {
        return std::format("{}: no model is loaded — `load` a part before assigning faces",
                           verb);
    }
    if (app.model->region_count <= 0) {
        return std::format("{}: {} has no faces to assign", verb, app.model->name);
    }
    if (face < 0 || face >= app.model->region_count) {
        return std::format("{}: face {} does not exist on {} — valid face ids are 0..{}", verb,
                           face, app.model->name, app.model->region_count - 1);
    }
    return std::nullopt;
}

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

/// One measured fullscreen cinema layout. The command path uses its settled
/// viewport aspect to frame the camera before frame zero; the draw path uses the
/// same rectangles, so framing can never target a different composition.
struct CinemaLayout {
    CinemaType type;
    float content_w = 1.0f;
    float strip_h = 1.0f;
    float content_h = 1.0f;
    float panel_w = 1.0f;
    float settled_view_aspect = 1.0f;
};

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

/// Executes at most one queued action. Every exit path other than a clean
/// `quit` sets `failed`, which run() turns into a nonzero exit code.
void tick_auto(AutoRunner& run, App& app, GLFWwindow* window) {
    if (!run.enabled()) {
        return;
    }
    auto fail = [&](const std::string& why) {
        std::fprintf(stderr, "auto: %s\n", why.c_str());
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
    };

    // A recording owns the queue until its last frame is written, exactly the
    // way `solve` owns it until the job settles. Nothing else may run in
    // between: a verb that changed the setup mid-take would put two different
    // states in one video.
    if (app.cinema.recording()) {
        return;
    }

    if (run.awaiting_solve) {
        const auto st = app.job.state();
        if (st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving) {
            return;
        }
        if (run.settle_frames > 0) {
            --run.settle_frames;
            return;
        }
        run.awaiting_solve = false;
        if (st == SolveJob::State::kFailed) {
            fail(std::format("{} failed: {}", run.awaiting_action, app.job.status_text()));
            return;
        }
        if (st == SolveJob::State::kCancelled) {
            fail(std::format("{} cancelled: {}", run.awaiting_action, app.job.status_text()));
            return;
        }
        std::fprintf(stderr, "auto: %s\n", app.status.c_str());
    }
    if (run.next >= run.actions.size()) {
        return;
    }

    const AutoAction action = run.actions[run.next++];
    const std::string& verb = action.verb;
    const auto& args = action.args;
    std::fprintf(stderr, "auto: %s\n", auto_action_text(action).c_str());

    if (verb == "load") {
        if (args.size() != 1) {
            return fail("load wants one path");
        }
        // load_model swallows import errors into app.status, and a failed load
        // leaves any previously opened model in place. Pre-checking existence
        // catches the dominant scripting mistake (wrong path) exactly.
        if (!std::filesystem::exists(args[0])) {
            return fail(std::format("load: no such file: {}", args[0]));
        }
        load_model(app, args[0]);
        if (!app.model) {
            return fail(std::format("load failed: {}", app.status));
        }
    } else if (verb == "h") {
        double mm = 0.0;
        if (args.size() != 1 || !parse_auto_double(args[0], mm) || mm <= 0.0) {
            return fail("h wants one positive element size in mm");
        }
        app.setup.mesh_size = mm / 1000.0; // SimSetup::mesh_size is metres
    } else if (verb == "material") {
        double e_gpa = 0.0;
        double nu = 0.0;
        if (args.size() != 2 || !parse_auto_double(args[0], e_gpa) ||
            !parse_auto_double(args[1], nu) || e_gpa <= 0.0 || nu <= -1.0 || nu >= 0.5) {
            return fail("material wants <E_GPa> <nu>, with E > 0 and -1 < nu < 0.5");
        }
        app.setup.youngs_modulus = e_gpa * 1e9;
        app.setup.poissons_ratio = nu;
    } else if (verb == "mesher") {
        if (args.size() != 1) {
            return fail("mesher wants one canonical mesher name");
        }
        const auto mesher = pipeline::mesher_from_name(args[0]);
        if (!mesher) {
            return fail(std::format("mesher '{}' is not recognised", args[0]));
        }
        app.setup.mesher = *mesher;
    } else if (verb == "solver") {
        if (args.size() != 1) {
            return fail("solver wants auto, direct, or cg");
        }
        if (args[0] == "auto") {
            app.setup.solve_method = fea::SolveMethod::kAuto;
        } else if (args[0] == "direct") {
            app.setup.solve_method = fea::SolveMethod::kDirect;
        } else if (args[0] == "cg") {
            app.setup.solve_method = fea::SolveMethod::kCG;
        } else {
            return fail("solver wants auto, direct, or cg");
        }
    } else if (verb == "order") {
        int order = 0;
        if (args.size() != 1 || !parse_auto_int(args[0], order) ||
            (order != 1 && order != 2)) {
            return fail("order wants 1 or 2");
        }
        app.setup.p_elevate = order == 2;
    } else if (verb == "adapt") {
        int passes = 0;
        double eta_target = 0.0;
        if (args.size() != 2 || !parse_auto_int(args[0], passes) ||
            !parse_auto_double(args[1], eta_target) || passes < 0 || passes > 8 ||
            eta_target < 0.0) {
            return fail("adapt wants <passes 0..8> <eta_target >= 0>");
        }
        app.setup.adapt_passes = passes;
        app.setup.eta_target = eta_target;
    } else if (verb == "spectral") {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("spectral wants on or off");
        }
        app.setup.spectral_smooth = args[0] == "on";
    } else if (verb == "feature") {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("feature wants on or off");
        }
        app.setup.use_feature_grading = args[0] == "on";
    } else if (verb == "fix") {
        int face = -1;
        if (args.size() != 1 || !parse_auto_int(args[0], face)) {
            return fail("fix wants one face id");
        }
        if (const auto why = bad_face_id(app, "fix", face)) {
            return fail(*why);
        }
        app.setup.loads.erase(face); // a face is fixed or loaded, never both
        app.setup.fixtures.insert(face);
        app.overlays_dirty = true;
    } else if (verb == "loadface") {
        int face = -1;
        double fx = 0.0, fy = 0.0, fz = 0.0;
        if (args.size() != 4 || !parse_auto_int(args[0], face) ||
            !parse_auto_double(args[1], fx) || !parse_auto_double(args[2], fy) ||
            !parse_auto_double(args[3], fz)) {
            return fail("loadface wants <face> <fx> <fy> <fz> (newtons)");
        }
        if (const auto why = bad_face_id(app, "loadface", face)) {
            return fail(*why);
        }
        app.setup.fixtures.erase(face);
        app.setup.loads[face].force = Eigen::Vector3d(fx, fy, fz);
        app.overlays_dirty = true;
    } else if (verb == "mesh") {
        if (!args.empty()) {
            return fail("mesh takes no arguments");
        }
        if (!app.model) {
            return fail("mesh with no model loaded");
        }
        fea::set_openmp_threads(app.testlab.settings.max_threads);
        app.live_mesh_seen_gen = 0;
        app.status = "meshing…";
        app.job.start_mesh(*app.model, app.setup);
        run.awaiting_action = "mesh";
        run.awaiting_solve = true;
        run.settle_frames = 1;
    } else if (verb == "solve") {
        if (!args.empty()) {
            return fail("solve takes no arguments");
        }
        if (!app.model) {
            return fail("solve with no model loaded");
        }
        fea::set_openmp_threads(app.testlab.settings.max_threads);
        app.live_mesh_seen_gen = 0;
        app.status = "solving…";
        if (app.cinema.active) {
            prepare_cinema_features(app.cinema, *app.model, app.setup);
        }
        app.job.start(*app.model, app.setup);
        run.awaiting_action = "solve";
        run.awaiting_solve = true;
        run.settle_frames = 1;
    } else if (verb == "frame") {
        if (!args.empty()) {
            return fail("frame takes no arguments");
        }
        app.viewport.frame_content(app.mode);
    } else if (verb == "wire") {
        // The results wireframe is near-black (0.02, 0.02, 0.04) and is baked
        // from the 8x-subdivided curved boundary, so at h=6 mm on a fitted
        // camera it covers the shaded field entirely: a captured von Mises
        // view measured (6, 6, 11) per pixel with it on. Interactively that
        // is one checkbox away; a script has no checkbox, so it needs this.
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) {
            return fail("wire wants on or off");
        }
        app.show_wireframe = args[0] == "on";
    } else if (verb == "savevtu") {
        if (args.size() != 1) {
            return fail("savevtu wants one output path");
        }
        std::string err;
        if (!export_result_vtu(app, args[0], err)) {
            return fail(std::format("savevtu failed: {}", err));
        }
    } else if (verb == "shot") {
        if (args.size() != 1) {
            return fail("shot wants one output path");
        }
        run.pending_shot = args[0];
    } else if (verb == "cinema") {
        const auto st = app.job.state();
        const bool worker_busy =
            st == SolveJob::State::kMeshing || st == SolveJob::State::kSolving;
        if (args.size() == 1 && args[0] == "on") {
            // The sinks have to be installed before the worker starts, so
            // toggling them under a live job would be a data race on
            // SolveJob::on_mesh_stage / on_solve_stage. Refuse rather than race.
            if (worker_busy) {
                return fail(
                    "cinema on while a mesh/solve is running — the stage sinks must be "
                    "installed before the worker starts");
            }
            app.cinema.active = true;
            app.cinema.t = 0.0;
            app.cinema.duration = CinemaState::kDefaultDuration;
            app.cinema.clear_stages();
            app.cinema.clear_solve_stages();
            // Installed only for the take: one copies a whole NodalMesh per
            // construction stage and the other a whole SolveResult per adaptive
            // pass (1.3 MB each on this film's case), which no ordinary solve
            // should pay for.
            app.job.on_mesh_stage = [cine = &app.cinema](const pipeline::MeshStage& stage) {
                cine->push_stage(stage);
            };
            app.job.on_solve_stage = [cine = &app.cinema](const pipeline::SolveStage& stage) {
                cine->push_solve_stage(stage);
            };
            // One continuous shot. Frame once for the settled split now, then
            // again before recording after the exact result motion envelope is
            // available; neither fit occurs inside the captured take.
            app.viewport.set_camera_locked(true);
            if (app.model) {
                build_cinema_skeleton(app.cinema, *app.model, app.setup, app.viewport);
                const ImGuiViewport* main_vp = ImGui::GetMainViewport();
                const CinemaLayout layout = cinema_layout(app, *main_vp);
                app.viewport.frame_content(DisplayMode::kCinema, layout.settled_view_aspect);
            }
        } else if (args.size() == 1 && args[0] == "off") {
            if (worker_busy) {
                return fail(
                    "cinema off while a mesh/solve is running — the stage sinks cannot "
                    "be removed from under the worker");
            }
            app.cinema.active = false;
            app.viewport.set_camera_locked(false);
            app.job.on_mesh_stage = {};
            app.job.on_solve_stage = {};
            // The take's own per-pass fields were uploaded over the studio's
            // result buffers, so hand those back before the studio draws again.
            if (app.result) {
                app.viewport.set_result(*app.result);
            }
            app.cinema.invalidate_uploads();
            // Hand the viewport back to whatever the studio actually holds:
            // DisplayMode::kCinema means nothing outside the cinema layout.
            app.mode = app.result
                           ? DisplayMode::kResultsVonMises
                           : (app.viewport.has_mesh_preview() ? DisplayMode::kMeshPreview
                                                              : DisplayMode::kSetup);
        } else if (args.size() == 2 && args[0] == "advisor") {
            if (!app.model) {
                return fail("cinema advisor with no model loaded");
            }
            // A missing directory or a graph without the trunk taps is NOT a
            // scripting failure: it is a real condition the cinema is required
            // to state on screen, so the take must go on and record it.
            load_cinema_advisor(app.cinema, *app.model, app.setup, args[1]);
        } else {
            return fail("cinema wants `on`, `off`, or `advisor <model dir>`");
        }
    } else if (verb == "record") {
        if (args.size() != 2) {
            return fail("record wants an output directory and a frame count");
        }
        int frames = 0;
        if (!parse_auto_int(args[1], frames) || frames <= 0) {
            return fail("record wants a positive frame count");
        }
        if (!app.cinema.active) {
            return fail("record before `cinema on` — there is no take to record");
        }
        std::error_code ec;
        std::filesystem::create_directories(args[0], ec);
        if (!std::filesystem::is_directory(std::filesystem::path{args[0]}, ec)) {
            return fail(std::format("record: cannot create output directory {}", args[0]));
        }
        if (app.result) {
            app.viewport.set_cinema_motion_bounds(*app.result,
                                                  static_cast<float>(app.deform_scale));
            const ImGuiViewport* main_vp = ImGui::GetMainViewport();
            const CinemaLayout layout = cinema_layout(app, *main_vp);
            app.viewport.frame_content(DisplayMode::kCinema, layout.settled_view_aspect);
        }
        app.cinema.record_dir = args[0];
        app.cinema.record_frames = frames;
        app.cinema.record_next = 0;
        // The take IS the requested frames at 1/60 s, so the act schedule is
        // scaled to exactly that and to nothing else.
        app.cinema.duration = static_cast<double>(frames) * CinemaState::kRecordStep;
        app.cinema.t = 0.0;
        app.cinema.invalidate_uploads();
        // Frames are captured, not watched: waiting for the display would make
        // a 1200-frame take cost 20 s of wall clock for nothing. Restored when
        // the take ends, and on the failure path too.
        glfwSwapInterval(0);
        std::printf("cinema: take %s frames %d fps 60 duration %.4f s\n",
                    app.cinema.record_dir.c_str(), frames, app.cinema.duration);
        std::fflush(stdout);
    } else if (verb == "quit") {
        if (!args.empty()) {
            return fail("quit takes no arguments");
        }
        glfwSetWindowShouldClose(window, 1);
    } else {
        return fail(std::format("unknown action: {}", auto_action_text(action)));
    }
}

/// End-of-frame half of `shot`: the back buffer holds the finished image here
/// (same window as service_screenshot, for the same reason).
void service_auto_shot(AutoRunner& run, GLFWwindow* window) {
    if (run.pending_shot.empty()) {
        return;
    }
    const std::string path = std::move(run.pending_shot);
    run.pending_shot.clear();
    if (capture_screenshot(window, path)) {
        std::fprintf(stderr, "auto: wrote %s\n", path.c_str());
    } else {
        std::fprintf(stderr, "auto: screenshot failed: %s\n", path.c_str());
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
    }
}

/// End-of-frame half of `record`, alongside `service_auto_shot` and for the
/// same reason: the back buffer holds the finished image between the last draw
/// call and glfwSwapBuffers.
///
/// One captured frame is one 1/60 s step of the virtual clock, and the clock is
/// set from the frame INDEX (CinemaState::seek_frame) rather than accumulated
/// from ImGui's DeltaTime, so the recorded composition is a pure function of
/// the frame number: the same take renders identically on a 400 fps box and
/// inside a 6 fps software-GL Xvfb.
void service_cinema_record(AutoRunner& run, App& app, GLFWwindow* window) {
    CinemaState& cine = app.cinema;
    if (!cine.recording()) {
        return;
    }
    const int index = cine.record_next;
    const std::string path =
        (std::filesystem::path{cine.record_dir} / std::format("frame_{:05d}.png", index))
            .string();
    if (!capture_screenshot(window, path)) {
        // A dropped frame would silently shorten the video and desynchronise
        // every act boundary the manifest claims, so this fails the run.
        std::fprintf(stderr, "auto: cinema record failed to write %s\n", path.c_str());
        cine.record_dir.clear();
        glfwSwapInterval(1);
        run.failed = true;
        run.next = run.actions.size();
        glfwSetWindowShouldClose(window, 1);
        return;
    }
    cine.record_next = index + 1;

    const CinemaCue cue = cinema_cue(cine);
    // Half-second granularity: enough for the render script to show live
    // progress without burying its log in one line per frame.
    if (index == 0 || cine.record_next >= cine.record_frames || index % 30 == 0) {
        std::printf("cinema: frame %d/%d t %.4f s act %s\n", cine.record_next,
                    cine.record_frames, cine.t, cinema_act_name(cue.act));
        std::fflush(stdout);
    }
    if (cine.record_next < cine.record_frames) {
        return;
    }

    // Take complete. The act windows and the summary are what
    // scripts/render_cinema.py records in its manifest, so they are printed
    // from the same schedule the frames were drawn with.
    for (int a = 0; a < kCinemaActCount; ++a) {
        const auto act = static_cast<CinemaAct>(a);
        double t0 = 0.0;
        double t1 = 0.0;
        cinema_act_window(cine, act, t0, t1);
        const int f0 = std::min(cine.record_frames - 1,
                                static_cast<int>(std::ceil(t0 / CinemaState::kRecordStep)));
        const int f1 =
            std::min(cine.record_frames - 1,
                     static_cast<int>(std::ceil(t1 / CinemaState::kRecordStep)) - 1);
        std::printf("cinema: act %s frames %d..%d t %.4f..%.4f s\n", cinema_act_name(act), f0,
                    std::max(f0, f1), t0, t1);
    }
    std::size_t candidates = 0;
#ifdef POLYMESH_WITH_ADVISOR
    if (cine.explanation && !cine.explanation->frames.empty()) {
        // One pass per enumerated candidate, then the final re-score.
        candidates = cine.explanation->frames.size() - 1;
    }
#endif
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    // Poster only after the analysis pane has finished opening. The old
    // opening-fade index landed mid-slide with clipped panel text.
    double opening_t0 = 0.0;
    double opening_t1 = 0.0;
    cinema_act_window(cine, CinemaAct::kSkeleton, opening_t0, opening_t1);
    const double poster_t = opening_t0 + 0.22 * (opening_t1 - opening_t0);
    const int poster =
        std::min(cine.record_frames - 1,
                 static_cast<int>(std::ceil(poster_t / CinemaState::kRecordStep)));
    // The numeric tail is the manifest's compact verification record. Nodes,
    // total DOF and quality all come from the final authoritative solve stage;
    // absent data stays zero rather than being reconstructed by the script.
    std::size_t nodes = 0;
    std::size_t dof = 0;
    double quality_min = 0.0;
    double quality_mean = 0.0;
    if (!cine.solve_stages.empty()) {
        nodes = cine.solve_stages.back().trace.n_nodes;
        dof = cine.solve_stages.back().trace.n_dof;
    }
    if (!cine.solve_insights.empty()) {
        quality_min = cine.solve_insights.back().quality_min;
        quality_mean = cine.solve_insights.back().quality_mean;
    }
    for (std::size_t i = 0; i < cine.stages.size(); ++i) {
        const auto& stage = cine.stages[i];
        std::printf("cinema: mesh_stage index %zu pass %d id %s elements %zu nodes %zu\n", i,
                    stage.pass, stage.stage.c_str(), stage.mesh.elements.size(),
                    stage.mesh.nodes.size());
    }
    for (std::size_t i = 0; i < cine.solve_stages.size(); ++i) {
        const auto& stage = cine.solve_stages[i];
        std::printf("cinema: solve_stage index %zu pass %d elements %zu nodes %zu dof %zu "
                    "global_eta %.9g h_mark %zu p_mark %zu shape_mark %zu\n",
                    i, stage.pass, stage.trace.n_elems, stage.trace.n_nodes, stage.trace.n_dof,
                    stage.trace.global_eta, stage.trace.n_h_mark, stage.trace.n_p_mark,
                    stage.trace.n_shape_mark);
    }
    const double stress_p99 =
        !cine.stress_histograms.empty() ? cine.stress_histograms.back().p99 : 0.0;
    const double error_p99 =
        !cine.error_histograms.empty() ? cine.error_histograms.back().p99 : 0.0;
    const double max_displacement = app.result ? app.result->max_displacement : 0.0;
    const double max_von_mises = app.result ? app.result->max_von_mises : 0.0;
    const double global_eta = app.result ? app.result->global_eta : 0.0;
    const double model_diagonal =
        app.model ? (app.model->bbox_max - app.model->bbox_min).norm() : 0.0;
    const double visible_displacement = app.deform_scale * max_displacement;
    const double visible_fraction =
        model_diagonal > 0.0 ? visible_displacement / model_diagonal : 0.0;
    std::printf(
        "cinema: record %s frames %d fps 60 candidates %zu stages %zu elements %zu "
        "nodes %zu dof %zu quality_min %.9g quality_mean %.9g youngs_pa %.9g "
        "poisson %.9g max_von_mises_pa %.9g stress_p99_pa %.9g global_eta %.9g "
        "error_p99 %.9g max_displacement_m %.9g deform_scale %.9g "
        "visible_displacement_m %.9g visible_fraction %.9g unchanged %zu "
        "removed %zu added %zu poster %d width %d height %d skipped %zu solve_stages %zu "
        "solver %s\n",
        cine.record_dir.c_str(), cine.record_frames, candidates, cine.stages.size(),
        app.viewport.cinema_element_count(), nodes, dof, quality_min, quality_mean,
        app.setup.youngs_modulus, app.setup.poissons_ratio, max_von_mises, stress_p99,
        global_eta, error_p99, max_displacement, app.deform_scale, visible_displacement,
        visible_fraction, app.viewport.cinema_unchanged_element_count(),
        app.viewport.cinema_removed_element_count(), app.viewport.cinema_added_element_count(),
        poster, fb_w, fb_h, app.viewport.cinema_skipped_element_count(),
        cine.solve_stages.size(), cinema_solver_token(cine));
    std::fflush(stdout);
    cine.record_dir.clear();
    glfwSwapInterval(1);
}

// ---- chrome bootstrap -----------------------------------------------------

/// Fills the maths gaps in whatever face the film ended up with.
///
/// Merged rather than substituted: ImGui keeps the FIRST glyph added for a
/// codepoint, so this supplies U+2190..U+22FF only where the primary face had
/// nothing and never overrides a glyph it did have. Restricted to those two
/// blocks so the merge cannot quietly restyle Latin or Greek text either.
///
/// A box with none of these installed keeps whatever the primary face has, which
/// is the same outcome as before this existed. Either way the choice is PRINTED:
/// a glyph that silently fell back is the kind of defect that reaches a
/// committed asset and is then argued about, so the recorder's log says which
/// file supplied the maths.
void merge_maths_glyphs(ImGuiIO& io, float size) {
    static const ImWchar kMathsRanges[] = {
        0x2190, 0x21FF, // arrows (→, ⇒)
        0x2200, 0x22FF, // maths operators (∇, √, ∫, ‖-adjacent, −, ≥)
        0,
    };
    static constexpr const char* kMathsFaces[] = {
        "/usr/share/fonts/google-noto/NotoSansMath-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/gdouros-symbola/Symbola.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/cambria.ttc",
        "/System/Library/Fonts/Supplemental/Symbola.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
    };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const char* path : kMathsFaces) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            continue;
        }
        if (io.Fonts->AddFontFromFileTTF(path, size, &cfg, kMathsRanges) != nullptr) {
            std::printf("cinema: maths glyphs merged from %s\n", path);
            std::fflush(stdout);
            return;
        }
    }
    std::printf("cinema: no maths fallback face found; the equation board draws ∇ and the "
                "rest of U+2190..U+22FF from the UI face alone\n");
    std::fflush(stdout);
}

/// Loads a proportional UI face at 16 px, and the SAME file again at
/// `kCinemaAtlasSize` for the film: $POLYMESH_GUI_FONT first, then the usual
/// per-platform locations. Returns false when none exist — ImGui's stock bitmap
/// font stays in place and everything still works. Existence is checked first
/// because AddFontFromFileTTF asserts on a missing file in debug builds.
///
/// The second face is not a luxury. ImGui rasterises one pixel size per
/// `ImFont` and scales every other size from it, so the film's 40 px headline
/// drawn from the 16 px atlas is a 2.5x bitmap upscale: legible on a monitor,
/// mush once the README's GIF halves it again. `cinema_out` receives it, or
/// stays null when no TTF loaded at all, in which case the film draws from
/// whatever face is there.
///
/// The glyph range is explicit. ImGui's default range is Latin only, so the
/// labels that carry σ, ≥, ×, ⌀ or · rendered as "?" boxes — which then landed
/// in committed screenshots. Anything drawn in the UI has to be in the atlas,
/// and the equation board draws ∇, √, ‖, ∫, → and Ω out of the two blocks
/// below. Sub- and superscripts are NOT in the atlas on purpose: the board
/// composes them from scaled, raised runs of ordinary digits, because
/// Liberation Sans — the first Linux fallback here — has no U+2081 and an
/// equation full of tofu boxes is worse than no equation.
///
/// Which is also why the film's face MERGES a maths fallback over the UI face.
/// Measured on this box: Liberation Sans, Fedora's UI default and the first
/// entry below, has no U+2207 ∇ and no U+21D2 ⇒ — so the gradient equation drew
/// two tofu boxes, in a panel whose whole job is to be readable. Swapping the
/// studio's UI face for a maths font to fix a film is the wrong trade; merging
/// the missing block into the film's own face is not. ImGui keeps the FIRST
/// glyph added for a codepoint, so the merge fills gaps and never overrides a
/// glyph the UI face already has.
bool load_ui_font(float atlas_scale, ImFont** cinema_out) {
    ImGuiIO& io = ImGui::GetIO();
    // Static: ImGui keeps the pointer until the atlas is built.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Latin + Latin-1 supplement (°, µ, ±, ½, ²)
        0x0370, 0x03FF, // Greek (σ, ν, Ω, θ, η, λ, ε)
        0x2010, 0x203A, // dashes, quotes, ·, —, ‖
        0x2190, 0x21FF, // arrows (→, ⇒)
        0x2200, 0x22FF, // maths operators (∇, √, ∫, −, ≈, ≤, ≥, ×, ∞)
        0x2300, 0x2300, // ⌀ diameter sign
        0,
    };
    auto try_load = [&io, cinema_out, atlas_scale](const char* path) {
        std::error_code ec;
        if (path == nullptr || path[0] == '\0' ||
            !std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            return false;
        }
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f * atlas_scale, nullptr, kRanges) ==
            nullptr) {
            return false;
        }
        if (cinema_out != nullptr) {
            *cinema_out = io.Fonts->AddFontFromFileTTF(path, kCinemaAtlasSize * atlas_scale,
                                                       nullptr, kRanges);
            if (*cinema_out != nullptr) {
                merge_maths_glyphs(io, kCinemaAtlasSize * atlas_scale);
            }
        }
        return true;
    };
    if (try_load(std::getenv("POLYMESH_GUI_FONT"))) {
        return true;
    }
    static constexpr const char* kFallbacks[] = {
        // Linux
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        // Windows — without these the stock ASCII bitmap font was used, which
        // is what produced "?" for every symbol in the GUI screenshots.
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        // macOS
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
    };
    for (const char* path : kFallbacks) {
        if (try_load(path)) {
            return true;
        }
    }
    ImFontConfig fallback;
    fallback.SizePixels = 13.0f * atlas_scale;
    io.Fonts->AddFontDefault(&fallback);
    return false;
}

void rebuild_ui_fonts(App& app, float scale) {
    const float old_scale = ui_scale;
    if (std::abs(scale - old_scale) < 0.01f) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    set_ui_scale(scale);
    io.FontGlobalScale = 1.0f / scale;
    apply_theme(active_theme);
    app.cinema_font = nullptr;
    app.custom_font = load_ui_font(scale, &app.cinema_font);
    io.Fonts->Build();
    ImGui_ImplOpenGL3_CreateFontsTexture();

    const float ratio = scale / old_scale;
    auto& settings = app.testlab.settings;
    settings.testlab_width *= ratio;
    settings.sim_width *= ratio;
    settings.results_width *= ratio;
}

/// Default window size, and the opt-in override that lets a headless capture
/// rig record at a size it chose.
///
/// The recorder writes the framebuffer, so the take's resolution IS the window
/// size: on a 1920x1080 Xvfb screen a 1600x1000 window still produces a
/// 1600x1000 video. $POLYMESH_GUI_SIZE=<w>x<h> overrides it, in the same style
/// as $POLYMESH_GUI_SHOT — an environment variable read once at startup, so a
/// capture script can set it without the app growing another command line.
/// Unset, the window is exactly the default it has always been.
constexpr int kDefaultWindowW = 1600;
constexpr int kDefaultWindowH = 1000;

/// Parses "<w>x<h>". Returns false on anything else, including trailing junk,
/// so a typo is reported instead of silently recording at the wrong size — a
/// take mislabelled 1080p is worse than a take that never started.
bool parse_window_size(const char* text, int& width, int& height) {
    if (text == nullptr) {
        return false;
    }
    const char* sep = std::strchr(text, 'x');
    if (sep == nullptr || sep == text || sep[1] == '\0') {
        return false;
    }
    const std::string w_text(text, sep);
    int w = 0;
    int h = 0;
    if (!parse_auto_int(w_text, w) || !parse_auto_int(std::string(sep + 1), h)) {
        return false;
    }
    // Lower bound is the window's own minimum size limit; upper bound keeps a
    // fat-fingered value from asking GL for a framebuffer no driver will make.
    if (w < 960 || h < 640 || w > 16384 || h > 16384) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

/// Signed distance to a regular hexagon of circumradius `r` centered on the
/// origin (negative inside). Icon drawing only — no scene math.
float hexagon_sdf(float px, float py, float r) {
    constexpr float kx = -0.8660254f;
    constexpr float ky = 0.5f;
    constexpr float kz = 0.5773503f;
    px = std::fabs(px);
    py = std::fabs(py);
    const float fold = 2.0f * std::min(kx * px + ky * py, 0.0f);
    px -= fold * kx;
    py -= fold * ky;
    px -= std::clamp(px, -kz * r, kz * r);
    py -= r;
    return std::sqrt(px * px + py * py) * (py < 0.0f ? -1.0f : 1.0f);
}

/// Runtime-generated 64x64 window icon: a hexagonal cell stroked in the Studio
/// accent over a graphite body, fully transparent outside the cell. Procedural
/// so the app ships no image assets.
void set_window_icon(GLFWwindow* window) {
    constexpr int kSize = 64;
    constexpr float kHalf = 32.0f;
    constexpr float kOuter = 24.0f; // apothem; half-width is 24/cos30 = 27.7 px
    constexpr float kInner = 12.0f; // inner ring reads as a graded cell
    unsigned char pixels[kSize * kSize * 4];
    // Analytic one-pixel coverage from a signed distance (1 inside, 0 outside).
    auto coverage = [](float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); };
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float px = static_cast<float>(x) + 0.5f - kHalf;
            const float py = static_cast<float>(y) + 0.5f - kHalf;
            const float d_out = hexagon_sdf(px, py, kOuter);
            const float d_in = hexagon_sdf(px, py, kInner);
            const float body = coverage(d_out);
            const float ring = coverage(std::fabs(d_out) - 1.3f);          // accent stroke
            const float ring_in = coverage(std::fabs(d_in) - 0.9f) * body; // dim stroke
            float r = 0.086f, g = 0.106f, b = 0.133f;                      // #161B22 cell body
            float a = body * 0.92f;
            r = r * (1.0f - ring_in) + 0.165f * ring_in; // #2A6E96
            g = g * (1.0f - ring_in) + 0.431f * ring_in;
            b = b * (1.0f - ring_in) + 0.588f * ring_in;
            a = std::max(a, ring_in);
            r = r * (1.0f - ring) + 0.298f * ring; // #4CC2FF
            g = g * (1.0f - ring) + 0.761f * ring;
            b = b * (1.0f - ring) + 1.000f * ring;
            a = std::max(a, ring);
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(kSize) +
                 static_cast<std::size_t>(x)) *
                4u;
            pixels[i + 0] = static_cast<unsigned char>(std::lround(r * 255.0f));
            pixels[i + 1] = static_cast<unsigned char>(std::lround(g * 255.0f));
            pixels[i + 2] = static_cast<unsigned char>(std::lround(b * 255.0f));
            pixels[i + 3] =
                static_cast<unsigned char>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
    const GLFWimage image{kSize, kSize, pixels};
    glfwSetWindowIcon(window, 1, &image);
}

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

void apply_mesh_preset(App& app, MeshPreset preset) {
    app.mesh_preset = preset;
    switch (preset) {
    case MeshPreset::kFast:
        app.setup.mesher = VolumeMesher::kTetFill;
        app.setup.adapt_passes = 0;
        app.setup.eta_target = 0.0;
        app.setup.adapt_leb_waves = 0;
        app.setup.use_feature_grading = false;
        app.setup.p_elevate = false;
        app.setup.skin_layers = 1;
        break;
    case MeshPreset::kBalanced:
        app.setup.mesher = VolumeMesher::kGradedTet;
        app.setup.adapt_passes = 2;
        app.setup.eta_target = 0.12;
        app.setup.adapt_leb_waves = 2;
        app.setup.use_feature_grading = true;
        app.setup.p_elevate = true;
        app.setup.skin_layers = 1;
        break;
    case MeshPreset::kRefined:
        app.setup.mesher = VolumeMesher::kGradedTet;
        app.setup.adapt_passes = 4;
        app.setup.eta_target = 0.05;
        app.setup.adapt_leb_waves = 3;
        app.setup.use_feature_grading = true;
        app.setup.p_elevate = true;
        app.setup.skin_layers = 2;
        break;
    case MeshPreset::kCustom:
        break;
    }
}

void draw_model_group(App& app) {
    iw::begin_group_box("Model");
    ImGui::TextWrapped("Drop STEP/BRep CAD anywhere on the window");
    iw::input_text("path", app.open_path, sizeof(app.open_path), "path/to/part.step");
    if (iw::button("Open model", ImVec2(-1, 0), true) && app.open_path[0] != '\0') {
        load_model(app, app.open_path);
    }
    if (app.model) {
        ImGui::TextWrapped("%s", app.model->name.c_str());
        ImGui::TextColored(palette.text_dim, "%zu triangles · %d CAD faces",
                           app.model->surface.triangles.size(), app.model->region_count);
    }
    iw::end_group_box();
}

void draw_material_mesh_group(App& app) {
    iw::begin_group_box("Material & mesh");
    double e_gpa = app.setup.youngs_modulus / 1e9;
    if (iw::input_double("Young's modulus (GPa)", &e_gpa, "%.1f")) {
        app.setup.youngs_modulus = e_gpa * 1e9;
    }
    iw::input_double("Poisson's ratio", &app.setup.poissons_ratio, "%.3f");
    ImGui::Spacing();

    int preset = static_cast<int>(app.mesh_preset);
    static const char* kPresets[] = {"Fast", "Standard", "Fine", "Manual"};
    if (iw::selector("Mesh preset", &preset, kPresets, 4)) {
        apply_mesh_preset(app, static_cast<MeshPreset>(preset));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fast: linear tet preview, no adaptivity.\n"
                          "Standard: graded curved mesh, two adaptive passes.\n"
                          "Fine: tighter η target and four passes.\n"
                          "Manual: keep the Advanced values below.");
    }

    double h_mm = app.setup.mesh_size * 1e3;
    if (iw::input_double("Element size (mm, 0 = auto)", &h_mm, "%.2f")) {
        app.setup.mesh_size = std::max(0.0, h_mm / 1e3);
    }
    iw::checkbox("Advanced controls", &app.advanced_setup);

    if (app.advanced_setup) {
        ImGui::Separator();
        static const char* kMesherLabels[] = {
            "Graded tet", "Tet grid",   "Hex grid", "Hex + pyramid", "Prism sweep",
            "Hybrid zoo", "Hybrid VEM", "Hex VEM",  "Varyhedron",    "CVT poly",
        };
        static constexpr VolumeMesher kMesherValues[] = {
            VolumeMesher::kGradedTet,  VolumeMesher::kTetFill,    VolumeMesher::kHexFill,
            VolumeMesher::kHexPyramid, VolumeMesher::kPrismSweep, VolumeMesher::kHybrid,
            VolumeMesher::kHybridVem,  VolumeMesher::kHexVem,     VolumeMesher::kVaryhedron,
            VolumeMesher::kCvtPoly,
        };
        int mesher_index = 0;
        for (int i = 0; i < static_cast<int>(std::size(kMesherValues)); ++i) {
            if (kMesherValues[static_cast<std::size_t>(i)] == app.setup.mesher) {
                mesher_index = i;
                break;
            }
        }
        if (iw::selector("Mesher", &mesher_index, kMesherLabels,
                         static_cast<int>(std::size(kMesherLabels)))) {
            app.setup.mesher = kMesherValues[static_cast<std::size_t>(mesher_index)];
            app.mesh_preset = MeshPreset::kCustom;
        }

        int passes = app.setup.adapt_passes;
        ImGui::TextColored(palette.text_dim, "Adaptive passes");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##adapt_passes", &passes, 0, 8)) {
            app.setup.adapt_passes = passes;
            app.mesh_preset = MeshPreset::kCustom;
        }
        double eta = app.setup.eta_target;
        if (iw::input_double("ZZ η target (0 = off)", &eta, "%.4g")) {
            app.setup.eta_target = std::max(0.0, eta);
            app.mesh_preset = MeshPreset::kCustom;
        }
        bool feature = app.setup.use_feature_grading;
        if (iw::checkbox("Geometry feature grading", &feature)) {
            app.setup.use_feature_grading = feature;
            app.mesh_preset = MeshPreset::kCustom;
        }
        bool curved = app.setup.p_elevate;
        if (iw::checkbox("Curved quadratic solve geometry", &curved)) {
            app.setup.p_elevate = curved;
            app.mesh_preset = MeshPreset::kCustom;
        }
        int skin = app.setup.skin_layers;
        ImGui::TextColored(palette.text_dim, "Boundary skin layers");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##skin_layers", &skin, 1, 4)) {
            app.setup.skin_layers = skin;
            app.mesh_preset = MeshPreset::kCustom;
        }
    }
    iw::end_group_box();
}

void draw_boundary_conditions_group(App& app) {
    iw::begin_group_box("Fixtures & loads");
    if (!app.model) {
        ImGui::TextColored(palette.text_dim, "Open a model to assign CAD faces");
    } else {
        if (app.mode != DisplayMode::kSetup) {
            ImGui::TextColored(palette.status_warn, "Face editing uses the CAD view");
            if (iw::button("Show CAD and select faces", ImVec2(-1, 0), true)) {
                app.mode = DisplayMode::kSetup;
                app.pick_faces = true;
                app.overlays_dirty = true;
            }
        } else {
            app.pick_faces = true;
            ImGui::TextWrapped("Click a face · right-drag orbit · shift+left-drag pan");
        }

        ImGui::TextColored(palette.text_dim, "CAD faces");
        const float rows = static_cast<float>(std::min(app.model->region_count, 4));
        const float list_h =
            std::clamp(rows * ImGui::GetTextLineHeightWithSpacing() + ui_px(8.0f),
                       ui_px(64.0f), ui_px(120.0f));
        if (ImGui::BeginChild("##face_list", ImVec2(-FLT_MIN, list_h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            for (int region = 0; region < app.model->region_count; ++region) {
                const bool is_fixture = app.setup.fixtures.contains(region);
                const bool is_load = app.setup.loads.contains(region);
                const char* tag = is_fixture ? " · fixture" : (is_load ? " · load" : "");
                if (is_fixture) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_fixture);
                } else if (is_load) {
                    ImGui::PushStyleColor(ImGuiCol_Text, palette.sim_load);
                }
                if (ImGui::Selectable(std::format("Face {}{}", region, tag).c_str(),
                                      app.selected_region == region)) {
                    app.selected_region = region;
                    app.mode = DisplayMode::kSetup;
                    app.overlays_dirty = true;
                    if (is_load) {
                        const auto& force = app.setup.loads[region].force;
                        app.load_force[0] = static_cast<float>(force[0]);
                        app.load_force[1] = static_cast<float>(force[1]);
                        app.load_force[2] = static_cast<float>(force[2]);
                    }
                }
                if (is_fixture || is_load) {
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndChild();

        if (app.selected_region >= 0) {
            const bool fixed = app.setup.fixtures.contains(app.selected_region);
            ImGui::Text("Selected face %d", app.selected_region);
            if (iw::button(fixed ? "Remove fixture" : "Fix all DOFs", ImVec2(-1, 0))) {
                if (fixed) {
                    app.setup.fixtures.erase(app.selected_region);
                } else {
                    app.setup.fixtures.insert(app.selected_region);
                    app.setup.loads.erase(app.selected_region);
                }
                app.overlays_dirty = true;
            }
            iw::input_float3("Force (N)", app.load_force);
            const bool loaded = app.setup.loads.contains(app.selected_region);
            if (iw::button(loaded ? "Update load" : "Apply load", ImVec2(-1, 0))) {
                app.setup.loads[app.selected_region].force =
                    Eigen::Vector3d(app.load_force[0], app.load_force[1], app.load_force[2]);
                app.setup.fixtures.erase(app.selected_region);
                app.overlays_dirty = true;
            }
            if (loaded && iw::button("Remove load", ImVec2(-1, 0))) {
                app.setup.loads.erase(app.selected_region);
                app.overlays_dirty = true;
            }
        } else {
            ImGui::TextColored(palette.text_dim, "No face selected");
        }
    }

    ImGui::TextColored(palette.sim_fixture, "%zu fixtures", app.setup.fixtures.size());
    ImGui::SameLine(0.0f, ui_px(16.0f));
    ImGui::TextColored(palette.sim_load, "%zu loads", app.setup.loads.size());
    if (!app.setup.fixtures.empty() || !app.setup.loads.empty()) {
        if (iw::button("Clear boundary conditions", ImVec2(-1, 0))) {
            app.setup.fixtures.clear();
            app.setup.loads.clear();
            app.overlays_dirty = true;
        }
    }
    iw::end_group_box();
}

void draw_resources_group(App& app) {
    iw::begin_group_box("Compute limits");
    int threads = app.testlab.settings.max_threads;
    const int hardware_threads = fea::openmp_default_threads();
    ImGui::TextColored(palette.text_dim, "Threads (0 = all, hardware %d)", hardware_threads);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##max_threads", &threads, 0, std::max(1, hardware_threads))) {
        app.testlab.settings.max_threads = threads;
    }
    double memory_gb = app.testlab.settings.max_mem_gb;
    if (iw::input_double("Memory cap (GB, 0 = auto)", &memory_gb, "%.2f")) {
        app.testlab.settings.max_mem_gb = std::max(0.0, memory_gb);
    }
    app.setup.max_mem_gb = app.testlab.settings.max_mem_gb;
    const auto budget = fea::effective_memory_budget(app.setup.max_mem_gb);
    const auto cap = fea::format_memory_bytes(budget.effective_cap_bytes);
    ImGui::TextColored(palette.status_ok, "Enforced cap: %s", cap.c_str());
    ImGui::TextWrapped("%s", fea::performance_description().c_str());
    iw::end_group_box();
}

void draw_run_group(App& app) {
    iw::begin_group_box("Run");
    const auto state = app.job.state();
    const bool busy = state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    const bool paused = busy && app.job.pause_requested();

    if (busy) {
        const auto progress = app.job.progress();
        const char* phase = progress.phase.empty()
                                ? (state == SolveJob::State::kMeshing ? "mesh" : "solve")
                                : progress.phase.c_str();
        ImGui::TextColored(paused ? palette.accent : palette.status_warn, "%s%s", phase,
                           paused ? " · paused" : "");
        float overall = static_cast<float>(std::clamp(progress.phase_frac, 0.0, 1.0));
        if (progress.pass_count > 0) {
            const float span = 1.0f / static_cast<float>(progress.pass_count + 1);
            overall = std::clamp(static_cast<float>(progress.pass) * span + overall * span,
                                 0.0f, 1.0f);
        }
        ImGui::ProgressBar(overall, ImVec2(-FLT_MIN, 0),
                           std::format("{:.0f}%", 100.0 * overall).c_str());
        ImGui::TextColored(palette.text_dim, "%.1f s elapsed", progress.elapsed_ms / 1000.0);
        if (progress.cg_iter > 0) {
            ImGui::Text("CG %d · residual %.3g", progress.cg_iter, progress.cg_resid);
        }
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        app.status = app.job.status_text();
    }

    auto apply_resource_caps = [&]() {
        fea::set_openmp_threads(app.testlab.settings.max_threads);
    };
    ImGui::BeginDisabled(!app.model || busy);
    if (iw::button("Mesh preview", ImVec2(-1, 0))) {
        apply_resource_caps();
        app.live_mesh_seen_gen = 0;
        app.status = "meshing…";
        app.job.start_mesh(*app.model, app.setup);
    }
    if (iw::button(busy ? "Working…" : "Solve study", ImVec2(-1, 0), true)) {
        apply_resource_caps();
        app.live_mesh_seen_gen = 0;
        app.status = "solving…";
        if (app.cinema.active) {
            prepare_cinema_features(app.cinema, *app.model, app.setup);
        }
        app.job.start(*app.model, app.setup);
    }
    ImGui::EndDisabled();

    if (busy) {
        const float gap = ui_px(8.0f);
        const float button_width = (ImGui::GetContentRegionAvail().x - gap) * 0.5f;
        if (paused) {
            if (iw::button("Resume", ImVec2(button_width, 0), true)) {
                app.job.request_resume();
                app.status = "resuming…";
            }
        } else if (iw::button("Pause", ImVec2(button_width, 0))) {
            app.job.request_pause();
            app.status = "pause requested…";
        }
        ImGui::SameLine(0.0f, gap);
        if (iw::button("Cancel", ImVec2(button_width, 0))) {
            app.job.request_cancel();
            app.status = "cancelling…";
        }
    }

    if (state == SolveJob::State::kFailed) {
        ImGui::TextColored(palette.status_err, "%s", app.job.status_text().c_str());
        if (iw::button("Dismiss error", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (state == SolveJob::State::kCancelled) {
        ImGui::TextColored(palette.status_warn, "%s", app.job.status_text().c_str());
        if (iw::button("Dismiss cancellation", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (!busy && state != SolveJob::State::kIdle) {
        ImGui::TextColored(palette.status_ok, "%s", app.job.status_text().c_str());
    }
    iw::end_group_box();
}

void draw_diagnostics_group(App& app) {
    iw::begin_group_box("Developer diagnostics");
    const auto progress = app.job.progress();
    if (progress.n_elems > 0 && progress.elapsed_ms > 0.0) {
        const double elements_per_second =
            static_cast<double>(progress.n_elems) / (progress.elapsed_ms / 1000.0);
        ImGui::Text("Throughput %.0f elements/s", elements_per_second);
    }
    const bool running = app.improve_running.load();
    const int improve_exit = app.improve_exit.load();
    if (running) {
        ImGui::TextColored(palette.status_warn, "Self-improve running");
    } else if (improve_exit == kImproveNoRun) {
        ImGui::TextColored(palette.text_dim, "Self-improve idle");
    } else if (improve_exit == 0) {
        ImGui::TextColored(palette.status_ok, "Last self-improve completed");
    } else {
        ImGui::TextColored(palette.status_err, "Last self-improve failed (exit %d)",
                           improve_exit);
    }
    auto launch_improve = [&app](const char* backend) {
        if (app.improve_running.exchange(true)) {
            return;
        }
        const std::string command =
            std::string("bash scripts/self_improve.sh --backend ") + backend;
        std::atomic<bool>* running_flag = &app.improve_running;
        std::atomic<int>* exit_code = &app.improve_exit;
        std::thread([running_flag, exit_code, command] {
            exit_code->store(exit_code_of(std::system(command.c_str())));
            running_flag->store(false);
        }).detach();
        app.status = std::string("self-improve (") + backend + ") launched";
    };
    ImGui::BeginDisabled(running);
    if (iw::button("Run with OMP", ImVec2(-1, 0))) {
        launch_improve("omp");
    }
    if (iw::button("Run with Grok", ImVec2(-1, 0))) {
        launch_improve("grok");
    }
    ImGui::EndDisabled();
    iw::end_group_box();
}

void draw_study_panel(App& app) {
    draw_model_group(app);
    draw_material_mesh_group(app);
    draw_run_group(app);
    draw_boundary_conditions_group(app);
    if (app.advanced_setup) {
        draw_resources_group(app);
    }
    if (app.workspace == WorkspaceMode::kDeveloper) {
        draw_diagnostics_group(app);
    }
}

void draw_analysis_panel(App& app) {
    iw::begin_group_box("Study status");
    if (!app.model) {
        ImGui::TextColored(palette.text_dim, "1  Open STEP/BRep CAD");
        ImGui::TextColored(palette.text_dim, "2  Select fixture and load faces");
        ImGui::TextColored(palette.text_dim, "3  Mesh preview or solve");
    } else {
        ImGui::TextWrapped("%s", app.model->name.c_str());
        ImGui::TextColored(palette.text_dim, "%zu fixtures · %zu loads",
                           app.setup.fixtures.size(), app.setup.loads.size());
        if (app.dof_count > 0) {
            ImGui::Text("%zu DOF", app.dof_count);
        }
        const auto state = app.job.state();
        if (state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving) {
            ImGui::TextWrapped("%s", app.job.status_text().c_str());
        } else if (app.result) {
            ImGui::TextColored(palette.status_ok, "Solved");
        } else if (app.mesh_preview) {
            ImGui::TextColored(palette.status_ok, "Mesh preview ready");
        } else {
            ImGui::TextColored(palette.status_ok, "Ready");
        }
    }
    iw::end_group_box();

    if (app.mesh_preview || app.result) {
        iw::begin_group_box("Display");
        static const char* kModes[] = {"CAD", "Mesh", "Stress", "Deflection", "Error η"};
        int mode = std::clamp(static_cast<int>(app.mode), 0, 4);
        if (iw::selector("Field", &mode, kModes, 5)) {
            app.mode = static_cast<DisplayMode>(mode);
            sanitize_display_mode(app);
        }
        iw::checkbox("Wireframe edges", &app.show_wireframe);
        if (app.result) {
            iw::checkbox("Undeformed outline", &app.show_undeformed);
            static const char* kDeformationModes[] = {"Auto", "True 1×", "Custom"};
            int deformation = static_cast<int>(app.deformation_view);
            if (iw::selector("Deformation", &deformation, kDeformationModes, 3)) {
                app.deformation_view = static_cast<DeformationView>(deformation);
                if (app.deformation_view == DeformationView::kAuto) {
                    app.deform_scale = app.deform_auto;
                } else if (app.deformation_view == DeformationView::kTrueScale) {
                    app.deform_scale = 1.0;
                }
            }
            if (app.deformation_view == DeformationView::kCustom) {
                const double scale_max =
                    std::max({100.0, app.deform_auto * 20.0, app.deform_scale * 2.0});
                iw::slider_double("Deformation scale", &app.deform_scale, 0.0, scale_max,
                                  "%.3gx");
            }
            ImGui::TextColored(palette.text_dim, "Auto scale %.3gx", app.deform_auto);
        }
        iw::end_group_box();
    }

    iw::begin_group_box("Results");
    if (app.result) {
        ImGui::Text("von Mises max");
        ImGui::TextColored(palette.accent, "%.4g MPa", app.result->max_von_mises / 1e6);
        ImGui::Text("Deflection max");
        ImGui::TextColored(palette.accent, "%.4g mm", app.result->max_displacement * 1e3);
        ImGui::Text("ZZ error");
        ImGui::TextColored(palette.accent, "global %.4g · nodal max %.4g",
                           app.result->global_eta, app.result->max_nodal_eta);
        ImGui::TextColored(palette.text_dim, "%zu nodes · %zu elements",
                           app.result->volume_mesh.nodes.size(),
                           app.result->volume_mesh.elements.size());
        if (iw::button("Export result VTU", ImVec2(-1, 0), true)) {
            const std::string output =
                app.model ? (app.model->name + "_result.vtu") : "result.vtu";
            std::string error;
            app.status = export_result_vtu(app, output, error)
                             ? std::format("wrote {}", output)
                             : std::format("export failed: {}", error);
        }
    } else if (app.mesh_preview) {
        ImGui::TextColored(palette.accent, "Mesh preview ready");
        ImGui::Text("%zu nodes", app.mesh_preview->mesh.nodes.size());
        ImGui::Text("%zu elements", app.mesh_preview->mesh.elements.size());
        ImGui::Text("%zu DOF", 3 * app.mesh_preview->mesh.nodes.size());
        ImGui::TextColored(palette.text_dim, "%s",
                           pipeline::mesher_name(app.setup.mesher).data());
    } else {
        ImGui::TextWrapped("Mesh statistics and solved fields will appear here.");
    }
    iw::end_group_box();
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
    const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
    const int framebuffer_w =
        std::max(1, static_cast<int>(std::lround(size.x * framebuffer_scale.x)));
    const int framebuffer_h =
        std::max(1, static_cast<int>(std::lround(size.y * framebuffer_scale.y)));
    app.viewport.render(framebuffer_w, framebuffer_h, app.mode,
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

/// The cinema's viewport: the offscreen render as a full-bleed image. No
/// colorbar, no frame button, no face picking — nothing that would have to be
/// cropped out of a recorded frame. Camera drag still works so a take can be
/// composed interactively; a recording never touches the mouse, so a recorded
/// frame is unaffected either way.
///
/// The mode, the exaggeration and the colour-scale maximum all come from
/// `cinema_render`, not from the studio's own sliders: the closing act shows one
/// adaptive pass's field at a time and ramps the load factor, and neither is
/// something the studio state knows about.
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
    const double phase_x = cue.solve_phase_t / std::max(cue.solve_phase_span, 1.0e-9);
    const auto smooth = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return static_cast<float>(x * x * (3.0 - 2.0 * x));
    };
    const bool final_stage =
        cue.solve_stage_index >= 0 &&
        static_cast<std::size_t>(cue.solve_stage_index + 1) >= app.cinema.solve_stages.size();
    float mechanics_visibility = 0.0f;
    switch (cue.solve_phase) {
    case SolvePhase::kStressHold:
        mechanics_visibility = smooth((phase_x - 0.62) / 0.28);
        break;
    case SolvePhase::kGradientSweep:
        mechanics_visibility = 1.0f;
        break;
    case SolvePhase::kGradientHold:
        mechanics_visibility = 1.0f - smooth((phase_x - 0.72) / 0.23);
        break;
    case SolvePhase::kErrorHold:
        mechanics_visibility = final_stage ? smooth((phase_x - 0.60) / 0.30) : 0.0f;
        break;
    case SolvePhase::kLoadRamp:
    case SolvePhase::kHold:
        mechanics_visibility = 1.0f;
        break;
    default:
        break;
    }
    const float mechanics_alpha = 0.90f * mechanics_visibility;
    const float mechanics_pulse =
        0.5f + 0.5f * std::sin(static_cast<float>(cue.solve_phase_t) * 5.2f);

    const auto convex_footprint = [&](const CinemaMechanicsMarker& marker) {
        std::vector<ImVec2> points;
        points.reserve(marker.surface_points.size());
        for (const auto& world : marker.surface_points) {
            if (const auto p =
                    project_cinema_point(app.viewport.camera, world, image_min, image_size)) {
                points.push_back(*p);
            }
        }
        std::sort(points.begin(), points.end(), [](const ImVec2& a, const ImVec2& b) {
            return a.x < b.x || (a.x == b.x && a.y < b.y);
        });
        points.erase(std::unique(points.begin(), points.end(),
                                 [](const ImVec2& a, const ImVec2& b) {
                                     return a.x == b.x && a.y == b.y;
                                 }),
                     points.end());
        if (points.size() < 3) {
            return points;
        }
        const auto cross = [](const ImVec2& a, const ImVec2& b, const ImVec2& c) {
            return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        };
        std::vector<ImVec2> hull;
        hull.reserve(2 * points.size());
        for (const ImVec2& point : points) {
            while (hull.size() >= 2 &&
                   cross(hull[hull.size() - 2], hull.back(), point) <= 0.0f) {
                hull.pop_back();
            }
            hull.push_back(point);
        }
        const std::size_t lower = hull.size();
        for (std::size_t i = points.size() - 1; i-- > 0;) {
            const ImVec2& point = points[i];
            while (hull.size() > lower &&
                   cross(hull[hull.size() - 2], hull.back(), point) <= 0.0f) {
                hull.pop_back();
            }
            hull.push_back(point);
        }
        if (!hull.empty()) {
            hull.pop_back();
        }
        return hull;
    };
    const auto draw_vector = [&](const Eigen::Vector3d& tail_world,
                                 const Eigen::Vector3d& head_world, const ImVec4& c,
                                 float alpha, const std::string& label, float label_side,
                                 float label_t) {
        const auto tail =
            project_cinema_point(app.viewport.camera, tail_world, image_min, image_size);
        const auto head =
            project_cinema_point(app.viewport.camera, head_world, image_min, image_size);
        if (!tail || !head) {
            return;
        }
        const ImVec2 delta(head->x - tail->x, head->y - tail->y);
        const float n = std::hypot(delta.x, delta.y);
        if (!(n > 2.0f)) {
            return;
        }
        const ImVec2 unit(delta.x / n, delta.y / n);
        const ImVec2 normal(-unit.y, unit.x);
        dl->AddLine(*tail, *head, color(c, (0.13f + 0.12f * mechanics_pulse) * alpha),
                    9.0f + 2.0f * mechanics_pulse);
        dl->AddLine(*tail, *head, color(c, alpha), 3.5f);
        const ImVec2 wing_a(head->x - 18.0f * unit.x + 9.0f * normal.x,
                            head->y - 18.0f * unit.y + 9.0f * normal.y);
        const ImVec2 wing_b(head->x - 18.0f * unit.x - 9.0f * normal.x,
                            head->y - 18.0f * unit.y - 9.0f * normal.y);
        dl->AddTriangleFilled(*head, wing_a, wing_b, color(c, alpha));
        if (label.empty()) {
            return;
        }
        const ImVec2 label_anchor(tail->x + label_t * delta.x, tail->y + label_t * delta.y);
        const float label_w = font->CalcTextSizeA(type.label, FLT_MAX, 0.0f, label.c_str()).x;
        const ImVec2 desired(label_anchor.x + label_side * 30.0f * normal.x - 0.5f * label_w,
                             label_anchor.y + label_side * 30.0f * normal.y -
                                 0.5f * type.label);
        const ImVec2 label_at = label_position(label, type.label, desired, label_anchor.x);
        dl->AddText(font, type.label, label_at, color(c, alpha), label.c_str());
    };

    Eigen::Vector3d total_load = Eigen::Vector3d::Zero();
    for (const auto& marker : app.cinema.load_markers) {
        total_load += marker.vector;
    }
    const double load_norm = std::max(total_load.norm(), 1.0);
    const double force_scale =
        cue.solve_phase == SolvePhase::kLoadRamp ? std::clamp(cue.load_factor, 0.0, 1.0) : 1.0;

    for (const auto& marker : app.cinema.support_markers) {
        const ImVec4 c = palette.sim_fixture;
        const std::vector<ImVec2> hull = convex_footprint(marker);
        if (hull.size() >= 3) {
            dl->AddConvexPolyFilled(hull.data(), static_cast<int>(hull.size()),
                                    color(c, 0.12f * mechanics_alpha));
            dl->AddPolyline(hull.data(), static_cast<int>(hull.size()),
                            color(c, mechanics_alpha), ImDrawFlags_Closed, 3.0f);
            ImVec2 centroid(0.0f, 0.0f);
            for (const ImVec2& point : hull) {
                centroid.x += point.x;
                centroid.y += point.y;
            }
            centroid.x /= static_cast<float>(hull.size());
            centroid.y /= static_cast<float>(hull.size());
            const std::size_t stride = std::max<std::size_t>(1, hull.size() / 6);
            for (std::size_t i = 0; i < hull.size(); i += stride) {
                const ImVec2 delta(hull[i].x - centroid.x, hull[i].y - centroid.y);
                const float length = std::max(std::hypot(delta.x, delta.y), 1.0f);
                const ImVec2 tick(hull[i].x + 8.0f * delta.x / length,
                                  hull[i].y + 8.0f * delta.y / length);
                dl->AddLine(hull[i], tick, color(c, 0.78f * mechanics_alpha), 2.0f);
            }
        } else if (const auto anchor = project_cinema_point(
                       app.viewport.camera,
                       displayed_marker_position(app.cinema, marker, render), image_min,
                       image_size)) {
            dl->AddCircle(*anchor, 13.0f, color(c, mechanics_alpha), 0, 3.0f);
        }

        const double reaction_magnitude = marker.reaction.norm();
        const double scaled_magnitude = force_scale * reaction_magnitude;
        if (!(scaled_magnitude > 1.0e-4)) {
            continue;
        }
        const Eigen::Vector3d tail = displayed_marker_position(app.cinema, marker, render);
        const double relative =
            std::clamp(std::sqrt(reaction_magnitude / load_norm), 0.0, 1.15);
        const double length =
            0.15 * force_scale * relative * std::max(app.cinema.model_diagonal, 1.0e-6);
        const Eigen::Vector3d head = tail + length * marker.reaction.normalized();
        const std::string label = reaction_magnitude / load_norm >= 0.02
                                      ? std::format("R  {:.3g} kN", scaled_magnitude / 1e3)
                                      : std::string{};
        draw_vector(tail, head, c, mechanics_alpha, label, 1.0f, 0.78f);
    }

    for (const auto& marker : app.cinema.load_markers) {
        if (!(marker.vector.norm() > 0.0) || !(force_scale > 1.0e-4)) {
            continue;
        }
        const Eigen::Vector3d head = displayed_marker_position(app.cinema, marker, render);
        const Eigen::Vector3d direction = marker.vector.normalized();
        const double length = 0.17 * force_scale * std::max(app.cinema.model_diagonal, 1.0e-6);
        const Eigen::Vector3d tail = head - length * direction;
        draw_vector(tail, head, palette.sim_load, mechanics_alpha,
                    std::format("F  {:.3g} kN", force_scale * marker.vector.norm() / 1e3),
                    -1.0f, 0.20f);
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
        }
    }
}

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
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        app.viewport.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
    }
    if (io.MouseWheel != 0.0f) {
        app.viewport.camera.dolly(io.MouseWheel);
    }
}

/// Fullscreen cinema layout: the panel left, the viewport right, the caption
/// strip along the bottom. No menu bar and no status strip, so a recorded frame
/// is the finished composition and `scripts/render_cinema.py` never has to crop.
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
    // The opening act belongs to the part, so the split OPENS with the panel
    // rather than standing empty beside it: the viewport has the whole window
    // while the panel is dark, and the panel slides in from the left as it fades
    // up. `CinemaCue::panel_open` drives the width and rises exactly once,
    // during the opening. Nothing later in the take may move it: the panel's
    // CONTENT swaps from the network to the equation board in the closing act,
    // and that is a dissolve inside a pane whose geometry never changes.
    //
    // The panel is always laid out at its FINAL width and translated, never
    // laid out narrow. Its wrapping, label gutter and column spacing are then
    // identical on every frame of the take, so a frame caught mid-slide cannot
    // clip something the settled layout fits.
    //
    // The part neither moves with the pane nor changes size. The projection
    // fixes the VERTICAL field (m(1,1) = 1/tan(fov/2); m(0,0) is that over the
    // aspect) and the pane height never changes, so the world-to-pixel scale is
    // invariant to the pane WIDTH in both axes. Only the pane's centre
    // translates, continuously, on the same smoothstep — a pan, not a cut.
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

/// Drag splitter between columns. Mutates `*width` by mouse delta * `sign`
/// (+1 grows left column to the right; -1 grows right column to the left).
void draw_column_splitter(const char* id, float row_h, float* width, float sign = 1.0f) {
    const float splitter = ui_px(6.0f);
    ImGui::InvisibleButton(id, ImVec2(splitter, row_h));
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
/// `cinema on` replaces the whole thing with the cinema composition; with
/// cinema off nothing below this branch changes.
void draw_frame(App& app) {
    if (app.cinema.active) {
        draw_cinema_frame(app);
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    auto& settings = app.testlab.settings;

    auto pick_theme = [&app, &settings](ThemeId id) {
        if (active_theme == id) {
            return;
        }
        apply_theme(id);
        settings.theme = id;
        app.viewport.invalidate_colors();
        app.overlays_dirty = true;
    };

    float menu_height = 0.0f;
    if (ImGui::BeginMainMenuBar()) {
        menu_height = ImGui::GetWindowSize().y;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save screenshot", "F12")) {
                app.shot_countdown = 1;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Workspace")) {
            if (ImGui::MenuItem("Study", nullptr, app.workspace == WorkspaceMode::kStudy)) {
                app.workspace = WorkspaceMode::kStudy;
            }
            if (ImGui::MenuItem("Developer / Test Lab", nullptr,
                                app.workspace == WorkspaceMode::kDeveloper)) {
                app.workspace = WorkspaceMode::kDeveloper;
                app.advanced_setup = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Studio theme", nullptr, active_theme == ThemeId::kStudio)) {
                pick_theme(ThemeId::kStudio);
            }
            if (ImGui::MenuItem("Interwebz theme", nullptr,
                                active_theme == ThemeId::kInterwebz)) {
                pick_theme(ThemeId::kInterwebz);
            }
            if (ImGui::MenuItem("Slate theme", nullptr, active_theme == ThemeId::kSlate)) {
                pick_theme(ThemeId::kSlate);
            }
            ImGui::Separator();
            ImGui::MenuItem("Advanced setup", nullptr, &app.advanced_setup);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About PolyMesh")) {
                ImGui::OpenPopup("About PolyMesh");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (ImGui::BeginPopupModal("About PolyMesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(palette.accent, "PolyMesh Studio");
        ImGui::Text("Version %s", POLYMESH_VERSION);
        ImGui::TextUnformatted("Adaptive polyhedral finite element analysis");
        ImGui::Separator();
        ImGui::TextColored(palette.text_dim,
                           "STEP/BRep · geometry-aware meshing · linear elasticity");
        ImGui::TextColored(palette.text_dim, "BSD-3-Clause");
        if (iw::button("Close", ImVec2(-1, 0), true)) {
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
    const float splitter = ui_px(6.0f);
    const float content_y = std::floor(viewport->Pos.y + menu_height);
    const float content_height =
        std::floor(viewport->Pos.y + viewport->Size.y - status_height) - content_y;
    const float content_width = std::floor(viewport->Size.x);
    const bool developer = app.workspace == WorkspaceMode::kDeveloper;

    if (developer) {
        const float min_view = ui_px(300.0f);
        const float max_side =
            std::max(ui_px(190.0f), (content_width - min_view - 3.0f * splitter) * 0.36f);
        settings.testlab_width =
            std::floor(std::clamp(settings.testlab_width, ui_px(210.0f), max_side));
        settings.sim_width =
            std::floor(std::clamp(settings.sim_width, ui_px(250.0f), max_side));
        settings.results_width =
            std::floor(std::clamp(settings.results_width, ui_px(210.0f), max_side));
    } else {
        const float min_view = ui_px(420.0f);
        const float max_side =
            std::max(ui_px(240.0f), (content_width - min_view - 2.0f * splitter) * 0.42f);
        settings.sim_width =
            std::floor(std::clamp(settings.sim_width, ui_px(280.0f), max_side));
        settings.results_width =
            std::floor(std::clamp(settings.results_width, ui_px(250.0f), max_side));
        const float overflow = settings.sim_width + settings.results_width + 2.0f * splitter +
                               min_view - content_width;
        if (overflow > 0.0f) {
            settings.results_width =
                std::max(ui_px(220.0f), settings.results_width - overflow);
        }
    }

    ImGui::SetNextWindowPos(ImVec2(std::floor(viewport->Pos.x), content_y));
    ImGui::SetNextWindowSize(ImVec2(content_width, content_height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##workspace", nullptr,
                 kPanelFlags | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    const float row_height = ImGui::GetContentRegionAvail().y;
    const ImVec2 panel_padding(ui_px(12.0f), ui_px(12.0f));

    if (developer) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
        ImGui::BeginChild("testlab", ImVec2(settings.testlab_width, row_height),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        draw_testlab_panel(app.testlab);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine(0.0f, 0.0f);
        draw_column_splitter("##split_tl_sim", row_height, &settings.testlab_width);
        ImGui::SameLine(0.0f, 0.0f);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
    ImGui::BeginChild("study", ImVec2(settings.sim_width, row_height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    draw_study_panel(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);
    draw_column_splitter("##split_study_view", row_height, &settings.sim_width);
    ImGui::SameLine(0.0f, 0.0f);

    const float right_band = settings.results_width + splitter;
    const float view_width = std::max(1.0f, ImGui::GetContentRegionAvail().x - right_band);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("viewport", ImVec2(view_width, row_height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    draw_viewport_content(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 0.0f);
    draw_column_splitter("##split_view_results", row_height, &settings.results_width, -1.0f);
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, panel_padding);
    ImGui::BeginChild("results", ImVec2(0.0f, row_height),
                      ImGuiChildFlags_AlwaysUseWindowPadding);
    if (developer) {
        draw_results_panel(app.testlab);
    } else {
        draw_analysis_panel(app);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

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
    } else {
        status_text = app.model ? app.model->name + " · " + app.status : app.status;
        if (app.dof_count > 0) {
            status_text += std::format(" · {} DOF", app.dof_count);
        }
        if (developer) {
            const std::string head =
                app.testlab.git_head.empty() ? "unknown" : app.testlab.git_head;
            status_text += std::format(" · dev {} · {}", head, app.testlab.status);
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
    const std::string window_title =
        std::format("PolyMesh Studio {} — Adaptive Polyhedral FEA", POLYMESH_VERSION);
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
    const bool ttf_loaded = load_ui_font(ui_scale, &cinema_font);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    App app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, drop_callback);
    glfwSetWindowContentScaleCallback(window, content_scale_callback);
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
    app.testlab.cache_git_head(); // V3c: once at startup
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
        // interactive SolveJob owns the viewport — and a campaign artifact left
        // on disk by an earlier run must never take the viewport away from a
        // part the user opened (argv / drag-drop / "open"): that is what made a
        // startup with a model argument render an empty gradient while the
        // status strip reported some other campaign's element counts. Only a
        // live harness run may take over; each rewrite of the .pmp re-dirties
        // this, so the live preview still works.
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
