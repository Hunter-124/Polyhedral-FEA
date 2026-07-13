// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Thin Lloyd restricted-CVT loop (G2 / ADR-0024 Q10 trap #4, ADR-0025).
// Density ρ(x) = 1 / h(x)³ must use the **same** size field as N_pred.
// Clipped cells via Geogram ConvexCell (mesh::clip / voronoi helpers).
// Constrained (fixed) sites stay put — G3 adds OCC projection for free wall
// sites; this node only provides the free/fixed Lloyd iteration.

#include "mesh/geogram_clip.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// Site for restricted CVT. `fixed` sites are never moved (sharp protectors).
struct CvtSite {
    Eigen::Vector3d pos{0, 0, 0};
    bool fixed = false;
};

/// Local size field h(x) > 0, metres. Density is ρ = 1/h³.
/// **Must** match the h field used for N_pred (ADR-0024 Q10 #4).
using SizeFieldFn = std::function<double(const Eigen::Vector3d&)>;

/// Density ρ = 1/h(x)³ with a hard floor on h to avoid blow-ups.
[[nodiscard]] inline double density_from_size(double h, double h_floor = 1e-12) {
    const double hh = (h > h_floor) ? h : h_floor;
    const double inv = 1.0 / hh;
    return inv * inv * inv;
}

struct CvtLloydParams {
    int max_iters = 32;
    /// Stop when max free-site move ≤ move_tol_rel × domain diagonal.
    double move_tol_rel = 1e-4;
    /// Optional size field. Nullopt / empty → uniform ρ = 1 (geometric CVT).
    /// When set, ρ = 1/h³ at tet-fan sample points (same h as N_pred).
    SizeFieldFn size_at;
    /// Minimum h used in density (m).
    double h_floor = 1e-12;
};

struct CvtLloydStats {
    int n_iters = 0;
    double max_move = 0.0;       // metres, last iteration
    double domain_diag = 0.0;    // metres
    std::size_t n_sites = 0;
    std::size_t n_fixed = 0;
    std::size_t n_free = 0;
    /// Sum of clipped cell volumes (should ≈ domain volume when covering).
    double sum_cell_volume = 0.0;
    bool converged = false;
    bool geogram_ok = false;
};

struct CvtLloydResult {
    std::vector<Eigen::Vector3d> positions;
    CvtLloydStats stats;
};

/// Voronoi cell of `site` inside `box`, clipped by bisectors to `others`.
/// Empty optional if geogram off or the cell is empty.
[[nodiscard]] std::optional<ClippedCell> restricted_voronoi_cell(
    const ClipBox& box, const Eigen::Vector3d& site,
    std::span<const Eigen::Vector3d> others);

/// Density-weighted centroid of the restricted Voronoi cell of `site`.
/// If `size_at` is empty, returns geometric barycenter. Nullopt if empty cell.
[[nodiscard]] std::optional<Eigen::Vector3d> restricted_voronoi_centroid(
    const ClipBox& box, const Eigen::Vector3d& site,
    std::span<const Eigen::Vector3d> others, const SizeFieldFn& size_at,
    double h_floor = 1e-12);

/// Lloyd iteration: free sites ← density-weighted restricted Voronoi centroids.
/// Fixed sites are copied through unchanged. Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] CvtLloydResult lloyd_cvt(const ClipBox& domain,
                                       std::span<const CvtSite> sites,
                                       const CvtLloydParams& params = {});

/// Seed a regular-ish lattice of free sites inside the box (diagnostic / tests).
[[nodiscard]] std::vector<CvtSite> seed_lattice_sites(const ClipBox& box,
                                                      int n_side);

}  // namespace polymesh::mesh
