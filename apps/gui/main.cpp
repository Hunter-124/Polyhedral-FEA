// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh Studio desktop app: import geometry, click faces to assign fixtures
// and loads, tune mesher/solver settings, solve, and inspect stress/deflection
// results. Studio-themed chrome with a fixed, constrained layout:
// Test Lab | Sim Setup | viewport | Results — panels cannot be dragged out
// of the frame, collapsed, or lost. Test Lab talks to the harness only via
// docs/dag/interfaces.md file formats (no apps/testlab link).
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
// POLYMESH_GUI_SIZE=<w>x<h> sets the window (and therefore the recorded frame)
// size at startup; unset, it is the default 1600x1000.

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
    /// `solve` holds the queue until the job settles. take_result() runs
    /// *later* in the frame that first observes kDone, so one extra frame is
    /// burned before app.status is read back for the outcome line.
    bool awaiting_solve = false;
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
    out.settled_view_aspect =
        std::max(1.0e-6f, (out.content_w - out.panel_w) / out.content_h);
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
            fail(std::format("solve failed: {}", app.job.status_text()));
            return;
        }
        if (st == SolveJob::State::kCancelled) {
            fail(std::format("solve cancelled: {}", app.job.status_text()));
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
                app.viewport.frame_content(DisplayMode::kCinema,
                                           layout.settled_view_aspect);
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
            app.viewport.frame_content(DisplayMode::kCinema,
                                       layout.settled_view_aspect);
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
    const int poster = std::min(
        cine.record_frames - 1,
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
        std::printf("cinema: mesh_stage index %zu pass %d id %s elements %zu nodes %zu\n",
                    i, stage.pass, stage.stage.c_str(), stage.mesh.elements.size(),
                    stage.mesh.nodes.size());
    }
    for (std::size_t i = 0; i < cine.solve_stages.size(); ++i) {
        const auto& stage = cine.solve_stages[i];
        std::printf("cinema: solve_stage index %zu pass %d elements %zu nodes %zu dof %zu "
                    "global_eta %.9g h_mark %zu p_mark %zu shape_mark %zu\n",
                    i, stage.pass, stage.trace.n_elems, stage.trace.n_nodes,
                    stage.trace.n_dof, stage.trace.global_eta, stage.trace.n_h_mark,
                    stage.trace.n_p_mark, stage.trace.n_shape_mark);
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
        global_eta, error_p99, max_displacement, app.deform_scale,
        visible_displacement, visible_fraction,
        app.viewport.cinema_unchanged_element_count(),
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
bool load_ui_font(ImFont** cinema_out) {
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
    auto try_load = [&io, cinema_out](const char* path) {
        std::error_code ec;
        if (path == nullptr || path[0] == '\0' ||
            !std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            return false;
        }
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, kRanges) == nullptr) {
            return false;
        }
        // A film face that fails to load is not a reason to lose the UI face
        // that just did: the film degrades to soft text, the studio does not
        // degrade at all.
        if (cinema_out != nullptr) {
            *cinema_out =
                io.Fonts->AddFontFromFileTTF(path, kCinemaAtlasSize, nullptr, kRanges);
            if (*cinema_out != nullptr) {
                merge_maths_glyphs(io, kCinemaAtlasSize);
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
    return false;
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

void draw_study_panel(App& app) {
    iw::begin_group_box("model");
    ImGui::TextColored(palette.text_dim, "drop .step/.stp/.brep on window");
    iw::input_text("path", app.open_path, sizeof(app.open_path), "path/to/part.step|.brep");
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
        // Order matches VolumeMesher enum. Graded tet is the product default.
        static const char* kMeshers[] = {
            "tet (grid)",  "hex (grid)",   "hex VEM (grid)", "graded tet (default)",
            "hex+pyramid", "prism (grid)", "hybrid zoo",     "octa (exp)",
            "hybrid VEM",  "Varyhedron",   "CVT poly (G4)",
        };
        if (iw::selector("mesher", &m, kMeshers, 11)) {
            app.setup.mesher = static_cast<VolumeMesher>(m);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "graded tet (default): multi-level LEB size field; CAD parts are\n"
                "solved on projected tet10 geometry (ADR-0035, 'curved solve geometry').\n"
                "hybrid zoo: hex bulk + pyramid skin → all-pyramid FE.\n"
                "hybrid VEM: hex FE bulk + native poly VEM transitions (ADR-0019).\n"
                "Varyhedron: variable poly packing (ADR-0021). Sharp-only edge protect;\n"
                "tet FE is the default product claim; VEM gated. Measure-first path:\n"
                "health + scorecard before packing loops (ADR-0023/24). STEP product\n"
                "CAD path needs OCC build. CAD edge profiles within element budget.\n"
                "CVT poly: restricted CVT clipped Voronoi → kPolyVem (G1–G4 / M5 gate).\n"
                "octa: experimental BCC (budget-capped; not product).");
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
        if (iw::checkbox("curved solve geometry", &pe)) {
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
        if (ImGui::BeginChild("##face_list", ImVec2(-FLT_MIN, list_h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
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
        if (ImGui::GetContentRegionAvail().x >
            ImGui::CalcTextSize(loads_txt.c_str()).x + 18.0f) {
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

    iw::begin_group_box("resources");
    {
        // Cap OpenMP threads for interactive mesh/solve (0 = process default).
        int hw = fea::openmp_default_threads();
        int thr = app.testlab.settings.max_threads;
        ImGui::TextColored(palette.text_dim, "max threads (0=all, hw=%d)", hw);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##max_threads", &thr, 0, std::max(1, hw))) {
            app.testlab.settings.max_threads = thr;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("OpenMP thread cap for mesh/assemble/solve hot paths.\n"
                              "0 keeps the process default (OMP_NUM_THREADS / hardware).");
        }
        double mem = app.testlab.settings.max_mem_gb;
        if (iw::input_double("max mem (GB, 0=auto)", &mem, "%.2f")) {
            app.testlab.settings.max_mem_gb = std::max(0.0, mem);
        }
        app.setup.max_mem_gb = app.testlab.settings.max_mem_gb;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Enforced before stiffness assembly/factorization.\n"
                              "0 uses 70%% of currently available system memory.");
        }

        static fea::EffectiveMemoryBudget shown_budget;
        static double budget_refresh_time = -1.0e9;
        static double shown_user_cap = -1.0;
        const double now = ImGui::GetTime();
        if (now - budget_refresh_time >= 1.0 || shown_user_cap != app.setup.max_mem_gb) {
            shown_budget = fea::effective_memory_budget(app.setup.max_mem_gb);
            shown_user_cap = app.setup.max_mem_gb;
            budget_refresh_time = now;
        }
        const auto cap_text = fea::format_memory_bytes(shown_budget.effective_cap_bytes);
        ImGui::TextColored(palette.status_ok, "ENFORCED cap: %s%s", cap_text.c_str(),
                           app.setup.max_mem_gb > 0.0 ? " (user/system minimum)"
                                                      : " (70% MemAvailable)");

        const fea::NodalMesh* projected_mesh = nullptr;
        if (app.mesh_preview) {
            projected_mesh = &app.mesh_preview->mesh;
        } else if (app.result) {
            projected_mesh = &app.result->volume_mesh;
        }
        if (projected_mesh != nullptr) {
            static const fea::NodalMesh* cached_mesh = nullptr;
            static std::size_t cached_nodes = 0;
            static std::size_t cached_elements = 0;
            static fea::SolveResourceEstimate projected;
            if (cached_mesh != projected_mesh ||
                cached_nodes != projected_mesh->nodes.size() ||
                cached_elements != projected_mesh->elements.size()) {
                const auto projected_free =
                    3 * static_cast<Eigen::Index>(projected_mesh->nodes.size());
                projected = fea::estimate_solve_resources(*projected_mesh, projected_free);
                cached_mesh = projected_mesh;
                cached_nodes = projected_mesh->nodes.size();
                cached_elements = projected_mesh->elements.size();
            }
            fea::SolveOptions projection_options;
            projection_options.max_mem_gb = app.setup.max_mem_gb;
            const auto projected_decision =
                fea::decide_solve_method(projected.nfree, projection_options, projected,
                                         shown_budget.effective_cap_bytes);
            const bool projected_over =
                projected_decision.estimated_bytes > shown_budget.effective_cap_bytes;
            const char* method =
                projected_decision.method == fea::SolveMethod::kDirect ? "LDLT" : "CG";
            const auto footprint =
                fea::format_memory_bytes(projected_decision.estimated_bytes);
            ImGui::TextColored(projected_over ? palette.status_warn : palette.text_dim,
                               "projected solve: %s (%s, conservative)", footprint.c_str(),
                               method);
        } else {
            ImGui::TextColored(palette.text_dim, "projected solve: mesh required");
        }
        ImGui::TextColored(palette.text_dim, "%s", fea::performance_description().c_str());
    }
    iw::end_group_box();

    iw::begin_group_box("mesh & solve");
    const auto state = app.job.state();
    const bool busy = state == SolveJob::State::kMeshing || state == SolveJob::State::kSolving;
    const bool paused = busy && app.job.pause_requested();
    // Live progress while worker runs (phase / frac / elapsed from SolveJob).
    // Elapsed is wall-clock polled every frame; phase_frac only advances at
    // report() boundaries (mesh/solve can sit on one fraction for a long time).
    if (busy) {
        const auto prog = app.job.progress();
        const char* phase = prog.phase.empty()
                                ? (state == SolveJob::State::kMeshing ? "mesh" : "solve")
                                : prog.phase.c_str();
        ImGui::TextColored(paused ? palette.accent : palette.status_warn, "phase: %s%s", phase,
                           paused ? " (paused)" : "");
        const float frac = static_cast<float>(std::clamp(prog.phase_frac, 0.0, 1.0));
        // Overall bar: blend adapt pass index when available.
        float overall = frac;
        if (prog.pass_count > 0) {
            const float span = 1.0f / static_cast<float>(prog.pass_count + 1);
            overall =
                std::clamp(static_cast<float>(prog.pass) * span + frac * span, 0.0f, 1.0f);
        }
        // Soft pulse while a long phase holds a fixed fraction so the bar still
        // reads as "alive" (mesh/CG do not emit mid-phase progress yet).
        float display = overall;
        if (!paused && overall < 0.995f) {
            const float pulse =
                0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 2.8f);
            display = std::clamp(overall + 0.025f * pulse, 0.0f, 0.99f);
        }
        ImGui::ProgressBar(display, ImVec2(-FLT_MIN, 0),
                           std::format("{:.0f}%", 100.0 * overall).c_str());
        ImGui::Text("elapsed: %.1f s", prog.elapsed_ms / 1000.0);
        if (prog.pass_count > 0) {
            ImGui::TextColored(palette.text_dim, "adapt pass %d / %d", prog.pass,
                               prog.pass_count);
        }
        if (prog.cg_iter > 0) {
            ImGui::Text("CG: iter %d  resid %.3g", prog.cg_iter, prog.cg_resid);
        }
        if (prog.n_elems > 0) {
            ImGui::TextColored(palette.text_dim, "mesh %zu elems · %zu nodes", prog.n_elems,
                               prog.n_nodes);
        }
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        app.status = app.job.status_text();
    }
    auto apply_resource_caps = [&]() {
        fea::set_openmp_threads(app.testlab.settings.max_threads);
    };
    ImGui::BeginDisabled(!app.model || busy);
    if (iw::button("mesh only", ImVec2(-1, 0))) {
        apply_resource_caps();
        app.live_mesh_seen_gen = 0;
        app.status = "meshing…";
        app.job.start_mesh(*app.model, app.setup);
    }
    if (iw::button(busy ? "working…" : "solve", ImVec2(-1, 0), /*primary=*/true)) {
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
        // Pause / play / cancel — cooperative between mesh/adapt/solve phases.
        if (paused) {
            if (iw::button("play (resume)", ImVec2(-1, 0), /*primary=*/true)) {
                app.job.request_resume();
                app.status = "resuming…";
            }
        } else if (iw::button("pause", ImVec2(-1, 0))) {
            app.job.request_pause();
            app.status = "pause requested…";
        }
        if (iw::button("cancel", ImVec2(-1, 0))) {
            app.job.request_cancel();
            app.status = "cancelling…";
        }
    }
    if (state == SolveJob::State::kFailed) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette.status_err);
        ImGui::TextWrapped("%s", app.job.status_text().c_str());
        ImGui::PopStyleColor();
        if (iw::button("dismiss error", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (state == SolveJob::State::kCancelled) {
        ImGui::TextColored(palette.status_warn, "%s", app.job.status_text().c_str());
        if (iw::button("dismiss cancel", ImVec2(-1, 0))) {
            app.job.clear_failure();
            app.status = "ready";
        }
    } else if (!busy &&
               (state != SolveJob::State::kIdle || app.result || !app.mesh_status.empty())) {
        ImGui::TextColored(palette.status_ok, "%s", app.job.status_text().c_str());
        const auto prog = app.job.progress();
        if (prog.elapsed_ms > 0.0 && prog.phase == "done") {
            ImGui::TextColored(palette.text_dim, "last run: %.1f s", prog.elapsed_ms / 1000.0);
        }
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

    iw::begin_group_box("diagnostics & self-improve");
    {
        const auto prog = app.job.progress();
        if (prog.n_elems > 0 && prog.elapsed_ms > 0.0) {
            const double eps = static_cast<double>(prog.n_elems) / (prog.elapsed_ms / 1000.0);
            ImGui::TextColored(palette.text_dim, "throughput: %.0f elem/s (%zu elems, %.1f s)",
                               eps, prog.n_elems, prog.elapsed_ms / 1000.0);
            if (prog.cg_iter > 0) {
                ImGui::TextColored(palette.text_dim, "CG: %d iters, resid %.2e", prog.cg_iter,
                                   prog.cg_resid);
            }
        }
        const bool running = app.improve_running.load();
        const int improve_exit = app.improve_exit.load();
        if (running) {
            ImGui::TextColored(palette.status_warn, "self-improve: running (see terminal)");
        } else if (improve_exit == kImproveNoRun) {
            ImGui::TextColored(palette.text_dim, "self-improve: idle");
        } else if (improve_exit == 0) {
            ImGui::TextColored(palette.text_dim, "self-improve: last run finished cleanly");
        } else {
            // The script's own failure, surfaced. Reporting "idle" for a run
            // that died is how a broken self-improve pass looked like no pass.
            ImGui::TextColored(palette.status_warn, "self-improve: last run failed (exit %d)",
                               improve_exit);
        }
        auto launch_improve = [&app](const char* backend) {
            if (app.improve_running.exchange(true)) {
                return;
            }
            const std::string cmd =
                std::string("bash scripts/self_improve.sh --backend ") + backend;
            std::atomic<bool>* flag = &app.improve_running;
            std::atomic<int>* exit_code = &app.improve_exit;
            std::thread([flag, exit_code, cmd] {
                exit_code->store(exit_code_of(std::system(cmd.c_str())));
                flag->store(false);
            }).detach();
            app.status = std::string("self-improve (") + backend + ") launched — see terminal";
        };
        ImGui::BeginDisabled(running);
        if (iw::button("self-improve (omp)", ImVec2(-1, 0))) {
            launch_improve("omp");
        }
        if (iw::button("self-improve (grok)", ImVec2(-1, 0))) {
            launch_improve("grok");
        }
        ImGui::EndDisabled();
        ImGui::TextColored(palette.text_dim,
                           "runs a CAD diagnostics battery → LLM edits meshers");
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
                const double diag = (app.model->bbox_max - app.model->bbox_min).norm();
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
                const std::string out =
                    app.model ? (app.model->name + "_result.vtu") : "result.vtu";
                std::string err;
                app.status = export_result_vtu(app, out, err)
                                 ? std::format("wrote {}", out)
                                 : std::format("export failed: {}", err);
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
std::optional<ImVec2> project_cinema_point(const Camera& camera,
                                           const Eigen::Vector3d& world,
                                           const ImVec2& image_min,
                                           const ImVec2& image_size) {
    const float aspect = image_size.x / std::max(image_size.y, 1.0f);
    const Eigen::Vector4f point(static_cast<float>(world.x()),
                                static_cast<float>(world.y()),
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
           static_cast<double>(render.deform_scale) *
               result.displacement.segment<3>(base);
}

void draw_cinema_mechanics(App& app, const CinemaCue& cue,
                           const CinemaRender& render, const CinemaType& type,
                           const ImVec2& image_min, const ImVec2& image_size) {
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
                       std::max(image_min.y + 8.0f,
                                image_min.y + image_size.y - size - 8.0f));
        return desired;
    };
    const double phase_x =
        cue.solve_phase_t / std::max(cue.solve_phase_span, 1.0e-9);
    const auto smooth = [](double x) {
        x = std::clamp(x, 0.0, 1.0);
        return static_cast<float>(x * x * (3.0 - 2.0 * x));
    };
    const bool final_stage =
        cue.solve_stage_index >= 0 &&
        static_cast<std::size_t>(cue.solve_stage_index + 1) >=
            app.cinema.solve_stages.size();
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
        mechanics_visibility =
            final_stage ? smooth((phase_x - 0.60) / 0.30) : 0.0f;
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

    for (std::size_t i = 0; i < app.cinema.support_markers.size(); ++i) {
        const auto& marker = app.cinema.support_markers[i];
        const float delay = 0.10f * static_cast<float>(i);
        const float reveal_raw =
            std::clamp((mechanics_visibility - delay) /
                           std::max(1.0f - delay, 1.0e-3f),
                       0.0f, 1.0f);
        const float support_reveal =
            reveal_raw * reveal_raw * (3.0f - 2.0f * reveal_raw);
        const float marker_alpha = 0.90f * support_reveal;
        const Eigen::Vector3d world =
            displayed_marker_position(app.cinema, marker, render);
        const auto projected =
            project_cinema_point(app.viewport.camera, world, image_min, image_size);
        if (!projected) {
            continue;
        }
        const ImVec2 p = *projected;
        const ImVec4 c = palette.sim_fixture;
        dl->AddCircleFilled(p, 7.0f, color(c, marker_alpha));
        dl->AddCircle(p, 15.0f + 3.0f * mechanics_pulse,
                      color(c, (0.32f + 0.20f * mechanics_pulse) * marker_alpha),
                      0, 2.0f);
        dl->AddLine(ImVec2(p.x - 14.0f, p.y + 13.0f),
                    ImVec2(p.x + 14.0f, p.y + 13.0f),
                    color(c, marker_alpha), 3.0f);
        for (int hatch = -2; hatch <= 2; ++hatch) {
            const float x = p.x + static_cast<float>(hatch) * 6.0f;
            dl->AddLine(ImVec2(x - 4.0f, p.y + 20.0f),
                        ImVec2(x + 3.0f, p.y + 13.0f),
                        color(c, 0.72f * marker_alpha), 1.5f);
        }
    }

    for (const auto& marker : app.cinema.load_markers) {
        if (!(marker.vector.norm() > 0.0)) {
            continue;
        }
        const double force_scale =
            cue.solve_phase == SolvePhase::kLoadRamp
                ? std::clamp(cue.load_factor, 0.0, 1.0)
                : 1.0;
        const double entry = static_cast<double>(mechanics_visibility);
        const double drawn_scale = force_scale * entry;
        if (!(drawn_scale > 1.0e-4)) {
            continue;
        }
        const Eigen::Vector3d centre =
            displayed_marker_position(app.cinema, marker, render);
        const Eigen::Vector3d direction = marker.vector.normalized();
        const double length =
            0.17 * drawn_scale * std::max(app.cinema.model_diagonal, 1.0e-6);
        const auto tail = project_cinema_point(
            app.viewport.camera, centre - length * direction, image_min, image_size);
        const auto head = project_cinema_point(
            app.viewport.camera, centre, image_min, image_size);
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
        const float arrow_alpha = mechanics_alpha * static_cast<float>(entry);
        dl->AddLine(*tail, *head,
                    color(c, (0.12f + 0.14f * mechanics_pulse) * arrow_alpha),
                    9.0f + 2.0f * mechanics_pulse);
        dl->AddLine(*tail, *head, color(c, arrow_alpha), 3.5f);
        const ImVec2 wing_a(head->x - 18.0f * unit.x + 9.0f * normal.x,
                            head->y - 18.0f * unit.y + 9.0f * normal.y);
        const ImVec2 wing_b(head->x - 18.0f * unit.x - 9.0f * normal.x,
                            head->y - 18.0f * unit.y - 9.0f * normal.y);
        dl->AddTriangleFilled(*head, wing_a, wing_b, color(c, arrow_alpha));
        const std::string label =
            std::format("{:.3g} kN", force_scale * marker.vector.norm() / 1e3);
        const float label_w =
            font->CalcTextSizeA(type.label, FLT_MAX, 0.0f, label.c_str()).x;
        const ImVec2 label_at = label_position(
            label, type.label,
            ImVec2(tail->x - label_w - 10.0f, tail->y - type.label * 1.35f),
            tail->x);
        dl->AddLine(*tail, ImVec2(label_at.x + label_w + 5.0f,
                                  label_at.y + type.label * 0.45f),
                    color(c, 0.45f * arrow_alpha), 1.0f);
        dl->AddText(font, type.label, label_at, color(c, arrow_alpha), label.c_str());
    }

    if (cue.action_bridge_alpha > 0.0f && app.cinema.advisor_ran) {
        const auto target = project_cinema_point(app.viewport.camera,
                                                 app.cinema.subject_center,
                                                 image_min, image_size);
        if (target) {
            const ImVec2 start(image_min.x + 3.0f,
                               image_min.y + image_size.y * 0.78f);
            const ImVec2 c1(image_min.x + image_size.x * 0.13f, start.y);
            const ImVec2 c2(target->x - image_size.x * 0.18f, target->y);
            const float a = cue.action_bridge_alpha;
            const ImVec4 flow =
                app.cinema.decision_applied ? palette.accent : palette.status_warn;
            dl->AddBezierCubic(start, c1, c2, *target,
                               color(flow, 0.11f * a), 7.0f);
            dl->AddBezierCubic(start, c1, c2, *target,
                               color(flow, 0.84f * a), 2.2f);
            const auto bezier = [&](float t) {
                const float q = 1.0f - t;
                return ImVec2(q * q * q * start.x + 3.0f * q * q * t * c1.x +
                                  3.0f * q * t * t * c2.x + t * t * t * target->x,
                              q * q * q * start.y + 3.0f * q * q * t * c1.y +
                                  3.0f * q * t * t * c2.y + t * t * t * target->y);
            };
            for (int i = 0; i < 5; ++i) {
                const float t = std::fmod(
                    static_cast<float>(cue.activation_wave) +
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

void draw_cinema_viewport(App& app, const CinemaRender& render,
                          const CinemaCue& cue, const CinemaType& type) {
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
/// `cinema on` replaces the whole thing with the cinema composition; with
/// cinema off nothing below this branch changes.
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

        // Everything the strip used to carry, segmented with " · ".
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
