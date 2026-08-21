// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Advisor activation cinema: the deployed advisor network lighting up with the
// activations of its own forward passes, CONCURRENTLY with the mesh being built
// out of the mesher's own construction stages, closing on the real
// solve/estimate/refine loop the answer is actually computed by -- all on a
// virtual clock, so a headless recording is identical whatever the real frame
// rate was.
//
// HONESTY IS THE POINT OF THIS FILE. Every node fill is a value from the
// production ONNX graph's trunk taps (`advisor::ActivationFrame::input/fc1/
// fc2/heads`), every connection strength is |w_ji * a_i| from the exported
// weight blocks (`advisor::NetworkEdges::weights`), every element in the reveal
// is an element the mesher emitted (`pipeline::MeshStage::mesh`), every field is
// one that pass really produced (`pipeline::SolveStage::result`), and every
// number on screen is the struct field named beside it. Only TIME, opacity, the
// shrink-toward-centroid reveal and the load factor are interpolated -- and the
// load factor is interpolated because linear elastostatics makes u(λ) = λ·u
// exact, so every frame of that ramp is a real solution rather than a blend of
// two. No displayed number is ever interpolated, and no activation, element
// count, error indicator or progress value is ever synthesised. When a source is
// missing the surface says WHICH one and skips that beat; it never substitutes a
// plausible value.
//
// CONCURRENCY IS A COMPOSITION, NOT A CHRONOLOGY CLAIM. `Advisor::explain()`
// runs to completion at `cinema advisor`, long before `solve` is issued, so the
// passes and the fill stages are two recorded sequences. The take shows them on
// one clock because the fill is the execution of what the passes chose, states
// that on screen, and keeps the causal order readable by locking the DECISION in
// before the first element appears. It never implies the mesh preceded the
// decision, and it never implies the two ran at the same instant.
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

/// The acts, in take order. The clock may run past `kSolve` (a recording is an
/// exact frame count, not an exact act sum); past the end the composition holds
/// the final frame rather than looping.
///
/// `kBuild` is the CONCURRENT act and the reason the take has this shape: the
/// advisor's pass lane is ONE continuous sweep over every forward pass that
/// spans `kDeliberate` and `kBuild` together, while the mesher's stage lane
/// runs inside `kBuild` alone. Through `kBuild` both counters advance on the
/// same virtual clock, so the network is firing while the fill is being built.
///
/// The causal order survives that overlap because the DECISION is locked and on
/// screen from the first frame of `kBuild`, a lead-in before the first element
/// appears (`CinemaState::kDecisionLead`). It is honest to state it there and
/// keep showing passes afterwards: `Advisor::explain()` ran to completion in
/// `load_cinema_advisor`, before `solve` was ever issued, so every pass on
/// screen — including the ones still to come — had already happened when the
/// mesher emitted its first stage. The cinema replays two recorded sequences;
/// it does not claim they were simultaneous, and it says so in the ticker.
enum class CinemaAct { kSkeleton = 0, kDeliberate, kBuild, kSolve };

/// Where the closing act is inside the real solve/estimate/refine loop.
///
/// This is the order in which the answer is actually computed, one
/// `pipeline::SolveStage` at a time: solve, recover the ZZ error field from
/// that solve, refine where the field asked, solve again. Nothing here is a
/// per-element solve order or an iteration counter, because a direct sparse
/// factorisation has neither.
enum class SolvePhase {
    kNone = 0,  // no SolveStage was delivered; nothing solved is drawn
    kField,     // that pass's own von Mises field on its own mesh, undeformed
    kError,     // the ZZ error field recovered from that same solve
    kRefine,    // the mesh the NEXT pass solved on, revealing element by element
    kLoadRamp,  // u(λ) = λ·u on the final field, λ from 0 to 1
    kHold,      // λ = 1, the finished answer
};

/// Phase id, for the ticker and the disclosures. Stable text.
[[nodiscard]] const char* cinema_solve_phase_name(SolvePhase phase);

/// Which real mesh the viewport's per-element cinema buffer should be holding.
///
/// Two different meshes are revealed element by element in this take and they
/// come from different places: the fill under construction
/// (`pipeline::MeshStage::mesh`, one snapshot per construction stage) and the
/// mesh a later adaptive pass actually solved on
/// (`pipeline::SolveStage::result.volume_mesh`). Naming the source is what stops
/// the refine beat from silently redrawing the pass-0 fill and calling it
/// refined.
enum class CinemaMeshSource {
    kNone = 0,   // nothing should be uploaded
    kFillStage,  // CinemaState::stages[index].mesh
    kSolvedPass, // CinemaState::solve_stages[index].result.volume_mesh
};

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
    /// Resultant of every `SimSetup::LoadSpec::force` this take is solving, in
    /// newtons. Stated beside the load factor λ so "λ = 0.500" reads as a real
    /// load case (264.099 N of 528.198 N on the film's part) rather than as a
    /// fraction of an unnamed quantity.
    double load_newtons = 0.0;
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
/// resolved against the real data (pass count, stage count, solve-stage count)
/// rather than a guess. Produced by `cinema_cue`; consumed by the drawing and
/// the recorder.
struct CinemaCue {
    CinemaAct act = CinemaAct::kSkeleton;
    double act_t = 0.0;    // seconds into the act
    double act_span = 1.0; // act length, seconds

    // ---- the advisor pass lane (spans kDeliberate and kBuild) -------------

    /// Index into `CinemaState::explanation->frames`. -1 when there are none.
    int frame_index = -1;
    /// One forward pass, in seconds. Reported on screen so the beat rate is
    /// auditable against the recorded frame count.
    double pass_beat_seconds = 0.0;
    /// True once the ranking's outcome may be stated: from the first frame of
    /// `kBuild`, which is also before the first element of the fill appears.
    bool decision_locked = false;

    // ---- the mesher's stage lane (inside kBuild) --------------------------

    /// Index into `CinemaState::stages`. -1 when none apply on this beat.
    int stage_index = -1;
    /// 0..1 through the current stage's element reveal (time only).
    double stage_reveal = 0.0;
    /// One construction stage, in seconds.
    double stage_beat_seconds = 0.0;

    /// Which mesh the per-element reveal is drawing, and its index in that
    /// source. The reveal fraction is `stage_reveal` for `kFillStage` and
    /// `refine_reveal` for `kSolvedPass`.
    CinemaMeshSource mesh_source = CinemaMeshSource::kNone;
    int mesh_source_index = -1;

    /// True while BOTH lanes are advancing on this frame — the fact the ticker
    /// states, so the claim on screen and the schedule cannot disagree.
    bool concurrent = false;

    // ---- the solve/estimate/refine loop (kSolve) --------------------------

    /// Index into `CinemaState::solve_stages`. -1 when none were delivered.
    int solve_stage_index = -1;
    SolvePhase solve_phase = SolvePhase::kNone;
    double solve_phase_t = 0.0;    // seconds into the phase
    double solve_phase_span = 1.0; // phase length, seconds
    /// 0..1 through the refined mesh's element reveal, in `kRefine`.
    double refine_reveal = 0.0;
    /// The load factor λ the frame is drawn at. Exactly 1 outside `kLoadRamp`,
    /// and drawn on screen wherever it is not: linear elastostatics gives
    /// u(λ) = λ·u for the same λ·f, so every intermediate frame of the ramp is
    /// the exact solution of a real load case and not an interpolation.
    double load_factor = 1.0;

    // ---- opacity and layout ----------------------------------------------

    /// Opacity of the whole network panel, 0..1. The opening act belongs to the
    /// part, so the network is held back and faded up across the back half of
    /// it rather than sitting there as a wall of lines from frame zero. This is
    /// opacity and nothing else: every node, every edge and every number is
    /// exactly what it would be at alpha 1.
    float network_alpha = 1.0f;
    /// How far the network panel is slid into its share of the width, 0..1.
    /// Separate from `network_alpha` because the panel is DIMMED in `kSolve`
    /// once its last pass is only being held, and a width that followed the
    /// opacity would grow the viewport pane mid-take. The pane's height sets
    /// the part's rendered size, so that would resize the subject inside one
    /// continuous shot. Rises once, during the opening, and then stays at 1.
    float network_open = 1.0f;
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
    /// How long the DECISION is on screen at the head of the concurrent act
    /// before the first element of the fill appears, seconds. Capped at a fifth
    /// of that act by `cinema_decision_lead()` so a short take does not spend
    /// the whole concurrent stretch holding a caption.
    ///
    /// This lead is what keeps the overlap honest. The fill is the execution of
    /// the advised action, so the action has to be readable BEFORE the thing it
    /// produced starts appearing; without the lead the two would arrive on the
    /// same frame and a viewer could not tell which followed which.
    static constexpr double kDecisionLead = 0.6;

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

    // ---- the network: act 2 (kDeliberate) and act 3 (kBuild) -------------

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

    // ---- the mesher's own stages: act 3 (kBuild) -------------------------

    /// Every construction stage of every fill this take ran, in emission order.
    ///
    /// Each entry keeps its whole `fea::NodalMesh`, which is what makes the
    /// reveal honest -- the elements drawn are the elements that stage actually
    /// contained -- and is also this feature's memory cost. On the film's case
    /// (sphere_box_s0 at the advisor's own h_rel = 0.08) a pass-0 stage is
    /// 11,692 tet4 cells over 4,382 nodes, so the whole pass-0 list is a few
    /// megabytes; on a 100k-element fill it would be tens of megabytes per
    /// stage, which is why the sink is installed only while `active`.
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

    // ---- the solve/estimate/refine loop: act 4 (kSolve) ------------------

    /// Every completed adaptive pass of this take's solve, in the order the
    /// pipeline finished them, from `pipeline::SolveJob::on_solve_stage`.
    ///
    /// This is the ONLY source the closing act draws a field from. Each entry
    /// carries that pass's own `SolveResult` -- its mesh, its displacement, its
    /// von Mises and its ZZ error -- so the field shown beside "pass 0" is the
    /// field pass 0 produced and not the final answer relabelled. Measured cost
    /// on the film's case: 1,314,253 B per stage, two stages.
    std::vector<pipeline::SolveStage> solve_stages;

    /// Worker-thread sink for `pipeline::SolveJob::on_solve_stage`. Copies the
    /// stage for the same reason `push_stage` does.
    void push_solve_stage(const pipeline::SolveStage& stage);
    /// Main-thread hand-off, the counterpart of `drain_stages`.
    void drain_solve_stages();
    /// Drops collected solve stages and the uploaded bookkeeping.
    void clear_solve_stages();

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

    /// Which mesh the viewport's per-element cinema buffer currently holds.
    /// Public so `sync_cinema_viewport` can keep the upload to actual changes:
    /// `Viewport::set_cinema_mesh` rebuilds every element's own faces (1200 B
    /// per tet4 cell, so 14 MB for this film's 11,692-element fill), which is
    /// not a per-frame cost.
    CinemaMeshSource uploaded_mesh_source = CinemaMeshSource::kNone;
    int uploaded_mesh_index = -1;
    /// Index of the solve stage whose `SolveResult` is currently uploaded to
    /// the viewport, -1 when none. Same role: the upload is a whole boundary
    /// re-bake, so it happens on pass changes and not per frame.
    int uploaded_solve_stage = -1;
    /// Forgets every upload, so a fresh take never draws the previous run's
    /// geometry on its first frame.
    void invalidate_uploads();

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
    /// skeleton act against 668x505 in the deliberation act before this existed.
    /// A subject that changes size mid-shot reads as a cut, so the composition
    /// reserves the tallest strip the take can need and holds it.
    float ticker_reserve = 0.0f;

  private:
    /// Worker-thread staging buffers. `push_stage` / `push_solve_stage` append
    /// here under the one mutex; the two `drain_*` calls move them out on the
    /// main thread. One mutex for both because the worker never holds it across
    /// anything and the two sinks fire from the same thread anyway.
    std::mutex stage_mutex_;
    std::vector<pipeline::MeshStage> pending_stages_;
    std::vector<pipeline::SolveStage> pending_solve_stages_;

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

/// Length of the DECISION lead-in at the head of the concurrent act, seconds:
/// `CinemaState::kDecisionLead`, but never more than a fifth of that act.
[[nodiscard]] double cinema_decision_lead(const CinemaState& state);

/// Act schedule at the current clock, resolved against the real pass, stage and
/// solve-stage counts. Act spans are fixed fractions of `state.duration`:
/// skeleton 0.075, deliberate 0.120, build 0.235, solve 0.570. The two data
/// lanes are NOT one act each -- the pass lane spans deliberate+build and the
/// stage lane sits inside build -- which is what makes them concurrent, and the
/// closing 0.570 is the solve/estimate/refine loop plus the load ramp, which is
/// the longest single thing this take shows.
[[nodiscard]] CinemaCue cinema_cue(const CinemaState& state);

/// Draw parameters for this instant. Time, opacity and the shrink-toward-
/// centroid fraction are the only interpolated quantities in the whole module.
[[nodiscard]] Viewport::CinemaView cinema_view(const CinemaState& state, const CinemaCue& cue);

/// Everything the viewport render call needs for this instant, in one struct so
/// the mode, the exaggeration and the colour-scale maximum cannot be chosen by
/// three different rules.
struct CinemaRender {
    DisplayMode mode = DisplayMode::kCinema;
    /// Displacement exaggeration to draw at. Zero through the mesh acts and the
    /// per-pass field beats -- those show the field on the geometry the solver
    /// actually used -- and `load_factor * base` through the load ramp, which is
    /// what makes the ramp the exact linear response and not an easing curve.
    float deform_scale = 0.0f;
    /// Denominator the field is normalised by. The scalar buffer is the pass's
    /// own field, so scaling the field by λ is done by DIVIDING this maximum by
    /// λ: the drawn colour is then λ·s / s_max, i.e. the λ-scaled field against
    /// a fixed full-load legend, with no second copy of the field anywhere.
    float result_max = 1.0f;
};

/// What the viewport should display now. `kCinema` for the skeleton, the
/// deliberation, the concurrent fill and the refine beats; the pass's own von
/// Mises or ZZ error field for the solve beats. Without a `pipeline::SolveStage`
/// the closing act holds the final fill stage and the ticker says so.
[[nodiscard]] CinemaRender cinema_render(const CinemaState& state, const CinemaCue& cue,
                                         double base_deform_scale);

/// The `solver <token>` word the recorder prints: which linear solver this
/// take's solves actually ran through. `direct_ldlt` / `cg` when that is
/// established, `note_absent` when solve stages arrived but neither the solver's
/// own note nor a DOF count could establish it, `no_solve_stage` when
/// `pipeline::SolveJob::on_solve_stage` delivered nothing at all. Never a
/// method this module inferred from anything but those two sources.
[[nodiscard]] const char* cinema_solver_token(const CinemaState& state);

/// Uploads the stage or the solve-pass result the cue selected (only when it
/// changed) and pushes the draw parameters. Main thread only.
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
[[nodiscard]] std::vector<CinemaLine>
cinema_ticker_body(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud);

/// Exact height in pixels the ticker strip needs at `wrap_width` for the rows
/// `cinema_ticker_chips` and `cinema_ticker_body` return, including the window
/// padding `draw_cinema_ticker` is drawn with. Measured with the live ImGui
/// font, so a different UI face resizes the strip instead of clipping it.
[[nodiscard]] float cinema_ticker_height(const CinemaState& state, const CinemaCue& cue,
                                         const CinemaHud& hud, float wrap_width);

/// Height the composition should give the ticker strip: the tallest
/// `cinema_ticker_height` of any act at this instant — with the two acts that
/// carry the pass lane probed on their LAST pass, where the DECISION rows join
/// the per-pass rows, and the solve act probed in every one of its phases,
/// because the load-ramp rows and the refine rows are different heights —
/// raised into `CinemaState::ticker_reserve` and returned from there. Constant
/// for the take, so the viewport pane keeps one height and the part keeps one
/// size.
[[nodiscard]] float cinema_ticker_reserve(CinemaState& state, const CinemaCue& cue,
                                          const CinemaHud& hud, float wrap_width);

/// The bottom ticker: act, beat, the real numbers of whatever the beat is
/// showing, the HUD line, and the provenance stamp. Every row wraps rather
/// than clipping, which is what `cinema_ticker_height` reserved room for.
void draw_cinema_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud);

} // namespace polymesh::gui
