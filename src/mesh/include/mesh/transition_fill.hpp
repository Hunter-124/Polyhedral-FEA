// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Hex core + pyramid skin (ADR-0013).
//
// Algorithm:
//   - Interior lattice cells (all 6 face-neighbors inside) → hex8
//   - Boundary lattice cells → six pyramid5 (apex = cell center, bases = faces)
// Interior hex–pyramid faces are conforming quads. Optional limited surface
// snap pulls boundary lattice nodes toward the STL without collapsing cells.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::mesh {

enum class TransitionCellKind : std::uint8_t { kHex8 = 0, kPyramid5 = 1 };

struct TransitionCell {
    TransitionCellKind kind = TransitionCellKind::kHex8;
    /// Hex: 8 nodes. Pyramid: 5 nodes (base 0..3, apex 4).
    std::array<std::uint32_t, 8> nodes{};
    std::uint8_t n_nodes = 8;
};

struct TransitionFillOutput {
    std::vector<Eigen::Vector3d> nodes;
    std::vector<TransitionCell> cells;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0;
    std::size_t n_hex = 0;
    std::size_t n_pyramid = 0;
    /// Max boundary-node distance to surface after optional snap (metres).
    double boundary_max_distance = 0.0;
};

/// Hex core + pyramid skin. If `snap_boundary`, lattice nodes on the free
/// surface are pulled toward the STL by at most 0.35 h.
TransitionFillOutput transition_fill_surface(const geom::TriSurface& surface,
                                             const Eigen::Vector3d& bbox_min,
                                             const Eigen::Vector3d& bbox_max, double h,
                                             bool snap_boundary = true);

} // namespace polymesh::mesh
