// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Shared Cartesian lattice + solid-angle-free ray-parity classification for
// product grid fills (tet/hex/graded/transition/prism). Fixes the classic
// shared-edge double-count that punched diagonal tunnels through AABB boxes
// (and any solid whose face diagonals align with cell centres).

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <vector>

namespace polymesh::mesh {

struct CartesianGrid {
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector3d cell = Eigen::Vector3d::Ones(); // dx, dy, dz (may be anisotropic)
    int nx = 0;
    int ny = 0;
    int nz = 0;

    [[nodiscard]] Eigen::Vector3d node(int i, int j, int k) const {
        return {origin[0] + static_cast<double>(i) * cell[0],
                origin[1] + static_cast<double>(j) * cell[1],
                origin[2] + static_cast<double>(k) * cell[2]};
    }

    [[nodiscard]] Eigen::Vector3d cell_center(int i, int j, int k) const {
        return node(i, j, k) + 0.5 * cell;
    }

    [[nodiscard]] std::size_t index(int i, int j, int k) const {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    }

    [[nodiscard]] long cell_count() const {
        return static_cast<long>(nx) * static_cast<long>(ny) * static_cast<long>(nz);
    }

    /// Largest cell edge — used as snap budget / mesher-note h.
    [[nodiscard]] double max_edge() const { return cell.maxCoeff(); }
};

/// Lattice that exactly fills [bbox_min, bbox_max] with n = ceil(extent/h)
/// cells per axis and dx = extent/n (so faces land on AABB corners/edges).
/// Throws ValidityError if h or bbox is invalid or the grid is absurdly fine.
CartesianGrid make_bbox_grid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             double h, long max_cells = 512 * 1024);

/// Same, but round each axis up to an even count (≥ min_cells). Used by graded
/// 2:1 fine/coarse grouping.
CartesianGrid make_bbox_grid_even(const Eigen::Vector3d& bbox_min,
                                  const Eigen::Vector3d& bbox_max, double h, int min_cells = 2,
                                  long max_cells = 512 * 1024);

/// Even-odd inside test with Z-axis rays. Shared triangle edges (same z within
/// eps) count once so coplanar face diagonals do not flip parity.
std::vector<bool> classify_cells_inside(const geom::TriSurface& surface,
                                        const CartesianGrid& grid);

/// Even-odd with rays along axis 0/1/2 (prism sweep uses longest axis).
std::vector<bool> classify_cells_inside_axis(const geom::TriSurface& surface,
                                             const CartesianGrid& grid, int ray_axis);

} // namespace polymesh::mesh
