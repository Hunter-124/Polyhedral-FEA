// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: fine boundary skin, coarse deep interior (2:1 Kuhn blocks).
//
// Cartesian lattice at the fine spacing; free-surface cells (and `skin_layers`
// of neighbors) plus optional sharp-feature / seed bands are refined into
// Kuhn tets at h_fine; deeper cells use 2×2×2 blocks at ~2·h_fine.
//
// When features or seeds are active, the fine lattice targets ~h/4 so rounded
// edges and error seeds get much smaller elements than a uniform Cartesian
// fill (still grid-based — ADR-0015, not Delaunay).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <span>

namespace polymesh::mesh {

struct GradedTetFillOutput {
    TetFillOutput mesh;    // nodes + tets + boundary quads at the fine lattice
    double h_coarse = 0.0; // metres
    double h_fine = 0.0;   // metres (typically h_coarse/2; ~h/4 with feature grading)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0;
    int skin_layers = 0;
    /// Coarse-block grouping size on the fine lattice (2 always today).
    int subdivision = 2;
    /// Coarse blocks forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse blocks forced fine by a posteriori error seeds.
    std::size_t n_seed_cells = 0;
};

/// `skin_layers` ≥ 1. Without features/seeds: fine ≈ h/2, coarse ≈ h.
/// With feature_band or seed_band > 0: fine ≈ h/4 near those bands (and skin),
/// coarse ≈ h/2 in the bulk — smaller elements on rounded/curved edges.
/// (`feature_band`, `seed_band` in metres; 0 disables).
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
