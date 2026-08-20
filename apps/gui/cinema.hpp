// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Advisor activation cinema: the deployed advisor network lighting up with the
// activations of its own forward passes, beside the mesh being built out of the
// mesher's own construction stages, on a virtual clock so a headless recording
// is identical whatever the real frame rate was.
//
// HONESTY IS THE POINT OF THIS FILE. Every node fill is a value from the
// production ONNX graph's trunk taps (`advisor::ActivationFrame::input/fc1/
// fc2/heads`), every connection strength is |w_ji * a_i| from the exported
// weight blocks (`advisor::NetworkEdges::weights`), every element in the reveal
// is an element the mesher emitted (`pipeline::MeshStage::mesh`), and every
// number on screen is the struct field named beside it. Only TIME, opacity and
// the shrink-toward-centroid reveal are interpolated -- no displayed number
// ever is, and no activation, element count or progress value is ever
// synthesised. When a source is missing the surface says WHICH one and skips
// that act; it never substitutes a plausible value.
//
// This module owns no studio state: the app hands it a `CinemaHud` snapshot of
// what the app already measured, so the HUD cannot drift from what the panels
// would report, and nothing in the studio changes when cinema is off.

#include "pipeline/scene.hpp"
#include "viewport.hpp"

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif

#include "imgui.h"

#include <Eigen/Core>

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace polymesh::gui {

/// Where the outline drawn in the opening act came from. The cinema states this
/// on screen because a BRep edge network and a tessellation crease network are
/// two different claims about the part, and only one of them is the CAD.
enum class SkeletonSource {
    kNone = 0,    // no model loaded, so nothing was extracted
    kBrepEdges,   // geom::extract_topology(*model.cad) edge `samples` polylines
    kSharpEdges,  // geom::detect_sharp_edges(model.surface) -- STL input
    kUnavailable, // extraction ran and threw; the reason is in `skeleton_note`
};

/// The acts, in take order. The clock may run past `kResult` (a recording is an
/// exact frame count, not an exact act sum); past the end the composition holds
/// the final frame rather than looping.
enum class CinemaAct { kSkeleton = 0, kAdvisor, kMesh, kResult };

/// Act id, also the token printed on stdout for the render script's manifest.
/// Stable: `scripts/render_cinema.py` parses these.
[[nodiscard]] const char* cinema_act_name(CinemaAct act);

/// One composed row of the bottom ticker: the exact string that will be drawn
/// and the palette colour it is drawn in.
///
/// The rows exist as strings BEFORE anything is drawn because the composition
/// sizes the ticker strip from them. A disclosure clipped off the bottom or the
/// right of a recorded frame is a disclosure that was not made, and the only
/// way to be sure it fits is to measure the text that is actually going in.
struct CinemaLine {
    ImVec4 color{1, 1, 1, 1};
    std::string text;
};

/// What the app measured about the run, lifted out of the app's own state by
/// the caller. Every field is a value the app already holds; the cinema derives
/// none of them.
struct CinemaHud {
    std::string part;              // pipeline::Model::name
    std::string mesher;            // pipeline::mesher_name(SimSetup::mesher)
    double mesh_size = 0.0;        // SimSetup::mesh_size, metres (0 = auto)
    double geometry_h = 0.0;       // VolumeMeshOutput::geometry_h, metres (0 = none yet)
    int order = 1;                 // SimSetup::p_elevate ? 2 : 1
    int adapt_passes = 0;          // SimSetup::adapt_passes
    double eta_target = 0.0;       // SimSetup::eta_target
    std::size_t nodes = 0;         // node count as the studio's own DOF line uses it
    std::size_t elements = 0;      // element count from the same mesh
    std::size_t dof = 0;           // App::dof_count (3 * nodes)
    bool has_result = false;       // a SolveResult exists for this take
    double max_von_mises = 0.0;    // SolveResult::max_von_mises, Pa
    double max_displacement = 0.0; // SolveResult::max_displacement, m
    double global_eta = 0.0;       // SolveResult::global_eta
    /// App::deform_scale — the studio's own displacement exaggeration for the
    /// result act. Stated on screen because a deformed shape drawn at 1e6x
    /// without saying so is a picture of a part that never bent that far.
    double deform_scale = 1.0;
    /// Viewport::cinema_element_count() -- the elements the reveal can draw.
    std::size_t cinema_elements = 0;
    /// Viewport::cinema_skipped_element_count() -- elements the viewport could
    /// not triangulate (degenerate connectivity, faceless poly-VEM cells).
    /// Stated on screen when nonzero rather than quietly narrowing the reveal.
    std::size_t cinema_skipped_elements = 0;
    /// $POLYMESH_CINEMA_STAMP, drawn verbatim. The render script puts the git
    /// revision and the model sha256 here; the cinema never computes it,
    /// because a provenance line a program invents is not provenance.
    std::string stamp;
};

/// Where the take is at one instant of the virtual clock, with every index
/// resolved against the real data (candidate count, stage count) rather than a
/// guess. Produced by `cinema_cue`; consumed by the drawing and the recorder.
struct CinemaCue {
    CinemaAct act = CinemaAct::kSkeleton;
    double act_t = 0.0;    // seconds into the act
    double act_span = 1.0; // act length, seconds
    /// Index into `CinemaState::explanation->frames`. -1 when there are none.
    int frame_index = -1;
    /// Index into `CinemaState::stages`. -1 when none were collected.
    int stage_index = -1;
    /// 0..1 through the current stage's element reveal (eased; time only).
    double stage_reveal = 0.0;
    /// One candidate, or one construction stage, in seconds. Reported on screen
    /// so the beat rate is auditable against the recorded frame count.
    double beat_seconds = 0.0;
    /// Opacity of the whole network panel, 0..1. The opening act belongs to the
    /// part, so the network is held back and faded up across the back half of
    /// it rather than sitting there as a wall of lines from frame zero. This is
    /// opacity and nothing else: every node, every edge and every number is
    /// exactly what it would be at alpha 1.
    float network_alpha = 1.0f;
};

/// The take. Owns the advisor explanation, the collected construction stages,
/// the virtual clock, the act schedule and the recorder.
///
/// Threading: `push_stage` runs on the `SolveJob` worker thread and is the only
/// member that takes `stage_mutex_`. Everything else is main-thread only, and
/// `drain_stages` is the single hand-off, so the GL uploads and the draw never
/// race the mesher.
class CinemaState {
  public:
    /// Take length when nothing asked for a specific one. `record` overrides it
    /// from the requested frame count, which is the authoritative case.
    static constexpr double kDefaultDuration = 20.0;
    /// The recorder's fixed timestep. A recording is defined by its frame
    /// count, so the clock is derived from the frame index and never
    /// accumulated -- there is no drift to reason about.
    static constexpr double kRecordStep = 1.0 / 60.0;
    /// Longest the opening fade may be, seconds. The fade actually used is
    /// `cinema_opening_fade()`, which also caps it at half the opening act so a
    /// short take does not spend its whole first act fading up.
    static constexpr double kOpeningFade = 0.5;

    // ---- take state ------------------------------------------------------

    /// True between `cinema on` and `cinema off`: fullscreen cinema layout.
    bool active = false;
    /// Virtual seconds since the take started. Only `advance` and `seek_frame`
    /// write it. NEVER driven by ImGui::GetIO().DeltaTime while recording.
    double t = 0.0;
    /// Total take length the act schedule is scaled to, seconds.
    double duration = kDefaultDuration;

    // ---- act 1: the part's own skeleton ---------------------------------

    SkeletonSource skeleton_source = SkeletonSource::kNone;
    std::size_t skeleton_polylines = 0;
    std::size_t skeleton_points = 0;
    /// Verbatim failure text when `skeleton_source == kUnavailable`.
    std::string skeleton_note;

    // ---- act 2: the network ---------------------------------------------

#ifdef POLYMESH_WITH_ADVISOR
    /// The forward passes that produced the decision, in chooser order.
    std::optional<advisor::AdvisorExplanation> explanation;
    /// The deployed graph's shape and weights, from `activation_layout.json`.
    advisor::NetworkLayout layout;
#endif
    /// The model directory `cinema advisor` was pointed at, for the footer.
    std::string advisor_dir;
    /// Why there is no explanation, drawn verbatim. Empty when there is one.
    std::string advisor_note;
    /// True when the decision was applied to the app's SimSetup, so the mesh
    /// act really is executing what the network chose. False on a refusal, on
    /// an unparseable mesher name, and when no advisor ran at all.
    bool decision_applied = false;
    /// What was applied, or why it was not. Drawn verbatim beside the decision.
    std::string decision_note;

    // ---- act 3: the mesher's own stages ---------------------------------

    /// Every construction stage of every fill this take ran, in emission order.
    ///
    /// Each entry keeps its whole `fea::NodalMesh`, which is what makes the
    /// reveal honest -- the elements drawn are the elements that stage actually
    /// contained -- and is also this feature's memory cost. On the cinema case
    /// (box_hole_s0 at the advisor's own h_rel = 0.2) a stage is a few hundred
    /// elements, so the whole list is kilobytes; on a 100k-element fill it
    /// would be tens of megabytes per stage, which is why the sink is installed
    /// only while `active`.
    std::vector<pipeline::MeshStage> stages;

    /// Worker-thread sink for `pipeline::SolveJob::on_mesh_stage`. Copies the
    /// stage: the callback's mesh is the worker's live buffer.
    void push_stage(const pipeline::MeshStage& stage);
    /// Main-thread hand-off: appends everything pushed since the last call to
    /// `stages`. Cheap no-op when the worker pushed nothing.
    void drain_stages();
    /// Drops collected stages and the uploaded-stage bookkeeping, so one take
    /// never shows another run's mesh.
    void clear_stages();

    // ---- recorder --------------------------------------------------------

    /// Output directory while a `record` is in flight; empty otherwise.
    std::string record_dir;
    int record_frames = 0; // total frames requested
    int record_next = 0;   // index of the next frame to draw and capture
    [[nodiscard]] bool recording() const {
        return !record_dir.empty() && record_next < record_frames;
    }

    /// Advances the virtual clock by `dt` seconds (interactive playback). The
    /// recorder does not call this: it sets `t` from the frame index instead, so
    /// the composition is a pure function of the frame number.
    void advance(double dt);
    /// Puts the clock on the exact instant of recorded frame `index`.
    void seek_frame(int index) { t = static_cast<double>(index) * kRecordStep; }

    /// Index of the stage currently uploaded to the viewport, -1 when none.
    /// Public so `sync_cinema_viewport` can keep the upload to stage changes.
    int uploaded_stage = -1;

    /// Height the ticker strip is reserved at for the whole take, pixels; 0
    /// until the first frame measures it. Maintained by
    /// `cinema_ticker_reserve` as a high-water mark over ALL FOUR acts, never
    /// as the current act's own need.
    ///
    /// The strip's height is what is left over for the viewport, and the
    /// viewport's HEIGHT is what sets the part's rendered size (the projection
    /// fixes the vertical field). A strip that grew from four rows in the
    /// opening act to eight when the DECISION lines arrive would therefore
    /// resize the part at every act boundary — measured at 701x529 px in the
    /// skeleton act against 668x505 in the advisor act before this existed.
    /// A subject that changes size mid-shot reads as a cut, so the composition
    /// reserves the tallest strip the take can need and holds it.
    float ticker_reserve = 0.0f;

  private:
    /// Worker-thread staging buffer. `push_stage` appends here under the mutex;
    /// `drain_stages` moves it out on the main thread.
    std::mutex stage_mutex_;
    std::vector<pipeline::MeshStage> pending_stages_;

    /// Per-frame connection ranking scratch, kept across frames so the top-K
    /// selection allocates once for the whole take rather than 60 times a
    /// second. Only `draw_cinema_network` touches it.
    friend void draw_cinema_network(CinemaState& state, const CinemaCue& cue);
    struct EdgePick {
        float rank = 0.0f;  // |w_ji * a_i|, the selection key
        float value = 0.0f; // w_ji * a_i, signed, for the colour
        int block = 0;      // index into NetworkLayout::edges
        int src = 0;
        int dst = 0;
    };
    std::vector<EdgePick> edge_scratch_;
};


/// Virtual-time window `[t0, t1)` of one act at the current take length.
/// Exposed so the recorder can report exact frame spans for the render
/// script's manifest without duplicating the schedule.
void cinema_act_window(const CinemaState& state, CinemaAct act, double& t0, double& t1);

/// Length of the opening fade-up at the current take length, seconds:
/// `CinemaState::kOpeningFade`, but never more than half the opening act.
/// A 120-frame take gives the skeleton act 0.24 s, and a fixed 0.5 s fade
/// would mean the outline never reached full strength inside its own act --
/// the opening would be a grey ghost of the part for the whole shot.
/// Also fixes the poster frame: the first frame at or after this is the first
/// fully composed one, which is what the recorder reports.
[[nodiscard]] double cinema_opening_fade(const CinemaState& state);

/// Act schedule at the current clock, resolved against the real candidate and
/// stage counts. Act spans are fixed fractions of `state.duration`: the two
/// data acts (advisor, mesh) take 0.38 each because neither is subordinate to
/// the other, and the opening skeleton and closing result take 0.12 each.
[[nodiscard]] CinemaCue cinema_cue(const CinemaState& state);

/// Draw parameters for this instant. Time, opacity and the shrink-toward-
/// centroid fraction are the only interpolated quantities in the whole module.
[[nodiscard]] Viewport::CinemaView cinema_view(const CinemaState& state, const CinemaCue& cue);

/// What the viewport should display now: `kCinema` for the skeleton, advisor
/// and mesh acts, and the real von Mises field for the result act when a
/// `SolveResult` exists. Without a result the result act holds the final fill
/// stage and the ticker says so.
[[nodiscard]] DisplayMode cinema_display_mode(const CinemaState& state, const CinemaCue& cue,
                                              bool has_result);

/// Uploads the stage the cue selected (only when it changed) and pushes the
/// draw parameters. Main thread only.
void sync_cinema_viewport(CinemaState& state, const CinemaCue& cue, Viewport& viewport);

/// Extracts the part's real outline and pushes it to the viewport skeleton:
/// BRep edge polylines when the model carries a `geom::CadModel`, otherwise the
/// tessellation's sharp-edge network. Records which, for the on-screen label.
void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model,
                           Viewport& viewport);

/// Loads `dir` as an advisor model, runs `explain()` on the loaded part plus
/// the current setup, and -- when the decision is not a refusal -- applies it
/// to `setup` so the mesh act executes what the network chose. Returns false
/// and fills `state.advisor_note` when there is nothing honest to draw (no
/// model, missing directory, a graph without the trunk taps, a build without
/// the advisor module). Never fabricates an explanation.
bool load_cinema_advisor(CinemaState& state, const pipeline::Model& model,
                         pipeline::SimSetup& setup, const std::string& dir);

/// The network column stack: input / trunk.fc1 / trunk.fc2 / heads as circular
/// nodes, sized and coloured by this frame's real activations, with the
/// strongest connections drawn as lines. Takes `state` by reference only to
/// reuse the connection-ranking scratch buffer across frames.
void draw_cinema_network(CinemaState& state, const CinemaCue& cue);

/// Window padding the ticker strip is drawn with. Shared with the composition
/// so the strip cannot be padded differently from the way it was measured.
inline constexpr float kTickerPadX = 12.0f;
inline constexpr float kTickerPadY = 8.0f;

/// The ticker's header chips (act, virtual clock, beat rate, recording state),
/// each an independently coloured run of text. Drawn flowed across as many
/// rows as they need.
[[nodiscard]] std::vector<CinemaLine> cinema_ticker_chips(const CinemaState& state,
                                                          const CinemaCue& cue);

/// The ticker's body rows for this instant: the act's own real numbers, the HUD
/// line and the provenance stamp, in draw order.
[[nodiscard]] std::vector<CinemaLine> cinema_ticker_body(const CinemaState& state,
                                                         const CinemaCue& cue,
                                                         const CinemaHud& hud);

/// Exact height in pixels the ticker strip needs at `wrap_width` for the rows
/// `cinema_ticker_chips` and `cinema_ticker_body` return, including the window
/// padding `draw_cinema_ticker` is drawn with. Measured with the live ImGui
/// font, so a different UI face resizes the strip instead of clipping it.
[[nodiscard]] float cinema_ticker_height(const CinemaState& state, const CinemaCue& cue,
                                         const CinemaHud& hud, float wrap_width);

/// Height the composition should give the ticker strip: the tallest
/// `cinema_ticker_height` of any of the four acts at this instant — with the
/// advisor act probed on its LAST pass, where the DECISION rows join the
/// per-pass rows and it is at its tallest — raised into
/// `CinemaState::ticker_reserve` and returned from there. Constant for the
/// take, so the viewport pane keeps one height and the part keeps one size.
[[nodiscard]] float cinema_ticker_reserve(CinemaState& state, const CinemaCue& cue,
                                          const CinemaHud& hud, float wrap_width);

/// The bottom ticker: act, beat, the real numbers of whatever the beat is
/// showing, the HUD line, and the provenance stamp. Every row wraps rather
/// than clipping, which is what `cinema_ticker_height` reserved room for.
void draw_cinema_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud);

} // namespace polymesh::gui
