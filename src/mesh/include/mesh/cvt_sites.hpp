// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Constrained CVT sites + OCC bridge (G3 / ADR-0024 Q2–Q3, ADR-0025).
// Sharp CAD edges → fixed sites; free interior/wall sites move under Lloyd
// then free wall candidates re-project via M10 project_point_on_surface.
// Seams are never fixed protectors.

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/geogram_clip.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace polymesh::mesh {

struct ConstrainedSiteSeedParams {
    /// Interior free lattice resolution along the shortest bbox axis.
    int interior_n_side = 3;
    /// Subsample every k-th sharp-edge sample (1 = all samples).
    int sharp_sample_stride = 1;
    /// Drop sharp samples closer than this fraction of domain diagonal.
    double sharp_min_sep_frac = 1e-3;
};

struct ConstrainedSiteSeedResult {
    std::vector<CvtSite> sites;
    std::size_t n_sharp_fixed = 0;
    std::size_t n_interior_free = 0;
    std::size_t n_sharp_edges_used = 0;
    std::size_t n_seams_skipped = 0;
    std::size_t n_smooth_skipped = 0;
};

/// Seed CVT sites: **sharp** CAD edge samples as `fixed`, plus free interior
/// lattice. Smooth and seam edges never become fixed protectors (ADR-0024).
[[nodiscard]] ConstrainedSiteSeedResult seed_constrained_cvt_sites(
    const ClipBox& domain, const geom::CadTopology* topo,
    const ConstrainedSiteSeedParams& params = {});

struct OccSiteProjectStats {
    std::size_t n_candidates = 0;
    std::size_t n_projected = 0;
    std::size_t n_skipped_near_sharp = 0;
    std::size_t n_failed = 0;
    double mean_residual = 0.0;
    double max_residual = 0.0;
};

/// Project free sites that lie within `wall_band` of the domain boundary onto
/// the live BRep (M10). Sites closer than `sharp_guard` to a **sharp** CAD
/// edge are left alone so fixed features stay authoritative.
/// No-op without OCC / empty cad.
[[nodiscard]] OccSiteProjectStats project_free_wall_sites(
    const geom::CadModel& cad, const geom::CadTopology* topo,
    const ClipBox& domain, std::vector<CvtSite>& sites, double wall_band,
    double sharp_guard);

struct ConstrainedLloydParams {
    CvtLloydParams lloyd;
    ConstrainedSiteSeedParams seed;
    /// After each Lloyd iter (and once at end): project free wall sites.
    bool project_each_iter = false;
    bool project_final = true;
    double wall_band_frac = 0.15;    // of domain diagonal
    double sharp_guard_frac = 0.05;  // of domain diagonal
};

struct ConstrainedLloydResult {
    std::vector<CvtSite> sites;
    CvtLloydStats lloyd_stats;
    OccSiteProjectStats project_stats;
    ConstrainedSiteSeedResult seed_stats;
};

/// Full G3 pipeline: seed (sharp fixed + interior free) → Lloyd with ρ=1/h³ →
/// optional OCC wall project of free sites.
[[nodiscard]] ConstrainedLloydResult constrained_lloyd_cvt(
    const ClipBox& domain, const geom::CadModel* cad,
    const geom::CadTopology* topo, const ConstrainedLloydParams& params = {});

}  // namespace polymesh::mesh
