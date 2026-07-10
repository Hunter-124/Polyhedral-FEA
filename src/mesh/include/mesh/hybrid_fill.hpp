// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: boundary skin at h/2, deep interior at h (conforming).
//
// Cartesian lattice at spacing h; free-surface cells (and `skin_layers` of
// neighbors) plus an optional band around sharp features are refined into
// 2×2×2 Kuhn tets at h/2; deeper cells use one Kuhn split at h.

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <span>

namespace polymesh::mesh {

struct GradedTetFillOutput {
    TetFillOutput mesh;    // nodes + tets + boundary quads at the fine lattice
    double h_coarse = 0.0; // metres
    double h_fine = 0.0;   // metres (typically h_coarse/2)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0;
    int skin_layers = 0;
    /// Coarse blocks forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse blocks forced fine by a posteriori error seeds.
    std::size_t n_seed_cells = 0;
};

/// `skin_layers` ≥ 1. Coarse spacing is `h` (metres); fine is `h/2` in the skin.
/// Optional sharp-edge band and/or error-seed balls also force fine blocks
/// (`feature_band`, `seed_band` in metres; 0 disables).
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0);

} // namespace polymesh::mesh
