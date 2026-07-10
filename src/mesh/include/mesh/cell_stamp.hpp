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

} // namespace polymesh::mesh
