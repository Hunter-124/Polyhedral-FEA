// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private state and cross-TU helpers of the polymesh-gui executable. Not part
// of any library; included only by the GUI's own translation units.

#include "cinema.hpp"
#include "live_view.hpp"
#include "pipeline/scene.hpp"
#include "testlab_panel.hpp"
#include "viewport.hpp"

#include "imgui.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

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

inline constexpr ImGuiWindowFlags kPanelFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoScrollWithMouse;

enum class WorkspaceMode { kStudy, kDeveloper };
enum class MeshPreset { kFast, kBalanced, kRefined, kCustom };
enum class DeformationView { kAuto, kTrueScale, kCustom };

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
    std::mutex pass_trace_mutex;
    std::vector<pipeline::PassTrace> pass_traces;
    LiveView live;
    SolveJob job;
    bool live_callbacks_attached = false;
    std::optional<SolveResult> result;
    std::optional<VolumeMeshOutput> mesh_preview;
    Viewport viewport;
    DisplayMode mode = DisplayMode::kSetup;
    WorkspaceMode workspace = WorkspaceMode::kStudy;
    MeshPreset mesh_preset = MeshPreset::kBalanced;
    DeformationView deformation_view = DeformationView::kAuto;
    bool advanced_setup = false;
    int material_preset = 0;
    int expanded_step = 0;
    bool model_step_seen = false;
    bool boundary_step_seen = false;
    std::string advisor_dir;
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
    std::string status = "drop a .step / .brep / .stl part, or type a path below";
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
    /// The same face again at `kCinemaAtlasSize`, for the film: ImGui rasterises
    /// one size per `ImFont`, so a 40 px headline from the 16 px atlas would be a
    /// blurry upscale. Null when no TTF loaded (the film then uses the UI face).
    ImFont* cinema_font = nullptr;
    /// Monospaced Chudware face for live telemetry and result values.
    ImFont* mono_font = nullptr;
    /// The activation cinema (cinema.hpp). Inert until `--auto cinema on`: no
    /// stage sink is installed and the clock does not run.
    CinemaState cinema;
    /// $POLYMESH_CINEMA_STAMP, read once at startup and drawn verbatim in the
    /// cinema footer. The render script supplies the git revision and the model
    /// sha256; the app never computes a provenance line of its own.
    std::string cinema_stamp;
};

// ---- main.cpp ----------------------------------------------------------------

/// Directory of the running executable; asset lookup for fonts is relative to it.
extern std::filesystem::path executable_dir;

/// Imports a part, clearing the BCs and results tied to the previous one.
/// Import errors land in `app.status`; a failed load keeps the previous model.
void load_model(App& app, const std::string& path);

/// Drops a display mode whose data is gone (no result, no mesh preview).
void sanitize_display_mode(App& app);

// ---- automation.cpp: screenshots and headless --auto scripting ---------------

/// Reads the default framebuffer and writes it as an RGBA PNG. MUST run after
/// every draw call of the frame and before glfwSwapBuffers.
bool capture_screenshot(GLFWwindow* window, const std::string& path);

/// Services a pending F12/menu capture and the POLYMESH_GUI_SHOT rewrite.
void service_screenshot(App& app, GLFWwindow* window);

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
std::vector<AutoAction> parse_auto_spec(const std::string& spec);

/// strtol with the whole-token check it normally lacks: a typo'd number in a
/// script must fail the run, not silently parse as 0.
bool parse_auto_int(const std::string& text, int& out);

/// Executes at most one queued action. Every exit path other than a clean
/// `quit` sets `failed`, which run() turns into a nonzero exit code.
void tick_auto(AutoRunner& run, App& app, GLFWwindow* window);

/// End-of-frame half of `shot` (same capture window as service_screenshot).
void service_auto_shot(AutoRunner& run, GLFWwindow* window);

/// End-of-frame half of `record`: writes one frame per 1/60 s virtual step and
/// prints the take manifest once the last frame is written.
void service_cinema_record(AutoRunner& run, App& app, GLFWwindow* window);

// ---- chrome.cpp: fonts, window size, window icon ----------------------------

/// Default window size. The recorder writes the framebuffer, so this is also
/// the recorded frame size; $POLYMESH_GUI_SIZE=<w>x<h> overrides it at startup.
inline constexpr int kDefaultWindowW = 1600;
inline constexpr int kDefaultWindowH = 1000;

/// Loads the UI face (and the mono telemetry face) at `atlas_scale` and the
/// film face again at `kCinemaAtlasSize` into `*cinema_out`. Returns false when
/// no TTF exists; ImGui's stock bitmap font then stays in place.
bool load_ui_font(float atlas_scale, ImFont** cinema_out, ImFont** mono_out);

/// Rebuilds the font atlas for a new UI scale (monitor DPI change).
void rebuild_ui_fonts(App& app, float scale);

/// Formats a colorbar/legend value with the unit's natural prefix.
std::string format_legend_value(float value, const char* unit);

/// Parses "<w>x<h>". Returns false on anything else, including trailing junk,
/// so a typo is reported instead of silently recording at the wrong size.
bool parse_window_size(const char* text, int& width, int& height);

/// Installs the procedural 64x64 window icon.
void set_window_icon(GLFWwindow* window);

// ---- study_panel.cpp: Sim Setup column ---------------------------------------

/// Writes the current solve result as VTU. Shared by the Results-panel
/// "export VTU" button and the --auto `savevtu` verb, so a headless run can
/// capture exactly the nodal field the viewport is showing.
bool export_result_vtu(const App& app, const std::string& path, std::string& err);

void draw_study_panel(App& app);

/// Detaches the live-view callbacks from the running job (before it is reset).
void detach_live_callbacks(App& app);

/// Copy of the per-pass trace under its lock, for the results panels.
std::vector<pipeline::PassTrace> pass_trace_snapshot(App& app);

/// The Results column.
void draw_analysis_panel(App& app);

// ---- viewport_panel.cpp: 3D viewport column ----------------------------------

/// Offscreen render, results colorbar, frame button, camera and face picking.
void draw_viewport_content(App& app);

// ---- cinema_frame.cpp: fullscreen cinema composition -------------------------

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

CinemaLayout cinema_layout(const App& app, const ImGuiViewport& vp);

/// Fullscreen cinema layout: the panel left, the viewport right, the caption
/// strip along the bottom. No menu bar and no status strip, so a recorded frame
/// is the finished composition and `scripts/render_cinema.py` never has to crop.
void draw_cinema_frame(App& app);

} // namespace polymesh::gui
