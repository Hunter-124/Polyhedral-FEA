// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// O(seeds · ball) / O(feature samples · ball) cell marking for Cartesian fills.
// Shared by graded tet and hybrid zoo (H0). Avoids O(cells · seeds) scans.

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/grid_classify.hpp"

#include <Eigen/Core>

#include <span>
#include <vector>

namespace polymesh::mesh {

/// Stamp seed balls onto coarse cells — O(seeds · ball volume), not O(cells · seeds).
void stamp_seed_cells(std::vector<char>& is_marked, std::vector<char>* is_seed_out, int nx,
                      int ny, int nz, const CartesianGrid& grid,
                      std::span<const Eigen::Vector3d> seeds, double seed_band);

/// Stamp sharp-feature polylines as balls along edges.
void stamp_feature_cells(std::vector<char>& is_marked, std::vector<char>* is_feature_out,
                         int nx, int ny, int nz, const CartesianGrid& grid,
                         const geom::TriSurface& surface,
                         std::span<const geom::SharpEdge> features, double feature_band);

/// Angle-adaptive curvature marking (per-cell, deterministic — no seed caps).
/// Samples every surface triangle at ~half-cell spacing; each sample carries
/// κ ≈ 2|H| interpolated from vertex mean curvature (max-principal proxy,
/// exact for cylinders). A cell turns θ_cell = h·κ across one bulk cell:
///   θ_cell > max_turn_rad          → mark L1 (needs h/2)
///   θ_cell > 2·max_turn_rad        → also mark L2 when is_l2 non-null (h/4)
/// Flat regions (κ→0) never mark, gentle curves stay coarse, and coverage is
/// contiguous along the curved wall — unlike capped seed balls, which leave
/// coarse rings mid-bore and stray fine islands on flats.
void stamp_curvature_cells(std::vector<char>& is_l1, std::vector<char>* is_l2,
                           std::vector<char>* is_tag_out, int nx, int ny, int nz,
                           const CartesianGrid& grid, const geom::TriSurface& surface,
                           double max_turn_rad);

} // namespace polymesh::mesh
