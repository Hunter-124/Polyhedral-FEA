// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: coarse bulk + fine skin/feature/seed bands (2:1 Kuhn).
//
// Coarse-primary lattice at target spacing h (same cost class as tet/hybrid
// classify). Free-surface skin, sharp-feature bands, and a posteriori seeds
// are refined by local 2×2×2 subdivision → Kuhn tets at ~h/2; deep bulk emits
// one Kuhn split per coarse cell at ~h. Grid-based, not Delaunay (ADR-0015).

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
    std::size_t n_fine_cells = 0; // count of h/2 Kuhn cubes (≤ 8 × refined coarse)
    int skin_layers = 0;
    /// Coarse→fine subdivision factor (always 2 for 2:1 Kuhn).
    int subdivision = 2;
    /// Coarse cells forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse cells forced fine by a posteriori / geometry seeds.
    std::size_t n_seed_cells = 0;
};

/// `skin_layers` ≥ 1 (coarse-cell hops from free surface). Fine ≈ h/2 near
/// free surface / feature / seed bands; coarse bulk ≈ h.
/// (`feature_band`, `seed_band` in metres; 0 disables).
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
