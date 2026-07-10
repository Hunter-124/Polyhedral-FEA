// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: multi-level LEB size field (ADR-0018).
//
// Coarse-primary Kuhn lattice at target spacing h. Cells marked L1 (features /
// thick free-surface skin) get one LEB pass (~h/2); L2 (high-κ seeds + feature
// core) get a second (~h/4). Thin plates skip free-surface hop flood so grading
// is feature-driven. Face-conforming via LEPP. Grid-based, not Delaunay
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
    double h_fine = 0.0;   // metres (~ h_coarse/4 at deepest L2)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0; // coarse cells marked L1 or L2
    int skin_layers = 0;
    /// Max LEB depth (2 → L2 ≈ h/4).
    int subdivision = 2;
    /// Coarse cells forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse cells forced fine by a posteriori / geometry seeds (L2).
    std::size_t n_seed_cells = 0;
};

/// Multi-level graded fill. `skin_layers` free-surface hops (skipped on thin
/// parts). Feature/seed bands drive L1/L2 LEB. Bands in metres; 0 disables.
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
