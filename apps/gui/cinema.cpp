// SPDX-License-Identifier: BSD-3-Clause
#include "cinema.hpp"

#include "colormap.hpp"
#include "theme.hpp"

#include "fea/solve.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <utility>

namespace polymesh::gui {
namespace {

/// Act spans as fractions of the take.
///
/// They are NOT one act per data source any more, because the two data sources
/// no longer take turns. The advisor's pass lane runs continuously across
/// `kDeliberate` AND `kBuild` (0.31 of the take), while the mesher's stage lane
/// runs inside `kBuild` alone (0.20): through `kBuild` both counters advance on
/// the same clock, which is the whole point of this shape. `kSolve` gets the
/// largest share because it is the only act that shows work with a real
/// ordering of its own -- solve, estimate, refine, solve, then the load ramp --
/// rather than a fixed-length sweep over a recorded list.
///
/// Measured pacing on the film's case (39 forward passes, 20 construction
/// stages of which 10 are the initial fill) at the render script's 1800-frame
/// default, 30.0 s:
///
///   forward pass       0.292 s before  ->  0.238 s   (0.31 x 30 / 39)
///   construction stage 0.570 s before  ->  0.540 s   ((0.20 x 30 - 0.6) / 10)
///
/// The "before" pass figure is the old 0.38-of-the-take advisor act over the
/// same 39 passes. The "before" stage figure is the old 0.38 mesh act over all
/// 20 stages on THIS case; the 0.633 s quoted when the schedule was designed was
/// the same fraction over box_hole_s0's 18. The 10 stages of the adaptive
/// remesh are no longer replayed as ten more stage names -- the closing act
/// shows the mesh that pass actually SOLVED instead, which is the thing the
/// error field asked for.
///
/// At 1200 frames (20.0 s), the length this composition is tuned for, those
/// become 0.159 s and 0.340 s; at 900 frames (15.0 s) they are 0.119 s and
/// 0.240 s. The fractions sum to 1 by construction; `cinema_cue` asserts nothing
/// and simply clamps, because a recording is an exact frame count, not an exact
/// act sum.
constexpr std::array<double, 4> kActFraction = {0.06, 0.11, 0.20, 0.63};

/// Relative lengths of the closing act's beats. Weights, not seconds: the act's
/// own span is divided among however many beats the real solve produced, so a
/// case with three adaptive passes gets nine beats out of the same act and a
/// non-adaptive one gets four.
///
/// The refine beat is the long one because it is the only beat that reveals a
/// whole mesh element by element; the field and error beats are stills of a
/// field that is already complete and only need long enough to be read. The
/// ramp is second-longest because it is the one continuous motion in the act.
constexpr double kFieldWeight = 1.0;
constexpr double kErrorWeight = 1.0;
constexpr double kRefineWeight = 2.4;
constexpr double kRampWeight = 2.2;
constexpr double kHoldWeight = 0.6;

/// Per-element reveal geometry, retuned for the film's mesh density.
///
/// This reveal was first built on box_hole_s0 at the advisor's h_rel = 0.2: 568
/// cells, each tens of pixels across in the cinema pane. The film's case is
/// sphere_box_s0 at h_rel = 0.08 — 11,692 cells, 20.6x as many, in the same
/// pane — and the settings that described 568 cells bury 11,692.
///
/// Measured, two binaries differing only in these four numbers, same take, same
/// frame indices, and the fraction of the part's own painted pixels that are
/// near-black (luminance < 60, i.e. edge rather than surface) inside the
/// concurrent act:
///
///   frame   0.35 / 0.50 / 1.00 / 1.5 px      0.22 / 0.33 / 0.30 / 1.0 px
///      72   36.2% dark, mean lum 69.9        6.6% dark, mean lum 98.1
///      78   50.3% dark, mean lum 65.0        6.3% dark, mean lum 112.9
///      90   24.9% dark, mean lum 93.1        3.9% dark, mean lum 118.4
///     125   21.9% dark, mean lum 85.4        3.6% dark, mean lum 105.3
///
/// At half the part's pixels being cell outline the fill is a black grid with
/// orange in the gaps: the shading that distinguishes one face of a cell from
/// another is gone, and so is the reveal front, because a front made of dark
/// outlines does not read against a dark background. Luminance spread over the
/// part drops from 42-60 to 18-27 with the thinner pass, which is the same fact
/// stated as contrast. The smaller shrink, collapsing over the first third of
/// each beat rather than the first half, is what keeps the front a surface of
/// distinct cells instead of detached specks at this cell size.
constexpr double kRevealShrink = 0.22;
constexpr double kRevealShrinkFraction = 0.33;
constexpr float kMeshEdgeAlpha = 0.30f;
constexpr float kMeshEdgeWidth = 1.0f;

/// Opacity the network panel is held at through the closing act, where its pass
/// lane has run out and it is showing the final re-score pass rather than
/// advancing. Opacity only: the panel's width share, its content and every
/// number in it are exactly what they were at full strength.
constexpr double kHeldNetworkAlpha = 0.55;

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
/// on-screen sentence shrinks with it. A few hundred is where individual paths
/// stay separable at the sizes the recorder captures.
constexpr std::size_t kDrawnConnections = 280;

/// Widest a head-unit node is allowed to get, and the gap between it and its
/// label. Fixed so the label gutter can be reserved before the column height
/// is known.
constexpr float kNodeRadiusMax = 9.0f;
constexpr float kLabelGap = 8.0f;

/// printf into a std::string.
///
/// The ticker and the network disclosures have to exist as strings before
/// anything is drawn, so the composition can size itself from them. They keep
/// the printf format strings this surface already used: rewriting a hundred
/// conversions into another formatting library would be a hundred chances to
/// change a displayed number, which is the one thing this file may not do.
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

/// How the surface names the pass a beat is showing. ONE definition, used by
/// both the network panel and the ticker, so the two cannot label the same
/// instant differently.
///
/// Two indices are in play and both are real: the pass ordinal (1-based, over
/// every forward pass INCLUDING the final re-score) and
/// `advisor::ActivationFrame::candidate`, a 0-based index into the enumerated
/// candidate grid. Side by side without saying which is which they read as an
/// off-by-one, and naming the pass ordinal a candidate ordinal is wrong
/// outright: the grid holds `n_frames - 1` candidates -- the same count
/// `service_cinema_record` prints -- and the last pass is not one of them.
std::string pass_role(int candidate, std::size_t n_frames) {
    if (candidate < 0) {
        return "final re-score of the recommended action";
    }
    return fmt("candidate grid index %d of %zu (0-based)", candidate,
               n_frames > 0 ? n_frames - 1 : 0);
}

/// Cubic smoothstep on [0,1]. Used for opacity and the shrink collapse only --
/// never for a displayed number.
double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

/// Overflow-safe logistic, the same branch-on-sign form as
/// `src/advisor/src/advisor.cpp`: exp of a large positive logit overflows to
/// inf and would turn the probability into NaN.
double sigmoid(double x) {
    if (x >= 0.0) {
        return 1.0 / (1.0 + std::exp(-x));
    }
    const double e = std::exp(x);
    return e / (1.0 + e);
}

ImU32 rgba(const std::array<float, 3>& rgb, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
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

/// `h` as the HUD should state it: the setup's own number, or the fact that it
/// is resolved by the mesher. A cinema that printed a resolved h it had not
/// measured would be exactly the kind of plausible-looking invention this
/// feature exists to avoid.
std::string h_text(const CinemaHud& hud) {
    if (hud.mesh_size > 0.0) {
        return std::format("h {:.4g} mm (SimSetup::mesh_size)", hud.mesh_size * 1e3);
    }
    if (hud.geometry_h > 0.0) {
        return std::format("h auto -> {:.4g} mm (VolumeMeshOutput::geometry_h)",
                           hud.geometry_h * 1e3);
    }
    return "h auto (SimSetup::mesh_size = 0; nothing meshed yet, so no resolved h to report)";
}

/// Virtual-time window the advisor's pass lane sweeps: from the end of the
/// opening to the end of the concurrent act, i.e. across BOTH data acts. One
/// definition, so the pass beat the ticker prints and the pass the network draws
/// can never come from two different schedules.
struct PassLane {
    double t0 = 0.0;
    double t1 = 0.0;
};
PassLane pass_lane(double total) {
    return {kActFraction[0] * total,
            (kActFraction[0] + kActFraction[1] + kActFraction[2]) * total};
}

/// Construction stages belonging to the initial fill, which is the fill the
/// concurrent act shows. Later passes' stages exist in the same list (a fill per
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
/// the order the pipeline computed them, and hands each to `fn(stage, phase,
/// weight)`.
///
/// Per pass: the field that solve produced, the ZZ error field recovered from
/// the same solve, then -- when another pass followed -- the mesh that next pass
/// actually solved on. After the last pass, the load ramp and the hold. That is
/// `pipeline::SolveJob`'s own loop order and nothing else; there is no beat for
/// a per-element solve order or an iteration count, because a direct sparse
/// factorisation has neither.
///
/// Generated rather than materialised so the per-frame cue costs no allocation:
/// it is walked twice, once to total the weights and once to locate the beat,
/// and `n` is `SimSetup::adapt_passes + 1`.
template <class Fn> void for_each_solve_beat(std::size_t n, Fn&& fn) {
    if (n == 0) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto stage = static_cast<int>(i);
        fn(stage, SolvePhase::kField, kFieldWeight);
        fn(stage, SolvePhase::kError, kErrorWeight);
        if (i + 1 < n) {
            fn(stage, SolvePhase::kRefine, kRefineWeight);
        }
    }
    const auto last = static_cast<int>(n) - 1;
    fn(last, SolvePhase::kLoadRamp, kRampWeight);
    fn(last, SolvePhase::kHold, kHoldWeight);
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
    case CinemaAct::kSolve:
        return "solve";
    }
    return "unknown"; // only reachable from an out-of-range int cast
}

const char* cinema_solve_phase_name(SolvePhase phase) {
    switch (phase) {
    case SolvePhase::kNone:
        return "none";
    case SolvePhase::kField:
        return "field";
    case SolvePhase::kError:
        return "error";
    case SolvePhase::kRefine:
        return "refine";
    case SolvePhase::kLoadRamp:
        return "load ramp";
    case SolvePhase::kHold:
        return "hold";
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
    stages.insert(stages.end(), std::make_move_iterator(pending.begin()),
                  std::make_move_iterator(pending.end()));
}

void CinemaState::clear_stages() {
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        pending_stages_.clear();
    }
    stages.clear();
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
    solve_stages.insert(solve_stages.end(), std::make_move_iterator(pending.begin()),
                        std::make_move_iterator(pending.end()));
}

void CinemaState::clear_solve_stages() {
    {
        const std::lock_guard<std::mutex> lock(stage_mutex_);
        pending_solve_stages_.clear();
    }
    solve_stages.clear();
    invalidate_uploads();
}

void CinemaState::invalidate_uploads() {
    uploaded_mesh_source = CinemaMeshSource::kNone;
    uploaded_mesh_index = -1;
    uploaded_solve_stage = -1;
}

void CinemaState::advance(double dt) {
    if (dt > 0.0) {
        t += dt;
    }
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

    // The opening act is the part's. The network panel is held dark through the
    // first half of it and faded up across the second, so the take opens on the
    // outline rather than on a full-strength wall of connections. Pure opacity:
    // the panel's content at alpha 0.4 is the same content it has at alpha 1.
    if (cue.act == CinemaAct::kSkeleton) {
        const double half = 0.5 * cue.act_span;
        cue.network_open =
            static_cast<float>(smoothstep((cue.act_t - half) / std::max(half, 1.0e-9)));
        cue.network_alpha = cue.network_open;
    } else if (cue.act == CinemaAct::kSolve) {
        // The pass lane has run out by here, so the panel is HOLDING its final
        // re-score pass rather than advancing, and it is dimmed to say so.
        // Opacity only: `network_open` stays at 1, because the width share sets
        // the viewport pane's width, the pane's height sets the part's rendered
        // size, and a pane that changed mid-take would resize the subject inside
        // what is meant to be one continuous shot.
        const double fade = std::min(0.5, 0.15 * cue.act_span);
        cue.network_alpha = static_cast<float>(
            1.0 - (1.0 - kHeldNetworkAlpha) * smoothstep(cue.act_t / std::max(fade, 1.0e-9)));
    }

    // ---- lane 1: the advisor's forward passes ----------------------------
    // One beat per real forward pass, in the order the chooser ran them, across
    // kDeliberate AND kBuild as a single uninterrupted sweep. Past the lane the
    // index sticks at the last pass, so the network stays lit with the decision
    // while the answer it chose is computed.
    std::size_t n_frames = 0;
#ifdef POLYMESH_WITH_ADVISOR
    if (state.explanation) {
        n_frames = state.explanation->frames.size();
    }
#endif
    bool pass_lane_live = false;
    if (n_frames > 0) {
        const PassLane lane = pass_lane(total);
        const double beat = (lane.t1 - lane.t0) / static_cast<double>(n_frames);
        cue.pass_beat_seconds = beat;
        if (state.t >= lane.t0) {
            const auto i =
                static_cast<std::size_t>((state.t - lane.t0) / std::max(beat, 1.0e-9));
            cue.frame_index = static_cast<int>(std::min(i, n_frames - 1));
            pass_lane_live = state.t < lane.t1;
        }
    }

    // The ranking's outcome may be stated from the first frame of the concurrent
    // act, which `cinema_decision_lead` guarantees is before the first element
    // of the fill appears.
    cue.decision_locked = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kSolve;

    // ---- lane 2: the mesher's own construction stages -------------------
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    bool stage_lane_live = false;
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
                stage_lane_live = true;
            }
        } else if (cue.act == CinemaAct::kSolve) {
            // The finished fill is where the closing act starts, and it stays
            // named in the ticker. No mesh source: the closing act draws fields
            // out of `solve_stages`, and the one beat that uses the per-element
            // buffer is kRefine, which points it at the refined mesh itself.
            cue.stage_index = static_cast<int>(n_fill - 1);
            cue.stage_reveal = 1.0;
        }
    }
    // The claim the ticker makes, taken from the schedule rather than restated
    // beside it: both lanes are advancing on this frame.
    cue.concurrent = pass_lane_live && stage_lane_live;

    // ---- the closing act: the real solve / estimate / refine loop --------
    if (cue.act == CinemaAct::kSolve && !state.solve_stages.empty()) {
        const std::size_t n_solve = state.solve_stages.size();
        double weight_total = 0.0;
        for_each_solve_beat(n_solve, [&](int, SolvePhase, double w) { weight_total += w; });
        const double unit = cue.act_span / std::max(weight_total, 1.0e-9);
        double at = 0.0;
        int beat_stage = 0;
        SolvePhase beat_phase = SolvePhase::kField;
        double beat_t0 = 0.0;
        double beat_span = 1.0;
        for_each_solve_beat(n_solve, [&](int stage, SolvePhase phase, double w) {
            const double span = w * unit;
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
        if (beat_phase == SolvePhase::kRefine) {
            cue.refine_reveal = std::clamp(x, 0.0, 1.0);
            cue.mesh_source = CinemaMeshSource::kSolvedPass;
            cue.mesh_source_index = beat_stage + 1;
        } else if (beat_phase == SolvePhase::kLoadRamp) {
            // Linear, never eased. λ is a number on screen and u(λ) = λ·u is
            // exact, so easing λ would give the deforming shape a rate of change
            // the linear solve does not have — which is the one thing a ramp
            // labelled "exact linear response" may not do.
            cue.load_factor = std::clamp(x, 0.0, 1.0);
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
        break;
    case CinemaAct::kDeliberate:
        // Nothing has been meshed while the network deliberates alone, so nothing
        // may be drawn as mesh. The outline is the whole picture here.
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
    case CinemaAct::kSolve:
        if (cue.solve_phase == SolvePhase::kRefine) {
            // The refined mesh appearing, element by element, in the order it is
            // stored in the mesh the next pass actually solved.
            view.skeleton_alpha = 0.35f;
            view.reveal = static_cast<float>(cue.refine_reveal);
            view.mesh_alpha = 1.0f;
            view.shrink = shrink_for(cue.refine_reveal);
        } else {
            // Every other beat of this act renders a field, so these values are
            // only reached when no `pipeline::SolveStage` arrived at all and the
            // act holds the finished fill instead. The ticker says which.
            view.skeleton_alpha = 0.25f;
            view.reveal = 1.0f;
            view.mesh_alpha = 1.0f;
            view.shrink = 0.0f;
        }
        break;
    }
    return view;
}

CinemaRender cinema_render(const CinemaState& state, const CinemaCue& cue,
                           double base_deform_scale) {
    CinemaRender out;
    if (cue.act != CinemaAct::kSolve || cue.solve_stage_index < 0 ||
        static_cast<std::size_t>(cue.solve_stage_index) >= state.solve_stages.size()) {
        return out; // kCinema, undeformed; `result_max` is unused there
    }
    const pipeline::SolveResult& result =
        state.solve_stages[static_cast<std::size_t>(cue.solve_stage_index)].result;
    switch (cue.solve_phase) {
    case SolvePhase::kField:
        // That pass's own von Mises, on the geometry that pass solved. Zero
        // exaggeration: this beat is about the field, and the shape's real
        // response is what the load ramp below is for.
        out.mode = DisplayMode::kResultsVonMises;
        out.result_max = static_cast<float>(result.max_von_mises);
        break;
    case SolvePhase::kError:
        // The ZZ error field recovered from that same solve — the field that
        // decided where the next pass refined.
        out.mode = DisplayMode::kResultsError;
        out.result_max = static_cast<float>(result.max_nodal_eta);
        break;
    case SolvePhase::kRefine:
        break; // kCinema: this beat draws the refined MESH, not a field
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

void sync_cinema_viewport(CinemaState& state, const CinemaCue& cue, Viewport& viewport) {
    // Per-element buffer: re-uploaded only when the cue names a different mesh,
    // because `set_cinema_mesh` rebuilds every element's own faces.
    if (cue.mesh_source != CinemaMeshSource::kNone && cue.mesh_source_index >= 0 &&
        (cue.mesh_source != state.uploaded_mesh_source ||
         cue.mesh_source_index != state.uploaded_mesh_index)) {
        const auto i = static_cast<std::size_t>(cue.mesh_source_index);
        const fea::NodalMesh* mesh = nullptr;
        if (cue.mesh_source == CinemaMeshSource::kFillStage && i < state.stages.size()) {
            mesh = &state.stages[i].mesh;
        } else if (cue.mesh_source == CinemaMeshSource::kSolvedPass &&
                   i < state.solve_stages.size()) {
            mesh = &state.solve_stages[i].result.volume_mesh;
        }
        if (mesh != nullptr) {
            viewport.set_cinema_mesh(*mesh);
            state.uploaded_mesh_source = cue.mesh_source;
            state.uploaded_mesh_index = cue.mesh_source_index;
        }
    }
    // Result buffers: one re-bake per adaptive pass, so the field drawn beside
    // "pass 0" is the field pass 0 produced and not the final answer relabelled.
    if (cue.solve_stage_index >= 0 && cue.solve_stage_index != state.uploaded_solve_stage &&
        static_cast<std::size_t>(cue.solve_stage_index) < state.solve_stages.size()) {
        viewport.set_result(
            state.solve_stages[static_cast<std::size_t>(cue.solve_stage_index)].result);
        state.uploaded_solve_stage = cue.solve_stage_index;
    }
    viewport.set_cinema_view(cinema_view(state, cue));
}

// ---- act 1: the part's own skeleton --------------------------------------

void build_cinema_skeleton(CinemaState& state, const pipeline::Model& model,
                           Viewport& viewport) {
    std::vector<std::vector<Eigen::Vector3d>> polylines;
    state.skeleton_note.clear();
    state.skeleton_source = SkeletonSource::kNone;

    if (model.cad && !model.cad->empty()) {
        try {
            // 16 samples per edge: enough that a fillet or a bore rim reads as
            // a curve rather than a chord chain at the 1600 px the recorder
            // captures, and the same density the CLI's BRep wire export uses.
            const geom::CadTopology topology = geom::extract_topology(*model.cad, 16);
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
        state.decision_note = std::format(
            "REFUSED and NOT applied ({}): ood_distance {:.4g}, failure_prob {:.4g}{} — the "
            "mesh act runs on the studio's own setup",
            decision.budget_refusal ? "no candidate fit the max_dof budget" : "vetoed",
            decision.ood_distance, decision.failure_prob,
            decision.note.empty() ? std::string{} : std::format(", note: {}", decision.note));
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
            "applied to SimSetup: mesher {} · h {:.4g} mm (h_rel {:.4g} × diag {:.4g} mm) · "
            "adapt {} · η target {:.4g} · order {}{}",
            decision.mesher, setup.mesh_size * 1e3, decision.h_rel, diag * 1e3,
            setup.adapt_passes, setup.eta_target, setup.p_elevate ? 2 : 1,
            decision.order > 2
                ? std::format(" (order {} executed as quadratic)", decision.order)
                : std::string{});
    } else {
        // Same refusal the CLI makes: meshing something other than what was
        // recommended, while reporting the recommendation, is the failure mode
        // this whole surface exists to rule out.
        state.decision_note = std::format(
            "NOT applied: advised mesher '{}' is not a name this build recognises "
            "(pipeline::mesher_from_name refused it) — the mesh act runs on the studio's own "
            "setup rather than silently meshing something else",
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

void draw_cinema_network(CinemaState& state, const CinemaCue& cue) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
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

    // ---- everything this panel will say, before anything is placed ------
    // The layout is sized FROM the text: the caption, the disclosure and the
    // head labels reserve exactly the room they need and the graph takes what
    // is left. Composing the strings first is what makes that possible, and a
    // clipped disclosure is a disclosure that was not made.

    // Per-layer normalisation. Trunk and head magnitudes differ by roughly a
    // factor of ten, so one shared scale would flatten the trunk into a flat
    // grey column — the same reason scripts/advisor/figures.py scales the
    // activation heatmap per row. The scales are printed below the graph.
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

    const std::vector<CinemaLine> caption = {
        {palette.text_dim, fmt("deployed advisor graph — %s", state.advisor_dir.c_str())},
        {palette.text_dim,
         "node fills are model.onnx's own tensors: outputs trunk_input, trunk_fc1, "
         "trunk_fc2 (post-GELU) and the head vector — not a re-implementation of the "
         "forward pass"},
    };

    std::vector<CinemaLine> disclosure;
    if (frame == nullptr) {
        disclosure.push_back(
            {palette.status_warn,
             "structure only: no forward pass is being shown on this beat, so "
             "every node is drawn at its minimum radius"});
    } else {
        // `drawn` is the real size of the subset that was just selected, so this
        // sentence tracks kDrawnConnections instead of restating it.
        disclosure.push_back(
            {palette.text_dim,
             fmt("showing the %zu strongest of %zu connections, by |weight × source "
                 "activation| for this frame (activation_layout.json edge blocks "
                 "%zu×%zu, %zu×%zu, %zu×%zu)",
                 drawn, total_connections, layout.edges[0].rows, layout.edges[0].cols,
                 layout.edges[1].rows, layout.edges[1].cols, layout.edges[2].rows,
                 layout.edges[2].cols)});
        disclosure.push_back(
            {palette.text_dim,
             fmt("node radius and fill are |a| / max|a| WITHIN each layer — input "
                 "±%.3g · trunk.fc1 ±%.3g · trunk.fc2 ±%.3g · heads ±%.3g — because "
                 "trunk and head magnitudes differ by ~10×, and one shared scale "
                 "would flatten the trunk",
                 static_cast<double>(layer_max[0]), static_cast<double>(layer_max[1]),
                 static_cast<double>(layer_max[2]), static_cast<double>(layer_max[3]))});
        disclosure.push_back(
            {palette.text_dim,
             fmt("pass %d of %zu · %s · blue → white → red is the sign of the "
                 "activation (colormap.hpp signed_colormap), and line opacity and width "
                 "are |weight × source activation| / max",
                 cue.frame_index + 1, frames.size(),
                 pass_role(frame->candidate, frames.size()).c_str())});
    }

    const auto& heads = layout.layers[3];
    std::vector<std::string> head_text(heads.size);
    for (std::size_t i = 0; i < heads.size; ++i) {
        const char* name = i < heads.labels.size() ? heads.labels[i].c_str() : "(unlabelled)";
        head_text[i] = values[3] != nullptr ? std::format("{} {:+.4g}", name, (*values[3])[i])
                                            : std::format("{} —", name);
    }

    // ---- geometry, derived from the measured text ------------------------
    const float fade = std::clamp(cue.network_alpha, 0.0f, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    // The prose is set smaller than the UI face so the graph gets the panel.
    // Nothing is dropped to make room: it wraps.
    const float body_size = std::floor(ImGui::GetFontSize() * 0.80f);
    const float para_gap = std::floor(body_size * 0.30f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float wrap = std::max(120.0f, region.x);

    const auto faded = [fade](ImVec4 c) {
        c.w *= fade;
        return ImGui::ColorConvertFloat4ToU32(c);
    };
    const auto para_h = [&](const std::string& s) {
        return font->CalcTextSizeA(body_size, FLT_MAX, wrap, s.c_str()).y + para_gap;
    };
    const auto block_h = [&](const std::vector<CinemaLine>& lines) {
        float h = 0.0f;
        for (const auto& line : lines) {
            h += para_h(line.text);
        }
        return h;
    };
    const auto draw_block = [&](const std::vector<CinemaLine>& lines, float y) {
        for (const auto& line : lines) {
            dl->AddText(font, body_size, ImVec2(origin.x, y), faded(line.color),
                        line.text.c_str(), nullptr, wrap);
            y += para_h(line.text);
        }
    };

    const float caption_h = block_h(caption);
    const float disclosure_h = block_h(disclosure);
    const float graph_top = origin.y + caption_h;
    const float graph_h = std::max(140.0f, region.y - caption_h - disclosure_h);

    // Head labels are real unit names carrying real values, and they are long
    // (`policy_mesher_logit_graded_tet +5.722`). Set them at whatever size makes
    // the longest one fit the gutter the panel can afford, then reserve exactly
    // that gutter — so no label is ever cut off, at any panel width.
    const auto widest_label = [&](float size) {
        float w = 0.0f;
        for (const auto& t : head_text) {
            w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x);
        }
        return w;
    };
    float label_size = std::floor(ImGui::GetFontSize() * 0.78f);
    float label_w = widest_label(label_size);
    const float label_budget = std::max(80.0f, region.x * 0.42f);
    if (label_w > label_budget && label_w > 0.0f) {
        label_size = std::max(8.0f, std::floor(label_size * label_budget / label_w));
        label_w = widest_label(label_size);
    }

    // Enough left margin for the leftmost column's own centred header.
    constexpr float kColumnMargin = 30.0f;
    const float gutter = label_w + kNodeRadiusMax + kLabelGap + 4.0f;
    const float band_w = std::max(120.0f, region.x - kColumnMargin - gutter);
    const float header_h = std::floor(body_size * 1.35f);
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

    draw_block(caption, origin.y);
    draw_block(disclosure, graph_top + graph_h);

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
                        rgba(signed_colormap(t), (0.05f + 0.85f * weight) * fade),
                        0.55f + 1.35f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.3f;
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float x = column_x(l);
        const float spacing = col_h / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 1.8f, kNodeRadiusMax);
        const std::string header = std::format("{} {}", layer.name, layer.size);
        const float header_w = font->CalcTextSizeA(body_size, FLT_MAX, 0.0f, header.c_str()).x;
        dl->AddText(font, body_size, ImVec2(x - 0.5f * header_w, graph_top),
                    faded(palette.text_dim), header.c_str());
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
                dl->AddCircleFilled(ImVec2(x, y), r * 3.0f, rgba(rgb, 0.055f * mag * fade));
                dl->AddCircleFilled(ImVec2(x, y), r * 1.8f, rgba(rgb, 0.110f * mag * fade));
            }
            dl->AddCircleFilled(ImVec2(x, y), r, rgba(rgb, (0.40f + 0.60f * mag) * fade));
            if (mag > 0.55f) {
                dl->AddCircle(ImVec2(x, y), r + 1.4f,
                              ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.45f * mag * fade)),
                              0, 1.2f);
            }
        }
    }

    // ---- head units, with their real names and real values --------------
    {
        const float head_r = std::clamp(
            0.46f * col_h / static_cast<float>(std::max<std::size_t>(heads.size, 1)), 1.8f,
            kNodeRadiusMax);
        const float x = column_x(3) + head_r + kLabelGap;
        const ImU32 col = faded(palette.text);
        for (std::size_t i = 0; i < heads.size; ++i) {
            dl->AddText(font, label_size, ImVec2(x, node_y(i, heads.size) - 0.5f * label_size),
                        col, head_text[i].c_str());
        }
    }

    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
#endif
}

// ---- ticker ---------------------------------------------------------------

namespace {

void push_line(std::vector<CinemaLine>& out, const ImVec4& color, std::string text) {
    out.push_back({color, std::move(text)});
}

void skeleton_ticker(const CinemaState& state, std::vector<CinemaLine>& out) {
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        push_line(out, palette.status_ok,
                  fmt("outline source: BRep feature edges — "
                      "geom::extract_topology(*model.cad, 16), %zu edge polylines / %zu "
                      "sampled points",
                      state.skeleton_polylines, state.skeleton_points));
        break;
    case SkeletonSource::kSharpEdges:
        push_line(out, palette.status_warn,
                  fmt("outline source: tessellation crease network — "
                      "geom::detect_sharp_edges(model.surface, 30°), %zu segments. This "
                      "part carries no BRep (STL input), so this is NOT a CAD skeleton",
                      state.skeleton_polylines));
        break;
    case SkeletonSource::kUnavailable:
        push_line(out, palette.status_err,
                  fmt("outline source: UNAVAILABLE — %s. Nothing is drawn in its place",
                      state.skeleton_note.c_str()));
        break;
    case SkeletonSource::kNone:
        push_line(out, palette.status_err,
                  "outline source: none — no part is loaded, so there is nothing to outline");
        break;
    }
}

void advisor_ticker(const CinemaState& state, const CinemaCue& cue,
                    std::vector<CinemaLine>& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)cue;
    push_line(out, palette.status_err,
              "advisor act skipped: built with POLYMESH_WITH_ADVISOR=OFF");
    if (!state.advisor_note.empty()) {
        push_line(out, palette.text_dim, state.advisor_note);
    }
#else
    if (!state.explanation) {
        push_line(out, palette.status_err,
                  fmt("advisor act skipped: %s",
                      state.advisor_note.empty()
                          ? "no `cinema advisor <dir>` was run for this take"
                          : state.advisor_note.c_str()));
        push_line(out, palette.text_dim,
                  "no decision is shown, and none was applied — the mesh act runs on "
                  "the studio's own setup");
        return;
    }
    const auto& explanation = *state.explanation;
    const auto& frames = explanation.frames;
    if (cue.frame_index >= 0 && static_cast<std::size_t>(cue.frame_index) < frames.size()) {
        const auto& f = frames[static_cast<std::size_t>(cue.frame_index)];
        const double failure_prob = sigmoid(f.outputs.failure_logit);
        push_line(out, palette.text,
                  fmt("pass %d/%zu — %s — scoring mesher %s · h_rel %.4g · order %d · "
                      "adapt %d · η target %.4g · p_elevate %d",
                      cue.frame_index + 1, frames.size(),
                      pass_role(f.candidate, frames.size()).c_str(), f.action.mesher.c_str(),
                      f.action.h_rel, f.action.order, f.action.adapt_passes,
                      f.action.eta_target, f.action.p_elevate ? 1 : 0));
        push_line(
            out, palette.text_dim,
            fmt("rel_err_rel %+.5g (ranking key, per-case, lower is better) · dof 10^%.3f = "
                "%.0f · mesh %.0f ms · solve %.0f ms · geo chamfer 10^%.3f",
                f.score, f.outputs.dof_log10, std::pow(10.0, f.outputs.dof_log10),
                std::pow(10.0, f.outputs.mesh_ms_log10),
                std::pow(10.0, f.outputs.solve_ms_log10), f.outputs.geo_chamfer_log10));
        // `ranked` and `over_budget` are candidate-loop bookkeeping: the final
        // re-score pass is not a candidate, so both stay false there and
        // reporting them would say "not ranked, within budget" about a pass
        // that was never a candidate in the first place.
        const std::string bookkeeping =
            f.candidate < 0
                ? std::string(" · re-score of the action the ranking already chose")
                : std::format(" · {} · {}",
                              f.over_budget ? "over the max_dof budget"
                                            : "within the max_dof budget",
                              f.ranked ? "ranked" : "not ranked (score was not finite)");
        push_line(out, f.gate_pass ? palette.status_ok : palette.status_warn,
                  fmt("gate σ(failure_logit) %.4g %s threshold %.4g → %s%s%s", failure_prob,
                      f.gate_pass ? "≤" : ">", explanation.gate_threshold,
                      f.gate_pass ? "PASS" : "DROPPED", bookkeeping.c_str(),
                      f.recommended ? " · THIS IS THE RECOMMENDED ACTION" : ""));
    } else {
        push_line(
            out, palette.text_dim,
            fmt("%zu forward passes recorded; the first beat has not started", frames.size()));
    }
    // The decision itself, from the first frame of the concurrent act onward —
    // which `cinema_decision_lead` puts strictly before the first element of the
    // fill. Before that the ticker must not pre-announce an outcome the beats
    // have not shown, and after it the fill on screen has to be readable as the
    // execution of THIS action rather than something that arrived first.
    if (cue.decision_locked) {
        const auto& d = explanation.decision;
        push_line(out, d.vetoed ? palette.status_err : palette.status_ok,
                  fmt("DECISION mesher %s · h_rel %.4g · order %d · adapt %d · η %.4g · "
                      "p_elevate %d · failure_prob %.4g · ood_distance %.4g%s%s",
                      d.mesher.c_str(), d.h_rel, d.order, d.adapt_passes, d.eta_target,
                      d.p_elevate ? 1 : 0, d.failure_prob, d.ood_distance,
                      d.vetoed ? " · REFUSED" : "", d.clamped ? " · clamped" : ""));
        push_line(out, state.decision_applied ? palette.status_ok : palette.status_warn,
                  state.decision_note);
        // Stated because the composition would otherwise invite the wrong
        // reading: passes keep arriving on screen after the outcome is shown, and
        // the fill grows beside them. Both sequences are recordings. The ranking
        // ran to completion inside Advisor::explain() before `solve` was issued,
        // so the decision really was final before the mesher emitted anything —
        // and the remaining passes on screen are the rest of that same finished
        // deliberation, not new evidence arriving late.
        if (cue.frame_index >= 0 && cue.frame_index + 1 < static_cast<int>(frames.size())) {
            push_line(out, palette.text_dim,
                      fmt("all %zu forward passes completed inside Advisor::explain() before "
                          "`solve` was issued; the pass lane is still replaying them (%d of "
                          "%zu shown) beside the fill they chose. The decision above was "
                          "already final when the mesher emitted its first stage",
                          frames.size(), cue.frame_index + 1, frames.size()));
        }
    }
#endif
}

void fill_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                 std::vector<CinemaLine>& out) {
    if (state.stages.empty()) {
        push_line(out, palette.status_err,
                  "no construction stages were collected: "
                  "pipeline::SolveJob::on_mesh_stage received none");
        push_line(out, palette.text_dim,
                  "the stage sink is installed only while `cinema on`, and only the "
                  "graded tet fill emits stages — run `cinema on` before `solve`. "
                  "Nothing is drawn in place of stages that were never emitted");
        return;
    }
    if (cue.stage_index < 0) {
        push_line(out, palette.text_dim,
                  fmt("the fill has not started: the advised action above is held on screen "
                      "for %.3f s (CinemaState::kDecisionLead) before the first element "
                      "appears, so nothing on this frame can be read as preceding it",
                      cinema_decision_lead(state)));
        return;
    }
    const auto idx = static_cast<std::size_t>(cue.stage_index);
    const auto& stage = state.stages[idx];
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    push_line(out, palette.text,
              fmt("stage %zu/%zu '%s' — emission index %d, adapt pass %d, %zu elements / "
                  "%zu nodes in this stage%s",
                  idx + 1, n_fill, stage.stage.c_str(), stage.index, stage.pass,
                  stage.mesh.elements.size(), stage.mesh.nodes.size(),
                  cue.concurrent ? " — CONCURRENT with the pass lane above: both counters "
                                   "are advancing on this frame"
                                 : ""));
    // The viewport draws every element whose index is below reveal * count, so
    // the drawn count is that product -- exact arithmetic on two real numbers,
    // not an estimate of progress.
    const auto drawn =
        static_cast<std::size_t>(cue.stage_reveal * static_cast<double>(hud.cinema_elements));
    push_line(out, palette.text_dim,
              fmt("reveal %.0f%% — %zu of %zu drawable elements at %.0f elements per "
                  "recorded frame, appearing in the mesher's own emission order (their "
                  "index in mesh.elements): nothing is sorted, and no element is drawn "
                  "before the stage that built it",
                  100.0 * cue.stage_reveal, drawn, hud.cinema_elements,
                  cue.stage_beat_seconds > 0.0
                      ? static_cast<double>(hud.cinema_elements) * CinemaState::kRecordStep /
                            cue.stage_beat_seconds
                      : 0.0));
    if (hud.cinema_skipped_elements > 0) {
        push_line(out, palette.status_warn,
                  fmt("%zu of this stage's %zu elements could not be triangulated for "
                      "drawing (degenerate connectivity or faceless poly cells) and are "
                      "absent from the reveal",
                      hud.cinema_skipped_elements, stage.mesh.elements.size()));
    }
}

/// The linear solver's own account of itself, or the fact that it gave none.
///
/// `fea::SolveOptions::on_note` is the only channel the solver has and it speaks
/// on the CG path and on a memory-budget downgrade. Below `cg_threshold` with
/// the budget satisfied it says nothing, which is what happens on this film's
/// case — so the surface reports the silence AND the arithmetic that makes the
/// silence conclusive, and never a method name dressed up as something the
/// solver said.
void solver_lines(const pipeline::SolveStage& stage, std::vector<CinemaLine>& out) {
    const fea::SolveOptions defaults;
    if (!stage.solver_note.empty()) {
        push_line(out, palette.text,
                  fmt("linear solver, verbatim from fea::SolveOptions::on_note: %s",
                      stage.solver_note.c_str()));
        return;
    }
    if (stage.trace.n_dof == 0) {
        push_line(out, palette.status_warn,
                  "this pass emitted no solver note (fea::SolveOptions::on_note was never "
                  "called) and PassTrace::n_dof is 0, so which linear solver ran is not "
                  "established here and is not guessed");
        return;
    }
    const bool below = static_cast<Eigen::Index>(stage.trace.n_dof) <= defaults.cg_threshold;
    push_line(out, below ? palette.status_ok : palette.status_warn,
              fmt("this pass emitted NO solver note — fea::SolveOptions::on_note was never "
                  "called, which fea does only when it neither iterates nor overrides its "
                  "own method choice. fea::select_solve_method sends SolveMethod::kAuto to "
                  "CG only when the FREE DOF count exceeds SolveOptions::cg_threshold = "
                  "%lld; the free set is a subset of this pass's PassTrace::n_dof = %zu, so "
                  "nfree ≤ %zu %s %lld and the system was %s",
                  static_cast<long long>(defaults.cg_threshold), stage.trace.n_dof,
                  stage.trace.n_dof, below ? "<" : ">",
                  static_cast<long long>(defaults.cg_threshold),
                  below ? "factorised directly (sparse LDLT). NO conjugate-gradient "
                          "iterations exist on this case, so none are animated"
                        : "not established by this bound alone, and no method is claimed"));
}

void solve_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                  std::vector<CinemaLine>& out) {
    if (state.solve_stages.empty()) {
        push_line(out, palette.status_err,
                  "no solve passes were collected: pipeline::SolveJob::on_solve_stage "
                  "received none, so this act holds the finished fill instead of a field");
        push_line(out, palette.text_dim,
                  "the solve-stage sink is installed only while `cinema on`, and it fires "
                  "from the adaptive loop — run `cinema on` before `solve`. Nothing is "
                  "drawn in place of a field that was never delivered here");
        return;
    }
    const auto i = static_cast<std::size_t>(std::max(cue.solve_stage_index, 0));
    const auto& stage = state.solve_stages[std::min(i, state.solve_stages.size() - 1)];
    const auto& r = stage.result;
    const auto& tr = stage.trace;
    push_line(out, palette.accent,
              fmt("solve pass %d of %zu completed passes · beat '%s' %.2f/%.2f s · %zu "
                  "elements / %zu nodes / %zu DOF (PassTrace) · %s",
                  stage.pass, state.solve_stages.size(),
                  cinema_solve_phase_name(cue.solve_phase), cue.solve_phase_t,
                  cue.solve_phase_span, tr.n_elems, tr.n_nodes, tr.n_dof,
                  stage.final_pass ? "no further pass ran after this one (SolveStage::"
                                     "final_pass)"
                                   : "another pass followed"));
    switch (cue.solve_phase) {
    case SolvePhase::kField:
        push_line(out, palette.status_ok,
                  fmt("THIS PASS'S OWN von Mises field — max %.4g MPa · max |u| %.4g mm · "
                      "nodal values are SolveStage::result.von_mises, drawn on the geometry "
                      "this pass solved (0× exaggeration: the real shape)",
                      r.max_von_mises / 1e6, r.max_displacement * 1e3));
        solver_lines(stage, out);
        break;
    case SolvePhase::kError:
        push_line(out, palette.status_ok,
                  fmt("the ZZ error field recovered FROM that same solve — global η %.4g · "
                      "η p50 %.4g · p90 %.4g · max %.4g · nodal values are "
                      "SolveStage::result.nodal_eta, colour scale is max_nodal_eta %.4g",
                      tr.global_eta, tr.eta_p50, tr.eta_p90, tr.eta_max, r.max_nodal_eta));
        push_line(out, palette.text_dim,
                  fmt("this is what decided the next refinement: %zu elements marked for h, "
                      "%zu for p, %zu on shape, predicted DOF factor %.4g. η target %.4g",
                      tr.n_h_mark, tr.n_p_mark, tr.n_shape_mark, tr.predicted_dof_factor,
                      hud.eta_target));
        break;
    case SolvePhase::kRefine: {
        const std::size_t next = i + 1;
        if (next < state.solve_stages.size()) {
            const auto& nx = state.solve_stages[next];
            push_line(out, palette.status_ok,
                      fmt("the refined mesh pass %d actually solved, appearing element by "
                          "element — %zu elements / %zu nodes / %zu DOF, from %zu / %zu / %zu "
                          "at pass %d (PassTrace::n_elems, n_nodes, n_dof)",
                          nx.pass, nx.trace.n_elems, nx.trace.n_nodes, nx.trace.n_dof,
                          tr.n_elems, tr.n_nodes, tr.n_dof, stage.pass));
            const auto drawn = static_cast<std::size_t>(
                cue.refine_reveal * static_cast<double>(hud.cinema_elements));
            push_line(out, palette.text_dim,
                      fmt("reveal %.0f%% — %zu of %zu drawable elements of "
                          "SolveStage::result.volume_mesh, in that mesh's own storage order. "
                          "%zu elements were marked for h refinement by the field above",
                          100.0 * cue.refine_reveal, drawn, hud.cinema_elements, tr.n_h_mark));
            if (nx.trace.n_elems == tr.n_elems) {
                push_line(out, palette.status_warn,
                          fmt("the delivered element count is UNCHANGED at %zu: the marks "
                              "above drove a remesh whose ship gate returned the same "
                              "count, so this beat is showing a re-fill and not a finer "
                              "mesh",
                              nx.trace.n_elems));
            }
        }
        break;
    }
    case SolvePhase::kLoadRamp:
    case SolvePhase::kHold:
        push_line(out, palette.status_ok,
                  fmt("load factor λ = %.3f — u(λ) = λ·u EXACTLY, because the solve is "
                      "linear elastostatics: λ·f produces λ·u and λ·σ, so this frame is the "
                      "real solution of a %.4g N load case and not an interpolation between "
                      "two pictures",
                      cue.load_factor, cue.load_factor * hud.load_newtons));
        push_line(out, palette.text_dim,
                  fmt("|u| %.4g mm and von Mises %.4g MPa at this λ, from the full-load "
                      "%.4g mm / %.4g MPa in SolveStage::result. Shape drawn at %.4g× the "
                      "λ-scaled displacement (λ × App::deform_scale %.4g — true scale is "
                      "invisible at this peak); colour scale is the FULL-load maximum, so "
                      "the field darkens with λ instead of renormalising",
                      cue.load_factor * r.max_displacement * 1e3,
                      cue.load_factor * r.max_von_mises / 1e6, r.max_displacement * 1e3,
                      r.max_von_mises / 1e6, cue.load_factor * hud.deform_scale,
                      hud.deform_scale));
        solver_lines(stage, out);
        break;
    case SolvePhase::kNone:
        break;
    }
}

/// Flows the header chips across rows `wrap_width` wide with `gap` between
/// them, drawing when `draw` is set. Returns the row count. One function for
/// both passes, so the height the strip reserves is the height it uses.
int flow_chips(const std::vector<CinemaLine>& chips, float wrap_width, float gap, bool draw) {
    int rows = 1;
    float x = 0.0f;
    for (std::size_t i = 0; i < chips.size(); ++i) {
        const float w = ImGui::CalcTextSize(chips[i].text.c_str()).x;
        const float advance = (i == 0 || x <= 0.0f) ? w : gap + w;
        if (i > 0 && x > 0.0f && x + advance > wrap_width) {
            ++rows;
            x = w;
            if (draw) {
                ImGui::TextColored(chips[i].color, "%s", chips[i].text.c_str());
            }
            continue;
        }
        if (draw) {
            if (i > 0 && x > 0.0f) {
                ImGui::SameLine(0.0f, gap);
            }
            ImGui::TextColored(chips[i].color, "%s", chips[i].text.c_str());
        }
        x += advance;
    }
    return rows;
}

constexpr float kTickerChipGap = 14.0f;

} // namespace

std::vector<CinemaLine> cinema_ticker_chips(const CinemaState& state, const CinemaCue& cue) {
    std::vector<CinemaLine> chips;
    push_line(chips, palette.accent,
              fmt("act %d/4 %s", static_cast<int>(cue.act) + 1, cinema_act_name(cue.act)));
    push_line(chips, palette.text_dim,
              fmt("t %.3f / %.3f s (virtual clock)", state.t, state.duration));
    // Both beats, whenever both lanes are live, because "beat" is no longer one
    // number: the pass lane and the stage lane run at different rates on the
    // same clock and the concurrency claim is only auditable if both are stated.
    if (cue.frame_index >= 0 && cue.pass_beat_seconds > 0.0) {
        push_line(chips, palette.text_dim,
                  fmt("pass beat %.3f s = %.1f frames at 60 fps", cue.pass_beat_seconds,
                      cue.pass_beat_seconds * 60.0));
    }
    // The stage beat only means something while the stage lane is running, so it
    // is not restated in the closing act where the finished fill is merely held.
    if (cue.act == CinemaAct::kBuild && cue.stage_index >= 0 && cue.stage_beat_seconds > 0.0) {
        push_line(chips, palette.text_dim,
                  fmt("stage beat %.3f s = %.1f frames", cue.stage_beat_seconds,
                      cue.stage_beat_seconds * 60.0));
    }
    if (cue.concurrent) {
        push_line(chips, palette.status_ok, "CONCURRENT: both lanes advancing");
    }
    if (cue.act == CinemaAct::kSolve && cue.solve_phase != SolvePhase::kNone) {
        push_line(chips, palette.accent,
                  fmt("solve beat %s %.2f s", cinema_solve_phase_name(cue.solve_phase),
                      cue.solve_phase_span));
    }
    if (state.recording()) {
        push_line(chips, palette.status_warn,
                  fmt("recording frame %d/%d", state.record_next + 1, state.record_frames));
    }
    return chips;
}

std::vector<CinemaLine> cinema_ticker_body(const CinemaState& state, const CinemaCue& cue,
                                           const CinemaHud& hud) {
    std::vector<CinemaLine> body;
    switch (cue.act) {
    case CinemaAct::kSkeleton:
        skeleton_ticker(state, body);
        break;
    case CinemaAct::kDeliberate:
        advisor_ticker(state, cue, body);
        break;
    case CinemaAct::kBuild:
        // Both lanes, in the order the causality runs: what the network is
        // scoring and what it decided, then the fill that decision produced.
        advisor_ticker(state, cue, body);
        fill_ticker(state, cue, hud, body);
        break;
    case CinemaAct::kSolve:
        solve_ticker(state, cue, hud, body);
        break;
    }
    // The HUD reports the setup that is actually meshing, sourced from
    // SimSetup, never from the decision struct: the two agree only when the
    // decision was applied, and when they disagree the setup is the truth.
    push_line(body, palette.text,
              fmt("part %s · mesher %s · %s · order %d · adapt %d · η target %.4g · "
                  "elements %zu · nodes %zu · DOF %zu",
                  hud.part.empty() ? "(none loaded)" : hud.part.c_str(), hud.mesher.c_str(),
                  h_text(hud).c_str(), hud.order, hud.adapt_passes, hud.eta_target,
                  hud.elements, hud.nodes, hud.dof));
    if (hud.stamp.empty()) {
        push_line(
            body, palette.text_dim,
            "POLYMESH_CINEMA_STAMP not set — no provenance line was supplied to this run");
    } else {
        push_line(body, palette.text_dim, hud.stamp);
    }
    return body;
}

float cinema_ticker_height(const CinemaState& state, const CinemaCue& cue,
                           const CinemaHud& hud, float wrap_width) {
    const float wrap = std::max(80.0f, wrap_width);
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const auto chips = cinema_ticker_chips(state, cue);
    float h = static_cast<float>(flow_chips(chips, wrap, kTickerChipGap, false)) *
              ImGui::GetTextLineHeightWithSpacing();
    for (const auto& line : cinema_ticker_body(state, cue, hud)) {
        h += ImGui::CalcTextSize(line.text.c_str(), nullptr, false, wrap).y + spacing;
    }
    // Plus the strip's own vertical window padding, both edges.
    return std::floor(h + 2.0f * kTickerPadY);
}

float cinema_ticker_reserve(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                            float wrap_width) {
    float want = 0.0f;
    // Every act, and inside the concurrent act every combination that changes
    // the row count: the strip has to be reserved at the tallest thing the take
    // can ever put in it, or a later act clips its own disclosures.
    const auto probe_height = [&](const CinemaCue& probe) {
        want = std::max(want, cinema_ticker_height(state, probe, hud, wrap_width));
    };
    for (int a = 0; a < 4; ++a) {
        CinemaCue probe = cue;
        probe.act = static_cast<CinemaAct>(a);
        probe.concurrent = probe.act == CinemaAct::kBuild;
        probe.decision_locked =
            probe.act == CinemaAct::kBuild || probe.act == CinemaAct::kSolve;
        // The concurrent act is at its tallest once a stage is selected, not
        // during the decision lead-in where the fill rows are one sentence.
        if (probe.act == CinemaAct::kBuild && !state.stages.empty()) {
            probe.stage_index = 0;
        }
#ifdef POLYMESH_WITH_ADVISOR
        if (state.explanation && !state.explanation->frames.empty()) {
            // Tallest advisor beat: on the last pass the DECISION row and the
            // applied/refused note join the three per-pass rows. In the
            // concurrent act the mid-lane case is taller still, because the
            // replay disclosure only exists while passes remain — so both are
            // probed rather than assumed.
            const auto n = static_cast<int>(state.explanation->frames.size());
            probe.frame_index = n - 1;
            probe_height(probe);
            if (n > 1) {
                probe.frame_index = n - 2;
            }
        }
#endif
        if (probe.act != CinemaAct::kSolve) {
            probe_height(probe);
            continue;
        }
        // The closing act's beats carry different rows — the refine beat names
        // two meshes, the ramp beat names λ and the solver — so every phase is
        // measured instead of the act being probed in whichever one it is in.
        for (const SolvePhase phase :
             {SolvePhase::kField, SolvePhase::kError, SolvePhase::kRefine,
              SolvePhase::kLoadRamp, SolvePhase::kHold}) {
            probe.solve_phase = phase;
            probe.solve_stage_index = state.solve_stages.empty() ? -1 : 0;
            probe_height(probe);
        }
    }
    // High-water mark, never lowered inside a take: a strip that shrank would
    // grow the viewport pane and resize the part, which is the same cut as a
    // strip that grew.
    state.ticker_reserve = std::max(state.ticker_reserve, want);
    return state.ticker_reserve;
}

void draw_cinema_ticker(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud) {
    const float wrap = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    flow_chips(cinema_ticker_chips(state, cue), wrap, kTickerChipGap, true);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap);
    for (const auto& line : cinema_ticker_body(state, cue, hud)) {
        ImGui::TextColored(line.color, "%s", line.text.c_str());
    }
    ImGui::PopTextWrapPos();
}

} // namespace polymesh::gui
