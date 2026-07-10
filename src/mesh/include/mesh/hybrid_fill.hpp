// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: fine boundary skin, coarse deep interior (2:1 Kuhn blocks).
//
// Cartesian lattice at the fine spacing (~h/2); free-surface cells (and
// `skin_layers` of neighbors) plus optional sharp-feature / seed bands are
// refined into Kuhn tets at h_fine; deeper cells use 2×2×2 blocks at ~h
// (h_coarse ≈ 2·h_fine ≈ h when the cell budget allows).
//
// Always 2:1 grading (subdivision = 2). A former ultra-fine h/4 global lattice
// for features blew past RAM/time while bulk still only reached h/2 — features
// now densify *which* blocks are fine, not the whole grid. Grid-based, not
// Delaunay (ADR-0015).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <span>

namespace polymesh::mesh {

struct GradedTetFillOutput {
    TetFillOutput mesh;    // nodes + tets + boundary quads at the fine lattice
    double h_coarse = 0.0; // metres (~ target h when budget allows)
    double h_fine = 0.0;   // metres (~ h_coarse/2)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0;
    int skin_layers = 0;
    /// Coarse-block grouping size on the fine lattice (always 2 for 2:1 Kuhn).
    int subdivision = 2;
    /// Coarse blocks forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse blocks forced fine by a posteriori error seeds.
    std::size_t n_seed_cells = 0;
};

/// `skin_layers` ≥ 1. Fine ≈ h/2 near free surface / feature / seed bands;
/// coarse bulk ≈ h. (`feature_band`, `seed_band` in metres; 0 disables).
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
