// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Joint (h, p, shape) adaptive driver (ADR-0019 §4, DAG node hp-driver).
//
// One loop looks at three signals per element and turns the matching knob:
//
//   Geometry  (a priori)   turning angle h·κ, thin-wall  →  h-refine
//   Smoothness (a posteriori) ZZ + hierarchical surplus  →  p-raise
//   Cost / shape fitness   DOF–time heuristics + shape  →  shape tendency
//
// Decision policy (deterministic; see drive_hp / decide_element):
//
//   1. Geometry gate. If the surface turns more than `turn_angle_deg` through
//      the cell (h·κ > θ) or the cell is thicker than a fraction of the local
//      wall (h > f·t), geometry error dominates: polynomial order cannot fix a
//      faceted boundary, so the action is h-refine.
//   2. Smoothness. Where geometry is flat, compare hierarchical surplus to the
//      ZZ residual. Large surplus / η ⇒ the next mode captures the error
//      (analytic interior) → p-raise if p < p_max. Small surplus / η with large
//      η ⇒ non-smooth residual (corner, crack tip) → h-refine.
//   3. Shape. When neither h nor p wins cleanly, or shape fitness strongly
//      prefers another bulk form (hex / tet / native-poly VEM), emit a shape
//      vote. Aggregate votes become a global mesher tendency for the next fill.
//   4. Cost. Each candidate action is scored as predicted_benefit / relative
//      DOF cost (calibrated heuristics; campaign-fitted weights land with
//      feedback-loop). Highest utility wins. Ties break in fixed order
//      h > p > shape > none, then by element index for full determinism
//      (seed only reorders equal-utility shape votes, not the primary pick).
//
// The driver does not hardcode benchmark answers: callers supply synthetic or
// measured indicators. Product path uses ZZ η + geometry sizing attributes;
// hierarchical surplus may be estimated from η ranking when modal surpluses
// are not yet available.

#include "adapt/loop.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace polymesh::adapt {

/// Which knob the driver turns for one element (or none).
enum class HpAction : std::uint8_t {
    kNone = 0,
    kHRefine = 1,
    kPRaise = 2,
    kShapeChange = 3,
};

/// Preferred bulk / transition form for the next mesh pass.
enum class ShapeTendency : std::uint8_t {
    kKeep = 0,       // leave current mesher dial alone
    kPreferHex = 1,  // FE hex bulk (cheap DOF / accuracy)
    kPreferTet = 2,  // tet fill for awkward / graded regions
    kPreferPoly = 3, // native-poly VEM transitions (unsplit cells)
};

/// Per-element inputs. Lengths in metres, κ in 1/m, indicators dimensionless.
struct ElementHpSignal {
    double h = 0.0;          // local edge length, m
    double kappa = 0.0;      // mean |curvature|, 1/m (0 = flat)
    double thickness = 0.0;  // local wall thickness, m; ≤0 or +inf ⇒ not thin-wall
    double eta = 0.0;        // ZZ (or energy) indicator ≥ 0
    double surplus = 0.0;    // hierarchical p→p+1 surplus ≥ 0 (or estimate)
    int p = 1;               // current polynomial order
    int p_max = 4;           // order cap for this element shape
    /// Shape fitness in [0, 1]; higher = better match. Relative differences drive votes.
    double hex_fit = 0.5;
    double tet_fit = 0.5;
    double poly_fit = 0.5;
};

/// Calibrated thresholds and cost weights (v1 heuristics; campaign override later).
struct HpDriverPolicy {
    /// Turning-angle threshold (degrees): refine h when h·κ > θ_rad.
    double turn_angle_deg = 15.0;
    /// Thin-wall: refine h when h > fraction · local thickness.
    double thin_wall_fraction = 0.35;
    /// Minimum η / max(η) to consider any action (ignore noise floor).
    double eta_rel_floor = 1e-4;
    /// surplus / max(η, eps) above this ⇒ field looks smooth enough for p.
    double smooth_surplus_ratio = 0.45;
    /// Geometry severity (dimensionless, see decide_element) above this forces h.
    double geometry_force_h = 1.0;
    /// Absolute utility floor; below this → kNone.
    double min_utility = 1e-12;
    double h_refine_factor = 0.75; // next local / uniform h multiplier
    double h_min = 0.0;           // metres; 0 = no floor
    int p_max_default = 4;
    /// Relative DOF / solve-time costs (dimensionless). Lower cost ⇒ higher utility.
    double cost_h = 8.0;     // 3-D local octree-ish refinement
    double cost_p = 2.5;     // one order raise (entity modes; +0.4·p applied)
    double cost_shape = 3.5; // remesh / formulation switch
    /// Dörfler θ used when building the h-mark set from h-candidates.
    double dorfler_theta = 0.3;
    /// Deterministic seed (currently reserved for future stochastic tie noise;
    /// primary decisions do not depend on it).
    std::uint64_t seed = 0;
};

/// Per-element outcome.
struct ElementHpDecision {
    HpAction action = HpAction::kNone;
    int p_next = 1;
    double h_next = 0.0;
    ShapeTendency shape = ShapeTendency::kKeep;
    double utility_h = 0.0;
    double utility_p = 0.0;
    double utility_shape = 0.0;
    /// Short machine-stable reason tag (no free-form prose).
    const char* reason = "none";
};

/// Aggregate plan for one adapt pass.
struct HpDriverPlan {
    std::vector<ElementHpDecision> decisions;
    std::vector<std::size_t> h_mark;     // elements marked for h-refine
    std::vector<std::size_t> p_mark;     // elements marked for p-raise
    std::vector<std::size_t> shape_mark; // elements voting shape change
    ShapeTendency global_shape = ShapeTendency::kKeep;
    /// Uniform / seeded h suggestion built from h-mark centroids (may be empty seeds).
    AdaptSuggestion h_suggestion{};
    /// Predicted relative DOF cost of applying the plan (1 = no change).
    double predicted_dof_factor = 1.0;
    std::size_t n_h = 0;
    std::size_t n_p = 0;
    std::size_t n_shape = 0;
    std::size_t n_none = 0;
};

/// Decide one element. Pure function of signal + policy (+ optional max_eta for floor).
ElementHpDecision decide_element(const ElementHpSignal& s, const HpDriverPolicy& policy,
                                 double max_eta);

/// Full mesh plan. `signals.size()` must match `centroids.size()` when centroids
/// are provided (used for h_suggestion seeds). Empty centroids ⇒ no seeds.
HpDriverPlan drive_hp(std::span<const ElementHpSignal> signals, const HpDriverPolicy& policy,
                      std::span<const Eigen::Vector3d> centroids = {},
                      double h_uniform = 0.0);

/// Estimate hierarchical surplus from ZZ η ranking when modal surpluses are absent.
/// High-η Dörfler elements get a small surplus (non-smooth); complement gets
/// surplus ≈ η (smooth interior candidates). Total η² = 0 ⇒ all surplus = 0.
std::vector<double> estimate_surplus_from_zz(const std::vector<double>& element_eta,
                                             double dorfler_theta = 0.3);

/// Build per-element signals from parallel arrays. Lengths must match (or be 1
/// for scalar broadcast of h / p). `thickness[i] <= 0` means “not a thin wall”.
/// Missing fit arrays (empty) default to 0.5. Missing surplus → estimate from η.
std::vector<ElementHpSignal> make_hp_signals(std::span<const double> h,
                                             std::span<const double> kappa,
                                             std::span<const double> thickness,
                                             std::span<const double> eta_zz,
                                             std::span<const double> surplus,
                                             std::span<const int> p_orders,
                                             std::span<const double> hex_fit = {},
                                             std::span<const double> tet_fit = {},
                                             std::span<const double> poly_fit = {},
                                             const HpDriverPolicy& policy = {});

/// Human-readable one-line summary for mesh notes / CLI.
std::string summarize_hp_plan(const HpDriverPlan& plan);

/// Map global tendency to a short tag (for mesher notes).
const char* shape_tendency_tag(ShapeTendency t);

} // namespace polymesh::adapt
