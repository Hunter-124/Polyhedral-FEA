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
#include <Eigen/Geometry>

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

/// Act spans as fractions of the 60 s public take.
///
/// The opening starts directly on the analysed CAD body. The cell microscope
/// receives a full ten seconds: enough to build a connected linear patch,
/// elevate the same topology to quadratic order, and hold both states for
/// comparison. The solve still has 21 seconds, and its recorded phases scale
/// proportionally to fit that window.
///
///   exact CAD / edge analysis    10.8 s
///   advisor                      7.8 s
///   emitted mesh                10.2 s
///   tet4 → tet10 microscope     10.2 s
///   solve / recovery            21.0 s
constexpr std::array<double, kCinemaActCount> kActFraction = {0.18, 0.13, 0.17, 0.17, 0.35};
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
constexpr double kRevealShrink = 0.14;
constexpr double kRevealShrinkFraction = 0.24;
constexpr float kMeshEdgeAlpha = 0.34f;
constexpr float kMeshEdgeWidth = 1.0f;

/// Width of the "just arrived" cell highlight trailing the reveal front, as a
/// fraction of the mesh's element count. At the showcase's 40k cells this is
/// roughly 1.8k cells: enough that the front reads as cells being drawn one
/// after another, small enough that the settled mesh keeps its own element
/// colour instead of the part turning into a highlight.
constexpr float kArrivalBand = 0.045f;

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

/// Connections drawn per frame. The deployed graph has 15,936; drawing all of
/// them fills the lane stack with a grey haze, so only the strongest subset is
/// drawn and the count is disclosed from this same constant.
///
/// 180 rather than the 280 this used to draw: the panel is now set at nearly
/// twice the type size, so the same haze costs twice the readable area, and the
/// weakest hundred of the old set were hairlines carrying |w·a| under a tenth of
/// the strongest.
constexpr std::size_t kDrawnConnections = 180;

/// Widest an activation node is allowed to get.
constexpr float kNodeRadiusMax = 8.0f;

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
constexpr std::array<HeadName, 20> kHeadNames{{
    {"rel_err", "predicted error"},
    {"rel_err_rel", "error vs this part's median"},
    {"geo_chamfer", "mesh-to-CAD distance"},
    {"geo_p99", "mesh-to-CAD worst 1%"},
    {"dof", "unknowns"},
    {"mesh_ms", "meshing time"},
    {"solve_ms", "solve time"},
    {"solve_flops", "portable solve work"},
    {"solve_bytes", "portable data traffic"},
    {"mesh_work", "host-normalized meshing work"},
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

std::string_view mesh_stage_plain(std::string_view stage) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kNames{{
        {"lattice", "seed lattice"},
        {"expand", "cell expansion"},
        {"snap", "CAD projection"},
        {"peel", "quality repair"},
        {"reproject", "boundary recovery"},
        {"smooth", "surface smoothing"},
        {"resnap", "CAD resnap"},
        {"pin", "feature pinning"},
        {"fill", "solver cells"},
        {"ship", "validated mesh"},
    }};
    for (const auto& [id, plain] : kNames) {
        if (stage == id) {
            return plain;
        }
    }
    return stage;
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

/// Area-weighted centroid of the exact tessellated region selected by the GUI.
/// Unlike the region AABB centre, this point lies on the selected surface in
/// the average-of-area sense and remains stable as tessellation density changes.
std::optional<Eigen::Vector3d> region_centroid(const pipeline::Model& model, int region) {
    Eigen::Vector3d weighted = Eigen::Vector3d::Zero();
    double area_sum = 0.0;
    for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        const auto& tri = model.surface.triangles[ti];
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
        const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
        const double area = 0.5 * (b - a).cross(c - a).norm();
        if (!(area > 0.0)) {
            continue;
        }
        weighted += area * (a + b + c) / 3.0;
        area_sum += area;
    }
    if (!(area_sum > 0.0)) {
        return std::nullopt;
    }
    return weighted / area_sum;
}
std::vector<Eigen::Vector3d> region_surface_points(const pipeline::Model& model, int region) {
    std::vector<Eigen::Vector3d> points;
    std::vector<std::uint8_t> used(model.surface.vertices.size(), 0);
    for (std::size_t ti = 0; ti < model.surface.triangles.size(); ++ti) {
        if (ti >= model.triangle_region.size() || model.triangle_region[ti] != region) {
            continue;
        }
        for (const std::uint32_t node : model.surface.triangles[ti]) {
            if (node < model.surface.vertices.size() && used[node] == 0) {
                used[node] = 1;
                points.push_back(model.surface.vertices[node]);
            }
        }
    }
    return points;
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
    type.note = std::floor(21.0f * s);
    type.footer = std::floor(18.0f * s);
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
    std::sort(finite.begin(), finite.end());
    const std::size_t p99_index = static_cast<std::size_t>(
        std::floor(0.99 * static_cast<double>(finite.size() - 1)));
    out.p99 = finite[p99_index];
    for (std::size_t i = 0; i < out.quantiles.size(); ++i) {
        const double q = 0.99 * static_cast<double>(i) /
                         static_cast<double>(out.quantiles.size() - 1);
        const double at = q * static_cast<double>(finite.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(at));
        const std::size_t hi = std::min(lo + 1, finite.size() - 1);
        const double part = at - static_cast<double>(lo);
        out.quantiles[i] = finite[lo] + part * (finite[hi] - finite[lo]);
    }
    // A single constrained-node singularity must not flatten 99% of the
    // teaching graph or viewport into one dark bin. The true max stays in
    // `max` and on screen; only the colour/chart scale caps at measured p99.
    const double display_span = out.p99 - out.min;
    for (const double value : finite) {
        std::size_t bin = 0;
        if (display_span > 0.0) {
            const double u = std::clamp((value - out.min) / display_span, 0.0, 1.0);
            bin = std::min(
                static_cast<std::size_t>(u * static_cast<double>(out.bins.size())),
                out.bins.size() - 1);
        }
        ++out.bins[bin];
        out.tallest_bin = std::max(out.tallest_bin, out.bins[bin]);
    }
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
    if (stress_histograms.size() < solve_stages.size()) {
        stress_histograms.resize(solve_stages.size());
        error_histograms.resize(solve_stages.size());
    }
    stress_histograms.back() = inspect_histogram(result.von_mises);
    error_histograms.back() = inspect_histogram(result.nodal_eta);
    if (result.reactions_complete &&
        result.reactions.size() ==
            3 * static_cast<Eigen::Index>(result.volume_mesh.nodes.size())) {
        for (auto& marker : support_markers) {
            marker.reaction.setZero();
            const auto members = result.boundary_region_nodes.find(marker.region);
            if (members == result.boundary_region_nodes.end()) {
                continue;
            }
            for (const std::uint32_t node : members->second) {
                if (node < result.volume_mesh.nodes.size()) {
                    marker.reaction +=
                        result.reactions.segment<3>(3 * static_cast<Eigen::Index>(node));
                }
            }
        }
    } else {
        for (auto& marker : support_markers) {
            marker.reaction.setZero();
        }
    }
    Eigen::Vector3d support_resultant = Eigen::Vector3d::Zero();
    for (const auto& marker : support_markers) {
        support_resultant += marker.reaction;
        std::printf("cinema: reaction region %d fx %.9g fy %.9g fz %.9g "
                    "magnitude %.9g\n",
                    marker.region, marker.reaction.x(), marker.reaction.y(),
                    marker.reaction.z(), marker.reaction.norm());
    }
    Eigen::Vector3d load_resultant = Eigen::Vector3d::Zero();
    for (const auto& marker : load_markers) {
        load_resultant += marker.vector;
    }
    const double reaction_balance =
        (support_resultant + load_resultant).norm() / std::max(load_resultant.norm(), 1.0);
    std::printf("cinema: reactions complete %d balance_rel %.9g "
                "support_fx %.9g support_fy %.9g support_fz %.9g\n",
                result.reactions_complete ? 1 : 0, reaction_balance, support_resultant.x(),
                support_resultant.y(), support_resultant.z());
    std::fflush(stdout);
    const auto resolve_nodes = [&](std::vector<CinemaMechanicsMarker>& markers,
                                   bool reaction_anchor) {
        for (auto& marker : markers) {
            Eigen::Vector3d anchor = marker.position;
            const Eigen::Vector3d direction =
                reaction_anchor ? marker.reaction : -marker.vector;
            if (direction.norm() > 0.0 && !marker.surface_points.empty()) {
                const Eigen::Vector3d unit = direction.normalized();
                anchor = *std::max_element(
                    marker.surface_points.begin(), marker.surface_points.end(),
                    [&](const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
                        return unit.dot(a - marker.position) < unit.dot(b - marker.position);
                    });
            }
            marker.position = anchor;
            marker.result_node = std::numeric_limits<std::size_t>::max();
            double best = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < result.volume_mesh.nodes.size(); ++i) {
                const double d2 = (result.volume_mesh.nodes[i] - anchor).squaredNorm();
                if (d2 < best) {
                    best = d2;
                    marker.result_node = i;
                }
            }
        }
    };
    resolve_nodes(support_markers, true);
    resolve_nodes(load_markers, false);
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

    // Start directly on the analysed CAD body, then extract its ordered edge
    // samples without inventing neighbouring hardware.
    if (cue.act == CinemaAct::kSkeleton) {
        const double wait = 0.06 * cue.act_span;
        const double slide = 0.08 * cue.act_span;
        cue.panel_open = static_cast<float>(
            smoothstep((cue.act_t - wait) / std::max(slide, 1.0e-9)));
        const double p = cue.act_t / std::max(cue.act_span, 1.0e-9);
        cue.spectral_edge_reveal = smoothstep((p - 0.12) / 0.28);
        cue.spectral_spectrum_reveal = smoothstep((p - 0.40) / 0.18);
        cue.spectral_filter_mix = smoothstep((p - 0.62) / 0.22);
        cue.spectral_curve_cursor =
            p < 0.62 ? cue.spectral_edge_reveal : 1.0 - cue.spectral_filter_mix;
        cue.spectral_curve_cursor_alpha = 1.0 - smoothstep((p - 0.84) / 0.12);
        cue.spectral_field_reveal =
            state.sizing.field_points.empty() ? 0.0 : smoothstep((p - 0.84) / 0.12);
        cue.spectral_overlay_alpha =
            static_cast<float>(smoothstep((p - 0.10) / 0.08));
    } else if (cue.act == CinemaAct::kDeliberate) {
        cue.spectral_edge_reveal = 1.0;
        cue.spectral_spectrum_reveal = 1.0;
        cue.spectral_filter_mix = 1.0;
        cue.spectral_curve_cursor = 0.0;
        cue.spectral_curve_cursor_alpha = 0.0;
        cue.spectral_field_reveal = state.sizing.field_points.empty() ? 0.0 : 1.0;
        const double bridge = std::min(1.3, 0.18 * cue.act_span);
        const double q = smoothstep(cue.act_t / std::max(bridge, 1.0e-9));
        cue.spectral_overlay_alpha =
            static_cast<float>(1.0 + q * (kSizingCarryAlpha - 1.0f));
    } else if (cue.act == CinemaAct::kBuild) {
        cue.spectral_edge_reveal = 1.0;
        cue.spectral_spectrum_reveal = 1.0;
        cue.spectral_filter_mix = 1.0;
        cue.spectral_curve_cursor = 0.0;
        cue.spectral_curve_cursor_alpha = 0.0;
        cue.spectral_field_reveal = state.sizing.field_points.empty() ? 0.0 : 1.0;
        const double handoff = std::max(cinema_decision_lead(state), 0.18 * cue.act_span);
        cue.spectral_overlay_alpha = static_cast<float>(
            kSizingCarryAlpha *
            (1.0 - smoothstep(cue.act_t / std::max(handoff, 1.0e-9))));
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
        const double scan_t1 = lane_t1;
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
            cue.action_bridge_alpha = static_cast<float>(
                smoothstep(cue.act_t / std::max(0.45 * lead, 1.0e-9)));
            if (x < 0.0) {
                cue.activation_wave = smoothstep(cue.act_t / std::max(lead, 1.0e-9));
            } else {
                const auto i = static_cast<std::size_t>(x / beat);
                const std::size_t clamped = std::min(i, n_fill - 1);
                cue.stage_index = static_cast<int>(clamped);
                const double within = x - static_cast<double>(clamped) * beat;
                cue.stage_reveal = std::clamp(within / beat, 0.0, 1.0);
                cue.activation_wave = cue.stage_reveal;
                // Cell counts can rise or fall across repair stages, so progress
                // follows completed mesher boundaries. A final same-topology
                // ship audit holds at 100% instead of consuming half the visual
                // build on the two-stage tet path.
                const bool final_audit =
                    n_fill > 1 &&
                    state.stages[n_fill - 2].mesh.nodes.size() ==
                        state.stages[n_fill - 1].mesh.nodes.size() &&
                    state.stages[n_fill - 2].mesh.elements.size() ==
                        state.stages[n_fill - 1].mesh.elements.size();
                const std::size_t active_stages = n_fill - (final_audit ? 1u : 0u);
                cue.mesh_action_reveal =
                    clamped >= active_stages
                        ? 1.0
                        : (static_cast<double>(clamped) + smoothstep(cue.stage_reveal)) /
                              static_cast<double>(active_stages);
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
    case CinemaAct::kSkeleton: {
        view.model_alpha = 1.0f;
        view.skeleton_alpha = 0.26f * cue.spectral_overlay_alpha;
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_curve_cursor = static_cast<float>(cue.spectral_curve_cursor);
        view.spectral_curve_cursor_alpha =
            static_cast<float>(cue.spectral_curve_cursor_alpha);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    }
    case CinemaAct::kDeliberate:
        // The shaded CAD body remains the reference while the advisor consumes
        // its measured features. No whole-part edge cage is composited over it.
        view.model_alpha = 1.0f;
        view.skeleton_alpha = 0.0f;
        view.reveal = 0.0f;
        view.mesh_alpha = 0.0f;
        view.shrink = 1.0f;
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_curve_cursor = static_cast<float>(cue.spectral_curve_cursor);
        view.spectral_curve_cursor_alpha =
            static_cast<float>(cue.spectral_curve_cursor_alpha);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    case CinemaAct::kBuild: {
        // Recorded construction prefixes replace the CAD body while the chosen
        // advisor output remains live in the pane. Later prefixes transition
        // from the exact preceding snapshot instead of restarting from empty.
        // The CAD body clears out over the first third of the fill. It was
        // previously faded in lockstep with the whole reveal, which left a
        // half-opaque grey surface depth-occluding the cells for most of the
        // act: the arriving cells were measured at a few thousand visible
        // pixels until the very end. The handoff still reads CAD → cells, but
        // the cells own the shot while they are being drawn.
        view.model_alpha =
            static_cast<float>(1.0 - smoothstep(cue.mesh_action_reveal / 0.32));
        view.skeleton_alpha = 0.0f;
        view.mesh_alpha = 1.0f;
        if (cue.stage_index > 0 &&
            static_cast<std::size_t>(cue.stage_index) < state.stages.size()) {
            const auto i = static_cast<std::size_t>(cue.stage_index);
            const bool same_topology =
                state.stages[i - 1].mesh.nodes.size() == state.stages[i].mesh.nodes.size() &&
                state.stages[i - 1].mesh.elements.size() ==
                    state.stages[i].mesh.elements.size();
            view.incremental_transition = !same_topology;
            view.transition_progress = static_cast<float>(smoothstep(cue.stage_reveal));
            view.reveal = same_topology ? 1.0f : view.transition_progress;
            view.shrink = same_topology ? 0.0f : shrink_for(cue.stage_reveal);
        } else {
            view.reveal =
                cue.stage_index >= 0 ? static_cast<float>(smoothstep(cue.stage_reveal)) : 0.0f;
            view.shrink = cue.stage_index >= 0 ? shrink_for(cue.stage_reveal) : 1.0f;
        }
        view.spectral_edge_reveal = static_cast<float>(cue.spectral_edge_reveal);
        view.spectral_curve_cursor = static_cast<float>(cue.spectral_curve_cursor);
        view.spectral_curve_cursor_alpha =
            static_cast<float>(cue.spectral_curve_cursor_alpha);
        view.spectral_field_reveal = static_cast<float>(cue.spectral_field_reveal);
        view.spectral_filter_mix = static_cast<float>(cue.spectral_filter_mix);
        view.spectral_overlay_alpha = cue.spectral_overlay_alpha;
        break;
    }
    case CinemaAct::kMeshHold: {
        // Open the finished cells enough to expose topology, hold the
        // microscope view, then close back to the delivered mesh. CAD feature
        // outlines stay off: only real element edges belong in this act.
        const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
        double exploded = 0.0;
        if (x < 0.16) {
            exploded = smoothstep(x / 0.16);
        } else if (x < 0.72) {
            exploded = 1.0;
        } else if (x < 0.90) {
            exploded = 1.0 - smoothstep((x - 0.72) / 0.18);
        }
        view.skeleton_alpha = 0.0f;
        view.reveal = 1.0f;
        view.mesh_alpha = 1.0f;
        view.shrink = static_cast<float>(0.10 * exploded);
        view.edge_alpha = kMeshEdgeAlpha + static_cast<float>(0.24 * exploded);
        break;
    }
    case CinemaAct::kSolve:
        if (cue.solve_phase == SolvePhase::kLoadRamp ||
            cue.solve_phase == SolvePhase::kHold) {
            view.rest_surface_alpha = 0.18f;
        }
        if (cue.solve_phase == SolvePhase::kRefine) {
            // Start on the exact ZZ field that requested refinement, then lay the
            // exact topology diff over it. Once the field has dissolved, the
            // existing structural transition continues without a reset.
            const double p = std::clamp(cue.refine_reveal, 0.0, 1.0);
            const float handoff =
                static_cast<float>(smoothstep(p / kFieldToMeshHandoff));
            const double added = smoothstep((p - 0.40) / 0.60);
            view.skeleton_alpha = 0.0f;
            view.reveal = static_cast<float>(added);
            view.mesh_alpha = handoff;
            view.shrink = 0.0f;
            view.incremental_transition = true;
            view.transition_progress = static_cast<float>(p);
            view.overlay_on_results = true;
            view.result_alpha = 1.0f - handoff;
        } else if (cue.solve_phase == SolvePhase::kRefineHold) {
            view.skeleton_alpha = 0.0f;
            view.reveal = 1.0f;
            view.mesh_alpha = 1.0f;
            view.shrink = 0.0f;
            view.incremental_transition = true;
            view.transition_progress = 1.0f;
        } else if (cue.solve_phase == SolvePhase::kStressSweep) {
            // The finished mesh carries into analysis and fades only as stress
            // colours arrive, avoiding a mesh→grey reset at the act/pass edge.
            const float mesh_carry = static_cast<float>(
                1.0 - smoothstep(cue.field_front / kMeshToFieldHandoff));
            view.skeleton_alpha = 0.0f;
            view.reveal = 1.0f;
            view.mesh_alpha = mesh_carry;
            view.shrink = 0.0f;
            view.overlay_on_results = mesh_carry > 0.0f;
        } else {
            // Result modes draw their own surface. kNone is the no-solve fallback
            // and therefore keeps the authoritative mesh instead.
            view.skeleton_alpha = 0.0f;
            view.reveal = 1.0f;
            view.mesh_alpha = cue.solve_phase == SolvePhase::kNone ? 1.0f : 0.0f;
            view.shrink = 0.0f;
        }
        break;
    }
    // Cells are lit as they arrive, and only while cells are actually arriving:
    // the band tapers to nothing as the reveal completes, so a finished mesh is
    // its own element colour rather than a mesh with a hot tail frozen into it.
    if (cue.act == CinemaAct::kBuild ||
        (cue.act == CinemaAct::kSolve && cue.solve_phase == SolvePhase::kRefine)) {
        const double front = std::clamp(static_cast<double>(view.reveal), 0.0, 1.0);
        view.arrival_band =
            kArrivalBand * static_cast<float>(1.0 - smoothstep((front - 0.88) / 0.12));
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
        index < state.stress_histograms.size() &&
                state.stress_histograms[index].p99 > 0.0
            ? state.stress_histograms[index].p99
            : result.max_von_mises;
    const double error_display_max =
        index < state.error_histograms.size() &&
                state.error_histograms[index].p99 > 0.0
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
        arm_sweep(cue.solve_phase == SolvePhase::kGradientSweep,
                  DisplayMode::kResultsVonMises,
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
        arm_sweep(cue.solve_phase == SolvePhase::kError,
                  carry_gradient ? DisplayMode::kResultsGradient
                                 : DisplayMode::kResultsVonMises,
                  static_cast<float>(carry_gradient ? gradient_display_max()
                                                    : stress_display_max));
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
            static_cast<float>(stress_display_max /
                               std::max(lambda, kMinLoadFactor));
        arm_sweep(cue.solve_phase == SolvePhase::kLoadRamp,
                  DisplayMode::kResultsError,
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
        // input supplied no feature story. Uniform takes still upload the
        // complete independently filtered CAD-edge network, but no fabricated
        // spatial h(x) field.
        viewport.set_cinema_feature_samples(
            state.sizing.field_points, state.sizing.field_h_before,
            state.sizing.field_h_after, state.sizing.curve_points,
            state.sizing.curvature_raw, state.sizing.curvature_filtered,
            state.sizing.network_points, state.sizing.network_curvature_raw,
            state.sizing.network_curvature_filtered);
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
            if (i > 0) {
                const auto& previous = state.stages[i - 1].mesh;
                const auto& current = state.stages[i].mesh;
                if (previous.nodes.size() != current.nodes.size() ||
                    previous.elements.size() != current.elements.size()) {
                    viewport.set_cinema_mesh_transition(previous, current);
                } else {
                    viewport.set_cinema_mesh(current);
                }
            } else {
                viewport.set_cinema_mesh(state.stages[i].mesh);
            }
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
    state.sizing.curve_points.clear();
    state.sizing.network_points.clear();
    state.sizing.network_curvature_raw.clear();
    state.sizing.network_curvature_filtered.clear();
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
        state.sizing.network_points.insert(state.sizing.network_points.end(),
                                           edge.samples.begin(), edge.samples.end());
        state.sizing.network_curvature_raw.insert(
            state.sizing.network_curvature_raw.end(), edge.kappa_samples.begin(),
            edge.kappa_samples.end());
        state.sizing.network_curvature_filtered.insert(
            state.sizing.network_curvature_filtered.end(), filtered.begin(), filtered.end());
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
        state.sizing.curve_points = edge.samples;
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
    state.subject_center = 0.5 * (model.bbox_min + model.bbox_max);
    state.model_diagonal = (model.bbox_max - model.bbox_min).norm();
    state.support_markers.clear();
    state.load_markers.clear();
    for (const int region : setup.fixtures) {
        if (const auto position = region_centroid(model, region)) {
            CinemaMechanicsMarker marker;
            marker.region = region;
            marker.position = *position;
            marker.surface_points = region_surface_points(model, region);
            state.support_markers.push_back(std::move(marker));
        }
    }
    for (const auto& [region, load] : setup.loads) {
        if (const auto position = region_centroid(model, region)) {
            CinemaMechanicsMarker marker;
            marker.region = region;
            marker.position = *position;
            marker.vector = load.force;
            marker.surface_points = region_surface_points(model, region);
            state.load_markers.push_back(std::move(marker));
        }
    }

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
    if (setup.bc_grading) {
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
    }

    state.uploaded_sizing_story = false;
    state.sizing.field_points.clear();
    state.sizing.field_h_before.clear();
    state.sizing.field_h_after.clear();
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
    state.advisor_ran = false;
    state.decision_vetoed = false;
    state.decision_unrecognized = false;
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
    return unavailable("advisor inference was not compiled into this build");
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
    state.advisor_ran = true;

    // Applying the decision is what makes the causal claim true: the mesher
    // must execute the action the network chose, or the video would be showing
    // two unrelated things side by side. A refusal is NOT applied -- it is a
    // real outcome and is shown as one.
    if (decision.vetoed) {
        state.decision_vetoed = true;
        const std::string why =
            decision.note.empty()
                ? (decision.budget_refusal ? "no candidate fit the degrees-of-freedom budget"
                                           : "the feasibility gate declined the prediction")
                : decision.note;
        state.decision_note = std::format(
            "advisor abstained: {}; the configured baseline below stays unchanged", why);
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
        state.decision_unrecognized = true;
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

    const pipeline::SolveStage* stage = nullptr;
    std::size_t stage_index = 0;
    if (cue.solve_stage_index >= 0 &&
        static_cast<std::size_t>(cue.solve_stage_index) < state.solve_stages.size()) {
        stage_index = static_cast<std::size_t>(cue.solve_stage_index);
        stage = &state.solve_stages[stage_index];
    }

    const SolvePhase phase = cue.solve_phase;
    const bool on_stress =
        phase == SolvePhase::kStressSweep || phase == SolvePhase::kStressHold;
    const bool on_gradient =
        phase == SolvePhase::kGradientSweep || phase == SolvePhase::kGradientHold;
    const bool on_error = phase == SolvePhase::kError || phase == SolvePhase::kErrorHold;
    const bool on_refine = phase == SolvePhase::kRefine || phase == SolvePhase::kRefineHold;
    const bool on_ramp = phase == SolvePhase::kLoadRamp || phase == SolvePhase::kHold;

    Runs equation{plain("K u = f", palette.accent)};
    const CinemaHistogram* histogram = nullptr;
    const char* histogram_unit = "";
    double histogram_scale = 1.0;
    if (stage != nullptr && on_stress) {
        equation = {plain("K u = f   ·   ε = B u   ·   σ = D ε   ·   σ", palette.text),
                    sub("vm", palette.text)};
        if (stage_index < state.stress_histograms.size()) {
            histogram = &state.stress_histograms[stage_index];
            histogram_unit = "MPa";
            histogram_scale = 1e-6;
        }
    } else if (stage != nullptr && on_gradient) {
        equation = {plain("|∇σ", palette.text), sub("vm", palette.text), plain("|", palette.text)};
        histogram = &const_cast<CinemaState&>(state).gradient_histogram(stage_index);
        histogram_unit = "MPa/mm";
        histogram_scale = 1e-9;
    } else if (stage != nullptr && on_error) {
        equation = {plain("η", palette.text), sub("e", palette.text),
                    plain(" = ‖σ* − σ", palette.text), sub("h", palette.text),
                    plain("‖", palette.text), sub("E,e", palette.text),
                    plain(" / ‖σ", palette.text), sub("h", palette.text),
                    plain("‖", palette.text), sub("E,Ω", palette.text)};
        if (stage_index < state.error_histograms.size()) {
            histogram = &state.error_histograms[stage_index];
            histogram_unit = "% local η";
            histogram_scale = 100.0;
        }
    } else if (stage != nullptr && on_refine) {
        equation = {plain("Σ", palette.text), sub("marked", palette.text),
                    plain(" η", palette.text), sub("e", palette.text),
                    sup("2", palette.text), plain(" ≥ θ Σ", palette.text),
                    sub("all", palette.text), plain(" η", palette.text),
                    sub("e", palette.text), sup("2", palette.text)};
    } else if (stage != nullptr && on_ramp) {
        equation = {plain("u(λ) = λu(1)   ·   σ(λ) = λσ(1)   ·   f(λ) = λf(1)",
                          palette.text)};
    }


    const float equation_y = origin.y + 10.0f;
    const float equation_w = runs_width(font, type.caption, equation);
    const float equation_size =
        equation_w > region.x && equation_w > 0.0f
            ? std::max(12.0f, type.caption * region.x / equation_w)
            : type.caption;
    draw_runs(dl, font, equation_size, ImVec2(origin.x, equation_y), equation, alpha);

    const float pipeline_h = type.legend * 3.4f;
    const float chart_top = equation_y + type.caption * 2.0f;
    const float chart_bottom = origin.y + region.y - pipeline_h;
    const float chart_h = std::max(180.0f, chart_bottom - chart_top);
    const ImVec2 chart_min(origin.x, chart_top);
    const ImVec2 chart_max(origin.x + region.x, chart_top + chart_h);
    dl->AddRectFilled(chart_min, chart_max, faded(palette.panel_bg, 0.62f * alpha), 8.0f);
    dl->AddRect(chart_min, chart_max, faded(palette.border, alpha), 8.0f);

    const float left = chart_min.x + 28.0f;
    const float right = chart_max.x - 18.0f;
    const float top = chart_min.y + 30.0f;
    const float bottom = chart_max.y - 38.0f;
    for (int i = 1; i < 4; ++i) {
        const float y = top + (bottom - top) * static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(left, y), ImVec2(right, y),
                    faded(palette.border, 0.42f * alpha), 1.0f);
    }
    dl->AddLine(ImVec2(left, bottom), ImVec2(right, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);
    dl->AddLine(ImVec2(left, top), ImVec2(left, bottom),
                faded(palette.text_dim, 0.75f * alpha), 1.2f);

    if (histogram != nullptr && histogram->samples > 0) {
        const float plot_w = right - left;
        const float plot_h = bottom - top;
        const double span = histogram->p99 - histogram->min;
        const auto point = [&](std::size_t i) {
            const float x =
                left + plot_w * static_cast<float>(i) /
                           static_cast<float>(histogram->quantiles.size() - 1);
            const double value = histogram->quantiles[i];
            const float y =
                span > 0.0
                    ? bottom - plot_h * std::clamp(
                                              static_cast<float>((value - histogram->min) /
                                                                 span),
                                              0.0f, 1.0f)
                    : bottom;
            return ImVec2(x, y);
        };
        for (std::size_t i = 1; i < histogram->quantiles.size(); ++i) {
            const double value = histogram->quantiles[i];
            const float t =
                span > 0.0
                    ? std::clamp(static_cast<float>((value - histogram->min) / span), 0.0f,
                                 1.0f)
                    : 0.0f;
            dl->AddLine(point(i - 1), point(i), rgba(fea_colormap(t), 0.94f * alpha), 3.2f);
        }
        const float mean_y =
            span > 0.0
                ? bottom - plot_h * std::clamp(
                                          static_cast<float>((histogram->mean -
                                                              histogram->min) /
                                                             span),
                                          0.0f, 1.0f)
                : bottom;
        dl->AddLine(ImVec2(left, mean_y), ImVec2(right, mean_y),
                    faded(palette.accent, 0.70f * alpha), 1.5f);
        const std::string minimum =
            fmt("min %.3g %s", histogram->min * histogram_scale, histogram_unit);
        const std::string maximum =
            fmt("p99 %.3g %s", histogram->p99 * histogram_scale, histogram_unit);
        const std::string mean =
            fmt("mean %.3g %s", histogram->mean * histogram_scale, histogram_unit);
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, bottom - type.legend * 1.35f),
                    faded(palette.text_dim, alpha), minimum.c_str());
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, top + 4.0f),
                    faded(palette.text_dim, alpha), maximum.c_str());
        dl->AddText(font, type.legend, ImVec2(left + 6.0f, mean_y - type.legend * 1.2f),
                    faded(palette.accent, alpha), mean.c_str());
        const char* percentile = "node percentile  0 → 99";
        const float label_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, percentile).x;
        dl->AddText(font, type.legend,
                    ImVec2(0.5f * (left + right - label_w), bottom + 8.0f),
                    faded(palette.text_dim, alpha), percentile);
    } else if (stage != nullptr && on_error) {
        const double measured = stage->trace.global_eta * 100.0;
        const double target = hud.eta_target * 100.0;
        const double scale = std::max({measured, target, 1.0e-12});
        const std::array<double, 2> values{{target, measured}};
        const std::array<const char*, 2> labels{{"η*", "ηZZ"}};
        const std::array<ImVec4, 2> colors{{palette.text_dim, palette.accent}};
        const float bar_w = (right - left) * 0.22f;
        for (std::size_t i = 0; i < values.size(); ++i) {
            const float x = left + (right - left) * (0.23f + 0.54f * static_cast<float>(i));
            const float h = (bottom - top) * static_cast<float>(values[i] / scale);
            dl->AddRectFilled(ImVec2(x - 0.5f * bar_w, bottom - h),
                              ImVec2(x + 0.5f * bar_w, bottom),
                              faded(colors[i], 0.82f * alpha), 5.0f);
            const std::string value = fmt("%.3g%%", values[i]);
            dl->AddText(font, type.label,
                        ImVec2(x - 0.5f * bar_w, bottom - h - type.label * 1.35f),
                        faded(colors[i], alpha), value.c_str());
            dl->AddText(font, type.legend,
                        ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
                        faded(palette.text_dim, alpha), labels[i]);
        }
    } else if (stage != nullptr && on_refine) {
        const std::size_t before = stage->trace.n_elems;
        const std::size_t after = stage_index + 1 < state.solve_stages.size()
                                      ? state.solve_stages[stage_index + 1].trace.n_elems
                                      : before;
        const std::array<std::size_t, 2> counts{{before, after}};
        const std::array<const char*, 2> labels{{"n", "n+1"}};
        const std::size_t max_count = std::max<std::size_t>({before, after, 1});
        const float bar_w = (right - left) * 0.24f;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            const float x = left + (right - left) * (0.24f + 0.52f * static_cast<float>(i));
            const float h = (bottom - top) * static_cast<float>(counts[i]) /
                            static_cast<float>(max_count);
            dl->AddRectFilled(ImVec2(x - 0.5f * bar_w, bottom - h),
                              ImVec2(x + 0.5f * bar_w, bottom),
                              faded(i == 0 ? palette.text_dim : palette.accent,
                                    0.82f * alpha),
                              5.0f);
            const std::string value = grouped(counts[i]) + " cells";
            dl->AddText(font, type.label,
                        ImVec2(x - 0.5f * bar_w, bottom - h - type.label * 1.35f),
                        faded(i == 0 ? palette.text : palette.accent, alpha),
                        value.c_str());
            dl->AddText(font, type.legend,
                        ImVec2(x - 0.5f * bar_w, bottom + 8.0f),
                        faded(palette.text_dim, alpha), labels[i]);
        }
    } else if (stage != nullptr && on_ramp) {
        dl->AddLine(ImVec2(left, bottom), ImVec2(right, top),
                    faded(palette.accent_soft_top, 0.88f * alpha), 6.0f);
        dl->AddLine(ImVec2(left, bottom), ImVec2(right, top),
                    faded(palette.status_warn, 0.92f * alpha), 2.2f);
        const float x = left + (right - left) * static_cast<float>(cue.load_factor);
        const float y = bottom - (bottom - top) * static_cast<float>(cue.load_factor);
        dl->AddCircleFilled(ImVec2(x, y), 9.0f, faded(palette.accent, alpha));
        dl->AddCircle(ImVec2(x, y), 15.0f,
                      faded(palette.accent_soft_top, 0.55f * alpha), 0, 2.0f);
    }

    int active = 0;
    if (on_stress) {
        active = 1;
    } else if (on_gradient) {
        active = 2;
    } else if (on_error) {
        active = 3;
    } else if (on_refine) {
        active = 4;
    } else if (on_ramp) {
        active = 5;
    }
    const float rail_y = chart_max.y + type.legend * 1.15f;
    const float rail_left = origin.x + 20.0f;
    const float rail_right = origin.x + region.x - 20.0f;
    dl->AddLine(ImVec2(rail_left, rail_y), ImVec2(rail_right, rail_y),
                faded(palette.text_dim, 0.45f * alpha), 2.0f);
    for (int i = 0; i < 6; ++i) {
        const float x = rail_left + (rail_right - rail_left) *
                                        static_cast<float>(i) / 5.0f;
        const bool lit = i <= active;
        dl->AddCircleFilled(ImVec2(x, rail_y), lit ? 7.0f : 4.5f,
                            faded(lit ? palette.accent : palette.text_dim, alpha));
        if (i == active) {
            dl->AddCircle(ImVec2(x, rail_y), 13.0f,
                          faded(palette.accent_soft_top, 0.65f * alpha), 0, 2.0f);
        }
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
                       "advisor support is not compiled into this build");
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

    // The network is the explanation: measured nodes, measured signed edges,
    // and a timed feed-forward pulse. Prose belongs in NOTES.md, not over the
    // graph.

    // ---- geometry, derived from the measured text ------------------------
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const float graph_top = origin.y + 4.0f;
    const float graph_h = std::max(140.0f, region.y - 4.0f);

    const auto& heads = layout.layers[3];
    int winner = -1;
    std::string winner_text;
    if (frame != nullptr) {
        const std::string want = std::string("policy_mesher_logit_") + frame->action.mesher;
        for (std::size_t i = 0; i < heads.labels.size(); ++i) {
            if (heads.labels[i] == want) {
                winner = static_cast<int>(i);
                const std::string_view name = head_name(heads.labels[i]);
                winner_text = values[3] != nullptr
                                  ? std::format("{} {:+.4g}", name, (*values[3])[i])
                                  : std::format("{} —", name);
                break;
            }
        }
    }

    // Four WIDE activation lanes. The old four tall columns forced a left-to-
    // right network into a portrait box and left half the panel to labels. Here
    // each layer spans the pane in the direction its units need room; circles
    // remain circles because positions, never node geometry, are transformed.
    constexpr float kSidePad = 14.0f;
    const float header_h = std::floor(type.legend * 1.25f);
    const float chip_h =
        frame != nullptr ? std::floor(type.label * 2.5f) : 0.0f;
    const float lanes_top = graph_top + header_h;
    const float lanes_h = std::max(
        120.0f, graph_h - header_h - chip_h - std::floor(type.legend * 0.6f));
    const float lane_h = lanes_h / static_cast<float>(values.size());
    const float band_w = std::max(120.0f, region.x - 2.0f * kSidePad);
    const auto row_y = [&](std::size_t layer) {
        return lanes_top + lane_h * (static_cast<float>(layer) + 0.58f);
    };
    const auto node_x = [&](std::size_t i, std::size_t n) {
        return origin.x + kSidePad +
               band_w * (static_cast<float>(i) + 0.5f) /
                   static_cast<float>(std::max<std::size_t>(n, 1));
    };
    const auto node_point = [&](std::size_t layer, std::size_t i) {
        return ImVec2(node_x(i, layout.layers[layer].size), row_y(layer));
    };
    const bool replaying = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kMeshHold;
    const auto wave_strength = [&](float lane) {
        float at = lane;
        if (cue.pass_lane_live) {
            const double beat = std::max(cue.pass_beat_seconds, 1.0e-6);
            at = static_cast<float>(
                std::fmod(std::max(cue.act_t, 0.0) / beat, 1.0) *
                static_cast<double>(values.size() - 1));
        } else if (replaying) {
            at = static_cast<float>(cue.activation_wave) *
                 static_cast<float>(values.size() - 1);
        } else {
            return 1.0f;
        }
        const float d = at - lane;
        return 0.42f + 0.58f * std::exp(-2.2f * d * d);
    };

    // Quiet lane bands keep the 81/96/96/20 topology legible while the measured
    // activations change. During candidate scoring their highlight advances one
    // layer per feed-forward beat; it is a timing cue only and never alters a
    // tensor value.
    for (std::size_t l = 0; l < values.size(); ++l) {
        const float top = lanes_top + lane_h * static_cast<float>(l) + type.legend * 1.15f;
        const float bottom = lanes_top + lane_h * static_cast<float>(l + 1) - 4.0f;
        const float pulse = wave_strength(static_cast<float>(l));
        dl->AddRectFilled(ImVec2(origin.x + kSidePad, top),
                          ImVec2(origin.x + region.x - kSidePad, bottom),
                          faded(l == 3 ? palette.accent : palette.panel_bg,
                                (l == 3 ? 0.028f : 0.055f) * pulse * alpha),
                          5.0f);
        dl->AddLine(ImVec2(origin.x + kSidePad, row_y(l)),
                    ImVec2(origin.x + region.x - kSidePad, row_y(l)),
                    faded(l == 3 ? palette.accent : palette.text_dim,
                          (0.08f + 0.10f * pulse) * alpha),
                    1.0f);
    }

    // ---- connections ----------------------------------------------------
    if (drawn > 0) {
        const float inv_max = value_max > 0.0f ? 1.0f / value_max : 0.0f;
        for (std::size_t k = 0; k < drawn; ++k) {
            const auto& pick = picks[k];
            const auto b = static_cast<std::size_t>(pick.block);
            const float t = std::clamp(pick.value * inv_max, -1.0f, 1.0f);
            const float weight = std::clamp(pick.rank * inv_max, 0.0f, 1.0f);
            const float pulse = wave_strength(static_cast<float>(b) + 0.5f);
            dl->AddLine(node_point(b, static_cast<std::size_t>(pick.src)),
                        node_point(b + 1, static_cast<std::size_t>(pick.dst)),
                        rgba(signed_colormap(t),
                             (0.04f + 0.86f * weight) * pulse * alpha),
                        0.55f + 1.55f * weight);
        }
    }

    // ---- nodes, drawn over the connections ------------------------------
    constexpr float kNodeMin = 1.8f;
    for (std::size_t l = 0; l < values.size(); ++l) {
        const auto& layer = layout.layers[l];
        if (layer.size == 0) {
            continue;
        }
        const float spacing = band_w / static_cast<float>(layer.size);
        const float r_max = std::clamp(0.46f * spacing, 2.6f, kNodeRadiusMax);
        const std::string count = grouped(layer.size);
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + kSidePad,
                           lanes_top + lane_h * static_cast<float>(l)),
                    faded(l == 3 ? palette.accent : palette.text_dim,
                          alpha * wave_strength(static_cast<float>(l))),
                    count.c_str());
        const float pulse = wave_strength(static_cast<float>(l));
        for (std::size_t i = 0; i < layer.size; ++i) {
            const float a = values[l] != nullptr ? (*values[l])[i] : 0.0f;
            const float mag = std::clamp(std::fabs(a) / layer_max[l], 0.0f, 1.0f);
            const float r = kNodeMin + (r_max - kNodeMin) * mag;
            const ImVec2 point = node_point(l, i);
            const auto rgb = signed_colormap(a / layer_max[l]);
            if (mag > 0.30f) {
                dl->AddCircleFilled(point, r * 3.0f,
                                    rgba(rgb, 0.065f * mag * pulse * alpha));
                dl->AddCircleFilled(point, r * 1.8f,
                                    rgba(rgb, 0.125f * mag * pulse * alpha));
            }
            dl->AddCircleFilled(point, r,
                                rgba(rgb, (0.42f + 0.58f * mag) * alpha));
            const bool chosen_head = l == 3 && static_cast<int>(i) == winner;
            if (mag > 0.55f || chosen_head) {
                dl->AddCircle(point, r + (chosen_head ? 3.0f : 1.4f),
                              faded(chosen_head ? palette.accent : palette.text,
                                    (chosen_head ? 0.95f : 0.45f * mag) * pulse * alpha),
                              0, chosen_head ? 2.4f : 1.2f);
            }
        }
    }

    // Outcome glyph: decision state → mesh. No prose is needed here; the
    // measured OOD distance or selected action remains the only text.
    if (frame != nullptr) {
        const float chip_top = lanes_top + lanes_h + type.legend * 0.25f;
        dl->AddRectFilled(ImVec2(origin.x, chip_top),
                          ImVec2(origin.x + region.x, chip_top + chip_h),
                          faded(palette.panel_bg, 0.72f * alpha), 7.0f);
        dl->AddRect(ImVec2(origin.x, chip_top),
                    ImVec2(origin.x + region.x, chip_top + chip_h),
                    faded(palette.accent, 0.62f * alpha), 7.0f, 0, 1.2f);
        const ImVec2 state_at(origin.x + 28.0f, chip_top + 0.5f * chip_h);
        const ImVec4 state_color =
            cue.pass_lane_live
                ? palette.accent
                : (state.decision_applied ? palette.status_ok : palette.status_warn);
        dl->AddCircle(state_at, 11.0f, faded(state_color, alpha), 0, 2.2f);
        if (cue.pass_lane_live) {
            dl->AddCircleFilled(state_at, 4.0f, faded(state_color, alpha));
        } else if (state.decision_applied) {
            dl->AddLine(ImVec2(state_at.x - 5.0f, state_at.y),
                        ImVec2(state_at.x - 1.0f, state_at.y + 5.0f),
                        faded(state_color, alpha), 2.2f);
            dl->AddLine(ImVec2(state_at.x - 1.0f, state_at.y + 5.0f),
                        ImVec2(state_at.x + 7.0f, state_at.y - 6.0f),
                        faded(state_color, alpha), 2.2f);
        } else {
            dl->AddLine(ImVec2(state_at.x - 7.0f, state_at.y + 7.0f),
                        ImVec2(state_at.x + 7.0f, state_at.y - 7.0f),
                        faded(state_color, alpha), 2.2f);
        }
        const ImVec2 arrow_a(state_at.x + 18.0f, state_at.y);
        const ImVec2 grid_at(state_at.x + 74.0f, state_at.y);
        dl->AddLine(arrow_a, ImVec2(grid_at.x - 19.0f, grid_at.y),
                    faded(state_color, 0.72f * alpha), 2.0f);
        dl->AddTriangleFilled(ImVec2(grid_at.x - 14.0f, grid_at.y),
                              ImVec2(grid_at.x - 22.0f, grid_at.y - 5.0f),
                              ImVec2(grid_at.x - 22.0f, grid_at.y + 5.0f),
                              faded(state_color, 0.72f * alpha));
        for (int i = -1; i <= 1; ++i) {
            const float d = static_cast<float>(i) * 7.0f;
            dl->AddLine(ImVec2(grid_at.x - 10.0f, grid_at.y + d),
                        ImVec2(grid_at.x + 10.0f, grid_at.y + d),
                        faded(palette.accent, alpha), 1.2f);
            dl->AddLine(ImVec2(grid_at.x + d, grid_at.y - 10.0f),
                        ImVec2(grid_at.x + d, grid_at.y + 10.0f),
                        faded(palette.accent, alpha), 1.2f);
        }
        const auto& decision = state.explanation->decision;
        std::string value;
        if (cue.pass_lane_live) {
            value = fmt("candidate %s / %s",
                        grouped(static_cast<std::size_t>(cue.frame_index) + 1).c_str(),
                        grouped(state.explanation->frames.size()).c_str());
        } else if (cue.stage_index >= 0 &&
                   static_cast<std::size_t>(cue.stage_index) < state.stages.size()) {
            const std::size_t total_cells =
                state.stages[static_cast<std::size_t>(cue.stage_index)].mesh.elements.size();
            const std::size_t visible_cells = std::min(
                total_cells, static_cast<std::size_t>(std::floor(
                                 cue.mesh_action_reveal * static_cast<double>(total_cells))));
            const std::string source = state.decision_applied
                                           ? std::string(mesher_plain(frame->action.mesher))
                                           : std::string("configured baseline");
            value = fmt("%s  →  %s / %s cells", source.c_str(),
                        grouped(visible_cells).c_str(), grouped(total_cells).c_str());
        } else if (state.decision_applied) {
            value = fmt("%s · h/L %.3g · p%d",
                        std::string(mesher_plain(frame->action.mesher)).c_str(),
                        frame->action.h_rel, frame->action.order);
        } else {
            value = fmt("d = %.3g · configured baseline", decision.ood_distance);
        }
        dl->AddText(font, type.label,
                    ImVec2(grid_at.x + 26.0f, state_at.y - 0.5f * type.label),
                    faded(state_color, alpha), value.c_str());
    }

    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
#endif
}

namespace {


void draw_cinema_features(const CinemaState& state, const CinemaCue& cue,
                          const CinemaType& type, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 region = ImGui::GetContentRegionAvail();
    dl->AddText(font, type.caption, origin, faded(palette.text, alpha),
                "CAD edge network  ↔  κ(s)  ↔  FFT");

    const float chart_top = origin.y + type.caption * 1.7f;
    const float chart_h =
        std::max(260.0f, region.y - (chart_top - origin.y) - type.label * 2.2f);
    const float chart_w = std::max(120.0f, region.x);
    dl->AddRectFilled(ImVec2(origin.x, chart_top),
                      ImVec2(origin.x + chart_w, chart_top + chart_h),
                      faded(palette.panel_bg, 0.52f * alpha), 6.0f);
    dl->AddRect(ImVec2(origin.x, chart_top), ImVec2(origin.x + chart_w, chart_top + chart_h),
                faded(palette.border, 0.8f * alpha), 6.0f);

    const float pad = 13.0f;
    const float split_y = chart_top + chart_h * 0.58f;
    dl->AddLine(ImVec2(origin.x + pad, split_y), ImVec2(origin.x + chart_w - pad, split_y),
                faded(palette.border, 0.72f * alpha), 1.0f);
    const float edge_right = origin.x + chart_w * 0.35f;
    const float curve_left = edge_right + pad * 1.5f;

    // The left inset is the selected CAD edge itself, projected through its two
    // widest world axes. Three adjacent samples and their circumcircle expose
    // the actual discrete-curvature construction; the right trace is the
    // measured κ(s) those samples produced.
    const auto& edge_points = state.sizing.curve_points;
    if (edge_points.size() >= 3) {
        Eigen::Vector3d lo = edge_points.front();
        Eigen::Vector3d hi = edge_points.front();
        for (const auto& p : edge_points) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        std::array<int, 3> axes{0, 1, 2};
        const Eigen::Vector3d range = hi - lo;
        std::sort(axes.begin(), axes.end(), [&](int a, int b) { return range[a] > range[b]; });
        const int ax = axes[0];
        const int ay = axes[1];
        const double sx = std::max(range[ax], 1.0e-12);
        const double sy = std::max(range[ay], 1.0e-12);
        const float edge_top = chart_top + pad;
        const float edge_bottom = split_y - pad;
        const auto edge_point = [&](const Eigen::Vector3d& p) {
            return ImVec2(origin.x + pad +
                              (edge_right - origin.x - 2.0f * pad) *
                                  static_cast<float>((p[ax] - lo[ax]) / sx),
                          edge_bottom - (edge_bottom - edge_top) *
                                            static_cast<float>((p[ay] - lo[ay]) / sy));
        };
        const double reveal =
            cue.spectral_edge_reveal * static_cast<double>(edge_points.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(reveal)), edge_points.size() - 1);
        for (std::size_t i = 1; i < edge_points.size(); ++i) {
            const bool lit = i <= whole;
            dl->AddLine(
                edge_point(edge_points[i - 1]), edge_point(edge_points[i]),
                faded(lit ? palette.accent : palette.text_dim, alpha * (lit ? 0.92f : 0.22f)),
                lit ? 2.4f : 1.0f);
        }
        const std::size_t sample = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::llround(
                cue.spectral_curve_cursor * static_cast<double>(edge_points.size() - 1))),
            1, edge_points.size() - 2);
        const float construction_alpha =
            alpha * static_cast<float>(cue.spectral_curve_cursor_alpha);
        const ImVec2 a = edge_point(edge_points[sample - 1]);
        const ImVec2 b = edge_point(edge_points[sample]);
        const ImVec2 c = edge_point(edge_points[sample + 1]);
        for (const ImVec2 p : {a, b, c}) {
            dl->AddCircleFilled(p, 4.0f, faded(palette.accent_soft_top, construction_alpha));
        }
        const float tangent_norm = std::hypot(c.x - a.x, c.y - a.y);
        if (tangent_norm > 1.0e-4f) {
            const ImVec2 tangent((c.x - a.x) / tangent_norm, (c.y - a.y) / tangent_norm);
            ImVec2 normal(-tangent.y, tangent.x);
            const ImVec2 inset_center(0.5f * (origin.x + edge_right),
                                      0.5f * (edge_top + edge_bottom));
            if (normal.x * (b.x - inset_center.x) + normal.y * (b.y - inset_center.y) < 0.0f) {
                normal.x = -normal.x;
                normal.y = -normal.y;
            }
            dl->AddLine(ImVec2(b.x - 24.0f * tangent.x, b.y - 24.0f * tangent.y),
                        ImVec2(b.x + 24.0f * tangent.x, b.y + 24.0f * tangent.y),
                        faded(palette.text, 0.76f * construction_alpha), 1.5f);
            const ImVec2 normal_tip(b.x + 34.0f * normal.x, b.y + 34.0f * normal.y);
            dl->AddLine(b, normal_tip, faded(palette.status_warn, 0.86f * construction_alpha),
                        2.0f);
            dl->AddCircleFilled(normal_tip, 3.5f,
                                faded(palette.status_warn, construction_alpha));
        }
        const float d = 2.0f * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (std::fabs(d) > 1.0e-4f) {
            const float aa = a.x * a.x + a.y * a.y;
            const float bb = b.x * b.x + b.y * b.y;
            const float cc = c.x * c.x + c.y * c.y;
            const ImVec2 center((aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d,
                                (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d);
            const float radius = std::hypot(center.x - b.x, center.y - b.y);
            const bool inside =
                center.x - radius >= origin.x + pad && center.x + radius <= edge_right - pad &&
                center.y - radius >= edge_top && center.y + radius <= edge_bottom;
            if (std::isfinite(radius) && inside) {
                dl->AddCircle(center, radius,
                              faded(palette.status_warn, 0.62f * construction_alpha), 0, 1.4f);
                dl->AddLine(center, b, faded(palette.status_warn, 0.78f * construction_alpha),
                            1.4f);
            }
        }
        const ImVec2 arrow_a(edge_right + 2.0f, 0.5f * (edge_top + edge_bottom));
        const ImVec2 arrow_b(curve_left - 6.0f, arrow_a.y);
        dl->AddLine(arrow_a, arrow_b, faded(palette.accent, 0.72f * alpha), 2.0f);
        dl->AddTriangleFilled(arrow_b, ImVec2(arrow_b.x - 8.0f, arrow_b.y - 5.0f),
                              ImVec2(arrow_b.x - 8.0f, arrow_b.y + 5.0f),
                              faded(palette.accent, 0.72f * alpha));
        dl->AddTriangleFilled(arrow_a, ImVec2(arrow_a.x + 8.0f, arrow_a.y - 5.0f),
                              ImVec2(arrow_a.x + 8.0f, arrow_a.y + 5.0f),
                              faded(palette.accent, 0.72f * alpha));
    }

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
            const float x = curve_left + (origin.x + chart_w - pad - curve_left) *
                                             static_cast<float>(station);
            const float y = plot_bottom -
                            (plot_bottom - plot_top) * static_cast<float>((value - lo) / span);
            return ImVec2(x, y);
        };

        const double revealed = cue.spectral_edge_reveal * static_cast<double>(raw.size() - 1);
        const std::size_t whole =
            std::min(static_cast<std::size_t>(std::floor(revealed)), raw.size() - 1);
        for (std::size_t i = 1; i <= whole; ++i) {
            dl->AddLine(point(stations[i - 1], raw[i - 1]), point(stations[i], raw[i]),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double y0 =
                raw[i - 1] + cue.spectral_filter_mix * (filtered[i - 1] - raw[i - 1]);
            const double y1 = raw[i] + cue.spectral_filter_mix * (filtered[i] - raw[i]);
            dl->AddLine(point(stations[i - 1], y0), point(stations[i], y1),
                        faded(palette.accent, alpha), 2.6f);
            dl->AddCircleFilled(point(stations[i], y1), 2.6f,
                                faded(palette.accent_soft_top, alpha));
        }
        if (whole + 1 < raw.size()) {
            const double part = revealed - static_cast<double>(whole);
            const double station =
                stations[whole] + part * (stations[whole + 1] - stations[whole]);
            const double raw_value = raw[whole] + part * (raw[whole + 1] - raw[whole]);
            const double filtered_value =
                filtered[whole] + part * (filtered[whole + 1] - filtered[whole]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            dl->AddLine(point(stations[whole], raw[whole]), point(station, raw_value),
                        faded(palette.text_dim, 0.62f * alpha), 1.2f);
            const double start =
                raw[whole] + cue.spectral_filter_mix * (filtered[whole] - raw[whole]);
            dl->AddLine(point(stations[whole], start), point(station, value),
                        faded(palette.accent, alpha), 2.6f);
        }
        if (cue.spectral_edge_reveal > 0.0 && cue.spectral_curve_cursor_alpha > 0.0) {
            const double cursor = std::clamp(cue.spectral_curve_cursor, 0.0, 1.0) *
                                  static_cast<double>(raw.size() - 1);
            const std::size_t lo_index =
                std::min(static_cast<std::size_t>(std::floor(cursor)), raw.size() - 1);
            const std::size_t hi_index = std::min(lo_index + 1, raw.size() - 1);
            const double part = cursor - static_cast<double>(lo_index);
            const double station =
                stations[lo_index] + part * (stations[hi_index] - stations[lo_index]);
            const double raw_value = raw[lo_index] + part * (raw[hi_index] - raw[lo_index]);
            const double filtered_value =
                filtered[lo_index] + part * (filtered[hi_index] - filtered[lo_index]);
            const double value =
                raw_value + cue.spectral_filter_mix * (filtered_value - raw_value);
            const ImVec2 scan = point(station, value);
            const float cursor_alpha =
                alpha * static_cast<float>(cue.spectral_curve_cursor_alpha);
            dl->AddLine(ImVec2(scan.x, plot_top), ImVec2(scan.x, plot_bottom),
                        faded(palette.accent_soft_top, 0.52f * cursor_alpha), 1.2f);
            dl->AddCircleFilled(scan, 4.6f, faded(palette.accent_soft_top, cursor_alpha));
        }
    }

    const auto& spectrum = state.sizing.curve_spectrum;
    const auto& kept = state.sizing.curve_mode_kept;
    if (spectrum.size() >= 2 && kept.size() == spectrum.size()) {
        // DC is the mean curvature, not spacing variation. `truncate_modes`
        // always preserves it and excludes it from modes_total, so omitting it
        // here both matches the report's denominator and stops one huge bar
        // from flattening every explanatory non-DC mode.
        const double max_magnitude =
            std::max(*std::max_element(spectrum.begin() + 1, spectrum.end()), 1.0e-12);
        const float bars_top = split_y + pad;
        const float bars_bottom = chart_top + chart_h - 11.0f;
        const float bars_h = std::max(1.0f, bars_bottom - bars_top);
        const float bars_w = chart_w - 2.0f * pad;
        const std::size_t mode_count = spectrum.size() - 1;
        const float slot = bars_w / static_cast<float>(mode_count);
        const std::size_t visible = std::min(
            mode_count, static_cast<std::size_t>(std::ceil(cue.spectral_spectrum_reveal *
                                                           static_cast<double>(mode_count))));
        const double log_max = std::log1p(max_magnitude);
        for (std::size_t mode = 0; mode < visible; ++mode) {
            const std::size_t i = mode + 1;
            const float magnitude =
                static_cast<float>(std::log1p(spectrum[i]) / std::max(log_max, 1.0e-12));
            const float x0 = origin.x + pad + static_cast<float>(mode) * slot;
            const float x1 = x0 + std::max(1.0f, slot - 1.0f);
            const float y0 = bars_bottom - bars_h * magnitude;
            const bool survives = kept[i] != 0;
            const float discarded_alpha =
                static_cast<float>(1.0 - 0.82 * cue.spectral_filter_mix);
            const ImVec4 color =
                survives ? palette.accent
                         : ImVec4(palette.text_dim.x, palette.text_dim.y, palette.text_dim.z,
                                  palette.text_dim.w * discarded_alpha);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, bars_bottom), faded(color, alpha),
                              1.0f);
        }
    }
    const std::string status =
        state.sizing.field_points.empty()
            ? fmt("%s / %s modes · all CAD curves filtered · uniform h unchanged",
                  grouped(state.sizing.curve_modes_kept).c_str(),
                  grouped(state.sizing.curve_modes_total).c_str())
            : fmt("%s / %s modes · reconstructed across CAD · measured h(x)",
                  grouped(state.sizing.curve_modes_kept).c_str(),
                  grouped(state.sizing.curve_modes_total).c_str());
    dl->AddText(
        font, type.label, ImVec2(origin.x, chart_top + chart_h + type.label * 0.48f),
        faded(state.sizing.field_points.empty() ? palette.text_dim : palette.accent, alpha),
        status.c_str());
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

void draw_cinema_cells(const CinemaState& state, const CinemaCue& cue,
                       const CinemaType& type, const CinemaHud& hud, float alpha) {
    if (alpha <= 0.0f) {
        return;
    }
    (void)hud;
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
    const float bar_y = origin.y + 10.0f;
    const float bar_h = std::max(18.0f, type.legend + 2.0f);
    if (solved != nullptr) {
        const std::size_t total =
            std::max<std::size_t>(1, std::accumulate(solved->type_counts.begin(),
                                                     solved->type_counts.end(),
                                                     std::size_t{0}));
        float x = origin.x;
        std::size_t dominant = 0;
        for (std::size_t i = 0; i < solved->type_counts.size(); ++i) {
            if (solved->type_counts[i] == 0) {
                continue;
            }
            if (solved->type_counts[i] > solved->type_counts[dominant]) {
                dominant = i;
            }
            const float w = region.x * static_cast<float>(solved->type_counts[i]) /
                            static_cast<float>(total);
            dl->AddRectFilled(ImVec2(x, bar_y), ImVec2(x + w, bar_y + bar_h),
                              rgba(element_type_color(static_cast<fea::ElementType>(i)), alpha),
                              2.0f);
            x += w;
        }
        // A full-width unlabelled bar says nothing. Name what it is a bar of.
        const auto dominant_type = static_cast<fea::ElementType>(dominant);
        const std::string mix =
            fmt("element mix · %s %.0f%%", fea::element_type_name(dominant_type),
                100.0 * static_cast<double>(solved->type_counts[dominant]) /
                    static_cast<double>(total));
        dl->AddText(font, type.legend,
                    ImVec2(origin.x + 8.0f, bar_y + 0.5f * (bar_h - type.legend)),
                    faded(palette.window_bg, 0.92f * alpha), mix.c_str());
    }

    const float card_top = bar_y + bar_h + 16.0f;
    const float card_h = std::max(260.0f, region.y - (card_top - origin.y) - 14.0f);
    const ImVec2 card_min(origin.x, card_top);
    const ImVec2 card_max(origin.x + region.x, card_top + card_h);
    dl->AddRectFilled(card_min, card_max, faded(palette.panel_bg, 0.50f * alpha), 8.0f);
    dl->AddRect(card_min, card_max, faded(palette.border, 0.72f * alpha), 8.0f);

    // A p2 tet is not a tet4 with extra dots on it. Its six midside nodes are
    // the degrees of freedom that bend the element's own geometry and let strain
    // vary linearly inside the cell, so that is what this card animates: the
    // midside nodes lift off their chords and curve every edge, with the tet4
    // chord kept underneath as the dim reference. The plot below is the real
    // quadratic edge basis, and one lit edge carries the plot's ξ cursor so the
    // curve and the geometry are visibly the same three functions.
    const char* title = "tet10 · p2 · quadratic tetrahedron";
    dl->AddText(font, type.caption, ImVec2(card_min.x + 18.0f, card_min.y + 15.0f),
                faded(palette.status_ok, alpha), title);
    const char* topology = "10 nodes · 30 DOF · midside nodes curve the cell (tet4: 4 · 12)";
    dl->AddText(font, type.legend,
                ImVec2(card_min.x + 18.0f, card_min.y + 15.0f + type.caption * 1.35f),
                faded(palette.text_dim, alpha), topology);

    const float t = static_cast<float>(cue.act_t);
    const float fade_seconds = 0.17f * static_cast<float>(cue.act_span);
    const float visible_t = std::max(0.0f, t - fade_seconds);
    const float progress =
        std::clamp(visible_t / std::max(0.62f * static_cast<float>(cue.act_span), 1.0e-6f),
                   0.0f, 1.0f);
    const float corner_alpha = static_cast<float>(smoothstep(progress / 0.26));
    const float midside_alpha = static_cast<float>(smoothstep((progress - 0.20) / 0.30));
    const float bend = 0.34f * static_cast<float>(smoothstep((progress - 0.34) / 0.30)) *
                       (0.80f + 0.20f * std::sin(t * 1.15f));
    const float plot_alpha =
        static_cast<float>(smoothstep((progress - 0.52) / 0.30)) * alpha;
    const float xi = 0.5f - 0.5f * std::cos(t * 0.85f);

    // Quadratic Lagrange basis on an edge: corner nodes at ξ = 0 and 1, midside
    // node at ξ = 0.5. The same three functions map the edge's geometry and
    // interpolate its field, which is the whole point of the element.
    const auto basis = [](float s) {
        return std::array<float, 3>{(1.0f - s) * (1.0f - 2.0f * s), s * (2.0f * s - 1.0f),
                                    4.0f * s * (1.0f - s)};
    };

    static const std::array<Eigen::Vector3f, 4> kCorners{{
        {-1.0f, -0.78f, -0.58f},
        {1.0f, -0.72f, -0.52f},
        {-0.48f, 0.96f, -0.44f},
        {0.10f, -0.05f, 1.0f},
    }};
    static constexpr std::array<std::array<std::size_t, 2>, 6> kEdges{{
        {{0, 1}}, {{0, 2}}, {{0, 3}}, {{1, 2}}, {{1, 3}}, {{2, 3}},
    }};
    static constexpr std::array<std::array<std::size_t, 3>, 4> kFaces{{
        {{0, 1, 2}}, {{0, 1, 3}}, {{0, 2, 3}}, {{1, 2, 3}},
    }};
    constexpr std::size_t kLitEdge = 0;

    // Everything below the element is anchored off the card's bottom so the
    // basis plot, its legend and the honest solve note cannot collide with the
    // measured-quality line on a short pane.
    const float quality_y = card_top + card_h - type.label * 1.8f;
    const float note_y = quality_y - type.legend * 1.75f;
    const float legend_y = note_y - type.legend * 1.55f;
    const float plot_bottom = legend_y - 12.0f;
    const float plot_top =
        std::max(card_min.y + 0.52f * card_h, plot_bottom - 0.24f * card_h);
    const float plot_left = card_min.x + 26.0f;
    const float plot_right = card_max.x - 26.0f;
    const float body_top = card_min.y + 0.17f * card_h;

    const float yaw = -0.70f + 0.10f * std::sin(t * 0.42f);
    const float pitch = 0.46f;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float scale =
        std::min(region.x * 0.26f, std::max(24.0f, 0.42f * (plot_top - body_top)));
    const ImVec2 center(card_min.x + 0.50f * region.x, 0.5f * (body_top + plot_top));
    // Orthographic and affine, so projecting the quadratic edge map is the same
    // curve as the quadratic map of the projected nodes.
    const auto project = [&](const Eigen::Vector3f& q) {
        const float rx = cy * q.x() + sy * q.z();
        const float rz = -sy * q.x() + cy * q.z();
        const float ry = cp * q.y() - sp * rz;
        return ImVec2(center.x + scale * rx, center.y - scale * ry);
    };

    std::array<ImVec2, kCorners.size()> corner2{};
    Eigen::Vector3f body = Eigen::Vector3f::Zero();
    for (std::size_t i = 0; i < kCorners.size(); ++i) {
        corner2[i] = project(kCorners[i]);
        body += kCorners[i];
    }
    body *= 0.25f;
    std::array<ImVec2, kEdges.size()> mid2{};
    for (std::size_t e = 0; e < kEdges.size(); ++e) {
        const Eigen::Vector3f chord =
            0.5f * (kCorners[kEdges[e][0]] + kCorners[kEdges[e][1]]);
        Eigen::Vector3f out = chord - body;
        const float n = out.norm();
        out = n > 1.0e-6f ? Eigen::Vector3f(out / n) : Eigen::Vector3f::UnitY();
        mid2[e] = project(chord + bend * out);
    }
    const auto edge_of = [](std::size_t a, std::size_t b) {
        for (std::size_t e = 0; e < kEdges.size(); ++e) {
            if ((kEdges[e][0] == a && kEdges[e][1] == b) ||
                (kEdges[e][0] == b && kEdges[e][1] == a)) {
                return e;
            }
        }
        return std::size_t{0};
    };
    const auto edge_point = [&](std::size_t e, float s) {
        const std::array<float, 3> n = basis(s);
        const ImVec2 a = corner2[kEdges[e][0]];
        const ImVec2 b = corner2[kEdges[e][1]];
        const ImVec2 m = mid2[e];
        return ImVec2(n[0] * a.x + n[1] * b.x + n[2] * m.x,
                      n[0] * a.y + n[1] * b.y + n[2] * m.y);
    };

    // Each curved face is filled as the quadratic triangle's own four
    // sub-triangles, so the tint follows the bent boundary instead of the
    // straight one.
    const ImU32 tint = faded(palette.accent, 0.085f * corner_alpha * alpha);
    for (const auto& face : kFaces) {
        const ImVec2 m01 = mid2[edge_of(face[0], face[1])];
        const ImVec2 m12 = mid2[edge_of(face[1], face[2])];
        const ImVec2 m20 = mid2[edge_of(face[2], face[0])];
        dl->AddTriangleFilled(corner2[face[0]], m01, m20, tint);
        dl->AddTriangleFilled(m01, corner2[face[1]], m12, tint);
        dl->AddTriangleFilled(m20, m12, corner2[face[2]], tint);
        dl->AddTriangleFilled(m01, m12, m20, tint);
    }
    for (std::size_t e = 0; e < kEdges.size(); ++e) {
        // The straight chord is the tet4 edge the midside node left behind: the
        // bend then reads as a difference between two elements, not as styling.
        dl->AddLine(corner2[kEdges[e][0]], corner2[kEdges[e][1]],
                    faded(palette.text_dim, 0.34f * midside_alpha * alpha), 1.2f);
        const bool lit = e == kLitEdge;
        constexpr int kEdgeSamples = 18;
        for (int k = 0; k <= kEdgeSamples; ++k) {
            dl->PathLineTo(edge_point(e, static_cast<float>(k) / kEdgeSamples));
        }
        dl->PathStroke(faded(lit ? palette.status_warn : palette.status_ok,
                             (lit ? 0.95f : 0.62f) * corner_alpha * alpha),
                       0, lit ? 3.6f : 2.6f);
    }
    for (const ImVec2 mid : mid2) {
        // Midside nodes are diamonds and corners are circles: at video scale a
        // shape difference survives where a radius difference does not.
        dl->AddNgonFilled(mid, 6.6f, faded(palette.status_ok, midside_alpha * alpha), 4);
        dl->AddNgon(mid, 10.0f, faded(palette.status_ok, 0.42f * midside_alpha * alpha), 4,
                    1.4f);
    }
    for (const ImVec2 point : corner2) {
        dl->AddCircleFilled(point, 7.0f, faded(palette.text, corner_alpha * alpha));
        dl->AddCircle(point, 10.0f,
                      faded(palette.accent_soft_top, 0.48f * corner_alpha * alpha), 0, 1.6f);
    }

    if (plot_alpha > 0.0f) {
        const ImVec2 marker = edge_point(kLitEdge, xi);
        dl->AddCircleFilled(marker, 5.2f, faded(palette.status_warn, plot_alpha));
        dl->AddCircle(marker, 11.0f, faded(palette.status_warn, 0.55f * plot_alpha), 0, 1.8f);

        const ImVec2 frame_min(plot_left - 12.0f, plot_top - 10.0f);
        const ImVec2 frame_max(plot_right + 12.0f, plot_bottom + 10.0f);
        dl->AddRectFilled(frame_min, frame_max, faded(palette.panel_bg, 0.72f * plot_alpha),
                          6.0f);
        dl->AddRect(frame_min, frame_max, faded(palette.border, 0.85f * plot_alpha), 6.0f);
        const auto at = [&](float s, float v) {
            return ImVec2(plot_left + (plot_right - plot_left) * s,
                          plot_bottom - (plot_bottom - plot_top) * (v + 0.20f) / 1.20f);
        };
        // N = 0 and N = 1 rails, then the two straight hats a tet4 edge
        // interpolates with, so the quadratic curves are read against p1.
        dl->AddLine(at(0.0f, 0.0f), at(1.0f, 0.0f),
                    faded(palette.text_dim, 0.55f * plot_alpha), 1.2f);
        dl->AddLine(at(0.0f, 1.0f), at(1.0f, 1.0f),
                    faded(palette.text_dim, 0.22f * plot_alpha), 1.0f);
        dl->AddLine(at(0.0f, 1.0f), at(1.0f, 0.0f),
                    faded(palette.text_dim, 0.26f * plot_alpha), 1.0f);
        dl->AddLine(at(0.0f, 0.0f), at(1.0f, 1.0f),
                    faded(palette.text_dim, 0.26f * plot_alpha), 1.0f);
        constexpr int kPlotSamples = 56;
        for (int c = 0; c < 3; ++c) {
            for (int k = 0; k <= kPlotSamples; ++k) {
                const float s = static_cast<float>(k) / kPlotSamples;
                dl->PathLineTo(at(s, basis(s)[static_cast<std::size_t>(c)]));
            }
            dl->PathStroke(faded(c == 2 ? palette.status_ok : palette.text,
                                 (c == 2 ? 0.95f : 0.85f) * plot_alpha),
                           0, c == 2 ? 3.0f : 2.0f);
        }
        // Named on the curves themselves; a colour key in a caption is one more
        // thing to hold in mind while the cursor is moving.
        dl->AddText(font, type.legend, at(0.02f, 0.88f),
                    faded(palette.text, 0.85f * plot_alpha), "N1");
        dl->AddText(font, type.legend, at(0.93f, 0.88f),
                    faded(palette.text, 0.85f * plot_alpha), "N2");
        dl->AddText(font, type.legend, at(0.47f, 0.88f),
                    faded(palette.status_ok, 0.95f * plot_alpha), "N3");
        const std::array<float, 3> n_xi = basis(xi);
        dl->AddLine(at(xi, -0.20f), at(xi, 1.0f),
                    faded(palette.status_warn, 0.45f * plot_alpha), 1.4f);
        for (int c = 0; c < 3; ++c) {
            dl->AddCircleFilled(at(xi, n_xi[static_cast<std::size_t>(c)]), 4.2f,
                                faded(c == 2 ? palette.status_ok : palette.text, plot_alpha));
        }
        const std::string cursor = fmt("ξ %.2f   N3 %.2f", xi, n_xi[2]);
        const float cursor_w =
            font->CalcTextSizeA(type.legend, FLT_MAX, 0.0f, cursor.c_str()).x;
        dl->AddText(font, type.legend, ImVec2(plot_right - cursor_w - 4.0f, plot_top + 2.0f),
                    faded(palette.status_warn, plot_alpha), cursor.c_str());
        const char* legend =
            "corner · midside basis on one edge · ξ runs along the lit edge · dim: tet4 linear";
        dl->AddText(font, type.legend, ImVec2(plot_left, legend_y),
                    faded(palette.text_dim, plot_alpha), legend);
    }
    const char* honest = "supported element path · this take solved tet4 · p1";
    dl->AddText(font, type.legend, ImVec2(plot_left, note_y),
                faded(palette.text_dim, 0.85f * alpha), honest);

    const double q_mean =
        solved != nullptr && solved->quality_measured > 0 ? solved->quality_mean : 0.0;
    const double q_min =
        solved != nullptr && solved->quality_measured > 0 ? solved->quality_min : 0.0;
    // "qmean", not a macron: ImGui composes no combining marks, so q + U+0304
    // rasterised as a missing-glyph box in the published take.
    const std::string quality = fmt("measured mesh   qmin %.3f   qmean %.3f", q_min, q_mean);
    const ImVec2 quality_size =
        font->CalcTextSizeA(type.label, FLT_MAX, 0.0f, quality.c_str());
    dl->AddText(font, type.label,
                ImVec2(origin.x + 0.5f * region.x - 0.5f * quality_size.x, quality_y),
                faded(palette.status_ok, alpha), quality.c_str());
    ImGui::Dummy(ImVec2(region.x, std::max(1.0f, region.y - 2.0f)));
}

} // namespace

void draw_cinema_panel(CinemaState& state, const CinemaCue& cue, const CinemaType& type,
                       const CinemaHud& hud) {
    // Four views share one pane. The chosen measured network remains present
    // while its selected action lands as real emitted cells; only the finished
    // mesh hold hands it to the cell audit, so "decision" and "conversion" are
    // one causal visual sentence rather than separate chapters.
    float feature_alpha = 0.0f;
    float network_alpha = 0.0f;
    float cell_alpha = 0.0f;
    float equation_alpha = 0.0f;
    if (cue.act == CinemaAct::kSkeleton) {
        feature_alpha = cue.panel_open;
    } else if (cue.act == CinemaAct::kDeliberate) {
        const double bridge = std::min(1.3, 0.18 * cue.act_span);
        const float blend = static_cast<float>(
            smoothstep(cue.act_t / std::max(bridge, 1.0e-9)));
        feature_alpha = 1.0f - blend;
        network_alpha = blend;
    } else if (cue.act == CinemaAct::kBuild) {
        network_alpha = 1.0f;
    } else if (cue.act == CinemaAct::kMeshHold) {
        const float blend = static_cast<float>(
            smoothstep(cue.act_t / std::max(0.17 * cue.act_span, 1.0e-9)));
        network_alpha = 1.0f - blend;
        cell_alpha = blend;
    } else {
        cell_alpha = 1.0f - cue.equations_alpha;
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

void skeleton_caption(const CinemaState& state, const CinemaCue& cue,
                      CinemaCaption& out) {
    const double p = cue.act_t / std::max(cue.act_span, 1.0e-9);
    if (p < 0.30) {
        return;
    }
    switch (state.skeleton_source) {
    case SkeletonSource::kBrepEdges:
        out.headline = "CAD  →  κ(s)  →  FFT";
        out.numbers =
            fmt("%s edges · %s exact samples",
                grouped(state.skeleton_polylines).c_str(),
                grouped(state.sizing.curve_points.size()).c_str());
        break;
    case SkeletonSource::kSharpEdges:
        out.headline = "surface  →  crease graph";
        out.numbers =
            fmt("%s creases", grouped(state.skeleton_polylines).c_str());
        break;
    case SkeletonSource::kUnavailable:
        out.headline = "CAD extraction unavailable";
        out.note = state.skeleton_note;
        out.note_color = palette.status_err;
        break;
    case SkeletonSource::kNone:
        out.headline = "No part";
        out.note_color = palette.status_err;
        break;
    }
}

void deliberate_caption(const CinemaState& state, const CinemaCue& cue, CinemaCaption& out) {
#ifndef POLYMESH_WITH_ADVISOR
    (void)state;
    (void)cue;
    out.headline = "Advisor unavailable";
    out.note = "advisor support is not compiled into this build";
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
        const auto& decision = explanation.decision;
        out.headline = state.decision_applied
                           ? fmt("%s · h/L %.3g · p%d",
                                 std::string(mesher_plain(decision.mesher)).c_str(),
                                 decision.h_rel, decision.order)
                           : fmt("d = %.3g", decision.ood_distance);
        out.headline_color =
            state.decision_applied ? palette.status_ok : palette.status_warn;
        out.numbers = fmt("%s passes · %s candidates",
                          grouped(frames.size()).c_str(), grouped(candidates).c_str());
        return;
    }
    out.headline = "81  →  96  →  96  →  20";
    if (cue.frame_index < 0 || static_cast<std::size_t>(cue.frame_index) >= frames.size()) {
        out.numbers = fmt("%s candidates", grouped(candidates).c_str());
        return;
    }
    out.numbers = fmt("%d / %s", cue.frame_index + 1,
                      grouped(frames.size()).c_str());
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
        out.headline = fmt("h %.3g mm · p%d", hud.mesh_size * 1e3, hud.order);
        out.numbers = std::string(mesher_plain(hud.mesher));
        out.headline_color =
            state.decision_applied ? palette.status_ok : palette.status_warn;
        out.note = "chooser complete · recorded stage follows";
        return;
    }
    const auto idx = static_cast<std::size_t>(cue.stage_index);
    if (idx >= state.stages.size()) {
        return;
    }
    const auto& stage = state.stages[idx];
    const std::size_t n_fill = initial_fill_stage_count(state.stages);
    if (idx == 0) {
        const std::size_t visible = static_cast<std::size_t>(
            smoothstep(cue.stage_reveal) *
            static_cast<double>(stage.mesh.elements.size()));
        out.headline = fmt("generating %s / %s cells",
                           grouped(visible).c_str(),
                           grouped(stage.mesh.elements.size()).c_str());
        out.note = "emission-order replay · advisor held";
    } else {
        out.headline = fmt("stage %s / %s · %s",
                           grouped(std::min(idx + 1, n_fill)).c_str(),
                           grouped(n_fill).c_str(),
                           std::string(mesh_stage_plain(stage.stage)).c_str());
        out.note = "recorded mesher boundary · advisor held";
    }
    out.numbers = fmt("%s nodes · p%d",
                      grouped(stage.mesh.nodes.size()).c_str(), hud.order);
    out.headline_color = palette.accent;
}

void mesh_hold_caption(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       CinemaCaption& out) {
    const double x = cue.act_t / std::max(cue.act_span, 1.0e-9);
    out.headline = x < 0.78 ? "tet4  ·  p1" : "K u = f";
    out.headline_color = palette.status_ok;
    if (!state.solve_stages.empty()) {
        const auto& stage = state.solve_stages.front();
        out.numbers = fmt("%s cells · %s nodes · %s unknowns",
                          grouped(stage.trace.n_elems).c_str(),
                          grouped(stage.trace.n_nodes).c_str(),
                          grouped(stage.trace.n_dof).c_str());
        if (!state.solve_insights.empty()) {
            const auto& insight = state.solve_insights.front();
            out.note =
                fmt("qmin %.4g · qmean %.4g", insight.quality_min, insight.quality_mean);
        }
    }
    (void)hud;
}

void solve_caption(CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                   CinemaCaption& out) {
    if (state.solve_stages.empty()) {
        out.headline = "No solve result";
        out.note_color = palette.status_err;
        return;
    }
    const auto i = static_cast<std::size_t>(std::max(cue.solve_stage_index, 0));
    const auto& stage = state.solve_stages[std::min(i, state.solve_stages.size() - 1)];
    const auto& r = stage.result;
    const auto& tr = stage.trace;
    const bool many = state.solve_stages.size() > 1;
    const std::string pass_tag =
        many ? fmt(" · %d/%zu", stage.pass + 1, state.solve_stages.size())
             : std::string{};
    out.note_color = palette.text_dim;
    const double stress_p99 =
        i < state.stress_histograms.size() ? state.stress_histograms[i].p99 : 0.0;
    const double error_p99 =
        i < state.error_histograms.size() ? state.error_histograms[i].p99 : 0.0;
    switch (cue.solve_phase) {
    case SolvePhase::kStressSweep:
        out.headline = "σvm(x)";
        out.numbers = fmt("%s cells · %s unknowns%s",
                          grouped(tr.n_elems).c_str(), grouped(tr.n_dof).c_str(),
                          pass_tag.c_str());
        out.note = stress_p99 > 0.0 ? fmt("p99 %.4g MPa", stress_p99 / 1e6)
                                    : std::string{};
        break;
    case SolvePhase::kStressHold:
        out.headline = fmt("σmax %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        out.numbers = fmt("umax %.4g mm%s", r.max_displacement * 1e3,
                          pass_tag.c_str());
        out.note = stress_p99 > 0.0 ? fmt("p99 %.4g MPa", stress_p99 / 1e6)
                                    : std::string{};
        break;
    case SolvePhase::kGradientSweep: {
        const double gp99 = state.gradient_histogram(i).p99;
        out.headline = "|∇σvm|(x)";
        out.numbers = gp99 > 0.0 ? fmt("p99 %.4g MPa/mm", gp99 / 1e9)
                                 : std::string{};
        break;
    }
    case SolvePhase::kGradientHold: {
        const double gmax = state.gradient_max(i);
        const double gp99 = state.gradient_histogram(i).p99;
        const std::size_t unresolved = state.gradient_unresolved(i);
        if (gmax > 0.0) {
            out.headline = fmt("|∇σ|max %.4g MPa/mm", gmax / 1e9);
            out.headline_color = palette.status_ok;
            out.numbers = gp99 > 0.0 ? fmt("p99 %.4g MPa/mm", gp99 / 1e9)
                                     : std::string{};
            if (unresolved > 0) {
                out.note = fmt("%s unresolved nodes", grouped(unresolved).c_str());
                out.note_color = palette.status_warn;
            }
        } else {
            out.headline = "|∇σ| unavailable";
            out.headline_color = palette.status_warn;
        }
        break;
    }
    case SolvePhase::kError:
        out.headline = "ηZZ";
        out.numbers = hud.eta_target > 0.0
                          ? fmt("%.3g%%  /  %.3g%%", tr.global_eta * 100.0,
                                hud.eta_target * 100.0)
                          : fmt("%.3g%%", tr.global_eta * 100.0);
        out.note = error_p99 > 0.0 ? fmt("p99 %.4g%%", error_p99 * 100.0)
                                   : std::string{};
        break;
    case SolvePhase::kErrorHold:
        out.headline = "ηe  →  h / p";
        out.numbers = fmt("%s h · %s p",
                          grouped(tr.n_h_mark).c_str(), grouped(tr.n_p_mark).c_str());
        out.note = error_p99 > 0.0 ? fmt("p99 %.4g%%", error_p99 * 100.0)
                                   : std::string{};
        break;
    case SolvePhase::kRefine: {
        const std::size_t next = i + 1;
        out.headline = "ηe  →  Δmesh";
        if (next < state.solve_stages.size()) {
            out.numbers = fmt("%s kept · %s removed · %s added",
                              grouped(hud.unchanged_elements).c_str(),
                              grouped(hud.removed_elements).c_str(),
                              grouped(hud.added_elements).c_str());
        }
        break;
    }
    case SolvePhase::kRefineHold: {
        const std::size_t next = i + 1;
        out.headline = "mesh n + 1";
        out.headline_color = palette.status_ok;
        if (next < state.solve_stages.size()) {
            const auto& nx = state.solve_stages[next];
            out.numbers = fmt("%s cells · %s unknowns",
                              grouped(nx.trace.n_elems).c_str(),
                              grouped(nx.trace.n_dof).c_str());
        }
        break;
    }
    case SolvePhase::kLoadRamp:
        out.headline = fmt("λ %.3f", cue.load_factor);
        out.numbers =
            fmt("%.4g kN · %.4g MPa · %.4g mm",
                cue.load_factor * hud.load_newtons / 1e3,
                cue.load_factor * r.max_von_mises / 1e6,
                cue.load_factor * r.max_displacement * 1e3);
        out.note = "u = λu(1) · σ = λσ(1)";
        break;
    case SolvePhase::kHold: {
        out.headline = fmt("σmax %.4g MPa", r.max_von_mises / 1e6);
        out.headline_color = palette.status_ok;
        const std::string_view token = cinema_solver_token(state);
        const char* method = token == "direct_ldlt"
                                 ? "LDLT"
                                 : (token == "cg" ? "CG" : "solver");
        out.numbers = fmt("umax %.4g mm · %s cells · %s unknowns · %s",
                          r.max_displacement * 1e3, grouped(tr.n_elems).c_str(),
                          grouped(tr.n_dof).c_str(), method);
        const double shown_mm = hud.deform_scale * r.max_displacement * 1e3;
        out.note = fmt("display %.4g mm · %.3g×", shown_mm, hud.deform_scale);
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
        {"advisor → mesh", CinemaAct::kDeliberate},
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
        skeleton_caption(state, cue, out);
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
                     ? fmt("%s · provenance unavailable · docs/assets/cinema/NOTES.md",
                           part.c_str())
                     : fmt("%s · %s · docs/assets/cinema/NOTES.md",
                           part.c_str(), hud.stamp.c_str());
    return out;
}

float cinema_strip_height(const CinemaType& type) {
    const float chapter_h = std::floor(type.chapter * 1.9f);
    const float left = std::floor(type.headline * 1.28f) +
                       std::floor(type.numbers * 1.42f);
    const float content = std::max(left, std::floor(type.note * 1.45f));
    const float footer = std::floor(type.footer * 1.45f);
    return std::floor(chapter_h + content + footer + 2.0f * kStripPadY);
}

void draw_cinema_strip(const CinemaState& state, const CinemaCue& cue, const CinemaHud& hud,
                       const CinemaType& type) {
    const CinemaCaption caption = cinema_caption(state, cue, hud);
    ImFont* font = type.font != nullptr ? type.font : ImGui::GetFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(80.0f, ImGui::GetContentRegionAvail().x);

    // Three causal chapters and one progress line. Advisor scoring, its chosen
    // action and the real cell landing remain one continuous chapter.
    const auto chapters = cinema_chapters();
    const CinemaAct here = cue.act == CinemaAct::kBuild || cue.act == CinemaAct::kMeshHold
                               ? CinemaAct::kDeliberate
                               : cue.act;
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

    // ---- horizontal information ledger ---------------------------------
    // Headline/numbers occupy the left, the one plain-language disclosure uses
    // the otherwise-empty right, and provenance retains the full width below.
    // The old four-row stack spent most of a 203 px strip on unused line tails.
    const float content_y = origin.y + chapter_h;
    const float left_w = std::floor(width * 0.61f);
    const float column_gap = 28.0f;
    const float right_x = origin.x + left_w + column_gap;
    const float right_w = std::max(80.0f, width - left_w - column_gap);
    draw_fitted(dl, font, type.headline, ImVec2(origin.x, content_y), left_w,
                caption.headline,
                ImGui::ColorConvertFloat4ToU32(caption.headline_color));
    draw_fitted(dl, font, type.numbers,
                ImVec2(origin.x, content_y + std::floor(type.headline * 1.28f)),
                left_w, caption.numbers,
                ImGui::ColorConvertFloat4ToU32(palette.text));
    draw_fitted(dl, font, type.note, ImVec2(right_x, content_y), right_w,
                caption.note, ImGui::ColorConvertFloat4ToU32(caption.note_color));
    const float content_h =
        std::max(std::floor(type.headline * 1.28f) +
                     std::floor(type.numbers * 1.42f),
                 std::floor(type.note * 1.45f));
    draw_fitted(dl, font, type.footer,
                ImVec2(origin.x, content_y + content_h), width, caption.footer,
                faded(palette.text_dim, 0.90f));
}

} // namespace polymesh::gui
