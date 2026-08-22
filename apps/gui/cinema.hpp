// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// The showcase film: the deployed advisor network choosing a mesh, the mesher
// building the one it chose, and the solver's own answer appearing in the order
// it is computed -- all on a virtual clock, so a headless recording is identical
// whatever the real frame rate was.
//
// HONESTY IS THE POINT OF THIS FILE. Every node fill is a value from the
// production ONNX graph's trunk taps (`advisor::ActivationFrame::input/fc1/
// fc2/heads`), every connection strength is |w_ji * a_i| from the exported
// weight blocks (`advisor::NetworkEdges::weights`), every element in the reveal
// is an element the mesher emitted (`pipeline::MeshStage::mesh`), every field is
// one that pass really produced (`pipeline::SolveStage::result`) or is computed
// from it by a named function, and every number on screen is the struct field or
// the function named beside it. Only TIME, opacity, the shrink-toward-centroid
// reveal, the spatial sweep front and the load factor are interpolated -- and
// the load factor is interpolated because linear elastostatics makes u(λ) = λ·u
// exact, so every frame of that ramp is a real solution rather than a blend of
// two. No displayed number is ever interpolated, and no activation, element
// count, error indicator or progress value is ever synthesised. When a source is
// missing the surface says WHICH one and skips that beat; it never substitutes a
// plausible value.
//
// IT IS ALSO MEANT TO BE READ BY SOMEONE WHO DID NOT WRITE IT. The film is
// aimed at a README, so the composition carries exactly four rows of text: a
// plain-English headline, the two-to-four numbers that matter on this beat, the
// one disclosure that applies to it, and the provenance stamp. That budget is
// the whole reason `docs/assets/cinema/NOTES.md` exists: the exhaustive
// per-beat disclosures the surface used to stack six deep in 13 px grey live
// there, beat by beat, and the film names that file on screen. A disclosure
// nobody can read at the size a README embeds is not a disclosure, and the
// honest fix is to make it legible somewhere rather than illegible here
// (ADR-0043).
//
// SEQUENCE, NOT SIMULTANEITY. `Advisor::explain()` runs to completion at
// `cinema advisor`, long before `solve` is issued, so the passes and the fill
// stages are two recorded sequences and the film shows them in that order: the
// pass lane sweeps the candidate grid inside `kDeliberate` and then HOLDS the
// pass that scored the recommended action for the whole of `kBuild`. Holding is
// what makes the two panes agree -- the activations on screen while the mesh
// grows are the activations of the forward pass that chose that mesh, not an
// unrelated later candidate. Nothing claims the two ran at the same instant.
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

#include <array>
#include <cstdint>
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
/// `kMeshHold` exists because the mesh is the thing this project builds and the
/// film used to cut away from it on the same frame the last element landed. It
/// is the finished fill, complete, held still, with its own counts on screen --
/// the beat a reader needs in order to see what was made before being shown
/// what it is for.
enum class CinemaAct { kSkeleton = 0, kDeliberate, kBuild, kMeshHold, kSolve };

/// Where the closing act is inside the real solve / estimate / refine loop.
///
/// This is the order in which the answer is actually computed, one
/// `pipeline::SolveStage` at a time, with a still hold after every moving beat
/// so the result of each step is on screen long enough to read. Nothing here is
/// a per-element solve order or an iteration counter, because a direct sparse
/// factorisation has neither.
///
/// The two `*Sweep` beats are SPATIAL REVEALS of a field that is already
/// complete: a plane travels across the part and the field's own colours are
/// uncovered behind it (`Viewport::FieldSweep`). The field is not animated and
/// no value on screen changes as the front passes -- what changes is how much of
/// it has been uncovered. That is the honest way to show a static solution
/// arriving, and it is stated on screen in those terms.
enum class SolvePhase {
    kNone = 0,      // no SolveStage was delivered; nothing solved is drawn
    kStressSweep,   // that pass's von Mises field, uncovered along the load axis
    kStressHold,    // the whole field, still
    kGradientSweep, // |∇σ| from `fea::nodal_scalar_gradient_magnitude`, uncovered
    kGradientHold,  // the whole gradient field, still
    kError,         // the ZZ error field recovered from that same solve
    kErrorHold,     // the whole error field, still
    kRefine,        // the mesh the NEXT pass solved, element by element
    kRefineHold,    // that mesh, complete, still
    kLoadRamp,      // u(λ) = λ·u on the final field, λ from 0 to 1
    kHold,          // λ = 1, the finished answer
};

/// Phase id, for the manifest and the notes. Stable text.
[[nodiscard]] const char* cinema_solve_phase_name(SolvePhase phase);

/// True for the beats that are a still hold of whatever the beat before them
/// finished. One predicate, so the pause the schedule pays for and the pause the
/// caption claims cannot disagree.
[[nodiscard]] bool cinema_phase_is_hold(SolvePhase phase);

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

/// Number of acts. Exposed because the recorder prints one act window per act
/// and the render script reads the table it prints.
inline constexpr int kCinemaActCount = 5;

/// One composed row of text and the palette colour it is drawn in.
struct CinemaLine {
    ImVec4 color{1, 1, 1, 1};
    std::string text;
};

/// The type the film is set in.
///
/// Sizes are in pixels at a 1080-line frame and scale with the frame height, so
/// the same composition is legible at 720p and does not become a wall of giant
/// text at 4K. They are this large for one measured reason: the README embeds a
/// GIF that is downscaled to roughly half the recorded width, so a 15 px row --
/// what this surface used to set its prose at -- arrives at the reader as 7 px
/// and cannot be read at all. The headline survives that halving; so do the
/// numbers.
struct CinemaType {
    /// The face to draw with. Null falls back to the UI font, which is correct
    /// but soft at these sizes: ImGui rasterises one size per face and scales
    /// the rest. `main.cpp` loads a second face at `kAtlasSize` for this.
    ImFont* font = nullptr;
    float headline = 40.0f; // the plain-English sentence
    float numbers = 27.0f;  // the numbers that matter on this beat
    float note = 20.0f;     // the one disclosure that applies to it
    float footer = 17.0f;   // provenance
    float chapter = 21.0f;  // the chapter bar
    float caption = 22.0f;  // panel captions and equations
    float label = 19.0f;    // panel unit labels
    float legend = 17.0f;   // panel legend
};

/// Size the second font face is rasterised at, and the frame height the sizes
/// in `CinemaType` are quoted for.
inline constexpr float kCinemaAtlasSize = 40.0f;
inline constexpr float kCinemaRefHeight = 1080.0f;

/// Real, once-per-mesh measurements used by the cell microscope. Entries are
/// aligned with `CinemaState::stages` / `solve_stages`; no frame recomputes
/// quality over a mesh that can contain hundreds of thousands of cells.
struct CinemaMeshInsight {
    std::array<std::size_t, 7> type_counts{};
    double quality_min = 0.0;
    double quality_mean = 0.0;
    std::size_t quality_measured = 0;
    std::size_t quality_unmeasured = 0;
};

/// The exact spectral-sizing evidence and one real CAD-edge curvature trace
/// shown in the opening chapter. `prepare_cinema_features` fills the plan
/// report from `pipeline::build_refinement_plan`; `build_cinema_skeleton`
/// fills the curve samples from `geom::extract_topology`.
struct CinemaSizingStory {
    bool prepared = false;
    bool brep_curvature = false;
    std::size_t geometry_seeds = 0;
    std::size_t bc_seeds = 0;
    double h_min = 0.0;
    pipeline::SpectralSizingReport spectral;

    std::uint32_t edge_id = 0;
    double edge_length = 0.0;
    std::size_t curve_modes_total = 0;
    std::size_t curve_modes_kept = 0;
    double curve_energy_fraction = 0.0;
    std::vector<double> stations;
    std::vector<double> curvature_raw;
    std::vector<double> curvature_filtered;
};

/// `CinemaType` for a frame `height` pixels tall.
[[nodiscard]] CinemaType cinema_type(ImFont* font, float height);

/// What the app measured about the run, lifted out of the app's own state by
/// the caller. Every field is a value the app already holds; the cinema derives
/// none of them.
struct CinemaHud {
    std::string part;        // pipeline::Model::name
    std::string mesher;      // pipeline::mesher_name(SimSetup::mesher)
    double mesh_size = 0.0;  // SimSetup::mesh_size, metres (0 = auto)
    double geometry_h = 0.0; // VolumeMeshOutput::geometry_h, metres (0 = none yet)
    int order = 1;           // SimSetup::p_elevate ? 2 : 1
    int adapt_passes = 0;    // SimSetup::adapt_passes
    double eta_target = 0.0; // SimSetup::eta_target
    /// Isotropic material values used both by the advisor feature row and by
    /// fea::Material in the authoritative solve.
    double youngs_modulus = 0.0; // SimSetup::youngs_modulus, Pa
    double poissons_ratio = 0.0; // SimSetup::poissons_ratio
    /// Undeformed model bounding-box diagonal, metres. The studio's automatic
    /// exaggeration targets a stated fraction of this length.
    double model_diagonal = 0.0;
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
    std::size_t unchanged_elements = 0;
    std::size_t removed_elements = 0;
    std::size_t added_elements = 0;
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

    // ---- the advisor pass lane (inside kDeliberate) -----------------------

    /// Index into `CinemaState::explanation->frames`. -1 when there are none.
    int frame_index = -1;
    /// One forward pass, in seconds. Reported in the manifest so the beat rate
    /// is auditable against the recorded frame count.
    double pass_beat_seconds = 0.0;
    /// True while the lane is still advancing through the candidate grid.
    bool pass_lane_live = false;
    /// True once the pass lane has stopped on the pass that scored the
    /// recommended action and is holding it: from the first frame of `kBuild`
    /// onward. `frame_index` then points at that pass and nothing else, which
    /// is what makes the lit graph and the growing mesh the same event.
    bool chosen_pass_held = false;
    /// True once the ranking's outcome may be stated: the same instant, which
    /// `cinema_decision_lead` guarantees is before the first element appears.
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

    // ---- the solve/estimate/refine loop (kSolve) --------------------------

    /// Index into `CinemaState::solve_stages`. -1 when none were delivered.
    int solve_stage_index = -1;
    SolvePhase solve_phase = SolvePhase::kNone;
    double solve_phase_t = 0.0;    // seconds into the phase
    double solve_phase_span = 1.0; // phase length, seconds
    /// 0..1 through the refined mesh's element reveal, in `kRefine`.
    double refine_reveal = 0.0;
    /// How far the spatial reveal front has travelled, 0..1 along
    /// `CinemaState::sweep_axis`. Exactly 1 (everything uncovered) outside the
    /// two sweep beats.
    double field_front = 1.0;
    /// The load factor λ the frame is drawn at. Exactly 1 outside `kLoadRamp`,
    /// and drawn on screen wherever it is not: linear elastostatics gives
    /// u(λ) = λ·u for the same λ·f, so every intermediate frame of the ramp is
    /// the exact solution of a real load case and not an interpolation.
    double load_factor = 1.0;

    // ---- opacity and layout ----------------------------------------------

    /// How far the left panel is slid into its share of the width, 0..1. Rises
    /// once, during the opening, and then stays at 1 for the rest of the take:
    /// the pane's height sets the part's rendered size, so a width that moved
    /// mid-take would resize the subject inside one continuous shot.
    float panel_open = 1.0f;
    /// Opacity of the network drawing and of the equation drawing. They cross
    /// fade at the `kSolve` boundary — the same panel, a different thing in it —
    /// and both are pure opacity: every node, every term and every number is
    /// exactly what it would be at alpha 1.
    float network_alpha = 1.0f;
    float equations_alpha = 0.0f;
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
    static constexpr double kDefaultDuration = 60.0;
    /// The recorder's fixed timestep. A recording is defined by its frame
    /// count, so the clock is derived from the frame index and never
    /// accumulated -- there is no drift to reason about.
    static constexpr double kRecordStep = 1.0 / 60.0;
    /// Longest the opening fade may be, seconds. The fade actually used is
    /// `cinema_opening_fade()`, which also caps it at half the opening act so a
    /// short take does not spend its whole first act fading up.
    static constexpr double kOpeningFade = 0.8;
    /// How long the DECISION is on screen at the head of the build act before
    /// the first element of the fill appears, seconds. Capped at a fifth of that
    /// act by `cinema_decision_lead()` so a short take does not spend the whole
    /// build stretch holding a caption.
    ///
    /// The fill is the execution of the advised action, so the action has to be
    /// readable BEFORE the thing it produced starts appearing; without the lead
    /// the two would arrive on the same frame and a viewer could not tell which
    /// followed which.
    static constexpr double kDecisionLead = 1.6;
    /// How long the network/equation cross fade takes, seconds. Capped at a
    /// tenth of the closing act by `cinema_panel_fade()`.
    static constexpr double kPanelFade = 0.8;

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

    /// Direction the closing act's field reveals travel in, unit length, and
    /// how it was chosen. Both are set by `build_cinema_skeleton` from the real
    /// load case: the axis is the resultant force direction and the sign is
    /// flipped, when the load faces sit at the far end of it, so that the front
    /// always STARTS at the loaded end and travels away from it. A part with no
    /// load gets its longest bounding-box axis and says so.
    Eigen::Vector3f sweep_axis{1.0f, 0.0f, 0.0f};
    std::string sweep_note;

    // ---- the network: kDeliberate, held through kBuild / kMeshHold --------

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
    /// What was applied, or why it was not, in one plain sentence.
    std::string decision_note;

    /// Index into `explanation->frames` of the pass that scored the recommended
    /// action, or -1 when there is no explanation. The build act holds this one.
    [[nodiscard]] int chosen_frame() const;

    // ---- the mesher's own stages: kBuild ---------------------------------

    /// Every construction stage of every fill this take ran, in emission order.
    ///
    /// Each entry keeps its whole `fea::NodalMesh`, which is what makes the
    /// reveal honest -- the elements drawn are the elements that stage actually
    /// contained -- and is also this feature's memory cost. The default initial
    /// fill carries 30,496 tet4 cells; a 100k-element fill would retain tens of
    /// megabytes per stage, which is why the sink is installed only while
    /// `active`.
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
    /// Once-per-stage element mix and shape-quality measurements. Populated
    /// when the worker snapshots are drained, so panel drawing is O(1).
    std::vector<CinemaMeshInsight> stage_insights;

    // ---- the solve/estimate/refine loop: kSolve ---------------------------

    /// Every completed adaptive pass of this take's solve, in the order the
    /// pipeline finished them, from `pipeline::SolveJob::on_solve_stage`.
    ///
    /// This is the ONLY source the closing act draws a field from. Each entry
    /// carries that pass's own `SolveResult` -- its mesh, its displacement, its
    /// von Mises and its ZZ error -- so the field shown beside "pass 0" is the
    /// field pass 0 produced and not the final answer relabelled. The default
    /// retains two full stages because it runs one measured adaptive pass.
    std::vector<pipeline::SolveStage> solve_stages;
    std::vector<CinemaMeshInsight> solve_insights;

    /// Spectral sizing and exact-edge evidence for this take.
    CinemaSizingStory sizing;

    /// Worker-thread sink for `pipeline::SolveJob::on_solve_stage`. Copies the
    /// stage for the same reason `push_stage` does.
    void push_solve_stage(const pipeline::SolveStage& stage);
    /// Main-thread hand-off, the counterpart of `drain_stages`.
    void drain_solve_stages();
    /// Replaces the final callback snapshot with `SolveJob::take_result()` after
    /// final quadratic promotion/re-solve. Intermediate pass snapshots stay
    /// untouched; the film's last pass must match the app's authoritative VTU
    /// result rather than the pre-finalisation callback.
    void adopt_final_result(const pipeline::SolveResult& result);
    /// Drops collected solve stages and the uploaded bookkeeping.
    void clear_solve_stages();

    /// |∇σ_vm| at every node of solve stage `index`, from
    /// `fea::nodal_scalar_gradient_magnitude` on that pass's own von Mises
    /// field and its own mesh, in Pa/m. Computed on first use and cached: the
    /// recovery is a least-squares fit per node over that node's element patch.
    /// It is cached rather than paid per frame, and the virtual clock means a
    /// first-use stall cannot change a recorded frame.
    ///
    /// Empty when the stage does not exist or the recovery could not run. The
    /// gradient beat then says so and draws no gradient field, because a
    /// gradient this module made up would be exactly the invention this whole
    /// surface exists to rule out.
    [[nodiscard]] const std::vector<double>& gradient_field(std::size_t index);
    /// Nodes whose patch could not determine a gradient, from the same call.
    /// Disclosed on screen when nonzero.
    [[nodiscard]] std::size_t gradient_unresolved(std::size_t index);
    /// Largest value in `gradient_field(index)`, Pa/m; 0 when there is none.
    [[nodiscard]] double gradient_max(std::size_t index);

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
    /// `Viewport::set_cinema_mesh` rebuilds every element's own faces (1296 B
    /// per tet4 cell), which is not a per-frame cost.
    CinemaMeshSource uploaded_mesh_source = CinemaMeshSource::kNone;
    int uploaded_mesh_index = -1;
    /// Index of the solve stage whose `SolveResult` is currently uploaded to
    /// the viewport, -1 when none. Same role: the upload is a whole boundary
    /// re-bake, so it happens on pass changes and not per frame.
    int uploaded_solve_stage = -1;
    /// Forgets every upload, so a fresh take never draws the previous run's
    /// geometry on its first frame.
    void invalidate_uploads();

  private:
    /// Worker-thread staging buffers. `push_stage` / `push_solve_stage` append
    /// here under the one mutex; the two `drain_*` calls move them out on the
    /// main thread. One mutex for both because the worker never holds it across
    /// anything and the two sinks fire from the same thread anyway.
    std::mutex stage_mutex_;
    std::vector<pipeline::MeshStage> pending_stages_;
    std::vector<pipeline::SolveStage> pending_solve_stages_;

    /// Lazily computed gradient recovery, one entry per solve stage. `computed`
    /// distinguishes "not tried yet" from "tried and there is none", so a
    /// recovery that legitimately returned nothing is not retried every frame.
    struct GradientCache {
        bool computed = false;
        std::size_t unresolved = 0;
        double max = 0.0;
        std::vector<double> values;
    };
    std::vector<GradientCache> gradients_;
    GradientCache& gradient_slot(std::size_t index);

    /// Per-frame connection ranking scratch, kept across frames so the top-K
    /// selection allocates once for the whole take rather than 60 times a
    /// second. Only `draw_cinema_network` touches it.
    friend void draw_cinema_network(CinemaState& state, const CinemaCue& cue,
                                    const CinemaType& type, float alpha);
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

/// Length of the DECISION lead-in at the head of the build act, seconds:
/// `CinemaState::kDecisionLead`, but never more than a fifth of that act.
[[nodiscard]] double cinema_decision_lead(const CinemaState& state);

/// Length of the panel cross fade, seconds: `CinemaState::kPanelFade`, but
/// never more than a tenth of the closing act.
[[nodiscard]] double cinema_panel_fade(const CinemaState& state);

/// Act schedule at the current clock, resolved against the real pass, stage and
/// solve-stage counts.
[[nodiscard]] CinemaCue cinema_cue(const CinemaState& state);

/// Draw parameters for this instant. Time, opacity, the shrink-toward-centroid
/// fraction and the sweep front are the only interpolated quantities in the
/// whole module.
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
    /// The spatial reveal for this instant. Inactive on every beat that is not
    /// a sweep, which is what leaves the held beats showing the whole field.
    Viewport::FieldSweep sweep;
};

/// What the viewport should display now. `kCinema` for the skeleton, the
/// deliberation, the fill and the refine beats; the pass's own von Mises,
/// recovered gradient or ZZ error field for the solve beats. Without a
/// `pipeline::SolveStage` the closing act holds the final fill stage and the
/// caption says so.
[[nodiscard]] CinemaRender cinema_render(CinemaState& state, const CinemaCue& cue,
                                         double base_deform_scale);

/// The `solver <token>` word the recorder prints: which linear solver this
/// take's solves actually ran through. `direct_ldlt` / `cg` when that is
/// established, `note_absent` when solve stages arrived but neither the solver's
/// own note nor a DOF count could establish it, `no_solve_stage` when
/// `pipeline::SolveJob::on_solve_stage` delivered nothing at all. Never a
/// method this module inferred from anything but those two sources.
[[nodiscard]] const char* cinema_solver_token(const CinemaState& state);

/// Uploads the stage or the solve-pass result the cue selected (only when it
/// changed), pushes the draw parameters and the spatial reveal. Main thread
/// only; may compute a pass's gradient recovery on first use.
void sync_cinema_viewport(CinemaState& state, const CinemaCue& cue, const CinemaRender& render,
                          Viewport& viewport);

/// Extracts the part's real outline and pushes it to the viewport skeleton:
/// BRep edge polylines when the model carries a `geom::CadModel`, otherwise the
/// tessellation's sharp-edge network. Records which, for the on-screen label,
/// and resolves the closing act's sweep axis from `setup`'s real load case.
void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model,
                           const pipeline::SimSetup& setup, Viewport& viewport);

/// Builds the refinement plan the imminent solve will use and records its real
/// spectral report for the feature chapter. Cinema-only duplicate evaluation:
/// the worker remains authoritative and receives the same immutable setup.
void prepare_cinema_features(CinemaState& state, const pipeline::Model& model,
                             const pipeline::SimSetup& setup);

/// Loads `dir` as an advisor model, runs `explain()` on the loaded part plus
/// the current setup, and -- when the decision is not a refusal -- applies it
/// to `setup` so the mesh act executes what the network chose. Returns false
/// and fills `state.advisor_note` when there is nothing honest to draw (no
/// model, missing directory, a graph without the trunk taps, a build without
/// the advisor module). Never fabricates an explanation.
bool load_cinema_advisor(CinemaState& state, const pipeline::Model& model,
                         pipeline::SimSetup& setup, const std::string& dir);

/// The left panel: the network column stack cross fading with the equation
/// board. Both are drawn from the same origin at the alphas the cue resolved,
/// so the swap is a dissolve inside one pane rather than a layout change --
/// the pane's width sets the viewport's width and must not move mid-take.
void draw_cinema_panel(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                       const CinemaHud& hud);

/// The network column stack: input / trunk.fc1 / trunk.fc2 / heads as circular
/// nodes, sized and coloured by this frame's real activations, with the
/// strongest connections drawn as lines. Takes `state` by reference only to
/// reuse the connection-ranking scratch buffer across frames.
void draw_cinema_network(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                         float alpha);

/// The equation board the closing act runs on: the relations the solver
/// actually evaluates, with the one this beat is computing lit and its own
/// numbers beside it. Every equation is the expression the cited code
/// implements; see `docs/assets/cinema/NOTES.md` for the citation table.
void draw_cinema_equations(const CinemaState& state, const CinemaCue& cue,
                           const CinemaType& type, const CinemaHud& hud, float alpha);

/// Padding the bottom strip is drawn with. Shared with the composition so the
/// strip cannot be padded differently from the way it was measured.
inline constexpr float kStripPadX = 22.0f;
inline constexpr float kStripPadY = 12.0f;

/// The four rows of the bottom strip, in draw order. Fixed shape, every act:
/// one plain-English headline, one row of the numbers that matter, one
/// disclosure, one provenance stamp. A row may be empty; the strip's height
/// never changes, because the leftover is the viewport pane and the pane's
/// height is what sets the part's rendered size.
struct CinemaCaption {
    ImVec4 headline_color{1, 1, 1, 1};
    std::string headline;
    std::string numbers;
    ImVec4 note_color{1, 1, 1, 1};
    std::string note;
    std::string footer;
};

[[nodiscard]] CinemaCaption cinema_caption(const CinemaState& state, const CinemaCue& cue,
                                           const CinemaHud& hud);

/// The chapter bar: the four things the film does, with the current one lit and
/// a progress fill under it. The one piece of pure orientation in the
/// composition -- it tells a first-time viewer where they are in a 60 s take
/// without their having to read a clock.
struct CinemaChapter {
    const char* label;
    /// Act this chapter is lit for; `kMeshHold` belongs to the build chapter.
    CinemaAct act;
};
[[nodiscard]] std::vector<CinemaChapter> cinema_chapters();
/// 0..1 through the whole take, for the chapter bar's fill. Time only.
[[nodiscard]] double cinema_progress(const CinemaState& state);

/// Exact height of the bottom strip, pixels: the chapter bar plus the four rows
/// at `type`'s sizes plus the padding. A constant for the take, which is what
/// keeps the part one size from the first frame to the last.
[[nodiscard]] float cinema_strip_height(const CinemaType& type);

/// The bottom strip: chapter bar, headline, numbers, disclosure, stamp. A row
/// too wide for the strip is SET SMALLER until it fits, never wrapped and never
/// clipped: wrapping would change the strip's height mid-take and clipping
/// would drop the end of a sentence the film is making.
void draw_cinema_strip(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       const CinemaType& type);

} // namespace polymesh::gui
