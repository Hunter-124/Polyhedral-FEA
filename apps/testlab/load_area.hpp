// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Load-area policy for polymesh_testlab, header-only so unit tests can exercise
// it without linking the campaign runner.
//
// `cad_rule_area` is the area obtained by applying the case's OWN selection rule
// (load box + |n.t_hat| > normal_min_dot) to the exact CAD tessellation: the
// mesh-independent continuum limit of that rule. It is
//   1. the load target: the traction is rescaled by
//      cad_rule_area / mesh_selected_area, so the applied RESULTANT is correct
//      on any mesh; and
//   2. the basis of the reported mesh fidelity deficit.
//
// After the rescale a deficit means a coarse traction DISTRIBUTION, not a wrong
// force, so it is reported and not a health failure (the campaign probes are
// predominantly far-field; Saint-Venant). An area that cannot be established is
// `unverified` with an EMPTY rel_err -- never 0.0, which reads as a pass.

#include <Eigen/Core>

#include <cmath>
#include <optional>
#include <string_view>

namespace polymesh::testlab {

/// Relative area tolerance, applied only where a deviation means the applied
/// resultant is genuinely wrong (i.e. nothing could be rescaled onto).
inline constexpr double kLoadAreaTol = 0.05;

/// Does the case's own load rule keep a face with this outward normal? THE ONE
/// definition of the normal test, shared by the mesh selector (select_load_faces)
/// and the CAD-tessellation rule area (with_exact_cad_selections) so they cannot
/// drift apart.
///
/// `normal_min_dot <= -1` means "no normal filter, load every in-box face", and a
/// null traction has no direction to filter on, so both keep everything. Otherwise
/// keep faces whose unit normal satisfies |n.t_hat| > normal_min_dot; the absolute
/// value tolerates inverted winding on mixed/hex skins.
///
/// Set-level behaviour is NOT part of this predicate: a filter that selects nothing
/// falls back to the whole in-box set, which each caller applies to its own
/// collection.
inline bool load_rule_keeps_normal(double normal_min_dot, const Eigen::Vector3d& traction,
                                   const Eigen::Vector3d& normal) {
    const double traction_norm = traction.norm();
    if (!(normal_min_dot > -1.0) || !(traction_norm > 1e-30)) {
        return true;
    }
    const double normal_norm = normal.norm();
    if (!(normal_norm > 0.0)) {
        return false; // degenerate face: no usable normal
    }
    return std::abs((normal / normal_norm).dot(traction / traction_norm)) > normal_min_dot;
}

/// True when the case's rule actually filters, i.e. when the fallback-to-box-only
/// branch is reachable for it. Kept beside the predicate so callers agree on this
/// too rather than each re-deriving `normal_min_dot > -1.0 && |t| > 0`.
inline bool load_rule_filters(double normal_min_dot, const Eigen::Vector3d& traction) {
    return normal_min_dot > -1.0 && traction.norm() > 1e-30;
}

/// Drift tolerance between an authored `expected_area` and `cad_rule_area`. Both
/// describe the SAME loaded region by construction, so the threshold sits orders
/// of magnitude above their observed agreement: it fires only on real drift.
inline constexpr double kAuthoredAreaTol = 0.01;

/// Mesh-INDEPENDENT cross-check of the case definition against the CAD. It keeps
/// the authored `expected_area` guard alive once the traction is rescaled onto
/// `cad_rule_area`. A disagreement is a case-definition or geometry bug, NOT mesh
/// quality, so it is reported separately from the mesh deficit.
struct AuthoredAreaCheck {
    /// True only when both an authored and a CAD-rule area were available.
    bool checked = false;
    /// |authored - cad_rule| / cad_rule. EMPTY when the check did not run.
    std::optional<double> rel_diff;
    /// False only when `checked` and the drift exceeds kAuthoredAreaTol.
    bool consistent = true;
};

inline AuthoredAreaCheck check_authored_area(std::optional<double> expected_area,
                                             std::optional<double> cad_rule_area) {
    AuthoredAreaCheck out;
    if (!expected_area || !(*expected_area > 0.0) || !cad_rule_area ||
        !(*cad_rule_area > 0.0)) {
        return out; // not checked; EMPTY rel_diff, and NOT reported as agreement
    }
    out.checked = true;
    out.rel_diff = std::abs(*expected_area - *cad_rule_area) / *cad_rule_area;
    out.consistent = *out.rel_diff <= kAuthoredAreaTol;
    return out;
}

/// How much the load-area check was able to establish. A genuine pass, a
/// rescaled-but-coarse run, and an unknown are three different things.
enum class LoadAreaStatus {
    /// No expected area of any kind could be established. rel_err is EMPTY.
    /// Not a pass and not a failure: unknown, and visibly so.
    kUnverified,
    /// An authored expected_area was compared against the mesh and there was no
    /// CAD-rule area to rescale onto, so a deviation IS a wrong resultant. The
    /// tolerance applies and this is the one status that can fail health.
    kVerified,
    /// A CAD-rule area was established, the traction was rescaled onto it, and
    /// the applied resultant is therefore correct by construction. rel_err is the
    /// measured mesh fidelity deficit, reported and not gated.
    kRescaledToExactCad,
};

constexpr std::string_view load_area_status_name(LoadAreaStatus status) {
    switch (status) {
    case LoadAreaStatus::kVerified:
        return "verified";
    case LoadAreaStatus::kRescaledToExactCad:
        return "rescaled_to_exact_cad";
    case LoadAreaStatus::kUnverified:
        break;
    }
    return "unverified";
}

struct LoadAreaAssessment {
    LoadAreaStatus status = LoadAreaStatus::kUnverified;
    /// EMPTY when nothing could be established. Never 0.0 as a stand-in.
    std::optional<double> rel_err;
    /// Health contribution: false ONLY for kVerified outside tolerance.
    bool ok = true;
};

/// Assess the loaded area of one load region.
///
/// `mesh_selected_area` MUST be the area the mesh actually selected, never a
/// value substituted from the CAD: the rescale path sets the reported area to the
/// CAD-rule area, so comparing that against the CAD would be vacuous and always
/// report zero error.
///
/// `cad_rule_area` takes precedence over `expected_area` because it is what the
/// traction was rescaled onto, so it is the basis on which the recorded deficit
/// is meaningful.
inline LoadAreaAssessment assess_load_area(std::optional<double> expected_area,
                                           std::optional<double> cad_rule_area,
                                           double mesh_selected_area) {
    LoadAreaAssessment out;
    if (!std::isfinite(mesh_selected_area) || mesh_selected_area <= 0.0) {
        return out; // nothing measurable; stays kUnverified with an EMPTY rel_err
    }
    if (cad_rule_area && *cad_rule_area > 0.0) {
        out.status = LoadAreaStatus::kRescaledToExactCad;
        out.rel_err = std::abs(mesh_selected_area - *cad_rule_area) / *cad_rule_area;
        out.ok = true; // resultant corrected by the rescale; deficit is distribution
        return out;
    }
    if (expected_area && *expected_area > 0.0) {
        out.status = LoadAreaStatus::kVerified;
        out.rel_err = std::abs(mesh_selected_area - *expected_area) / *expected_area;
        out.ok = *out.rel_err <= kLoadAreaTol;
        return out;
    }
    return out; // kUnverified, EMPTY rel_err
}

} // namespace polymesh::testlab
