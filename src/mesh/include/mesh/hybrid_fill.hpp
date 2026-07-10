// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: coarse bulk + refined skin/feature/seed bands (ADR-0018).
//
// Coarse-primary Kuhn lattice at target spacing h (same cost class as
// tet/hybrid classify). Free-surface skin, sharp-feature bands, and a
// posteriori seeds mark cells for LEB refinement (ADR-0016) so the mesh stays
// face-conforming — no 2:1 hanging mid-edge nodes. Grid-based, not Delaunay
// (ADR-0015).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <span>

namespace polymesh::mesh {

struct GradedTetFillOutput {
    TetFillOutput mesh;    // nodes + tets + boundary quads
    double h_coarse = 0.0; // metres (~ target h when budget allows)
    double h_fine = 0.0;   // metres (~ h_coarse/2 on refined cells)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0; // coarse cells marked for LEB refinement
    int skin_layers = 0;
    /// LEB refine passes on fine-marked cells (always 2 ≈ half-edge target).
    int subdivision = 2;
    /// Coarse cells forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse cells forced fine by a posteriori / geometry seeds.
    std::size_t n_seed_cells = 0;
};

/// `skin_layers` ≥ 1 (coarse-cell hops from free surface). Marked cells are
/// LEB-refined `subdivision` times (≈ half-edge); bulk stays at ~h.
/// (`feature_band`, `seed_band` in metres; 0 disables).
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
