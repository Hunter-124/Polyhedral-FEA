// SPDX-License-Identifier: BSD-3-Clause
// Cinema clock: act schedule, cue resolution, take state and viewport sync.
#include "cinema.hpp"
#include "cinema_internal.hpp"

#include "fea/cell_quality.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace polymesh::gui {

using detail::initial_fill_stage_count;
using detail::smoothstep;

namespace detail {

double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

std::size_t initial_fill_stage_count(const std::vector<pipeline::MeshStage>& stages) {
    std::size_t n = 0;
    for (const auto& stage : stages) {
        if (stage.pass == 0) {
            ++n;
        }
    }
    return n;
}

} // namespace detail

namespace {

/// Act spans as fractions of the take, tuned at the render script's 3600-frame
/// (60 s) default. The advisor lane spends the first 65% of its act on the real
/// candidate passes and the rest holding the final state; the closing act's
/// pass beats scale proportionally when more than one solve stage exists.
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

/// Per-element reveal: cells land shrunk toward their own centroid so each
/// reads as a separate object, and the edge pass stays a light annotation over
/// shaded faces so dense meshes do not turn into near-black outline.
constexpr double kRevealShrink = 0.22;
constexpr double kRevealShrinkFraction = 0.33;
constexpr float kMeshEdgeAlpha = 0.18f;
constexpr float kMeshEdgeWidth = 0.8f;

/// Width of the field sweep's leading band, as a fraction of the part's extent
/// along the sweep axis. Wide enough that the highlight reads as a moving front
/// rather than a hard edge, narrow enough that it is a front and not a fade.
constexpr float kSweepFeather = 0.10f;

/// The completed target-spacing field remains as a restrained annotation while
/// the advisor starts scoring, then disappears only as the mesher replaces
/// targets with actual cells.
constexpr float kSizingCarryAlpha = 0.22f;
/// Fraction of a refinement beat spent dissolving the measured ZZ field into
/// the exact old→new topology transition it requested.
constexpr double kFieldToMeshHandoff = 0.32;
/// Fraction of a stress reveal over which the authoritative cell rendering
/// remains on top, then yields to the solved field arriving beneath it.
constexpr double kMeshToFieldHandoff = 0.42;

/// Floor on λ when it is used as a DIVISOR for the colour scale. λ itself is
/// never floored — the number on screen and the displacement are the real λ,
/// including exactly 0 — but s_max / λ has to stay finite, and at λ = 1e-3 every
/// drawn colour is already within one part in a thousand of the bottom of the
/// colormap, which is the correct picture of a part carrying no load.
constexpr double kMinLoadFactor = 1.0e-3;

/// Generates the closing act's beat sequence for `n` completed solve passes, in
/// the order the pipeline computed them, and hands each to `fn(stage, phase)`.
///
/// Per pass: stress plus its still hold; on the first pass only, the recovered
/// gradient plus its hold (it explains a stress field, it does not advance the
/// loop); the ZZ error field that pass computed plus its hold, on every pass,
/// because it is the loop's decision input including the decision to stop;
/// then, when another pass followed, the next solved mesh revealed and held.
/// The last pass ends with the exact linear load ramp and the final hold.
///
/// Generated rather than materialised so the per-frame cue does not allocate.
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

CinemaHistogram inspect_histogram(const std::vector<double>& values) {
    CinemaHistogram out;
    std::vector<double> finite;
    finite.reserve(values.size());
    double sum = 0.0;
    for (const double value : values) {
        if (!std::isfinite(value)) {
            continue;
        }
        finite.push_back(value);
        if (out.samples == 0) {
            out.min = value;
            out.max = value;
        } else {
            out.min = std::min(out.min, value);
            out.max = std::max(out.max, value);
        }
        sum += value;
        ++out.samples;
    }
    if (finite.empty()) {
        return out;
    }
    out.mean = sum / static_cast<double>(out.samples);
    const std::size_t p99_index =
        static_cast<std::size_t>(std::floor(0.99 * static_cast<double>(finite.size() - 1)));
    std::nth_element(finite.begin(), finite.begin() + static_cast<std::ptrdiff_t>(p99_index),
                     finite.end());
    out.p99 = finite[p99_index];
    // A single constrained-node singularity must not flatten 99% of the
    // teaching graph or viewport into one dark bin. The true max stays in
    // `max` and on screen; only the colour/chart scale caps at measured p99.
    const double display_span = out.p99 - out.min;
    for (const double value : finite) {
        std::size_t bin = 0;
        if (display_span > 0.0) {
            const double u = std::clamp((value - out.min) / display_span, 0.0, 1.0);
            bin = std::min(static_cast<std::size_t>(u * static_cast<double>(out.bins.size())),
                           out.bins.size() - 1);
        }
        ++out.bins[bin];
        out.tallest_bin = std::max(out.tallest_bin, out.bins[bin]);
    }
    return out;
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
        stress_histograms.push_back(inspect_histogram(stage.result.von_mises));
        error_histograms.push_back(inspect_histogram(stage.result.nodal_eta));
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
            const std::size_t index =
                static_cast<std::size_t>(std::floor(q * static_cast<double>(eta.size() - 1)));
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
    if (stress_histograms.size() < solve_stages.size()) {
        stress_histograms.resize(solve_stages.size());
        error_histograms.resize(solve_stages.size());
    }
    stress_histograms.back() = inspect_histogram(result.von_mises);
    error_histograms.back() = inspect_histogram(result.nodal_eta);
    const auto resolve_nodes = [&](std::vector<CinemaMechanicsMarker>& markers) {
        for (auto& marker : markers) {
            marker.result_node = std::numeric_limits<std::size_t>::max();
            double best = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < result.volume_mesh.nodes.size(); ++i) {
                const double d2 =
                    (result.volume_mesh.nodes[i] - marker.position).squaredNorm();
                if (d2 < best) {
                    best = d2;
                    marker.result_node = i;
                }
            }
        }
    };
    resolve_nodes(support_markers);
    resolve_nodes(load_markers);
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
    stress_histograms.clear();
    error_histograms.clear();
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
    slot.histogram = inspect_histogram(slot.values);
    return slot;
}

const std::vector<double>& CinemaState::gradient_field(std::size_t index) {
    return gradient_slot(index).values;
}

std::size_t CinemaState::gradient_unresolved(std::size_t index) {
    return gradient_slot(index).unresolved;
}

double CinemaState::gradient_max(std::size_t index) { return gradient_slot(index).max; }

const CinemaHistogram& CinemaState::gradient_histogram(std::size_t index) {
    return gradient_slot(index).histogram;
}

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

    // Open on exact CAD and build the spectral story to completion. That final
    // state is not erased at the act boundary: it crosses into advisor scoring
    // and then remains dimly on the part until actual cells replace it.
    if (cue.act == CinemaAct::kSkeleton) {
        const double wait = 0.08 * cue.act_span;
        const double slide = 0.14 * cue.act_span;
        cue.panel_open =
            static_cast<float>(smoothstep((cue.act_t - wait) / std::max(slide, 1.0e-9)));
        const double p = cue.act_t / std::max(cue.act_span, 1.0e-9);
        cue.spectral_edge_reveal = smoothstep((p - 0.08) / 0.20);
        cue.spectral_spectrum_reveal = smoothstep((p - 0.24) / 0.22);
        cue.spectral_filter_mix = smoothstep((p - 0.42) / 0.25);
        cue.spectral_field_reveal = smoothstep((p - 0.56) / 0.30);
        cue.spectral_overlay_alpha = static_cast<float>(smoothstep((p - 0.05) / 0.08));
    } else if (cue.act == CinemaAct::kDeliberate) {
        cue.spectral_edge_reveal = 1.0;
        cue.spectral_spectrum_reveal = 1.0;
        cue.spectral_filter_mix = 1.0;
        cue.spectral_field_reveal = 1.0;
        const double bridge = std::min(1.3, 0.18 * cue.act_span);
        const double q = smoothstep(cue.act_t / std::max(bridge, 1.0e-9));
        cue.spectral_overlay_alpha = static_cast<float>(1.0 + q * (kSizingCarryAlpha - 1.0f));
    } else if (cue.act == CinemaAct::kBuild) {
        cue.spectral_edge_reveal = 1.0;
        cue.spectral_spectrum_reveal = 1.0;
        cue.spectral_filter_mix = 1.0;
        cue.spectral_field_reveal = 1.0;
        const double handoff = std::max(cinema_decision_lead(state), 0.18 * cue.act_span);
        cue.spectral_overlay_alpha = static_cast<float>(
            kSizingCarryAlpha * (1.0 - smoothstep(cue.act_t / std::max(handoff, 1.0e-9))));
    } else if (cue.act == CinemaAct::kSolve) {
        // The completed cell microscope dissolves directly into the solver's
        // equation board. Replaying the network here would move backwards.
        const double fade = cinema_panel_fade(state);
        cue.equations_alpha =
            static_cast<float>(smoothstep(cue.act_t / std::max(fade, 1.0e-9)));
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
            cue.action_bridge_alpha =
                static_cast<float>(smoothstep(cue.act_t / std::max(0.45 * lead, 1.0e-9)));
            if (x < 0.0) {
                cue.activation_wave = smoothstep(cue.act_t / std::max(lead, 1.0e-9));
            } else {
                const auto i = static_cast<std::size_t>(x / beat);
                const std::size_t clamped = std::min(i, n_fill - 1);
                cue.stage_index = static_cast<int>(clamped);
                const double within = x - static_cast<double>(clamped) * beat;
                cue.stage_reveal = std::clamp(within / beat, 0.0, 1.0);
                cue.activation_wave = cue.stage_reveal;
                // A later audit snapshot with identical topology carries the
                // already-landed mesh at full reveal, so the cells do not
                // vanish between `fill` and `ship`.
                const bool same_topology = clamped > 0 &&
                                           state.stages[clamped - 1].mesh.nodes.size() ==
                                               state.stages[clamped].mesh.nodes.size() &&
                                           state.stages[clamped - 1].mesh.elements.size() ==
                                               state.stages[clamped].mesh.elements.size();
                cue.mesh_action_reveal =
                    same_topology ? 1.0 : smoothstep((cue.stage_reveal - 0.28) / 0.72);
                cue.mesh_source = CinemaMeshSource::kFillStage;
                cue.mesh_source_index = cue.stage_index;
            }
        } else if (cue.act == CinemaAct::kMeshHold) {
            cue.stage_index = static_cast<int>(n_fill - 1);
            cue.stage_reveal = 1.0;
            cue.mesh_action_reveal = 1.0;
            cue.activation_wave = 1.0;
            cue.action_bridge_alpha = static_cast<float>(
                1.0 - 0.55 * smoothstep(cue.act_t / std::max(cue.act_span, 1.0e-9)));
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
            cue.mesh_action_reveal = 1.0;
            cue.activation_wave = 1.0;
            cue.action_bridge_alpha = 1.0f - cue.equations_alpha;
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
        case SolvePhase::kError:
            // Eased because this is a display handoff across two already
            // completed states, not a physical time variable. Behind the front
            // is the arriving measured field; ahead is the prior measured field
            // (or the authoritative mesh-grey state for the first stress beat).
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
            // the linear solve does not have. The visual handoff from the ZZ map
            // to stress may ease independently; it changes no λ or field value.
            cue.load_factor = std::clamp(x, 0.0, 1.0);
            cue.field_front = smoothstep(x);
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
        // The completed spacing field stays on the same part as the deployed
        // network begins scoring, then settles into a low-opacity input map.
        view.skeleton_alpha = 1.0f;
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    case CinemaAct::kBuild:
        // The target-spacing annotation yields only when the chosen action starts
        // producing actual cells in those same locations.
        view.skeleton_alpha = 0.45f;
        view.reveal = cue.stage_index >= 0 ? static_cast<float>(cue.mesh_action_reveal) : 0.0f;
        view.mesh_alpha = 1.0f;
        view.shrink = cue.stage_index >= 0 ? shrink_for(cue.mesh_action_reveal) : 1.0f;
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    case CinemaAct::kMeshHold: {
        // Open the finished cells just enough to expose their topology, hold
        // that view, then close them and leave the exact delivered mesh still
        // for the final fifth. The subject never rotates or changes data.
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
        if (cue.solve_phase == SolvePhase::kRefine) {
            // Start on the exact ZZ field that requested refinement, then lay the
            // exact topology diff over it. Once the field has dissolved, the
            // existing structural transition continues without a reset.
            const double p = std::clamp(cue.refine_reveal, 0.0, 1.0);
            const float handoff = static_cast<float>(smoothstep(p / kFieldToMeshHandoff));
            const double added = smoothstep((p - 0.40) / 0.60);
            view.skeleton_alpha = 0.35f * handoff;
            view.reveal = static_cast<float>(added);
            view.mesh_alpha = handoff;
            view.shrink = 0.0f;
            view.incremental_transition = true;
            view.transition_progress = static_cast<float>(p);
            view.overlay_on_results = true;
            view.result_alpha = 1.0f - handoff;
        } else if (cue.solve_phase == SolvePhase::kRefineHold) {
            view.skeleton_alpha = 0.35f;
            view.reveal = 1.0f;
            view.mesh_alpha = 1.0f;
            view.shrink = 0.0f;
            view.incremental_transition = true;
            view.transition_progress = 1.0f;
        } else if (cue.solve_phase == SolvePhase::kStressSweep) {
            // The finished mesh carries into analysis and fades only as stress
            // colours arrive, avoiding a mesh→grey reset at the act/pass edge.
            const float mesh_carry =
                static_cast<float>(1.0 - smoothstep(cue.field_front / kMeshToFieldHandoff));
            view.skeleton_alpha = 0.25f * mesh_carry;
            view.reveal = 1.0f;
            view.mesh_alpha = mesh_carry;
            view.shrink = 0.0f;
            view.overlay_on_results = mesh_carry > 0.0f;
        } else {
            // Result modes draw their own surface. kNone is the no-solve fallback
            // and therefore keeps the authoritative mesh instead.
            view.skeleton_alpha = 0.25f;
            view.reveal = 1.0f;
            view.mesh_alpha = cue.solve_phase == SolvePhase::kNone ? 1.0f : 0.0f;
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
    const double stress_display_max =
        index < state.stress_histograms.size() && state.stress_histograms[index].p99 > 0.0
            ? state.stress_histograms[index].p99
            : result.max_von_mises;
    const double error_display_max =
        index < state.error_histograms.size() && state.error_histograms[index].p99 > 0.0
            ? state.error_histograms[index].p99
            : result.max_nodal_eta;
    const auto gradient_display_max = [&]() {
        const CinemaHistogram& histogram = state.gradient_histogram(index);
        return histogram.p99 > 0.0 ? histogram.p99 : state.gradient_max(index);
    };
    const auto arm_sweep = [&](bool moving, DisplayMode carry = DisplayMode::kCinema,
                               float carry_max = 1.0f) {
        out.sweep.active = moving;
        out.sweep.axis = state.sweep_axis;
        out.sweep.feather = kSweepFeather;
        out.sweep.front =
            moving ? static_cast<float>(cue.field_front) * (1.0f + kSweepFeather) : 1.0f;
        out.sweep.carry_mode = carry;
        out.sweep.carry_max = carry_max;
    };
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
    case SolvePhase::kStressHold:
        // That pass's own von Mises, on the geometry that pass solved. Zero
        // exaggeration: these beats are about the field, and the shape's real
        // response is what the load ramp below is for.
        out.mode = DisplayMode::kResultsVonMises;
        out.result_max = static_cast<float>(stress_display_max);
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
        const bool has_gradient = !state.gradient_field(index).empty() && gmax > 0.0;
        if (!has_gradient) {
            out.mode = DisplayMode::kResultsVonMises;
            out.result_max = static_cast<float>(stress_display_max);
        } else {
            out.mode = DisplayMode::kResultsGradient;
            out.result_max = static_cast<float>(gradient_display_max());
        }
        arm_sweep(cue.solve_phase == SolvePhase::kGradientSweep, DisplayMode::kResultsVonMises,
                  static_cast<float>(stress_display_max));
        break;
    }
    case SolvePhase::kError:
    case SolvePhase::kErrorHold: {
        // The ZZ error field grows directly out of whichever measured field was
        // on screen before it: gradient on pass 0, stress on later passes.
        out.mode = DisplayMode::kResultsError;
        out.result_max = static_cast<float>(error_display_max);
        const double gmax = state.gradient_max(index);
        const bool carry_gradient =
            index == 0 && !state.gradient_field(index).empty() && gmax > 0.0;
        arm_sweep(
            cue.solve_phase == SolvePhase::kError,
            carry_gradient ? DisplayMode::kResultsGradient : DisplayMode::kResultsVonMises,
            static_cast<float>(carry_gradient ? gradient_display_max() : stress_display_max));
        break;
    }
    case SolvePhase::kRefine:
        // Keep the ZZ field under the exact topology transition during the
        // opacity handoff configured by `cinema_view`.
        out.mode = DisplayMode::kResultsError;
        out.result_max = static_cast<float>(error_display_max);
        break;
    case SolvePhase::kRefineHold:
        break; // kCinema: the new mesh is complete and held
    case SolvePhase::kLoadRamp:
    case SolvePhase::kHold: {
        out.mode = DisplayMode::kResultsVonMises;
        const double lambda = std::clamp(cue.load_factor, 0.0, 1.0);
        out.deform_scale = static_cast<float>(lambda * base_deform_scale);
        // The stress scales with the load exactly as the displacement does, so
        // the colour has to ramp with the shape or the frame would show a fully
        // stressed part that has barely moved. Dividing the measured p99 display
        // cap by λ makes the drawn colour λ·s / p99 while the true peak remains
        // stated numerically in the pane and strip.
        out.result_max =
            static_cast<float>(stress_display_max / std::max(lambda, kMinLoadFactor));
        arm_sweep(cue.solve_phase == SolvePhase::kLoadRamp, DisplayMode::kResultsError,
                  static_cast<float>(error_display_max));
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
    // The solver's own words first. CG notes may also contain "LDLT" when a
    // memory budget caused a downgrade, so test CG before direct LDLT.
    for (const auto& stage : state.solve_stages) {
        if (stage.result.solver_note.find("CG") != std::string::npos ||
            stage.result.solver_note.find("cg") != std::string::npos) {
            return "cg";
        }
    }
    for (const auto& stage : state.solve_stages) {
        if (stage.result.solver_note.find("LDLT") != std::string::npos ||
            stage.result.solver_note.find("ldlt") != std::string::npos) {
            return "direct_ldlt";
        }
    }
    // No note. That is not a licence to guess: it is a fact with a consequence.
    // `fea::select_solve_method` sends auto to CG only above the FREE-DOF
    // threshold. Both explicit `SimSetup::solve_method` overrides and an
    // automatic memory-budget downgrade emit notes, so only a genuinely silent
    // legacy/direct result reaches this arithmetic fallback: total DOF below
    // the free-DOF threshold proves direct factorisation.
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
            state.sizing.field_points, state.sizing.field_h_before, state.sizing.field_h_after,
            state.sizing.edge_points, state.sizing.edge_h_before, state.sizing.edge_h_after);
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

} // namespace polymesh::gui
