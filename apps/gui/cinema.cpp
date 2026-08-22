// SPDX-License-Identifier: BSD-3-Clause
#include "cinema.hpp"

#include "geom/signal_fft.hpp"
#include "colormap.hpp"
#include "theme.hpp"

#include "fea/cell_quality.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace polymesh::gui {
namespace {

/// Act spans as fractions of the take, tuned at the render script's 3600-frame
/// default (60.0 s).
/// The opening now gives the curvature/FFT trace six readable seconds at full
/// size. The advisor lane spends its first 65% on the real candidate passes and
/// its last 35% holding the final re-score/refusal state, so the network's
/// settled activation pattern is inspectable instead of flashing for one beat.
/// The adaptive closing act receives 27 seconds and scales its real pass beats
/// proportionally when more than one solve stage exists.
///
/// Measured pacing on the film's default 60 s take:
///
///   exact-CAD / spectral opening  7.800 s
///   advisor passes + result hold  9.000 s
///   graded fill                   10.800 s
///   exploded + closed mesh hold  5.400 s
///   solve / recovery sequence    27.000 s
///
/// Candidate-specific prose is deliberately absent from the fast pass lane.
/// The network is the motion there; the strip carries one stable explanation.
constexpr std::array<double, kCinemaActCount> kActFraction = {0.13, 0.15, 0.18, 0.09, 0.45};
double beat_seconds(SolvePhase phase) {
    switch (phase) {
    case SolvePhase::kStressSweep:
        return 2.6;
    case SolvePhase::kStressHold:
        return 3.2;
    case SolvePhase::kGradientSweep:
        return 2.6;
    case SolvePhase::kGradientHold:
        return 3.0;
    case SolvePhase::kError:
        return 2.4;
    case SolvePhase::kErrorHold:
        return 2.8;
    case SolvePhase::kRefine:
        return 3.2;
    case SolvePhase::kRefineHold:
        return 3.4;
    case SolvePhase::kLoadRamp:
        return 4.4;
    case SolvePhase::kHold:
        return 5.4;
    case SolvePhase::kNone:
        break;
    }
    return 0.0;
}

/// Per-element reveal geometry, retuned for the denser complex-CAD take.
///
/// The old full-opacity 1.5 px edge pass was measured on a 568-cell fixture.
/// At 11,692 cells it already put up to half the subject into near-black
/// outline; the complex showcase carries tens of thousands of cells. Keep the
/// centroid shrink that lets a new cell read as a separate object, but make the
/// edge pass a restrained annotation over shaded faces rather than the image.
constexpr double kRevealShrink = 0.22;
constexpr double kRevealShrinkFraction = 0.33;
constexpr float kMeshEdgeAlpha = 0.18f;
constexpr float kMeshEdgeWidth = 0.8f;

/// Width of the field sweep's leading band, as a fraction of the part's extent
/// along the sweep axis. Wide enough that the highlight reads as a moving front
/// rather than a hard edge, narrow enough that it is a front and not a fade.
constexpr float kSweepFeather = 0.10f;

/// Floor on λ when it is used as a DIVISOR for the colour scale. λ itself is
/// never floored — the number on screen and the displacement are the real λ,
/// including exactly 0 — but s_max / λ has to stay finite, and at λ = 1e-3 every
/// drawn colour is already within one part in a thousand of the bottom of the
/// colormap, which is the correct picture of a part carrying no load.
constexpr double kMinLoadFactor = 1.0e-3;

/// Connections drawn per frame. The deployed graph has 15,936 of them for this
/// layout; drawing thousands fills the column band with a solid grey haze that
/// buries the nodes and shows nothing, so only the strongest subset is drawn
/// and the count is disclosed on screen from THIS constant -- shrink it and the
/// on-screen sentence shrinks with it.
///
/// 180 rather than the 280 this used to draw: the panel is now set at nearly
/// twice the type size, so the same haze costs twice the readable area, and the
/// weakest hundred of the old set were hairlines carrying |w·a| under a tenth of
/// the strongest.
constexpr std::size_t kDrawnConnections = 180;

/// Widest a head-unit node is allowed to get, and the gap between it and its
/// label.
constexpr float kNodeRadiusMax = 8.0f;
constexpr float kLabelGap = 10.0f;

/// printf into a std::string.
///
/// The captions have to exist as strings BEFORE anything is drawn, so a row can
/// be set at whatever size makes it fit. They keep the printf format strings
/// this surface already used: rewriting a hundred conversions into another
/// formatting library would be a hundred chances to change a displayed number,
/// which is the one thing this file may not do.
#if defined(__GNUC__)
[[gnu::format(printf, 1, 2)]]
#endif
std::string fmt(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list measure;
    va_copy(measure, args);
    const int n = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        std::vsnprintf(out.data(), static_cast<std::size_t>(n) + 1, format, args);
    }
    va_end(args);
    return out;
}

/// Thousands separators. "11,692 cells" is read at a glance and "11692 cells"
/// is counted, and this film is aimed at readers who are doing the former.
std::string grouped(std::size_t n) {
    std::string digits = std::format("{}", n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (i - lead) % 3 == 0 && i >= lead) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

/// Plain-English names for the graph's own head units.
///
/// The names on the left of this table are the deployed model's own tensor
/// labels, read out of `activation_layout.json`; the artifact is NOT rewritten,
/// because it is the exporter's record of what the graph emits and a film is not
/// a reason to edit a model artifact. The names on the right are what a reader
/// who has never seen this codebase can understand. The mapping is stated on
/// screen ("head names in plain language") and the artifact's own spelling for
/// every one of them is in docs/assets/cinema/NOTES.md.
///
/// An unmapped label falls through VERBATIM rather than being prettified by a
/// rule: a silent underscore-to-space transform would have turned a head this
/// table has not been taught about into a confident-looking label nobody chose.
struct HeadName {
    std::string_view tensor;
    const char* plain;
};
constexpr std::array<HeadName, 17> kHeadNames{{
    {"rel_err", "predicted error"},
    {"rel_err_rel", "error vs this part's median"},
    {"geo_chamfer", "mesh-to-CAD distance"},
    {"geo_p99", "mesh-to-CAD worst 1%"},
    {"dof", "unknowns"},
    {"mesh_ms", "meshing time"},
    {"solve_ms", "solve time"},
    {"failure_logit", "failure risk"},
    {"policy_h_rel", "cell size"},
    {"policy_adapt_passes", "refinement passes"},
    {"policy_eta_target", "error target"},
    {"policy_order_logit_1", "order 1 (linear)"},
    {"policy_order_logit_2", "order 2 (quadratic)"},
    {"policy_mesher_logit_graded_tet", "mesher: graded tets"},
    {"policy_mesher_logit_hex", "mesher: hex"},
    {"policy_mesher_logit_hybrid_vem", "mesher: hybrid VEM"},
    {"policy_mesher_logit_hybrid_zoo", "mesher: hybrid, hex + pyramids"},
}};

std::string_view head_name(std::string_view tensor) {
    for (const auto& entry : kHeadNames) {
        if (entry.tensor == tensor) {
            return entry.plain;
        }
    }
    return tensor;
}

/// Plain-English names for `pipeline::kMeshStageNames`, in the same spirit: the
/// stage ids are the pipeline's stable vocabulary and stay in the manifest, and
/// what the film says is what the step did.
std::string_view stage_name(std::string_view stage) {
    if (stage == "lattice") {
        return "laying down the cell grid";
    }
    if (stage == "expand") {
        return "splitting hexes into pyramids";
    }
    if (stage == "snap") {
        return "pulling the surface onto the CAD";
    }
    if (stage == "peel") {
        return "removing the cells the snap flattened";
    }
    if (stage == "reproject") {
        return "re-projecting the stragglers";
    }
    if (stage == "smooth") {
        return "evening out the surface spacing";
    }
    if (stage == "resnap") {
        return "snapping what moved, again";
    }
    if (stage == "pin") {
        return "pinning CAD edges and corners";
    }
    if (stage == "fill") {
        return "converting to solver elements";
    }
    if (stage == "ship") {
        return "final checks";
    }
    return stage;
}

/// Plain-English names for the mesher vocabulary `pipeline::mesher_name`
/// returns. The vocabulary strings themselves are what the model was trained on
/// and what the CLI accepts, so they are never rewritten anywhere but here.
std::string_view mesher_plain(std::string_view mesher) {
    if (mesher == "graded_tet") {
        return "graded tets";
    }
    if (mesher == "hybrid_zoo") {
        return "hybrid: hex bulk, pyramid skin";
    }
    if (mesher == "hybrid_vem") {
        return "hybrid with polyhedral transitions";
    }
    if (mesher == "hex" || mesher == "hex_fill") {
        return "hexes";
    }
    if (mesher == "hex_vem") {
        return "hexes with polyhedral transitions";
    }
    if (mesher == "tet") {
        return "tets";
    }
    return mesher;
}

/// Cubic smoothstep on [0,1]. Used for opacity, the shrink collapse and the
/// sweep front only -- never for a displayed number.
double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}


ImU32 rgba(const std::array<float, 3>& rgb, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
}

ImU32 faded(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

/// Axis-aligned box of one tessellation region, in model coordinates (metres).
///
/// `pipeline::extract_case_features` takes fix/load context as
/// `RefineRegion` boxes, which is what the CLI's `--fix-box` / `--load-box`
/// supply by hand. The studio selects boundary conditions by face id instead,
/// so the box a GUI face contributes is the box of that face's own triangles.
/// Returns nullopt for a region with no triangles, so an empty selection
/// contributes nothing rather than a degenerate box at the origin.
std::optional<std::pair<Eigen::Vector3d, Eigen::Vector3d>>
region_box(const pipeline::Model& model, int region) {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
    bool any = false;
    const auto& tris = model.surface.triangles;
    for (std::size_t ti = 0; ti < tris.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        for (const std::uint32_t vi : tris[ti]) {
            const Eigen::Vector3d& p = model.surface.vertices[vi];
            if (any) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            } else {
                lo = p;
                hi = p;
                any = true;
            }
        }
    }
    if (!any) {
        return std::nullopt;
    }
    return std::make_pair(lo, hi);
}

/// Construction stages belonging to the initial fill, which is the fill the
/// build act shows. Later passes' stages exist in the same list (a fill per
/// adaptive remesh) but they are not the fill the decision produced, and the
/// closing act shows the mesh a later pass SOLVED rather than re-running the
/// same ten stage names a second time.
std::size_t initial_fill_stage_count(const std::vector<pipeline::MeshStage>& stages) {
    std::size_t n = 0;
    for (const auto& stage : stages) {
        if (stage.pass == 0) {
            ++n;
        }
    }
    return n;
}

/// Generates the closing act's beat sequence for `n` completed solve passes, in
/// the order the pipeline computed them, and hands each to `fn(stage, phase)`.
///
/// Per pass: stress plus its still hold. On the first pass, the recovered
/// gradient plus its hold. Every pass then shows the ZZ error field it actually
/// computed, even when that field met policy and no remesh followed; hiding a
/// final-pass estimator made a high-quality single-pass take look as if error
/// recovery did not exist. When another pass followed, the next solved mesh is
/// revealed and held. The last pass ends with the exact linear load ramp and
/// the longest final hold.
///
/// The gradient appears once because it explains a stress field rather than
/// advancing the adaptive loop. The error field appears on every pass because
/// it is the loop's decision input, including the decision to stop.
///
/// Generated rather than materialised so the per-frame cue costs no allocation:
/// it is walked twice, once to total the seconds and once to locate the beat.
template <class Fn> void for_each_solve_beat(std::size_t n, Fn&& fn) {
    if (n == 0) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto stage = static_cast<int>(i);
        fn(stage, SolvePhase::kStressSweep);
        fn(stage, SolvePhase::kStressHold);
        if (i == 0) {
            fn(stage, SolvePhase::kGradientSweep);
            fn(stage, SolvePhase::kGradientHold);
        }
        fn(stage, SolvePhase::kError);
        fn(stage, SolvePhase::kErrorHold);
        if (i + 1 < n) {
            fn(stage, SolvePhase::kRefine);
            fn(stage, SolvePhase::kRefineHold);
        }
    }
    const auto last = static_cast<int>(n) - 1;
    fn(last, SolvePhase::kLoadRamp);
    fn(last, SolvePhase::kHold);
}

} // namespace

const char* cinema_act_name(CinemaAct act) {
    switch (act) {
    case CinemaAct::kSkeleton:
        return "skeleton";
    case CinemaAct::kDeliberate:
        return "deliberate";
    case CinemaAct::kBuild:
        return "build";
    case CinemaAct::kMeshHold:
        return "mesh_hold";
    case CinemaAct::kSolve:
        return "solve";
    }
    return "unknown"; // only reachable from an out-of-range int cast
}

const char* cinema_solve_phase_name(SolvePhase phase) {
    switch (phase) {
    case SolvePhase::kNone:
        return "none";
    case SolvePhase::kStressSweep:
        return "stress_sweep";
    case SolvePhase::kStressHold:
        return "stress_hold";
    case SolvePhase::kGradientSweep:
        return "gradient_sweep";
    case SolvePhase::kGradientHold:
        return "gradient_hold";
    case SolvePhase::kError:
        return "error";
    case SolvePhase::kErrorHold:
        return "error_hold";
    case SolvePhase::kRefine:
        return "refine";
    case SolvePhase::kRefineHold:
        return "refine_hold";
    case SolvePhase::kLoadRamp:
        return "load_ramp";
    case SolvePhase::kHold:
        return "hold";
    }
    return "unknown"; // only reachable from an out-of-range int cast
}

bool cinema_phase_is_hold(SolvePhase phase) {
    switch (phase) {
    case SolvePhase::kStressHold:
    case SolvePhase::kGradientHold:
    case SolvePhase::kErrorHold:
    case SolvePhase::kRefineHold:
    case SolvePhase::kHold:
        return true;
    default:
        return false;
    }
}

CinemaType cinema_type(ImFont* font, float height) {
    // One scale for every size, from the frame height: the composition is a
    // fixed fraction of the frame, so a 720p take is the same film smaller and
    // not the same pixels in a smaller frame.
    const float s = std::max(0.4f, height / kCinemaRefHeight);
    CinemaType type;
    type.font = font;
    type.headline = std::floor(40.0f * s);
    type.numbers = std::floor(27.0f * s);
    type.note = std::floor(20.0f * s);
    type.footer = std::floor(17.0f * s);
    type.chapter = std::floor(21.0f * s);
    type.caption = std::floor(22.0f * s);
    type.label = std::floor(19.0f * s);
    type.legend = std::floor(17.0f * s);
    return type;
}

namespace {

CinemaMeshInsight inspect_mesh(const fea::NodalMesh& mesh) {
    CinemaMeshInsight out;
    for (const auto& element : mesh.elements) {
        const auto index = static_cast<std::size_t>(element.type);
        if (index < out.type_counts.size()) {
            ++out.type_counts[index];
        }
    }
    const fea::CellQualityStats quality = fea::summarize_cell_quality(mesh);
    out.quality_min = quality.min;
    out.quality_mean = quality.mean;
    out.quality_measured = quality.n_measured;
    out.quality_unmeasured = quality.n_unmeasured;
    return out;
}

} // namespace

// ---- take state -----------------------------------------------------------

void CinemaState::push_stage(const pipeline::MeshStage& stage) {
    // Worker thread. The stage's mesh is the worker's live buffer, so the copy
    // is mandatory, not a convenience.
    const std::lock_guard<std::mutex> lock(stage_mutex_);
    pending_stages_.push_back(stage);
}

void CinemaState::drain_stages() {
    std::vector<pipeline::MeshStage> pending;
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        if (pending_stages_.empty()) {
            return;
        }
        pending.swap(pending_stages_);
    }
    for (const auto& stage : pending) {
        stage_insights.push_back(inspect_mesh(stage.mesh));
    }
    stages.insert(stages.end(), std::make_move_iterator(pending.begin()),
                  std::make_move_iterator(pending.end()));
}

void CinemaState::clear_stages() {
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        pending_stages_.clear();
    }
    stages.clear();
    stage_insights.clear();
    invalidate_uploads();
}

void CinemaState::push_solve_stage(const pipeline::SolveStage& stage) {
    // Worker thread, same contract as push_stage: the callback's SolveResult is
    // the worker's own, so the copy is mandatory.
    const std::lock_guard<std::mutex> lock(stage_mutex_);
    pending_solve_stages_.push_back(stage);
}

void CinemaState::drain_solve_stages() {
    std::vector<pipeline::SolveStage> pending;
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        if (pending_solve_stages_.empty()) {
            return;
        }
        pending.swap(pending_solve_stages_);
    }
    for (const auto& stage : pending) {
        solve_insights.push_back(inspect_mesh(stage.result.volume_mesh));
    }
    solve_stages.insert(solve_stages.end(), std::make_move_iterator(pending.begin()),
                        std::make_move_iterator(pending.end()));
}

void CinemaState::adopt_final_result(const pipeline::SolveResult& result) {
    if (solve_stages.empty()) {
        return;
    }
    pipeline::SolveStage& stage = solve_stages.back();
    const bool finalisation_changed =
        stage.result.volume_mesh.nodes.size() != result.volume_mesh.nodes.size() ||
        stage.result.volume_mesh.elements.size() != result.volume_mesh.elements.size() ||
        stage.result.max_von_mises != result.max_von_mises;

    stage.final_pass = true;
    stage.trace.n_elems = result.volume_mesh.elements.size();
    stage.trace.n_nodes = result.volume_mesh.nodes.size();
    stage.trace.n_dof = static_cast<std::size_t>(result.displacement.size());
    stage.trace.global_eta = result.global_eta;
    if (!result.element_eta.empty()) {
        std::vector<double> eta = result.element_eta;
        std::sort(eta.begin(), eta.end());
        const auto at = [&](double q) {
            const std::size_t index = static_cast<std::size_t>(
                std::floor(q * static_cast<double>(eta.size() - 1)));
            return eta[index];
        };
        stage.trace.eta_p50 = at(0.50);
        stage.trace.eta_p90 = at(0.90);
        stage.trace.eta_max = eta.back();
    }
    if (finalisation_changed) {
        // The old marks were computed on the pre-promotion field. The final
        // result has been re-solved and recovered; no second marking pass ran,
        // so carrying the old mesh's counts onto it would be a false claim.
        stage.trace.n_h_mark = 0;
        stage.trace.n_p_mark = 0;
        stage.trace.n_shape_mark = 0;
    }
    stage.result = result;
    if (solve_insights.size() < solve_stages.size()) {
        solve_insights.resize(solve_stages.size());
    }
    solve_insights.back() = inspect_mesh(result.volume_mesh);
    gradients_.clear();
    invalidate_uploads();
}

void CinemaState::clear_solve_stages() {
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        pending_solve_stages_.clear();
    }
    solve_stages.clear();
    solve_insights.clear();
    gradients_.clear();
    invalidate_uploads();
}

void CinemaState::invalidate_uploads() {
    uploaded_mesh_source = CinemaMeshSource::kNone;
    uploaded_mesh_index = -1;
    uploaded_solve_stage = -1;
    uploaded_sizing_story = false;
}

void CinemaState::advance(double dt) {
    if (dt > 0.0) {
        t += dt;
    }
}

int CinemaState::chosen_frame() const {
#ifdef POLYMESH_WITH_ADVISOR
    if (!explanation || explanation->frames.empty()) {
        return -1;
    }
    const auto& frames = explanation->frames;
    // The pass that scored the recommended action, which is the one whose
    // activations belong beside the mesh that action produced. Falling back to
    // the last pass is not a guess about which candidate won: the last pass IS
    // the final re-score of the recommended action (`candidate == -1`), so on a
    // run where no candidate frame carries the flag it is still a pass over
    // exactly the action being built.
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].recommended) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(frames.size()) - 1;
#else
    return -1;
#endif
}

CinemaState::GradientCache& CinemaState::gradient_slot(std::size_t index) {
    if (gradients_.size() != solve_stages.size()) {
        gradients_.assign(solve_stages.size(), GradientCache{});
    }
    static GradientCache empty;
    if (index >= gradients_.size()) {
        empty = GradientCache{};
        empty.computed = true;
        return empty;
    }
    GradientCache& slot = gradients_[index];
    if (slot.computed) {
        return slot;
    }
    slot.computed = true;
    const pipeline::SolveResult& result = solve_stages[index].result;
    slot.values = fea::nodal_scalar_gradient_magnitude(result.volume_mesh, result.von_mises,
                                                       &slot.unresolved);
    for (const double v : slot.values) {
        slot.max = std::max(slot.max, v);
    }
    return slot;
}

const std::vector<double>& CinemaState::gradient_field(std::size_t index) {
    return gradient_slot(index).values;
}

std::size_t CinemaState::gradient_unresolved(std::size_t index) {
    return gradient_slot(index).unresolved;
}

double CinemaState::gradient_max(std::size_t index) { return gradient_slot(index).max; }

// ---- act sequencing -------------------------------------------------------

void cinema_act_window(const CinemaState& state, CinemaAct act, double& t0, double& t1) {
    const double total = std::max(state.duration, 1.0e-6);
    const auto want = static_cast<std::size_t>(act);
    double start = 0.0;
    for (std::size_t a = 0; a < kActFraction.size(); ++a) {
        const double span = kActFraction[a] * total;
        if (a == want) {
            t0 = start;
            t1 = start + span;
            return;
        }
        start += span;
    }
    t0 = total;
    t1 = total;
}

double cinema_opening_fade(const CinemaState& state) {
    const double skeleton_span = kActFraction[0] * std::max(state.duration, 1.0e-6);
    return std::min(CinemaState::kOpeningFade, 0.5 * skeleton_span);
}

double cinema_decision_lead(const CinemaState& state) {
    const double build_span = kActFraction[2] * std::max(state.duration, 1.0e-6);
    return std::min(CinemaState::kDecisionLead, 0.2 * build_span);
}

double cinema_panel_fade(const CinemaState& state) {
    const double solve_span = kActFraction[4] * std::max(state.duration, 1.0e-6);
    return std::min(CinemaState::kPanelFade, 0.1 * solve_span);
}

double cinema_progress(const CinemaState& state) {
    return std::clamp(state.t / std::max(state.duration, 1.0e-6), 0.0, 1.0);
}

CinemaCue cinema_cue(const CinemaState& state) {
    CinemaCue cue;
    const double total = std::max(state.duration, 1.0e-6);

    double start = 0.0;
    for (std::size_t a = 0; a < kActFraction.size(); ++a) {
        const double span = kActFraction[a] * total;
        const bool last = a + 1 == kActFraction.size();
        if (state.t < start + span || last) {
            cue.act = static_cast<CinemaAct>(a);
            cue.act_span = span;
            // Clamped, not wrapped: a clock that overran the schedule holds the
            // final instant of the act it is in.
            cue.act_t = std::clamp(state.t - start, 0.0, span);
            break;
        }
        start += span;
    }

    // Open on the exact CAD alone, then slide the spectral analysis in during
    // the first sixth of the act. The curve trace is fully open for roughly six
    // seconds on the default take, long enough to follow raw curvature into its
    // denoised signal and sizing statistics.
    if (cue.act == CinemaAct::kSkeleton) {
        const double wait = 0.08 * cue.act_span;
        const double slide = 0.14 * cue.act_span;
        cue.panel_open = static_cast<float>(
            smoothstep((cue.act_t - wait) / std::max(slide, 1.0e-9)));
        const double p = cue.act_t / std::max(cue.act_span, 1.0e-9);
        cue.spectral_edge_reveal = smoothstep((p - 0.08) / 0.20);
        cue.spectral_spectrum_reveal = smoothstep((p - 0.24) / 0.22);
        cue.spectral_filter_mix = smoothstep((p - 0.42) / 0.25);
        cue.spectral_field_reveal = smoothstep((p - 0.56) / 0.30);
        cue.spectral_overlay_alpha = static_cast<float>(
            smoothstep((p - 0.05) / 0.08) *
            (1.0 - smoothstep((p - 0.96) / 0.04)));
        cue.network_alpha = 0.0f;
        cue.equations_alpha = 0.0f;
    } else if (cue.act == CinemaAct::kSolve) {
        // The panel's content swaps: the network has said everything it has to
        // say by the time the answer starts arriving, and what the closing act
        // is actually doing is arithmetic that can be written down. A dissolve,
        // not a cut, and not a layout change.
        const double fade = cinema_panel_fade(state);
        const auto x = static_cast<float>(smoothstep(cue.act_t / std::max(fade, 1.0e-9)));
        cue.network_alpha = 1.0f - x;
        cue.equations_alpha = x;
    }

    // ---- lane 1: the advisor's forward passes ----------------------------
    // One beat per real forward pass, in the order the chooser ran them, inside
    // the deliberation act. From the build act onward the lane STOPS on the pass
    // that scored the recommended action and holds it, which is what makes the
    // lit graph and the growing mesh the same event rather than two things
    // happening beside each other.
    std::size_t n_frames = 0;
#ifdef POLYMESH_WITH_ADVISOR
    if (state.explanation) {
        n_frames = state.explanation->frames.size();
    }
#endif
    if (n_frames > 0) {
        const double lane_t0 = kActFraction[0] * total;
        const double lane_t1 = (kActFraction[0] + kActFraction[1]) * total;
        const double scan_t1 = lane_t0 + 0.65 * (lane_t1 - lane_t0);
        const double beat = (scan_t1 - lane_t0) / static_cast<double>(n_frames);
        cue.pass_beat_seconds = beat;
        if (cue.act == CinemaAct::kSkeleton) {
            cue.frame_index = -1;
        } else if (cue.act == CinemaAct::kDeliberate && state.t < scan_t1) {
            const auto i =
                static_cast<std::size_t>((state.t - lane_t0) / std::max(beat, 1.0e-9));
            cue.frame_index = static_cast<int>(std::min(i, n_frames - 1));
            cue.pass_lane_live = true;
        } else {
            cue.frame_index = state.chosen_frame();
            cue.chosen_pass_held = true;
        }
    }

    // The ranking's outcome may be stated from the first frame of the build act,
    // which `cinema_decision_lead` guarantees is before the first element of the
    // fill appears.
    cue.decision_locked = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kMeshHold ||
                          cue.act == CinemaAct::kSolve;

    // ---- lane 2: the mesher's own construction stages -------------------
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    if (n_fill > 0) {
        const double lead = cinema_decision_lead(state);
        const double span = std::max(kActFraction[2] * total - lead, 1.0e-6);
        const double beat = span / static_cast<double>(n_fill);
        cue.stage_beat_seconds = beat;
        if (cue.act == CinemaAct::kBuild) {
            const double x = cue.act_t - lead;
            if (x >= 0.0) {
                const auto i = static_cast<std::size_t>(x / beat);
                const std::size_t clamped = std::min(i, n_fill - 1);
                cue.stage_index = static_cast<int>(clamped);
                // Linear in time on purpose: the on-screen spawn rate is then
                // proportional to the stage's own element count, so a stage that
                // added twice as many elements visibly takes twice the work.
                const double within = x - static_cast<double>(clamped) * beat;
                cue.stage_reveal = std::clamp(within / beat, 0.0, 1.0);
                cue.mesh_source = CinemaMeshSource::kFillStage;
                cue.mesh_source_index = cue.stage_index;
            }
        } else if (cue.act == CinemaAct::kMeshHold) {
            // Hold the authoritative mesh the first solve consumed. On a
            // quadratic CAD run this is the promoted tet10/hex20 node set, not
            // the linear construction scaffold wearing final counts.
            cue.stage_index = static_cast<int>(n_fill - 1);
            cue.stage_reveal = 1.0;
            if (!state.solve_stages.empty()) {
                cue.mesh_source = CinemaMeshSource::kSolvedPass;
                cue.mesh_source_index = 0;
            } else {
                cue.mesh_source = CinemaMeshSource::kFillStage;
                cue.mesh_source_index = cue.stage_index;
            }
        } else if (cue.act == CinemaAct::kSolve) {
            // The finished fill stays named in the caption. No mesh source: the
            // closing act draws fields out of `solve_stages`, and the one beat
            // that uses the per-element buffer is kRefine, which points it at
            // the refined mesh itself.
            cue.stage_index = static_cast<int>(n_fill - 1);
            cue.stage_reveal = 1.0;
        }
    }

    // ---- the closing act: the real solve / estimate / refine loop --------
    if (cue.act == CinemaAct::kSolve && !state.solve_stages.empty()) {
        const std::size_t n_solve = state.solve_stages.size();
        double seconds_total = 0.0;
        for_each_solve_beat(
            n_solve, [&](int, SolvePhase phase) { seconds_total += beat_seconds(phase); });
        const double scale = cue.act_span / std::max(seconds_total, 1.0e-9);
        double at = 0.0;
        int beat_stage = 0;
        SolvePhase beat_phase = SolvePhase::kStressSweep;
        double beat_t0 = 0.0;
        double beat_span = 1.0;
        for_each_solve_beat(n_solve, [&](int stage, SolvePhase phase) {
            const double span = beat_seconds(phase) * scale;
            // The beats are contiguous, so the last one whose start is at or
            // before the clock is the one the clock is inside — and a clock that
            // overran the act holds the final beat rather than wrapping.
            if (cue.act_t >= at) {
                beat_stage = stage;
                beat_phase = phase;
                beat_t0 = at;
                beat_span = span;
            }
            at += span;
        });
        cue.solve_stage_index = beat_stage;
        cue.solve_phase = beat_phase;
        cue.solve_phase_span = beat_span;
        cue.solve_phase_t = std::clamp(cue.act_t - beat_t0, 0.0, beat_span);
        const double x = cue.solve_phase_t / std::max(beat_span, 1.0e-9);
        switch (beat_phase) {
        case SolvePhase::kStressSweep:
        case SolvePhase::kGradientSweep:
            // Eased, because this front is a camera move over a finished field
            // and not a physical quantity: nothing on screen is labelled with
            // it, and a linear front starts and stops with a visible jerk. The
            // colours it uncovers are the pass's own values throughout.
            cue.field_front = smoothstep(x);
            break;
        case SolvePhase::kRefine:
            cue.refine_reveal = std::clamp(x, 0.0, 1.0);
            cue.mesh_source = CinemaMeshSource::kSolvedPass;
            cue.mesh_source_index = beat_stage + 1;
            break;
        case SolvePhase::kRefineHold:
            cue.refine_reveal = 1.0;
            cue.mesh_source = CinemaMeshSource::kSolvedPass;
            cue.mesh_source_index = beat_stage + 1;
            break;
        case SolvePhase::kLoadRamp:
            // Linear, never eased. λ is a number on screen and u(λ) = λ·u is
            // exact, so easing λ would give the deforming shape a rate of change
            // the linear solve does not have — which is the one thing a ramp
            // labelled "exact linear response" may not do.
            cue.load_factor = std::clamp(x, 0.0, 1.0);
            break;
        default:
            break;
        }
    }
    return cue;
}

Viewport::CinemaView cinema_view(const CinemaState& state, const CinemaCue& cue) {
    Viewport::CinemaView view;
    view.edges = true;
    view.edge_alpha = kMeshEdgeAlpha;
    view.edge_width = kMeshEdgeWidth;
    // Elements land shrunk toward their own centroid so each reads as a separate
    // cell as it appears, and close up to touching partway through the beat.
    // Geometry, not data: the element is the element.
    const auto shrink_for = [](double reveal) {
        return static_cast<float>(
            kRevealShrink * (1.0 - smoothstep(std::min(1.0, reveal / kRevealShrinkFraction))));
    };
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        view.skeleton_alpha = static_cast<float>(
            smoothstep(state.t / std::max(cinema_opening_fade(state), 1.0e-9)));
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    case CinemaAct::kDeliberate:
        // Nothing has been meshed while the network deliberates, so nothing may
        // be drawn as mesh. The outline is the whole picture here.
        view.skeleton_alpha = 1.0f;
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        break;
    case CinemaAct::kBuild:
        // The outline stays as a dim reference frame so the growing fill can be
        // read against the part it is filling. Through the DECISION lead-in the
        // cue has selected no stage yet, and a reveal of 0 draws no element:
        // the fill may not start before the action it executes is readable.
        view.skeleton_alpha = 0.45f;
        view.reveal = cue.stage_index >= 0 ? static_cast<float>(cue.stage_reveal) : 0.0f;
        view.mesh_alpha = 1.0f;
        view.shrink = cue.stage_index >= 0 ? shrink_for(cue.stage_reveal) : 1.0f;
        break;
    case CinemaAct::kMeshHold: {
        // A six-second result beat can do more than freeze: open the finished
        // cells just enough to expose their topology, hold that microscope
        // view, then close them and leave the exact delivered mesh still for
        // the final fifth. The subject never rotates or changes data.
        const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
        double exploded = 0.0;
        if (x < 0.18) {
            exploded = smoothstep(x / 0.18);
        } else if (x < 0.55) {
            exploded = 1.0;
        } else if (x < 0.78) {
            exploded = 1.0 - smoothstep((x - 0.55) / 0.23);
        }
        view.skeleton_alpha = 0.30f;
        view.reveal = 1.0f;
        view.mesh_alpha = 1.0f;
        view.shrink = static_cast<float>(0.10 * exploded);
        view.edge_alpha = kMeshEdgeAlpha + static_cast<float>(0.24 * exploded);
        break;
    }
    case CinemaAct::kSolve:
        if (cue.solve_phase == SolvePhase::kRefine ||
            cue.solve_phase == SolvePhase::kRefineHold) {
            // Persistent cells remain rendered and briefly open by 0.06 toward
            // their own centroids. The first 40% collapses/fades removed cells;
            // the remaining 60% reveals only replacements in next-mesh order.
            view.skeleton_alpha = 0.35f;
            const double added =
                smoothstep((std::clamp(cue.refine_reveal, 0.0, 1.0) - 0.40) / 0.60);
            view.reveal = static_cast<float>(added);
            view.mesh_alpha = 1.0f;
            view.shrink = 0.0f;
            view.incremental_transition = true;
            view.transition_progress = static_cast<float>(cue.refine_reveal);
        } else {
            // Every other beat of this act renders a field, so these values are
            // only reached when no `pipeline::SolveStage` arrived at all and the
            // act holds the finished fill instead. The caption says which.
            view.skeleton_alpha = 0.25f;
            view.reveal = 1.0f;
            view.mesh_alpha = 1.0f;
            view.shrink = 0.0f;
        }
        break;
    }
    return view;
}

CinemaRender cinema_render(CinemaState& state, const CinemaCue& cue,
                           double base_deform_scale) {
    CinemaRender out;
    if (cue.act != CinemaAct::kSolve || cue.solve_stage_index < 0 ||
        static_cast<std::size_t>(cue.solve_stage_index) >= state.solve_stages.size()) {
        return out; // kCinema, undeformed; `result_max` is unused there
    }
    const auto index = static_cast<std::size_t>(cue.solve_stage_index);
    const pipeline::SolveResult& result = state.solve_stages[index].result;
    const auto arm_sweep = [&](bool moving) {
        out.sweep.active = true;
        out.sweep.axis = state.sweep_axis;
        out.sweep.feather = kSweepFeather;
        out.sweep.front =
            moving ? static_cast<float>(cue.field_front) * (1.0f + kSweepFeather) : 1.0f;
    };
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
    case SolvePhase::kStressHold:
        // That pass's own von Mises, on the geometry that pass solved. Zero
        // exaggeration: these beats are about the field, and the shape's real
        // response is what the load ramp below is for.
        out.mode = DisplayMode::kResultsVonMises;
        out.result_max = static_cast<float>(result.max_von_mises);
        arm_sweep(cue.solve_phase == SolvePhase::kStressSweep);
        break;
    case SolvePhase::kGradientSweep:
    case SolvePhase::kGradientHold: {
        // |∇σ_vm| recovered from that same field by
        // `fea::nodal_scalar_gradient_magnitude`. When the recovery produced
        // nothing there is no gradient to draw, so the beat keeps the stress
        // field on screen and the caption says the gradient is unavailable —
        // a gradient this module invented would be exactly the fabrication
        // this whole surface exists to rule out.
        const double gmax = state.gradient_max(index);
        if (state.gradient_field(index).empty() || !(gmax > 0.0)) {
            out.mode = DisplayMode::kResultsVonMises;
            out.result_max = static_cast<float>(result.max_von_mises);
        } else {
            out.mode = DisplayMode::kResultsGradient;
            out.result_max = static_cast<float>(gmax);
        }
        arm_sweep(cue.solve_phase == SolvePhase::kGradientSweep);
        break;
    }
    case SolvePhase::kError:
    case SolvePhase::kErrorHold:
        // The ZZ error field recovered from that same solve — the field that
        // decided where the next pass refined.
        out.mode = DisplayMode::kResultsError;
        out.result_max = static_cast<float>(result.max_nodal_eta);
        break;
    case SolvePhase::kRefine:
    case SolvePhase::kRefineHold:
        break; // kCinema: these beats draw the refined MESH, not a field
    case SolvePhase::kLoadRamp:
    case SolvePhase::kHold: {
        out.mode = DisplayMode::kResultsVonMises;
        const double lambda = std::clamp(cue.load_factor, 0.0, 1.0);
        out.deform_scale = static_cast<float>(lambda * base_deform_scale);
        // The stress scales with the load exactly as the displacement does, so
        // the colour has to ramp with the shape or the frame would show a fully
        // stressed part that has barely moved. The scalar buffer stays the
        // pass's own field and the MAXIMUM is divided by λ instead, which makes
        // the drawn colour λ·s / s_max: the λ-scaled field against a fixed
        // full-load legend, with no second copy of the field anywhere.
        out.result_max =
            static_cast<float>(result.max_von_mises / std::max(lambda, kMinLoadFactor));
        break;
    }
    case SolvePhase::kNone:
        break;
    }
    // A field whose maximum is zero would divide the colormap by zero. One is
    // the neutral denominator and leaves every value where it is.
    if (!(out.result_max > 0.0f)) {
        out.result_max = 1.0f;
    }
    return out;
}

const char* cinema_solver_token(const CinemaState& state) {
    if (state.solve_stages.empty()) {
        return "no_solve_stage";
    }
    // The solver's own words first. `fea::SolveOptions::on_note` is the only
    // channel the linear solver has, and it speaks on the CG path and on a
    // memory-budget downgrade; below `cg_threshold` with the budget satisfied it
    // says nothing at all, which is the normal outcome at this project's DOF
    // counts and is exactly the case on the film's part.
    for (const auto& stage : state.solve_stages) {
        if (stage.solver_note.find("CG") != std::string::npos ||
            stage.solver_note.find("cg") != std::string::npos) {
            return "cg";
        }
    }
    // No note. That is not a licence to guess: it is a fact with a consequence.
    // `fea::select_solve_method` sends `kAuto` to CG only when the FREE DOF count
    // exceeds `SolveOptions::cg_threshold`, the free set is a subset of the
    // mesh's DOF, and the one override that could have changed the choice is the
    // memory-budget downgrade — which emits a note. So a silent solve whose total
    // DOF is below the threshold was factorised, and nothing was inferred beyond
    // arithmetic on two real numbers.
    const fea::SolveOptions defaults;
    for (const auto& stage : state.solve_stages) {
        if (stage.trace.n_dof == 0 ||
            static_cast<Eigen::Index>(stage.trace.n_dof) > defaults.cg_threshold) {
            return "note_absent";
        }
    }
    return "direct_ldlt";
}

void sync_cinema_viewport(CinemaState& state, const CinemaCue& cue, const CinemaRender& render,
                          Viewport& viewport) {
    if (!state.uploaded_sizing_story) {
        // Empty vectors deliberately clear the previous take's VBO when this
        // input supplied no spectral story. A failed extractor must never leave
        // the last part's spacing rings hovering over the new skeleton.
        viewport.set_cinema_sizing_samples(
            state.sizing.field_points, state.sizing.field_h_before,
            state.sizing.field_h_after, state.sizing.edge_points,
            state.sizing.edge_h_before, state.sizing.edge_h_after);
        state.uploaded_sizing_story = true;
    }
    // Per-element buffer: re-uploaded only when the cue names a different mesh,
    // because `set_cinema_mesh` rebuilds every element's own faces.
    if (cue.mesh_source != CinemaMeshSource::kNone && cue.mesh_source_index >= 0 &&
        (cue.mesh_source != state.uploaded_mesh_source ||
         cue.mesh_source_index != state.uploaded_mesh_index)) {
        const auto i = static_cast<std::size_t>(cue.mesh_source_index);
        bool uploaded = false;
        if (cue.mesh_source == CinemaMeshSource::kFillStage && i < state.stages.size()) {
            viewport.set_cinema_mesh(state.stages[i].mesh);
            uploaded = true;
        } else if (cue.mesh_source == CinemaMeshSource::kSolvedPass &&
                   i < state.solve_stages.size()) {
            if (i > 0) {
                viewport.set_cinema_mesh_transition(
                    state.solve_stages[i - 1].result.volume_mesh,
                    state.solve_stages[i].result.volume_mesh);
            } else {
                viewport.set_cinema_mesh(state.solve_stages[i].result.volume_mesh);
            }
            uploaded = true;
        }
        if (uploaded) {
            state.uploaded_mesh_source = cue.mesh_source;
            state.uploaded_mesh_index = cue.mesh_source_index;
        }
    }
    // Result buffers: one re-bake per adaptive pass, so the field drawn beside
    // "pass 0" is the field pass 0 produced and not the final answer relabelled.
    // The recovered gradient rides along on the same upload — it is a per-node
    // field of that same pass and interpolating it onto the boundary samples is
    // the same work the von Mises field already pays for.
    if (cue.solve_stage_index >= 0 && cue.solve_stage_index != state.uploaded_solve_stage &&
        static_cast<std::size_t>(cue.solve_stage_index) < state.solve_stages.size()) {
        const auto i = static_cast<std::size_t>(cue.solve_stage_index);
        const std::vector<double>& gradient = state.gradient_field(i);
        viewport.set_result(state.solve_stages[i].result,
                            gradient.empty() ? nullptr : &gradient);
        state.uploaded_solve_stage = cue.solve_stage_index;
    }
    viewport.set_field_sweep(render.sweep);
    viewport.set_cinema_view(cinema_view(state, cue));
}

// ---- act 1: the part's own skeleton --------------------------------------

namespace {

/// Direction the closing act's field reveals travel in, and one sentence saying
/// how it was chosen.
///
/// The front must START at the loaded end: a stress field that appears from the
/// clamped end and creeps toward the load is a picture of nothing. So the axis
/// is the resultant force direction, and its SIGN is decided by where the loaded
/// faces actually are — the mean projection of their own triangles against the
/// midpoint of the part's projection. That is a comparison of two measured
/// numbers, not an assumption about which face of a part is usually loaded.
///
/// With no load there is no load axis, and the longest bounding-box edge is the
/// one honest choice left: it is the direction the part is longest in, the
/// caption says that is what it is, and nothing about it is presented as
/// physics.
std::pair<Eigen::Vector3f, std::string> resolve_sweep_axis(const pipeline::Model& model,
                                                           const pipeline::SimSetup& setup) {
    Eigen::Vector3d force = Eigen::Vector3d::Zero();
    Eigen::Vector3d load_centre = Eigen::Vector3d::Zero();
    int load_boxes = 0;
    for (const auto& [face, load] : setup.loads) {
        force += load.force;
        if (const auto box = region_box(model, face)) {
            load_centre += 0.5 * (box->first + box->second);
            ++load_boxes;
        }
    }
    const Eigen::Vector3d span = model.bbox_max - model.bbox_min;
    if (force.norm() <= 0.0 || load_boxes == 0) {
        int longest = 0;
        for (int i = 1; i < 3; ++i) {
            if (span[i] > span[longest]) {
                longest = i;
            }
        }
        Eigen::Vector3f axis = Eigen::Vector3f::Zero();
        axis[longest] = 1.0f;
        return {axis, "this take has no load case, so the reveal runs along the part's "


                      "longest axis — a camera move, not a direction of anything physical"};
    }
    Eigen::Vector3d axis = force.normalized();
    load_centre /= static_cast<double>(load_boxes);
    // Flip so that the loaded end is at the LOW end of the projection, which is
    // where the viewport's own sweep front starts.
    const double mid = 0.5 * axis.dot(model.bbox_min + model.bbox_max);
    if (axis.dot(load_centre) > mid) {
        axis = -axis;
    }
    return {axis.cast<float>(),
            "the reveal travels from the loaded faces toward the far end, along the "
            "resultant of every SimSetup::LoadSpec::force in this take"};
}

void capture_curve_spectrum(CinemaSizingStory& sizing) {
    sizing.curve_spectrum.clear();
    sizing.curve_mode_kept.clear();
    if (sizing.stations.size() < 3 ||
        sizing.stations.size() != sizing.curvature_raw.size()) {
        return;
    }

    std::size_t n = 8;
    while (n < sizing.curvature_raw.size()) {
        n <<= 1;
    }
    const auto interpolate = [&](double station) {
        const auto it =
            std::upper_bound(sizing.stations.begin(), sizing.stations.end(), station);
        if (it == sizing.stations.begin()) {
            return sizing.curvature_raw.front();
        }
        if (it == sizing.stations.end()) {
            return sizing.curvature_raw.back();
        }
        const std::size_t hi =
            static_cast<std::size_t>(it - sizing.stations.begin());
        const std::size_t lo = hi - 1;
        const double t = (station - sizing.stations[lo]) /
                         (sizing.stations[hi] - sizing.stations[lo]);
        return sizing.curvature_raw[lo] +
               t * (sizing.curvature_raw[hi] - sizing.curvature_raw[lo]);
    };

    // This is the exact even-reflection preparation used by
    // geom::lowpass_signal before its FFT.
    std::vector<std::complex<double>> spectrum(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        const double station =
            static_cast<double>(i) / static_cast<double>(n - 1);
        const std::complex<double> value{interpolate(station), 0.0};
        spectrum[i] = value;
        spectrum[2 * n - 1 - i] = value;
    }
    geom::fft_inplace(spectrum, false);
    std::vector<std::complex<double>> kept = spectrum;
    (void)geom::truncate_modes(kept, 0.995);

    // A real input has a conjugate-symmetric spectrum. Plot one half, including
    // DC and Nyquist, so frequency increases monotonically across the panel
    // instead of showing the mirrored half twice.
    const std::size_t visible_modes = n + 1;
    sizing.curve_spectrum.reserve(visible_modes);
    sizing.curve_mode_kept.reserve(visible_modes);
    for (std::size_t k = 0; k < visible_modes; ++k) {
        sizing.curve_spectrum.push_back(std::abs(spectrum[k]));
        sizing.curve_mode_kept.push_back(
            static_cast<std::uint8_t>(k == 0 || std::norm(kept[k]) > 0.0));
    }
}

void capture_curve_story(CinemaState& state, const geom::CadTopology& topology) {
    state.sizing.edge_id = 0;
    state.sizing.edge_length = 0.0;
    state.sizing.curve_modes_total = 0;
    state.sizing.curve_modes_kept = 0;
    state.sizing.curve_energy_fraction = 0.0;
    state.sizing.stations.clear();
    state.sizing.curvature_raw.clear();
    state.sizing.curvature_filtered.clear();
    state.sizing.curve_spectrum.clear();
    state.sizing.curve_mode_kept.clear();
    state.sizing.edge_points.clear();
    state.sizing.edge_h_before.clear();
    state.sizing.edge_h_after.clear();
    double best_score = -1.0;
    for (const auto& edge : topology.edges) {
        if (edge.samples.size() < 3 || edge.kappa_samples.size() != edge.samples.size()) {
            continue;
        }
        std::vector<double> stations(edge.samples.size(), 0.0);
        for (std::size_t i = 1; i < edge.samples.size(); ++i) {
            stations[i] = stations[i - 1] + (edge.samples[i] - edge.samples[i - 1]).norm();
        }
        if (!(stations.back() > 0.0)) {
            continue;
        }
        for (double& station : stations) {
            station /= stations.back();
        }
        geom::FilterReport report;
        std::vector<double> filtered =
            geom::lowpass_signal(stations, edge.kappa_samples, 0.995, &report);
        if (filtered.size() != edge.kappa_samples.size()) {
            continue;
        }
        const auto [lo, hi] =
            std::minmax_element(edge.kappa_samples.begin(), edge.kappa_samples.end());
        double mean = 0.0;
        for (const double kappa : edge.kappa_samples) {
            mean += std::fabs(kappa);
        }
        mean /= static_cast<double>(edge.kappa_samples.size());
        // Prefer a genuinely varying curvature trace; when every curved edge is
        // analytic-constant, still choose the strongest real curve rather than
        // drawing an invented spectrum.
        const double score =
            edge.length * (std::fabs(*hi - *lo) + 0.05 * mean);
        if (score <= best_score) {
            continue;
        }
        best_score = score;
        state.sizing.edge_id = edge.id;
        state.sizing.edge_length = edge.length;
        state.sizing.curve_modes_total = report.modes_total;
        state.sizing.curve_modes_kept = report.modes_kept;
        state.sizing.curve_energy_fraction =
            report.energy_total > 0.0 ? report.energy_kept / report.energy_total : 1.0;
        state.sizing.stations = std::move(stations);
        state.sizing.curvature_raw = edge.kappa_samples;
        state.sizing.curvature_filtered = std::move(filtered);
        state.sizing.edge_points = edge.samples;
    }
    capture_curve_spectrum(state.sizing);
}
} // namespace

void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model,
                           const pipeline::SimSetup& setup, Viewport& viewport) {
    std::vector<std::vector<Eigen::Vector3d>> polylines;
    state.skeleton_note.clear();
    state.skeleton_source = SkeletonSource::kNone;
    state.sizing = CinemaSizingStory{};

    if (model.cad && !model.cad->empty()) {
        try {
            // 32 samples are the product sizing pass's own density. They keep
            // the outline smooth at 1080p and give the opening feature panel
            // the exact curvature trace the FFT denoiser receives.
            const geom::CadTopology topology = geom::extract_topology(*model.cad, 32);
            capture_curve_story(state, topology);
            polylines.reserve(topology.edges.size());
            for (const auto& edge : topology.edges) {
                if (edge.samples.size() >= 2) {
                    polylines.push_back(edge.samples);
                }
            }
            state.skeleton_source = SkeletonSource::kBrepEdges;
        } catch (const std::exception& e) {
            // A build without OpenCASCADE, or a BRep the extractor rejects.
            // Both are real conditions with a real message; neither licenses
            // falling back to the tessellation while claiming BRep edges.
            state.skeleton_source = SkeletonSource::kUnavailable;
            state.skeleton_note = e.what();
        }
    } else {
        try {
            // STL input carries no BRep. The crease network of the tessellation
            // is a different measurement of the same part; it is labelled as
            // such on screen and never called a BRep skeleton.
            const auto sharp = geom::detect_sharp_edges(model.surface, 30.0);
            polylines.reserve(sharp.size());
            for (const auto& edge : sharp) {
                if (edge.v0 < model.surface.vertices.size() &&
                    edge.v1 < model.surface.vertices.size()) {
                    polylines.push_back(
                        {model.surface.vertices[edge.v0], model.surface.vertices[edge.v1]});
                }
            }
            state.skeleton_source = SkeletonSource::kSharpEdges;
        } catch (const std::exception& e) {
            state.skeleton_source = SkeletonSource::kUnavailable;
            state.skeleton_note = e.what();
        }
    }

    state.skeleton_polylines = polylines.size();
    state.skeleton_points = 0;
    for (const auto& line : polylines) {
        state.skeleton_points += line.size();
    }
    viewport.set_skeleton(polylines);

    auto [axis, note] = resolve_sweep_axis(model, setup);
    state.sweep_axis = axis;
    state.sweep_note = std::move(note);
}

void prepare_cinema_features(CinemaState& state, const pipeline::Model& model,
                             const pipeline::SimSetup& setup) {
    std::vector<pipeline::RefineRegion> regions;
    regions.reserve(setup.fixtures.size() + setup.loads.size());
    for (const int face : setup.fixtures) {
        if (const auto box = region_box(model, face)) {
            regions.push_back({box->first, box->second, 0.5});
        }
    }
    for (const auto& [face, load] : setup.loads) {
        (void)load;
        if (const auto box = region_box(model, face)) {
            regions.push_back({box->first, box->second, 0.25});
        }
    }

    state.uploaded_sizing_story = false;
    state.sizing.field_points.clear();
    state.sizing.field_h_before.clear();
    state.sizing.field_h_after.clear();
    state.sizing.edge_h_before.clear();
    state.sizing.edge_h_after.clear();
    state.sizing.sampled_h_before_min = 0.0;
    state.sizing.sampled_h_before_max = 0.0;
    state.sizing.sampled_h_after_min = 0.0;
    state.sizing.sampled_h_after_max = 0.0;
    try {
        double h = setup.mesh_size;
        if (!(h > 0.0)) {
            h = pipeline::resolve_mesh_size(model, h, 30.0, setup.max_elems, setup.max_dof,
                                            setup.p_elevate)
                    .h;
        }
        const pipeline::RefinementPlan plan =
            pipeline::build_refinement_plan(model, h, regions, setup.use_feature_grading,
                                            setup.spectral_smooth, 0);
        std::optional<pipeline::RefinementPlan> baseline_plan;
        if (setup.spectral_smooth && plan.spectral.applied) {
            baseline_plan = pipeline::build_refinement_plan(
                model, h, regions, setup.use_feature_grading, false, 0);
        }
        const pipeline::RefinementPlan& baseline =
            baseline_plan ? *baseline_plan : plan;

        state.sizing.prepared = true;
        state.sizing.brep_curvature = plan.geometry_curvature_from_brep;
        state.sizing.geometry_seeds = plan.n_geometry_seeds;
        state.sizing.bc_seeds = plan.n_bc_seeds;
        state.sizing.h_min = plan.h_min;
        state.sizing.spectral = plan.spectral;

        const auto valid_h = [](double value) {
            return std::isfinite(value) && value > 0.0;
        };
        const auto append_sample =
            [&](const Eigen::Vector3d& point, std::vector<Eigen::Vector3d>& points,
                std::vector<double>& before, std::vector<double>& after) {
                if (!baseline.size_field || !plan.size_field) {
                    return;
                }
                const double h_before = baseline.size_field(point);
                const double h_after = plan.size_field(point);
                if (!valid_h(h_before) || !valid_h(h_after)) {
                    return;
                }
                points.push_back(point);
                before.push_back(h_before);
                after.push_back(h_after);
            };

        // Bound the point pass while retaining deterministic coverage of the
        // whole tessellated surface. The selected BRep edge is stored
        // separately and always keeps all of its samples.
        constexpr std::size_t kMaxFieldSamples = 1800;
        const std::size_t stride = std::max<std::size_t>(
            1, (model.surface.vertices.size() + kMaxFieldSamples - 1) /
                   kMaxFieldSamples);
        for (std::size_t i = 0; i < model.surface.vertices.size(); i += stride) {
            append_sample(model.surface.vertices[i], state.sizing.field_points,
                          state.sizing.field_h_before, state.sizing.field_h_after);
        }
        std::vector<Eigen::Vector3d> accepted_edge_points;
        accepted_edge_points.reserve(state.sizing.edge_points.size());
        for (const auto& point : state.sizing.edge_points) {
            append_sample(point, accepted_edge_points, state.sizing.edge_h_before,
                          state.sizing.edge_h_after);
        }
        state.sizing.edge_points = std::move(accepted_edge_points);

        const auto range = [](const std::vector<double>& values) {
            if (values.empty()) {
                return std::pair{0.0, 0.0};
            }
            const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
            return std::pair{*lo, *hi};
        };
        std::tie(state.sizing.sampled_h_before_min,
                 state.sizing.sampled_h_before_max) =
            range(state.sizing.field_h_before);
        std::tie(state.sizing.sampled_h_after_min,
                 state.sizing.sampled_h_after_max) =
            range(state.sizing.field_h_after);
    } catch (const std::exception&) {
        // The worker remains authoritative and will report the actual failure.
        // The feature panel simply declines to draw a report it could not
        // compute; it never blocks the solve or substitutes plausible values.
        state.sizing.prepared = false;
        state.sizing.spectral = {};
        state.sizing.field_points.clear();
        state.sizing.field_h_before.clear();
        state.sizing.field_h_after.clear();
        state.sizing.edge_h_before.clear();
        state.sizing.edge_h_after.clear();
    }

    const auto& spectral = state.sizing.spectral;
    std::printf("cinema: spectral applied %d modes_total %zu modes_kept %zu "
                "energy_kept %.9g edge_seeds %zu predicted_before %.9g "
                "predicted_after %.9g geometry_seeds %zu bc_seeds %zu brep_curvature %d "
                "field_samples %zu h_before_min %.9g h_before_max %.9g "
                "h_after_min %.9g h_after_max %.9g\n",
                spectral.applied ? 1 : 0, spectral.modes_total, spectral.modes_kept,
                spectral.energy_kept, spectral.n_edge_curve_seeds, spectral.predicted_before,
                spectral.predicted_after, state.sizing.geometry_seeds, state.sizing.bc_seeds,
                state.sizing.brep_curvature ? 1 : 0, state.sizing.field_points.size(),
                state.sizing.sampled_h_before_min, state.sizing.sampled_h_before_max,
                state.sizing.sampled_h_after_min, state.sizing.sampled_h_after_max);
    std::fflush(stdout);
}

// ---- act 2: the network ---------------------------------------------------

bool load_cinema_advisor(CinemaState& state, const pipeline::Model& model,
                         pipeline::SimSetup& setup, const std::string& dir) {
    state.advisor_dir = dir;
    state.advisor_note.clear();
    state.decision_applied = false;
    state.decision_note.clear();

    auto unavailable = [&state, &dir](std::string why) {
        state.advisor_note = std::move(why);
        state.decision_note =
            "no decision was applied: the mesh act runs on the studio's own setup";
        std::printf("cinema: advisor unavailable %s: %s\n", dir.c_str(),
                    state.advisor_note.c_str());
        std::fflush(stdout);
        return false;
    };

#ifndef POLYMESH_WITH_ADVISOR
    (void)model;
    (void)setup;
    return unavailable(
        "this polymesh-gui was configured with POLYMESH_WITH_ADVISOR=OFF, so it "
        "carries no inference module at all — reconfigure with "
        "-DPOLYMESH_WITH_ADVISOR=ON");
#else
    state.explanation.reset();
    state.layout = advisor::NetworkLayout{};

    if (model.surface.triangles.empty()) {
        return unavailable(
            "no part is loaded, so there is no feature row to run the network on");
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(std::filesystem::path{dir}, ec)) {
        return unavailable(std::format("no such model directory: {}", dir));
    }

    // The feature row is built exactly the way the product path builds it
    // (apps/cli/main.cpp cmd_solve): fix/load context as region boxes, then
    // pipeline::extract_case_features, then advisor::to_columns inside
    // Advisor::explain. The studio selects by face id rather than by box, so
    // each selected face contributes the box of its own triangles.
    std::vector<pipeline::RefineRegion> fix_regions;
    std::vector<pipeline::RefineRegion> load_regions;
    for (const int face : setup.fixtures) {
        if (const auto box = region_box(model, face)) {
            fix_regions.push_back({box->first, box->second, 0.5});
        }
    }
    Eigen::Vector3d load_dir = Eigen::Vector3d::Zero();
    for (const auto& [face, load] : setup.loads) {
        if (const auto box = region_box(model, face)) {
            load_regions.push_back({box->first, box->second, 0.25});
        }
        load_dir += load.force;
    }
    if (load_dir.norm() > 0.0) {
        load_dir.normalize();
    }

    try {
        const auto features = pipeline::extract_case_features(model, fix_regions, load_regions,
                                                              load_dir, setup.poissons_ratio);
        const advisor::Advisor advisor(dir);
        if (!advisor.has_activations()) {
            return unavailable("model dir exports no activation taps — re-export with "
                               "scripts/advisor/export_onnx.py");
        }
        state.layout = advisor.layout();
        state.explanation = advisor.explain(features, static_cast<double>(setup.max_dof));
    } catch (const std::exception& e) {
        return unavailable(e.what());
    }

    const auto& explanation = *state.explanation;
    const auto& decision = explanation.decision;

    // Applying the decision is what makes the causal claim true: the mesher
    // must execute the action the network chose, or the video would be showing
    // two unrelated things side by side. A refusal is NOT applied -- it is a
    // real outcome and is shown as one.
    if (decision.vetoed) {
        const std::string why =
            decision.note.empty()
                ? (decision.budget_refusal ? "no candidate fit the degrees-of-freedom budget"
                                           : "the feasibility gate declined the prediction")
                : decision.note;
        state.decision_note = std::format(
            "advisor abstained: {}; the verified fallback below stays unchanged", why);
    } else if (const auto mesher = pipeline::mesher_from_name(decision.mesher)) {
        const double diag = (model.bbox_max - model.bbox_min).norm();
        setup.mesher = *mesher;
        setup.mesh_size = std::max(decision.h_rel * diag, 1e-9);
        setup.adapt_passes = decision.adapt_passes;
        setup.eta_target = decision.eta_target;
        // The solve path has one p-elevation step (tet4/hex8 -> tet10/hex20),
        // so an order above 2 is executed as quadratic. Same mapping the CLI
        // applies, and the HUD reports the executed order, not the asked one.
        setup.p_elevate = decision.p_elevate || decision.order >= 2;
        state.decision_applied = true;
        state.decision_note = std::format(
            "applied to the run below: {} at {:.3g} mm cells, {} refinement pass{}, "
            "order {}",
            mesher_plain(decision.mesher), setup.mesh_size * 1e3, setup.adapt_passes,
            setup.adapt_passes == 1 ? "" : "es", setup.p_elevate ? 2 : 1);
    } else {
        // Same refusal the CLI makes: meshing something other than what was
        // recommended, while reporting the recommendation, is the failure mode
        // this whole surface exists to rule out.
        state.decision_note = std::format(
            "NOT applied: this build does not recognise the advised mesher '{}', so the mesh "
            "below is the studio's own setup rather than something else meshed silently",
            decision.mesher);
    }

    // One candidate per enumerated action, then the final re-score pass, so the
    // candidate count is the frame count minus that last pass.
    const std::size_t n_frames = explanation.frames.size();
    const std::size_t candidates = n_frames > 0 ? n_frames - 1 : 0;
    std::printf("cinema: advisor %s candidates %zu gate_threshold %.6g frames %zu decision %s "
                "h_rel %.6g order %d adapt_passes %d eta_target %.6g vetoed %d "
                "ood_distance %.6g applied %d\n",
                dir.c_str(), candidates, explanation.gate_threshold, n_frames,
                decision.mesher.c_str(), decision.h_rel, decision.order, decision.adapt_passes,
                decision.eta_target, decision.vetoed ? 1 : 0, decision.ood_distance,
                state.decision_applied ? 1 : 0);
    std::fflush(stdout);
    return true;
#endif
}

// ---- the left panel -------------------------------------------------------

namespace {

/// One run of text inside a drawn line: the size multiplier and the baseline
/// offset are what give this surface real subscripts and superscripts without
/// requiring the font atlas to carry the Unicode ones. Liberation Sans, the
/// first fallback face on Linux, does not have U+2081 or U+207B, and an
/// equation that renders "sigma-box-box" is worse than no equation.
struct Run {
    std::string text;
    float scale = 1.0f; // of the line's size
    float rise = 0.0f;  // of the line's size; negative is up
    ImVec4 color{1, 1, 1, 1};
};

using Runs = std::vector<Run>;

Run plain(std::string text, ImVec4 color) { return {std::move(text), 1.0f, 0.0f, color}; }
Run sub(std::string text, ImVec4 color) { return {std::move(text), 0.62f, 0.30f, color}; }
Run sup(std::string text, ImVec4 color) { return {std::move(text), 0.62f, -0.34f, color}; }

float runs_width(ImFont* font, float size, const Runs& runs) {
    float w = 0.0f;
    for (const auto& run : runs) {
        w += font->CalcTextSizeA(size * run.scale, FLT_MAX, 0.0f, run.text.c_str()).x;
    }
    return w;
}

void draw_runs(ImDrawList* dl, ImFont* font, float size, ImVec2 at, const Runs& runs,
               float alpha) {
    float x = at.x;
    for (const auto& run : runs) {
        const float s = size * run.scale;
        dl->AddText(font, s, ImVec2(x, at.y + run.rise * size + (size - s) * 0.5f),
                    faded(run.color, alpha), run.text.c_str());
        x += font->CalcTextSizeA(s, FLT_MAX, 0.0f, run.text.c_str()).x;
    }
}

/// One group on the equation board: what it is for in words, the relations
/// themselves, and the live numbers this beat puts against them.
struct EquationGroup {
    const char* title;
    std::vector<Runs> lines;
    Runs live; // empty unless this group is the lit one
    bool lit = false;
};

} // namespace

void draw_cinema_equations(const CinemaState& state, const CinemaCue& cue,
                           const CinemaType& type, const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();

    const ImVec4 lit = palette.text;
    const ImVec4 dim = palette.text_dim;
    const ImVec4 hot = palette.accent;

    // Which group this beat is computing. One switch, so the equation that is
    // lit and the field on screen cannot come from two different opinions.
    const SolvePhase phase = cue.solve_phase;
    const bool on_stress =
        phase == SolvePhase::kStressSweep || phase == SolvePhase::kStressHold;
    const bool on_gradient =
        phase == SolvePhase::kGradientSweep || phase == SolvePhase::kGradientHold;
    const bool on_error = phase == SolvePhase::kError || phase == SolvePhase::kErrorHold;
    const bool on_refine = phase == SolvePhase::kRefine || phase == SolvePhase::kRefineHold;
    const bool on_ramp = phase == SolvePhase::kLoadRamp || phase == SolvePhase::kHold;

    const pipeline::SolveStage* stage = nullptr;
    if (cue.solve_stage_index >= 0 &&
        static_cast<std::size_t>(cue.solve_stage_index) < state.solve_stages.size()) {
        stage = &state.solve_stages[static_cast<std::size_t>(cue.solve_stage_index)];
    }

    std::vector<EquationGroup> groups;

    // 1. The system. fea/assembly.hpp:21-32 (K_e = integral of B^T D B), and
    //    fea/solve.hpp:136 plus solve.cpp:574-595 for the eliminated system the
    //    factorisation actually receives.
    {
        EquationGroup g{"linear system", {}, {}, on_stress || on_ramp};
        g.lines.push_back({plain("K u = f", g.lit ? hot : lit)});
        Runs ke{plain("K", dim), sub("e", dim), plain(" = ∫ B", dim), sup("T", dim),
                plain(" D B dV", dim)};
        g.lines.push_back(std::move(ke));
        if (g.lit && stage != nullptr) {
            const std::string_view token = cinema_solver_token(state);
            const char* method = token == "direct_ldlt"
                                     ? "factorised once"
                                     : (token == "cg" ? "conjugate gradient"
                                                      : "solver method not reported");
            g.live = {plain(fmt("%s unknowns, held supports eliminated, %s",
                                grouped(stage->trace.n_dof).c_str(), method),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // 2. Strain and stress. B has unhalved shear rows (engineering strain), so
    //    D's shear block carries mu and not 2 mu — fea/src/assembly.cpp:39-60,
    //    fea/src/material.cpp:6-18.
    {
        EquationGroup g{"strain + stress", {}, {}, on_stress || on_ramp};
        g.lines.push_back({plain("ε = B u", g.lit ? lit : dim),
                           plain("        σ = D(E,ν) ε", g.lit ? lit : dim)});
        g.lines.push_back({plain("λ = Eν / (1+ν)(1−2ν),   μ = E / 2(1+ν)", dim)});
        if (g.lit) {
            g.live = {plain(fmt("E = %.6g GPa · ν = %.6g", hud.youngs_modulus / 1e9,
                                hud.poissons_ratio),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // 3. Von Mises, exactly as fea/src/stress.cpp:177-183 computes it.
    {
        EquationGroup g{"von Mises stress", {}, {}, on_stress || on_ramp};
        Runs first{plain("σ", g.lit ? hot : lit),          sub("vm", g.lit ? hot : lit),
                   plain(" = √( ½[(σ", g.lit ? hot : lit), sub("11", g.lit ? hot : lit),
                   plain("−σ", g.lit ? hot : lit),         sub("22", g.lit ? hot : lit),
                   plain(")", g.lit ? hot : lit),          sup("2", g.lit ? hot : lit),
                   plain(" + …]", g.lit ? hot : lit)};
        Runs second{plain("+ 3(σ", g.lit ? hot : lit), sub("12", g.lit ? hot : lit),
                    sup("2", g.lit ? hot : lit),       plain(" + σ", g.lit ? hot : lit),
                    sub("23", g.lit ? hot : lit),      sup("2", g.lit ? hot : lit),
                    plain(" + σ", g.lit ? hot : lit),  sub("13", g.lit ? hot : lit),
                    sup("2", g.lit ? hot : lit),       plain(") )", g.lit ? hot : lit)};
        g.lines.push_back(std::move(first));
        g.lines.push_back(std::move(second));
        if (g.lit && stage != nullptr) {
            g.live = {plain(
                fmt("peak %.4g MPa on this pass", stage->result.max_von_mises / 1e6), hot)};
        }
        groups.push_back(std::move(g));
    }

    // 4. The recovered gradient — the definition of what
    //    fea::nodal_scalar_gradient_magnitude actually fits.
    {
        EquationGroup g{"stress gradient", {}, {}, on_gradient};
        g.lines.push_back({plain("|∇σ", g.lit ? hot : dim), sub("vm", g.lit ? hot : dim),
                           plain("|,  g = argmin Σ (σ", g.lit ? hot : dim),
                           sub("j", g.lit ? hot : dim), plain("−σ", g.lit ? hot : dim),
                           sub("i", g.lit ? hot : dim), plain("− g·d)", g.lit ? hot : dim),
                           sup("2", g.lit ? hot : dim)});
        g.lines.push_back({plain("least squares over each node's own cells", dim)});
        if (g.lit && cue.solve_stage_index >= 0) {
            const auto i = static_cast<std::size_t>(cue.solve_stage_index);
            // gradient_max is cached; this is a lookup, not a recovery.
            const double gmax = const_cast<CinemaState&>(state).gradient_max(i);
            g.live = {plain(gmax > 0.0 ? fmt("steepest %.4g MPa per mm", gmax / 1e9)
                                       : std::string("no gradient could be recovered here"),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // 5. Zienkiewicz-Zhu, as fea/include/fea/zz.hpp:27-35 defines it and
    //    fea/src/zz.cpp:270-286 computes it.
    {
        EquationGroup g{"ZZ error estimate", {}, {}, on_error};
        g.lines.push_back({plain("η", g.lit ? hot : dim), sub("e", g.lit ? hot : dim),
                           plain(" = ‖σ* − σ", g.lit ? hot : dim), sub("h", g.lit ? hot : dim),
                           plain("‖", g.lit ? hot : dim), sub("E,e", g.lit ? hot : dim),
                           plain(" / ‖σ", g.lit ? hot : dim), sub("h", g.lit ? hot : dim),
                           plain("‖", g.lit ? hot : dim), sub("E,Ω", g.lit ? hot : dim)});
        g.lines.push_back({plain("‖σ‖", dim), sub("E", dim), sup("2", dim),
                           plain(" = ∫ σ", dim), sup("T", dim), plain(" D", dim),
                           sup("−1", dim), plain(" σ dV", dim)});
        if (g.lit && stage != nullptr) {
            g.live = {plain(fmt("%.3g%% estimated, target %.3g%%",
                                stage->trace.global_eta * 100.0, hud.eta_target * 100.0),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // 6. Dörfler set selection — adapt/src/error.cpp:11-38, applied to the
    //    h-marked set at adapt/src/hp_driver.cpp:377-394.
    {
        EquationGroup g{"adaptive marking", {}, {}, on_refine};
        g.lines.push_back({plain("mark the largest η", g.lit ? hot : dim),
                           sub("e", g.lit ? hot : dim), plain(" until Σ η", g.lit ? hot : dim),
                           sub("e", g.lit ? hot : dim), sup("2", g.lit ? hot : dim),
                           plain(" ≥ θ Σ η", g.lit ? hot : dim), sup("2", g.lit ? hot : dim)});
        if (g.lit && stage != nullptr) {
            g.live = {plain(fmt("%s cells marked for smaller cells",
                                grouped(stage->trace.n_h_mark).c_str()),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // 7. The ramp. Established from the code rather than assumed: K depends on
    //    geometry and material only, both solve paths are linear operators on
    //    the right-hand side, and von Mises is homogeneous of degree one in
    //    stress — so scaling f scales u and sigma by exactly the same factor.
    {
        EquationGroup g{"linear load response", {}, {}, on_ramp};
        // Written with an English connective rather than ⇒: the double arrow is
        // missing from Liberation Sans, and "gives" reads better than either a
        // merged glyph or a second → in a line that already has three.
        g.lines.push_back(
            {plain("f → λf   gives   u → λu   and   σ → λσ", g.lit ? hot : dim)});
        if (g.lit) {
            g.live = {plain(fmt("λ = %.3f — %.4g N of %.4g N", cue.load_factor,
                                cue.load_factor * hud.load_newtons, hud.load_newtons),
                            hot)};
        }
        groups.push_back(std::move(g));
    }

    // ---- layout: title, lines, live row, per group ----------------------
    const float eq_size = type.caption;
    const float title_size = type.label;
    const float line_h = std::floor(eq_size * 1.42f);
    const float title_h = std::floor(title_size * 1.55f);
    const float group_gap = std::floor(eq_size * 0.75f);
    const float rule_w = 4.0f;
    const float indent = rule_w + std::floor(eq_size * 0.7f);
    const float wrap = std::max(120.0f, region.x - indent);

    float total = 0.0f;
    for (const auto& g : groups) {
        total += title_h + static_cast<float>(g.lines.size()) * line_h +
                 (g.live.empty() ? 0.0f : line_h) + group_gap;
    }
    // Set the whole board smaller rather than dropping a group: the board is
    // what the closing act is FOR, and a group that is not on screen is a
    // relation the film silently stopped claiming.
    const float squeeze =
        total > region.y && total > 0.0f ? std::max(0.55f, region.y / total) : 1.0f;

    float y = origin.y;
    for (const auto& g : groups) {
        const float ts = std::floor(title_size * squeeze);
        const float es = std::floor(eq_size * squeeze);
        const float th = title_h * squeeze;
        const float lh = line_h * squeeze;
        const float group_top = y;
        dl->AddText(
            font, ts, ImVec2(origin.x + indent, y),
            faded(g.lit ? palette.accent : palette.text_dim, alpha * (g.lit ? 1.0f : 0.5f)),
            g.title);
        y += th;
        const float body_alpha = alpha * (g.lit ? 1.0f : 0.30f);
        for (const auto& line : g.lines) {
            // Fit rather than clip: a relation with its right-hand side cut off
            // is a different claim from the one this file made.
            const float w = runs_width(font, es, line);
            const float s = w > wrap && w > 0.0f ? es * wrap / w : es;
            draw_runs(dl, font, s, ImVec2(origin.x + indent, y), line, body_alpha);
            y += lh;
        }
        if (!g.live.empty()) {
            const float w = runs_width(font, es, g.live);
            const float s = w > wrap && w > 0.0f ? es * wrap / w : es;
            draw_runs(dl, font, s, ImVec2(origin.x + indent, y), g.live, alpha);
            y += lh;
        }
        if (g.lit) {
            // The lit group carries a rule down its left edge: at a glance it
            // says which one of seven relations the picture on the right is.
            dl->AddRectFilled(ImVec2(origin.x, group_top),
                              ImVec2(origin.x + rule_w, y - group_gap * 0.2f),
                              faded(palette.accent, alpha), 1.5f);
        }
        y += group_gap * squeeze;
    }
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_network(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                         float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
    (void)type;
    ImGui::TextColored(palette.status_warn,
                       "no network to draw: this polymesh-gui was built with "
                       "POLYMESH_WITH_ADVISOR=OFF");
    ImGui::TextWrapped("%s", state.advisor_note.c_str());
#else
    if (!state.explanation || state.layout.empty()) {
        ImGui::TextColored(palette.status_warn, "no advisor forward pass to draw");
        ImGui::Spacing();
        if (state.advisor_note.empty()) {
            ImGui::TextWrapped(
                "run `cinema advisor <model dir>` — until then nothing is drawn here, because "
                "the only honest picture of a network that has not run is an empty one");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, palette.status_err);
            ImGui::TextWrapped("%s", state.advisor_note.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    const advisor::NetworkLayout& layout = state.layout;
    const auto& frames = state.explanation->frames;
    if (layout.layers.size() != 4 || layout.edges.size() != 3) {
        ImGui::TextColored(
            palette.status_err,
            "activation_layout.json describes %zu layers and %zu weight blocks; "
            "this surface draws the four-column trunk (input / trunk.fc1 / "
            "trunk.fc2 / heads) and its three blocks, so it will not guess",
            layout.layers.size(), layout.edges.size());
        return;
    }

    // The four activation vectors of the pass this beat is showing. Null before
    // the first beat: the structure is drawn, unlit, and said to be unlit.
    std::array<const std::vector<float>*, 4> values{nullptr, nullptr, nullptr, nullptr};
    const advisor::ActivationFrame* frame = nullptr;
    if (cue.frame_index >= 0 && static_cast<std::size_t>(cue.frame_index) < frames.size()) {
        frame = &frames[static_cast<std::size_t>(cue.frame_index)];
        values = {&frame->input, &frame->fc1, &frame->fc2, &frame->heads};
        for (std::size_t l = 0; l < 4; ++l) {
            if (values[l]->size() != layout.layers[l].size) {
                ImGui::TextColored(
                    palette.status_err,
                    "layer '%s' is %zu units in activation_layout.json but the "
                    "graph tap returned %zu — the artifacts disagree, so nothing "
                    "is drawn",
                    layout.layers[l].name.c_str(), layout.layers[l].size, values[l]->size());
                return;
            }
        }
    }

    // Per-layer normalisation. Trunk and head magnitudes differ by roughly a
    // factor of ten, so one shared scale would flatten the trunk into a flat
    // grey column — the same reason scripts/advisor/figures.py scales the
    // activation heatmap per row.
    std::array<float, 4> layer_max{1.0f, 1.0f, 1.0f, 1.0f};
    for (std::size_t l = 0; l < values.size(); ++l) {
        if (values[l] == nullptr) {
            continue;
        }
        float m = 0.0f;
        for (const float v : *values[l]) {
            m = std::max(m, std::fabs(v));
        }
        layer_max[l] = m > 0.0f ? m : 1.0f;
    }

    // Connections ranked by |w_ji * a_i| for THIS frame: a large weight on a
    // silent unit carries nothing, so weight alone would be the wrong ranking.
    std::size_t total_connections = 0;
    for (const auto& block : layout.edges) {
        total_connections += block.rows * block.cols;
    }
    auto& picks = state.edge_scratch_;
    picks.clear();
    std::size_t drawn = 0;
    float value_max = 0.0f;
    if (frame != nullptr && total_connections > 0) {
        picks.reserve(total_connections);
        for (std::size_t b = 0; b < layout.edges.size(); ++b) {
            const auto& block = layout.edges[b];
            if (block.weights.size() != block.rows * block.cols ||
                block.cols != values[b]->size() || block.rows != values[b + 1]->size()) {
                continue; // a block that does not match the layers it joins is not drawn
            }
            const auto& src = *values[b];
            for (std::size_t j = 0; j < block.rows; ++j) {
                const float* row = block.weights.data() + j * block.cols;
                for (std::size_t i = 0; i < block.cols; ++i) {
                    const float v = row[i] * src[i];
                    value_max = std::max(value_max, std::fabs(v));
                    picks.push_back({std::fabs(v), v, static_cast<int>(b), static_cast<int>(i),
                                     static_cast<int>(j)});
                }
            }
        }
        drawn = std::min(kDrawnConnections, picks.size());
        if (drawn > 0) {
            std::nth_element(picks.begin(),
                             picks.begin() + static_cast<std::ptrdiff_t>(drawn) - 1,
                             picks.end(),
                             [](const CinemaState::EdgePick& a,
                                const CinemaState::EdgePick& b) { return a.rank > b.rank; });
            // Weakest of the kept set first, so the strongest connections end
            // up on top instead of buried under near-silent ones.
            std::sort(picks.begin(), picks.begin() + static_cast<std::ptrdiff_t>(drawn),
                      [](const CinemaState::EdgePick& a, const CinemaState::EdgePick& b) {
                          return a.rank < b.rank;
                      });
        }
    }

    // ---- what the panel says, before anything is placed ------------------
    // Two lines, not six. The exhaustive version of these disclosures — every
    // normalisation constant, every edge block shape, the exact colormap — is
    // in docs/assets/cinema/NOTES.md, which the strip names on screen. Six
    // stacked paragraphs at 13 px was a wall nobody read; two at 22 px is a
    // caption people do.
    const std::string caption =
        cue.chosen_pass_held
            ? std::string("the forward pass that chose this mesh — the deployed network's own "
                          "tensors")
            : std::string("the deployed network scoring one candidate mesh per beat — its own "
                          "tensors, not a re-implementation");
    const std::string legend =
        frame == nullptr
            ? std::string("structure only: no forward pass is being shown on this beat")
            : fmt("%zu strongest of %zu connections · circle size is how hard a unit is "
                  "firing · blue is negative, red positive · head names in plain language",
                  drawn, total_connections);

    // ---- geometry, derived from the measured text ------------------------
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    const auto para_h = [&](float size, const std::string& s) {
        return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str()).y +
               std::floor(size * 0.35f);
    };
    const float caption_h = para_h(type.caption, caption);
    const float legend_h = para_h(type.legend, legend);
    dl->AddText(font, type.caption, origin, faded(palette.text, alpha), caption.c_str(),
                nullptr, wrap);

    const float graph_top = origin.y + caption_h;
    const float graph_h = std::max(140.0f, region.y - caption_h - legend_h);
    dl->AddText(font, type.legend, ImVec2(origin.x, graph_top + graph_h),
                faded(palette.text_dim, alpha), legend.c_str(), nullptr, wrap);

    // Head labels are real unit names carrying real values. Set them at
    // whatever size makes the longest one fit the gutter the panel can afford,
    // then reserve exactly that gutter — so no label is ever cut off.
    const auto& heads = layout.layers[3];
    std::vector<std::string> head_text(heads.size);
    for (std::size_t i = 0; i < heads.size; ++i) {
        const std::string_view name = i < heads.labels.size() ? head_name(heads.labels[i])
                                                              : std::string_view("(unnamed)");
        head_text[i] = values[3] != nullptr ? std::format("{} {:+.4g}", name, (*values[3])[i])
                                            : std::format("{} —", name);
    }
    const auto widest_label = [&](float size) {
        float w = 0.0f;
        for (const auto& t : head_text) {
            w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x);
        }
        return w;
    };
    float label_size = type.label;
    float label_w = widest_label(label_size);
    const float label_budget = std::max(80.0f, region.x * 0.46f);
    if (label_w > label_budget && label_w > 0.0f) {
        label_size = std::max(8.0f, std::floor(label_size * label_budget / label_w));
        label_w = widest_label(label_size);
    }

    constexpr float kColumnMargin = 24.0f;
    const float gutter = label_w + kNodeRadiusMax + kLabelGap + 4.0f;
    const float band_w = std::max(120.0f, region.x - kColumnMargin - gutter);
    const float header_h = std::floor(type.legend * 1.5f);
    const float col_top = graph_top + header_h;
    const float col_h = std::max(60.0f, graph_h - header_h - 4.0f);

    // Columns span the whole band rather than sitting in quarter-slots: the
    // extra separation is what makes an individual connection followable.
    const auto column_x = [&](std::size_t c) {
        return origin.x + kColumnMargin +
               band_w * static_cast<float>(c) / static_cast<float>(values.size() - 1);
    };
    const auto node_y = [&](std::size_t i, std::size_t n) {
        return col_top + col_h * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    };

    // ---- connections ----------------------------------------------------
    if (drawn > 0) {
        const float inv_max = value_max > 0.0f ? 1.0f / value_max : 0.0f;
        for (std::size_t k = 0; k < drawn; ++k) {
            const auto& pick = picks[k];
            const auto b = static_cast<std::size_t>(pick.block);
            const float t = std::clamp(pick.value * inv_max, -1.0f, 1.0f);
            const float weight = std::clamp(pick.rank * inv_max, 0.0f, 1.0f);
            // Sign is the colour (the same signed_colormap the nodes use);
            // opacity AND width are the normalised magnitude, so the paths that
            // actually carry this pass are the ones that read, and the rest are
            // hairlines instead of a uniform grey haze.
            dl->AddLine(ImVec2(column_x(b), node_y(static_cast<std::size_t>(pick.src),
                                                   layout.layers[b].size)),
                        ImVec2(column_x(b + 1), node_y(static_cast<std::size_t>(pick.dst),
                                                       layout.layers[b + 1].size)),
                        rgba(signed_colormap(t), (0.05f + 0.85f * weight) * alpha),
                        0.55f + 1.35f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.3f;
    static constexpr std::array<const char*, 4> kColumnPlain{
        {"what it measures", "hidden layer 1", "hidden layer 2", "what it predicts"}};
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float x = column_x(l);
        const float spacing = col_h / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 1.8f, kNodeRadiusMax);
        const std::string header = std::format("{} · {}", kColumnPlain[l], layer.size);
        const float header_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, header.c_str()).x;
        dl->AddText(font, type.legend, ImVec2(x - 0.5f * header_w, graph_top),
                    faded(palette.text_dim, alpha), header.c_str());
        for (std::size_t i = 0; i < layer.size; ++i) {
            const float a = values[l] != nullptr ? (*values[l])[i] : 0.0f;
            const float mag = std::clamp(std::fabs(a) / layer_max[l], 0.0f, 1.0f);
            const float r = kNodeMin + (r_max - kNodeMin) * mag;
            const float y = node_y(i, layer.size);
            const auto rgb = signed_colormap(a / layer_max[l]);
            // Halo on the units carrying this pass, so the firing pattern is
            // legible over the connections. Opacity only: the radius is still
            // |a| / max|a| within the layer and nothing else.
            if (mag > 0.30f) {
                dl->AddCircleFilled(ImVec2(x, y), r * 3.0f, rgba(rgb, 0.055f * mag * alpha));
                dl->AddCircleFilled(ImVec2(x, y), r * 1.8f, rgba(rgb, 0.110f * mag * alpha));
            }
            dl->AddCircleFilled(ImVec2(x, y), r, rgba(rgb, (0.40f + 0.60f * mag) * alpha));
            if (mag > 0.55f) {
                dl->AddCircle(ImVec2(x, y), r + 1.4f,
                              ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.45f * mag * alpha)),
                              0, 1.2f);
            }
        }
    }

    // ---- head units, with their plain names and their real values --------
    {
        const float head_r = std::clamp(
            0.46f * col_h / static_cast<float>(std::max<std::size_t>(heads.size, 1)), 1.8f,
            kNodeRadiusMax);
        const float x = column_x(3) + head_r + kLabelGap;
        // The unit the DECISION came out of, named as such. Without it a reader
        // sees seventeen numbers and no answer; with it the panel and the mesh
        // on the right are visibly the same event.
        int winner = -1;
        if (frame != nullptr) {
            const std::string want =
                std::string("policy_mesher_logit_") + frame->action.mesher;
            for (std::size_t i = 0; i < heads.labels.size(); ++i) {
                if (heads.labels[i] == want) {
                    winner = static_cast<int>(i);
                    break;
                }
            }
        }
        for (std::size_t i = 0; i < heads.size; ++i) {
            const bool lit = static_cast<int>(i) == winner;
            const float y = node_y(i, heads.size);
            if (lit) {
                const float w =
                    font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, head_text[i].c_str()).x;
                dl->AddRectFilled(ImVec2(x - 5.0f, y - 0.62f * label_size),
                                  ImVec2(x + w + 5.0f, y + 0.62f * label_size),
                                  faded(palette.accent_mid, 0.35f * alpha), 3.0f);
            }
            dl->AddText(font, label_size, ImVec2(x, node_y(i, heads.size) - 0.5f * label_size),
                        faded(lit ? palette.accent : palette.text, alpha),
                        head_text[i].c_str());
        }
    }

    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
#endif
}

namespace {

std::string mesh_mix_text(const CinemaMeshInsight& insight) {
    std::string out;
    for (std::size_t i = 0; i < insight.type_counts.size(); ++i) {
        const std::size_t count = insight.type_counts[i];
        if (count == 0) {
            continue;
        }
        if (!out.empty()) {
            out += " · ";
        }
        const auto type = static_cast<fea::ElementType>(i);
        out += std::format("{} {}", grouped(count), fea::element_type_name(type));
    }
    return out.empty() ? std::string("no cells measured") : out;
}

void draw_cinema_features(const CinemaState& state, const CinemaCue& cue,
                          const CinemaType& type, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                "Exact curvature → frequency modes → target spacing");
    const std::string summary =
        state.sizing.spectral.applied
            ? fmt("%s / %s field modes · %.2f%% energy · N density %.0f → %.0f",
                  grouped(state.sizing.spectral.modes_kept).c_str(),
                  grouped(state.sizing.spectral.modes_total).c_str(),
                  100.0 * state.sizing.spectral.energy_kept,
                  state.sizing.spectral.predicted_before,
                  state.sizing.spectral.predicted_after)
            : std::string("spectral sizing report arrives with the verified mesh setup");
    dl->AddText(font, type.label, ImVec2(origin.x, origin.y + type.caption * 1.55f),
                faded(state.sizing.spectral.applied ? palette.accent : palette.text_dim, alpha),
                summary.c_str(), nullptr, wrap);
    const std::string rings =
        fmt("%s on-part samples · ring diameter = target h · orange fine → cyan coarse",
            grouped(state.sizing.field_points.size()).c_str());
    dl->AddText(font, type.legend, ImVec2(origin.x, origin.y + type.caption * 2.55f),
                faded(palette.text_dim, alpha), rings.c_str(), nullptr, wrap);

    const float chart_top = origin.y + type.caption * 3.65f;
    const float chart_h = std::max(240.0f, std::min(region.y * 0.54f, 420.0f));
    const float chart_w = std::max(120.0f, region.x);
    dl->AddRectFilled(ImVec2(origin.x, chart_top),
                      ImVec2(origin.x + chart_w, chart_top + chart_h),
                      faded(palette.panel_bg, 0.52f * alpha), 6.0f);
    dl->AddRect(ImVec2(origin.x, chart_top),
                ImVec2(origin.x + chart_w, chart_top + chart_h),
                faded(palette.border, 0.8f * alpha), 6.0f);

    const float pad = 13.0f;
    const float split_y = chart_top + chart_h * 0.58f;
    dl->AddLine(ImVec2(origin.x + pad, split_y),
                ImVec2(origin.x + chart_w - pad, split_y),
                faded(palette.border, 0.72f * alpha), 1.0f);
    dl->AddText(font, type.legend, ImVec2(origin.x + pad, chart_top + 8.0f),
                faded(palette.text_dim, alpha), "SIGNAL DOMAIN  κ(s) along selected CAD edge");
    dl->AddText(font, type.legend, ImVec2(origin.x + pad, split_y + 7.0f),
                faded(palette.text_dim, alpha),
                "FREQUENCY DOMAIN  |FFT(κ − mean κ)| · teal retained, grey discarded");

    const auto& raw = state.sizing.curvature_raw;
    const auto& filtered = state.sizing.curvature_filtered;
    const auto& stations = state.sizing.stations;
    if (raw.size() >= 2 && filtered.size() == raw.size() && stations.size() == raw.size()) {
        double lo = raw.front();
        double hi = raw.front();
        for (std::size_t i = 0; i < raw.size(); ++i) {
            lo = std::min({lo, raw[i], filtered[i]});
            hi = std::max({hi, raw[i], filtered[i]});
        }
        const double span = std::max(hi - lo, 1.0e-12);
        const float plot_top = chart_top + type.legend * 1.65f;
        const float plot_bottom = split_y - 10.0f;
        const auto point = [&](double station, double value) {
            const float x = origin.x + pad +
                            (chart_w - 2.0f * pad) * static_cast<float>(station);
            const float y = plot_bottom -
                            (plot_bottom - plot_top) * static_cast<float>((value - lo) / span);
            return ImVec2(x, y);
        };

        const double revealed =
            cue.spectral_edge_reveal * static_cast<double>(raw.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(revealed)), raw.size() - 1);
        for (std::size_t i = 1; i <= whole; ++i) {
            dl->AddLine(point(stations[i - 1], raw[i - 1]),
                        point(stations[i], raw[i]),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double y0 = raw[i - 1] +
                              cue.spectral_filter_mix * (filtered[i - 1] - raw[i - 1]);
            const double y1 =
                raw[i] + cue.spectral_filter_mix * (filtered[i] - raw[i]);
            dl->AddLine(point(stations[i - 1], y0), point(stations[i], y1),
                        faded(palette.accent, alpha), 2.6f);
            dl->AddCircleFilled(point(stations[i], y1), 2.6f,
                                faded(palette.accent_soft_top, alpha));
        }
        if (whole + 1 < raw.size()) {
            const double part = revealed - static_cast<double>(whole);
            const double station =
                stations[whole] + part * (stations[whole + 1] - stations[whole]);
            const double raw_value =
                raw[whole] + part * (raw[whole + 1] - raw[whole]);
            const double filtered_value =
                filtered[whole] + part * (filtered[whole + 1] - filtered[whole]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            dl->AddLine(point(stations[whole], raw[whole]), point(station, raw_value),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double start = raw[whole] +
                                 cue.spectral_filter_mix *
                                     (filtered[whole] - raw[whole]);
            dl->AddLine(point(stations[whole], start), point(station, value),
                        faded(palette.accent, alpha), 2.6f);
            const ImVec2 scan = point(station, value);
            dl->AddLine(ImVec2(scan.x, plot_top), ImVec2(scan.x, plot_bottom),
                        faded(palette.accent_soft_top, 0.5f * alpha), 1.0f);
            dl->AddCircleFilled(scan, 4.2f, faded(palette.accent_soft_top, alpha));
        }
        const std::string edge =
            fmt("edge %u · %.3g mm · %s/%s curve modes",
                state.sizing.edge_id, state.sizing.edge_length * 1e3,
                grouped(state.sizing.curve_modes_kept).c_str(),
                grouped(state.sizing.curve_modes_total).c_str());
        const float ew = font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, edge.c_str()).x;
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + chart_w - ew - pad, chart_top + 8.0f),
                    faded(palette.accent, alpha), edge.c_str());
    } else {
        dl->AddText(font, type.label, ImVec2(origin.x + pad, chart_top + 36.0f),
                    faded(palette.status_warn, alpha),
                    "No curved CAD edge supplied a resolvable curvature trace.");
    }

    const auto& spectrum = state.sizing.curve_spectrum;
    const auto& kept = state.sizing.curve_mode_kept;
    if (spectrum.size() >= 2 && kept.size() == spectrum.size()) {
        // DC is the mean curvature, not spacing variation. `truncate_modes`
        // always preserves it and excludes it from modes_total, so omitting it
        // here both matches the report's denominator and stops one huge bar
        // from flattening every explanatory non-DC mode.
        const double max_magnitude =
            std::max(*std::max_element(spectrum.begin() + 1, spectrum.end()),
                     1.0e-12);
        const float bars_top = split_y + type.legend * 1.8f;
        const float bars_bottom = chart_top + chart_h - 11.0f;
        const float bars_h = std::max(1.0f, bars_bottom - bars_top);
        const float bars_w = chart_w - 2.0f * pad;
        const std::size_t mode_count = spectrum.size() - 1;
        const float slot = bars_w / static_cast<float>(mode_count);
        const std::size_t visible = std::min(
            mode_count,
            static_cast<std::size_t>(std::ceil(
                cue.spectral_spectrum_reveal * static_cast<double>(mode_count))));
        const double log_max = std::log1p(max_magnitude);
        for (std::size_t mode = 0; mode < visible; ++mode) {
            const std::size_t i = mode + 1;
            const float magnitude = static_cast<float>(
                std::log1p(spectrum[i]) / std::max(log_max, 1.0e-12));
            const float x0 = origin.x + pad + static_cast<float>(mode) * slot;
            const float x1 = x0 + std::max(1.0f, slot - 1.0f);
            const float y0 = bars_bottom - bars_h * magnitude;
            const bool survives = kept[i] != 0;
            const float discarded_alpha =
                static_cast<float>(1.0 - 0.82 * cue.spectral_filter_mix);
            const ImVec4 color =
                survives ? palette.accent
                         : ImVec4(palette.text_dim.x, palette.text_dim.y,
                                  palette.text_dim.z,
                                  palette.text_dim.w * discarded_alpha);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, bars_bottom),
                              faded(color, alpha), 1.0f);
        }
    }
    const std::array<const char*, 5> steps{
        "1 sample κ(s)", "2 FFT", "3 rank energy", "4 inverse FFT", "5 map h(x)"};
    const std::array<double, 5> progress{
        cue.spectral_edge_reveal, cue.spectral_spectrum_reveal,
        cue.spectral_filter_mix, cue.spectral_filter_mix,
        cue.spectral_field_reveal};
    const float gap = 6.0f;
    const float box_w = (chart_w - gap * 4.0f) / 5.0f;
    const float box_y = chart_top + chart_h + type.label * 0.85f;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const float x = origin.x + static_cast<float>(i) * (box_w + gap);
        const bool active = progress[i] > (i == 3 ? 0.45 : 0.04);
        dl->AddRectFilled(ImVec2(x, box_y), ImVec2(x + box_w, box_y + type.label * 2.2f),
                          faded(active ? palette.accent_mid : palette.panel_bg,
                                alpha * (active ? 0.66f : 0.44f)),
                          5.0f);
        dl->AddText(font, type.legend, ImVec2(x + 7.0f, box_y + 7.0f),
                    faded(active ? palette.text : palette.text_dim, alpha),
                    steps[i], nullptr, std::max(20.0f, box_w - 14.0f));
    }
    const std::string floor =
        state.sizing.brep_curvature
            ? "After filtering, exact BRep curvature is re-imposed as a floor — "
              "real features cannot blur."
            : "No exact-BRep curvature report is available for this input.";
    dl->AddText(font, type.legend, ImVec2(origin.x, box_y + type.label * 2.7f),
                faded(state.sizing.brep_curvature ? palette.status_ok : palette.status_warn,
                      alpha),
                floor.c_str(), nullptr, wrap);
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_cells(const CinemaState& state, const CinemaCue& cue,
                       const CinemaType& type, const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    std::size_t index = cue.stage_index >= 0 ? static_cast<std::size_t>(cue.stage_index)
                                            : (n_fill > 0 ? n_fill - 1 : 0);
    if (n_fill > 0) {
        index = std::min(index, n_fill - 1);
    }
    const CinemaMeshInsight* emitted =
        index < state.stage_insights.size() ? &state.stage_insights[index] : nullptr;
    const CinemaMeshInsight* solved =
        !state.solve_insights.empty() ? &state.solve_insights.front() : emitted;

    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                "Cell microscope — actual emitted topology");
    std::string stage = "waiting for the first emitted cell";
    if (index < state.stages.size() && emitted != nullptr) {
        stage = std::format("{} · {}", stage_name(state.stages[index].stage),
                            mesh_mix_text(*emitted));
    }
    dl->AddText(font, type.label, ImVec2(origin.x, origin.y + type.caption * 1.55f),
                faded(palette.accent, alpha), stage.c_str(), nullptr, wrap);

    const float bar_y = origin.y + type.caption * 3.2f;
    const float bar_h = 18.0f;
    if (emitted != nullptr) {
        const std::size_t total =
            std::max<std::size_t>(1, std::accumulate(emitted->type_counts.begin(),
                                                     emitted->type_counts.end(),
                                                     std::size_t{0}));
        float x = origin.x;
        for (std::size_t i = 0; i < emitted->type_counts.size(); ++i) {
            if (emitted->type_counts[i] == 0) {
                continue;
            }
            const float w = region.x * static_cast<float>(emitted->type_counts[i]) /
                            static_cast<float>(total);
            const auto rgb = element_type_color(static_cast<fea::ElementType>(i));
            dl->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + w, bar_y + bar_h),
                              rgba(rgb, alpha), 2.0f);
            x += w;
        }
    }

    const float card_top = bar_y + bar_h + type.label * 1.4f;
    const float card_h = std::min(300.0f, region.y * 0.42f);
    const float card_gap = 12.0f;
    const float card_w = (region.x - card_gap) * 0.5f;
    const auto draw_tet = [&](float x, bool quadratic) {
        dl->AddRectFilled(ImVec2(x, card_top), ImVec2(x + card_w, card_top + card_h),
                          faded(palette.panel_bg, 0.58f * alpha), 7.0f);
        dl->AddRect(ImVec2(x, card_top), ImVec2(x + card_w, card_top + card_h),
                    faded(palette.border, alpha), 7.0f);
        const std::array<ImVec2, 4> p{{
            {x + card_w * 0.50f, card_top + card_h * 0.19f},
            {x + card_w * 0.18f, card_top + card_h * 0.78f},
            {x + card_w * 0.82f, card_top + card_h * 0.78f},
            {x + card_w * 0.63f, card_top + card_h * 0.53f},
        }};
        constexpr std::array<std::array<int, 2>, 6> edges{{
            {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}},
        }};
        for (const auto& edge : edges) {
            dl->AddLine(p[edge[0]], p[edge[1]], faded(palette.text, 0.72f * alpha), 2.0f);
            if (quadratic) {
                const ImVec2 mid{0.5f * (p[edge[0]].x + p[edge[1]].x),
                                 0.5f * (p[edge[0]].y + p[edge[1]].y)};
                dl->AddCircleFilled(mid, 4.2f, faded(palette.accent, alpha));
            }
        }
        for (const ImVec2 point : p) {
            dl->AddCircleFilled(point, 6.0f, faded(palette.text, alpha));
        }
        const char* title = quadratic ? "order 2 · tet10" : "order 1 · tet4";
        const char* subline =
            quadratic ? "six midside nodes bend with the exact CAD"
                      : "four corners define the linear fill";
        dl->AddText(font, type.label, ImVec2(x + 12.0f, card_top + 10.0f),
                    faded(quadratic ? palette.accent : palette.text, alpha), title);
        dl->AddText(font, type.legend,
                    ImVec2(x + 12.0f, card_top + card_h - type.legend * 2.2f),
                    faded(palette.text_dim, alpha), subline, nullptr, card_w - 24.0f);
    };
    draw_tet(origin.x, false);
    draw_tet(origin.x + card_w + card_gap, hud.order >= 2);

    const float facts_y = card_top + card_h + type.label * 1.1f;
    std::string quality = "quality summary arrives with the emitted mesh";
    if (solved != nullptr && solved->quality_measured > 0) {
        quality = fmt("shape quality  min %.4g · mean %.4g · %s cells measured",
                      solved->quality_min, solved->quality_mean,
                      grouped(solved->quality_measured).c_str());
    }
    dl->AddText(font, type.label, ImVec2(origin.x, facts_y),
                faded(palette.status_ok, alpha), quality.c_str(), nullptr, wrap);
    const std::string facts = fmt(
        "spectral sizing %s · exact BRep curvature %s · polynomial order %d · "
        "ZZ recovery %s",
        state.sizing.spectral.applied ? "on" : "off",
        state.sizing.brep_curvature ? "on" : "off", hud.order,
        state.solve_stages.empty() ? "waiting" : "measured");
    dl->AddText(font, type.legend, ImVec2(origin.x, facts_y + type.label * 1.7f),
                faded(palette.text_dim, alpha), facts.c_str(), nullptr, wrap);
    dl->AddText(font, type.legend, ImVec2(origin.x, facts_y + type.label * 3.1f),
                faded(palette.status_warn, 0.78f * alpha),
                "experimental alternatives: varyhedron · CVT poly-VEM · octahedral — "
                "not used in this verified solve",
                nullptr, wrap);
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

} // namespace

void draw_cinema_panel(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                       const CinemaHud& hud) {
    // Four real views share one fixed pane: CAD spectrum, deployed network,
    // emitted-cell microscope, solver equations. Transitions are opacity-only,
    // so neither the part nor the panel geometry jumps between chapters.
    float feature_alpha = 0.0f;
    float network_alpha = 0.0f;
    float cell_alpha = 0.0f;
    float equation_alpha = 0.0f;
    if (cue.act == CinemaAct::kSkeleton) {
        feature_alpha = cue.panel_open;
    } else if (cue.act == CinemaAct::kDeliberate) {
        network_alpha = 1.0f;
    } else if (cue.act == CinemaAct::kBuild) {
        const double lead = cinema_decision_lead(state);
        const float blend = static_cast<float>(
            smoothstep((cue.act_t - lead) / std::max(0.7, 0.08 * cue.act_span)));
        network_alpha = 1.0f - blend;
        cell_alpha = blend;
    } else if (cue.act == CinemaAct::kMeshHold) {
        cell_alpha = 1.0f;
    } else {
        network_alpha = cue.network_alpha;
        equation_alpha = cue.equations_alpha;
    }

    const ImVec2 origin = ImGui::GetCursorPos();
    draw_cinema_features(state, cue, type, feature_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_network(state, cue, type, network_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_cells(state, cue, type, hud, cell_alpha);
    ImGui::SetCursorPos(origin);
    draw_cinema_equations(state, cue, type, hud, equation_alpha);
}

namespace {

void skeleton_caption(const CinemaState& state, CinemaCaption& out) {
    out.headline = "Exact CAD + spectral size field";
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        out.numbers =
            fmt("%s exact edges · %s selected-edge samples · %s on-part target sizes",
                grouped(state.skeleton_polylines).c_str(),
                grouped(state.sizing.edge_points.size()).c_str(),
                grouped(state.sizing.field_points.size()).c_str());
        out.note =
            "κ(s) sweep → FFT energy selection → inverse transform → h(x) rings on the part";
        out.note_color = palette.text_dim;
        break;
    case SkeletonSource::kSharpEdges:
        out.numbers =
            fmt("%s tessellation creases", grouped(state.skeleton_polylines).c_str());
        out.note = "mesh input · no BRep curvature or CAD-edge spectrum is claimed";
        out.note_color = palette.status_warn;
        break;
    case SkeletonSource::kUnavailable:
        out.headline = "CAD feature extraction unavailable";
        out.note = state.skeleton_note;
        out.note_color = palette.status_err;
        break;
    case SkeletonSource::kNone:
        out.headline = "No part loaded";
        out.note = "no substitute geometry is drawn";
        out.note_color = palette.status_err;
        break;
    }
}

void deliberate_caption(const CinemaState& state, const CinemaCue& cue, CinemaCaption& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)state;
    (void)cue;
    out.headline = "Advisor unavailable";
    out.note = "POLYMESH_WITH_ADVISOR=OFF";
    out.note_color = palette.status_err;
#else
    if (!state.explanation) {
        out.headline = "Advisor unavailable";
        out.note = state.advisor_note.empty() ? std::string("no measured forward pass")
                                               : state.advisor_note;
        out.note_color = palette.status_err;
        return;
    }
    const auto& explanation = *state.explanation;
    const auto& frames = explanation.frames;
    const std::size_t candidates = frames.empty() ? 0 : frames.size() - 1;
    if (cue.chosen_pass_held) {
        out.headline = state.decision_applied ? "Advisor decision — final network state"
                                              : "Advisor abstained — final network state";
        out.headline_color =
            state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("final re-score held for inspection · %s candidates compared",
                          grouped(candidates).c_str());
        out.note = state.decision_note;
        out.note_color =
            state.decision_applied ? palette.text_dim : palette.status_warn;
        return;
    }
    out.headline = "Mesh search — deployed advisor network";
    if (cue.frame_index < 0 || static_cast<std::size_t>(cue.frame_index) >= frames.size()) {
        out.numbers = fmt("%s candidate meshes · measured forward passes only",
                          grouped(candidates).c_str());
        return;
    }
    out.numbers = fmt("forward pass %d / %s · %s candidates compared", cue.frame_index + 1,
                      grouped(frames.size()).c_str(), grouped(candidates).c_str());
    out.note = fmt("circle = activation · line = |weight × activation| · %.3g%% failure gate",
                   explanation.gate_threshold * 100.0);
    out.note_color = palette.text_dim;
#endif
}

void build_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.stages.empty()) {
        out.headline = "No construction stages emitted";
        out.note = "nothing is substituted for missing mesher snapshots";
        out.note_color = palette.status_err;
        return;
    }
    if (cue.stage_index < 0) {
        out.headline = state.decision_applied ? "Advisor selection locked"
                                              : "Advisor abstained — verified fallback";
        out.headline_color = state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("%s · %.3g mm target · order %d · %d refinement pass%s",
                          std::string(mesher_plain(hud.mesher)).c_str(),
                          hud.mesh_size * 1e3, hud.order, hud.adapt_passes,
                          hud.adapt_passes == 1 ? "" : "es");
        out.note = state.decision_note;
        out.note_color = state.decision_applied ? palette.text_dim : palette.status_warn;
        return;
    }
    const auto idx = static_cast<std::size_t>(cue.stage_index);
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    if (idx >= state.stages.size()) {
        return;
    }
    const auto& stage = state.stages[idx];
    out.headline = fmt("Stage %zu / %zu — %s", idx + 1, n_fill,
                       std::string(stage_name(stage.stage)).c_str());
    const std::size_t total = stage.mesh.elements.size();
    const auto drawn =
        static_cast<std::size_t>(cue.stage_reveal * static_cast<double>(total));
    out.numbers = fmt("%s / %s cells · %s nodes · order-%d target",
                      grouped(drawn).c_str(), grouped(total).c_str(),
                      grouped(stage.mesh.nodes.size()).c_str(), hud.order);
    out.note = "exact stage snapshot · mesher emission order · cell microscope at left";
    out.note_color = palette.text_dim;
    if (hud.cinema_skipped_elements > 0) {
        out.note = fmt("%s cells could not be triangulated and are excluded from this view",
                       grouped(hud.cinema_skipped_elements).c_str());
        out.note_color = palette.status_warn;
    }
}

void mesh_hold_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       CinemaCaption& out) {
    const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
    out.headline = x < 0.78 ? "Cell topology — exploded view"
                            : "Authoritative mesh — ready to solve";
    out.headline_color = palette.status_ok;
    if (!state.solve_stages.empty()) {
        const auto& stage = state.solve_stages.front();
        out.numbers = fmt("%s cells · %s nodes · %s unknowns · order %d",
                          grouped(stage.trace.n_elems).c_str(),
                          grouped(stage.trace.n_nodes).c_str(),
                          grouped(stage.trace.n_dof).c_str(), hud.order);
        if (!state.solve_insights.empty()) {
            const auto& insight = state.solve_insights.front();
            out.note = fmt("%s · shape quality min %.4g / mean %.4g",
                           mesh_mix_text(insight).c_str(), insight.quality_min,
                           insight.quality_mean);
        }
    }
    if (out.note.empty()) {
        out.note = state.decision_note;
    }
    out.note_color = palette.text_dim;
}

void solve_caption(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.solve_stages.empty()) {
        out.headline = "No solve passes were delivered";
        out.note = "pipeline::SolveJob::on_solve_stage received none, so this act holds the "
                   "finished mesh instead of a field — nothing is drawn in place of an answer "
                   "that never arrived";
        out.note_color = palette.status_err;
        return;
    }
    const auto i = static_cast<std::size_t>(std::max(cue.solve_stage_index, 0));
    const auto& stage = state.solve_stages[std::min(i, state.solve_stages.size() - 1)];
    const auto& r = stage.result;
    const auto& tr = stage.trace;
    const bool many = state.solve_stages.size() > 1;
    const std::string pass_tag =
        many ? fmt(" (pass %d of %zu)", stage.pass + 1, state.solve_stages.size())
             : std::string{};
    out.note_color = palette.text_dim;
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
        out.headline = "Stress field — spatial reveal";
        out.numbers =
            fmt("%s cells · %s unknowns · solved once%s", grouped(tr.n_elems).c_str(),
                grouped(tr.n_dof).c_str(), pass_tag.c_str());
        out.note = "completed von Mises field · reveal starts at the loaded surface";
        break;
    case SolvePhase::kStressHold:
        out.headline = fmt("Peak stress %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        out.numbers = fmt("largest movement %.4g mm · undeformed shape%s",
                          r.max_displacement * 1e3, pass_tag.c_str());
        out.note = "von Mises stress on the exact geometry this pass solved";
        break;
    case SolvePhase::kGradientSweep:
        out.headline = "Stress gradient — spatial reveal";
        out.numbers = "|∇σvm| recovered at every resolvable node";
        out.note = "least-squares recovery over each node's own neighboring cells";
        break;
    case SolvePhase::kGradientHold: {
        const double gmax = state.gradient_max(i);
        const std::size_t unresolved = state.gradient_unresolved(i);
        if (gmax > 0.0) {
            out.headline = fmt("Steepest change %.4g MPa per mm", gmax / 1e9);
            out.headline_color = palette.status_ok;
            out.numbers = "red = the steepest local stress change";
            out.note =
                unresolved > 0
                    ? fmt("%s nodes had coplanar neighborhoods and report zero gradient",
                          grouped(unresolved).c_str())
                    : std::string("stress-riser intensity, measured per unit distance");
            out.note_color = unresolved > 0 ? palette.status_warn : palette.text_dim;
        } else {
            out.headline = "No gradient could be recovered here";
            out.headline_color = palette.status_warn;
            out.numbers = "the stress field itself is still on screen";
            out.note = "nothing is drawn in place of a gradient that could not be computed";
            out.note_color = palette.status_warn;
        }
        break;
    }
    case SolvePhase::kError:
        out.headline = "Estimated solution error";
        if (hud.eta_target > 0.0) {
            out.numbers = fmt("ZZ indicator %.3g%% · target %.3g%%",
                              tr.global_eta * 100.0, hud.eta_target * 100.0);
        } else {
            out.numbers = fmt("ZZ global indicator %.3g%% · verification pass",
                              tr.global_eta * 100.0);
        }
        out.note = "recovered stress vs solved stress · energy-norm indicator";
        break;
    case SolvePhase::kErrorHold:
        out.headline = "Local error map";
        out.numbers = fmt("%s h-refinement marks · %s p-refinement marks",
                          grouped(tr.n_h_mark).c_str(), grouped(tr.n_p_mark).c_str());
        out.note = i + 1 < state.solve_stages.size()
                       ? "this measured field drives the next solved mesh"
                       : "reported in full · no unrecorded refinement is implied";
        break;
    case SolvePhase::kRefine: {
        const std::size_t next = i + 1;
        out.headline = "Replacing only cells changed by refinement";
        if (next < state.solve_stages.size()) {
            out.numbers = fmt("%s kept · %s removed · %s replacement cells",
                              grouped(hud.unchanged_elements).c_str(),
                              grouped(hud.removed_elements).c_str(),
                              grouped(hud.added_elements).c_str());
            out.note = "corner-topology diff · unchanged cells stay rendered · changed cells "
                       "collapse and spawn";
        }
        break;
    }
    case SolvePhase::kRefineHold: {
        const std::size_t next = i + 1;
        out.headline = "Incremental refinement complete";
        out.headline_color = palette.status_ok;
        if (next < state.solve_stages.size()) {
            const auto& nx = state.solve_stages[next];
            out.numbers = fmt("%s cells · %s preserved from the previous pass · %s unknowns",
                              grouped(nx.trace.n_elems).c_str(),
                              grouped(hud.unchanged_elements).c_str(),
                              grouped(nx.trace.n_dof).c_str());
            out.note = fmt("%s old cells replaced by %s solved cells; polynomial promotion "
                           "does not count as a topology change",
                           grouped(hud.removed_elements).c_str(),
                           grouped(hud.added_elements).c_str());
        }
        break;
    }
    case SolvePhase::kLoadRamp:
        out.headline = fmt("Load response — %.4g / %.4g N",
                           cue.load_factor * hud.load_newtons, hud.load_newtons);
        out.numbers =
            fmt("%.4g MPa · %.4g mm at this load", cue.load_factor * r.max_von_mises / 1e6,
                cue.load_factor * r.max_displacement * 1e3);
        out.note = fmt(
            "exact linear response · true %.4g mm · shown %.4g mm at %.3g×",
            cue.load_factor * r.max_displacement * 1e3,
            cue.load_factor * hud.deform_scale * r.max_displacement * 1e3,
            cue.load_factor * hud.deform_scale);
        break;
    case SolvePhase::kHold: {
        out.headline = fmt("Full load — %.4g MPa peak", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        const std::string_view token = cinema_solver_token(state);
        const char* method = token == "direct_ldlt"
                                 ? "factorised, not iterated"
                                 : (token == "cg" ? "conjugate gradient"
                                                  : "solver method not reported");
        out.numbers = fmt("%.4g mm largest movement · %s cells · %s unknowns · %s",
                          r.max_displacement * 1e3, grouped(tr.n_elems).c_str(),
                          grouped(tr.n_dof).c_str(), method);
        const double shown_mm = hud.deform_scale * r.max_displacement * 1e3;
        const double shown_pct =
            hud.model_diagonal > 0.0
                ? 100.0 * hud.deform_scale * r.max_displacement / hud.model_diagonal
                : 0.0;
        out.note = fmt("true %.4g mm · display %.4g mm (%.2f%% of model) · %.3g×",
                       r.max_displacement * 1e3, shown_mm, shown_pct, hud.deform_scale);
        break;
    }
    case SolvePhase::kNone:
        break;
    }
}

/// Draws `text` at `size`, or at whatever smaller size makes it fit `width`.
/// Never wraps and never clips: wrapping would change the strip's height
/// mid-take and clipping would drop the end of a sentence the film is making.
void draw_fitted(ImDrawList* dl, ImFont* font, float size, ImVec2 at, float width,
                 const std::string& text, ImU32 color) {
    if (text.empty()) {
        return;
    }
    const float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
    const float s = w > width && w > 0.0f ? std::max(8.0f, size * width / w) : size;
    dl->AddText(font, s, ImVec2(at.x, at.y + (size - s) * 0.5f), color, text.c_str());
}

} // namespace

std::vector<CinemaChapter> cinema_chapters() {
    return {
        {"exact CAD", CinemaAct::kSkeleton},
        {"advisor", CinemaAct::kDeliberate},
        {"mesher", CinemaAct::kBuild},
        {"analysis", CinemaAct::kSolve},
    };
}

CinemaCaption cinema_caption(const CinemaState& state, const CinemaCue& cue,
                             const CinemaHud& hud) {
    CinemaCaption out;
    out.headline_color = palette.text;
    out.note_color = palette.text_dim;
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        skeleton_caption(state, out);
        break;
    case CinemaAct::kDeliberate:
        deliberate_caption(state, cue, out);
        break;
    case CinemaAct::kBuild:
        build_caption(state, cue, hud, out);
        break;
    case CinemaAct::kMeshHold:
        mesh_hold_caption(state, cue, hud, out);
        break;
    case CinemaAct::kSolve:
        // The gradient captions read a cached recovery, which is why this one
        // takes the state by reference. Nothing here computes a field.
        solve_caption(const_cast<CinemaState&>(state), cue, hud, out);
        break;
    }
    // The footer names the part, the run, and where the exhaustive disclosures
    // live. That pointer is load-bearing: it is what makes it honest to have
    // put one disclosure on screen instead of six.
    const std::string part = hud.part.empty() ? std::string("(no part)") : hud.part;
    out.footer = hud.stamp.empty()
                     ? fmt("%s · POLYMESH_CINEMA_STAMP not set, so this run carries no "
                           "provenance line · full disclosures: docs/assets/cinema/NOTES.md",
                           part.c_str())
                     : fmt("%s · %s · full disclosures: docs/assets/cinema/NOTES.md",
                           part.c_str(), hud.stamp.c_str());
    return out;
}

float cinema_strip_height(const CinemaType& type) {
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float rows = std::floor(type.headline * 1.28f) + std::floor(type.numbers * 1.42f) +
                       std::floor(type.note * 1.38f) + std::floor(type.footer * 1.45f);
    return std::floor(chapter_h + rows + 2.0f * kStripPadY);
}

void draw_cinema_strip(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       const CinemaType& type) {
    const CinemaCaption caption = cinema_caption(state, cue, hud);
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);

    // ---- the chapter bar -------------------------------------------------
    // Four words and a progress line. It exists so a first-time viewer knows
    // where they are in half a second without reading a clock.
    const auto chapters = cinema_chapters();
    const CinemaAct here = cue.act == CinemaAct::kMeshHold ? CinemaAct::kBuild : cue.act;
    float x = origin.x;
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float gap = std::floor(type.chapter * 1.6f);
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const bool current = chapters[i].act == here;
        const bool done = static_cast<int>(chapters[i].act) < static_cast<int>(here);
        const ImVec4 color =
            current ? palette.accent : (done ? palette.text : palette.text_dim);
        const float alpha = current ? 1.0f : (done ? 0.55f : 0.30f);
        const std::string label = fmt("%zu. %s", i + 1, chapters[i].label);
        dl->AddText(font, type.chapter, ImVec2(x, origin.y), faded(color, alpha),
                    label.c_str());
        x += font->CalcTextSizeA(type.chapter, FLT_MAX, 0.0f, label.c_str()).x + gap;
    }
    const float bar_y = origin.y + chapter_h - 5.0f;
    dl->AddRectFilled(ImVec2(origin.x, bar_y), ImVec2(origin.x + width, bar_y + 2.0f),
                      faded(palette.text_dim, 0.22f));
    dl->AddRectFilled(
        ImVec2(origin.x, bar_y),
        ImVec2(origin.x + width * static_cast<float>(cinema_progress(state)), bar_y + 2.0f),
        faded(palette.accent, 0.85f));

    // ---- the four rows ---------------------------------------------------
    float y = origin.y + chapter_h;
    draw_fitted(dl, font, type.headline, ImVec2(origin.x, y), width, caption.headline,
                ImGui::ColorConvertFloat4ToU32(caption.headline_color));
    y += std::floor(type.headline * 1.28f);
    draw_fitted(dl, font, type.numbers, ImVec2(origin.x, y), width, caption.numbers,
                ImGui::ColorConvertFloat4ToU32(palette.text));
    y += std::floor(type.numbers * 1.42f);
    draw_fitted(dl, font, type.note, ImVec2(origin.x, y), width, caption.note,
                ImGui::ColorConvertFloat4ToU32(caption.note_color));
    y += std::floor(type.note * 1.38f);
    draw_fitted(dl, font, type.footer, ImVec2(origin.x, y), width, caption.footer,
                faded(palette.text_dim, 0.85f));
}

} // namespace polymesh::gui
