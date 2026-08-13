// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// The load-area policy for polymesh_testlab, kept header-only so unit tests can
// exercise it without linking the campaign runner.
//
// WHY THIS EXISTS. The original check was worse than no check. It compared the
// selected load-face area against an expected area ONLY when the case supplied
// select.expected_area, and otherwise left load_area_rel_err at its 0.0 default
// with load_area_ok true. A zero relative error reads as a perfect match, so
// meshes missing 14-66% of their loaded face recorded a flawless area check and
// passed the health gate. Because a case traction is a pressure, those meshes
// applied a proportionally smaller force: they solved the WRONG problem while
// being labelled healthy. Exactly the two families with curved loaded surfaces
// (sphere_box, stepped_shaft) are the ones that omit expected_area, so the
// blindness landed precisely where it did the most damage.
//
// WHAT REPLACED IT. `cad_rule_area` is the area of the loaded region computed by
// applying the case's OWN selection rule (load box + |n.t_hat| > normal_min_dot)
// to the exact CAD tessellation rather than to the candidate mesh. It is the
// continuum limit of the same rule, so it is mesh-independent and directly
// comparable with what the mesh managed to select. It is used twice:
//
//   1. As the load target. The traction is rescaled by
//      cad_rule_area / mesh_selected_area so the applied RESULTANT is correct by
//      construction on any mesh. This generalises the pre-existing exact-CAD
//      fallback from "the selection came back empty" to "the selected area
//      deviates", reusing machinery that was already there.
//   2. As the reported fidelity measure. The residual deficit is recorded on
//      every row so analysis can filter on it.
//
// WHY A DEFICIT IS NOT A HEALTH FAILURE. Once the resultant is rescaled, a large
// deficit no longer means the run applied the wrong force; it means the traction
// DISTRIBUTION is coarse. The campaign probes are predominantly far-field (tip
// deflection, strain energy, and an SCF at a hole remote from the load), where
// Saint-Venant makes a corrected resultant with an imperfect distribution a
// legitimate measurement. So those rows stay usable and stay flagged. Fixing the
// distribution needs more facets on curved loaded surfaces -- a sizing policy,
// deliberately not folded in here.
//
// An area that cannot be established at all is reported as `unverified` with an
// EMPTY rel_err. Never 0.0, which reads as a pass; never treated as verified.

#include <Eigen/Core>

#include <cmath>
#include <optional>
#include <string_view>

namespace polymesh::testlab {

/// Relative area tolerance, applied only where a deviation means the applied
/// resultant is genuinely wrong (i.e. nothing could be rescaled onto).
inline constexpr double kLoadAreaTol = 0.05;

/// Does the case's own load rule keep a face with this outward normal?
///
/// THE ONE definition of the normal test, called by both sides that need it: the
/// mesh selector in select_load_faces and the CAD-tessellation rule area in
/// with_exact_cad_selections. They used to implement it separately and drifted --
/// the CAD side silently substituted the selector slab's thin axis at min_dot 0.7
/// whenever a case asked for `normal_min_dot = -1`, so it measured a 0.7-filtered
/// cap while the mesh loaded every in-box face. cad_rule_area then rescaled the
/// traction onto a region the case never asked for: on sphere_box_s2_c1 the two
/// sides disagreed by 130.7% and the resultant was scaled down 2.3x. One function
/// makes that class of divergence structurally impossible.
///
/// `normal_min_dot <= -1` means "no normal filter, load every in-box face", and a
/// null traction has no direction to filter on, so both keep everything. Otherwise
/// keep faces whose unit normal satisfies |n.t_hat| > normal_min_dot; the absolute
/// value tolerates inverted winding on mixed/hex skins.
///
/// Set-level behaviour is NOT part of this predicate: a filter that selects nothing
/// falls back to the whole in-box set, which each caller applies to its own
/// collection.
inline bool load_rule_keeps_normal(double normal_min_dot,
                                   const Eigen::Vector3d& traction,
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
/// describe the SAME loaded region by construction -- one hand-authored from the
/// generator's formula, one measured from the CAD by the case's own rule -- and
/// across the 48 corpus cases that author a value they agree to 4.6e-10. This
/// threshold is therefore seven orders of magnitude looser than observed
/// agreement: it fires only on real drift, never on numerical noise.
inline constexpr double kAuthoredAreaTol = 0.01;

/// Mesh-INDEPENDENT cross-check of the case definition against the CAD.
///
/// Rescaling the traction onto `cad_rule_area` would otherwise quietly retire the
/// authored `expected_area` guard, since the authored value stops being consulted
/// once a rule area exists. Comparing the two keeps that guard alive in the only
/// form that still means something: a disagreement here is a case-definition or
/// geometry bug (the STEP changed, or the authored formula is wrong), NOT a mesh
/// quality problem. It is reported separately from the mesh deficit so the two are
/// never conflated -- they have different causes and different remedies.
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
