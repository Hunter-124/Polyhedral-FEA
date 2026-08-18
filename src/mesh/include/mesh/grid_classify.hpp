// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Shared Cartesian lattice + solid-angle-free ray-parity classification for
// product grid fills (tet/hex/graded/transition/prism). Fixes the classic
// shared-edge double-count that punched diagonal tunnels through AABB boxes
// (and any solid whose face diagonals align with cell centres).

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <cstddef>
#include <functional>
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

/// A coarse Cartesian solid/void classification with one optional local
/// child level. `child_inside_mask[c]` stores the eight h/2 child samples for
/// coarse cell `c` (bit = a + 2*b + 4*d). A zero or 0xff mask is uniform;
/// every other mask is a surface-straddling cell that the caller must emit at
/// h/2. The fine lattice is sampling-only: `grid` always remains the requested
/// coarse lattice.
struct FeatureAwareClassification {
    CartesianGrid grid;
    std::vector<bool> inside;
    /// Requested-grid centre classification before child samples broaden mixed
    /// parents to `inside = any child`. Populated only when child sampling runs.
    std::vector<bool> coarse_inside;
    std::vector<std::uint8_t> child_inside_mask;
    std::size_t n_mixed_cells = 0;
    int refinement_levels = 0;
    double surface_volume = 0.0;
    double classified_volume = 0.0;
    double relative_volume_error = 0.0;
};

/// Default product-mesh cell budget (fine lattice for graded is ~8× denser).
inline constexpr long kDefaultMaxGridCells = 512 * 1024;

/// Minimum target \(h\) so a Cartesian lattice at spacing \(h/\mathrm{subdivision}\)
/// stays within `max_cells`. Use `subdivision=2` for graded-tet fine grids (h/2).
/// Returns a conservative isotropic estimate (metres); actual grids may still
/// auto-coarsen slightly due to ceil/even rounding.
double min_h_for_cell_budget(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             long max_cells = kDefaultMaxGridCells, int subdivision = 1);

/// Lattice that exactly fills [bbox_min, bbox_max] with n = ceil(extent/h)
/// cells per axis and dx = extent/n (so faces land on AABB corners/edges).
/// If the requested \(h\) would exceed `max_cells`, the grid is **auto-coarsened**
/// (larger effective cell size) instead of throwing — product meshers must always
/// produce a mesh. Throws only on invalid h/bbox.
CartesianGrid make_bbox_grid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             double h, long max_cells = kDefaultMaxGridCells);

/// Same, but round each axis up to an even count (≥ min_cells). Used by graded
/// 2:1 fine/coarse grouping. Auto-coarsens when over `max_cells` (keeps even n).
CartesianGrid make_bbox_grid_even(const Eigen::Vector3d& bbox_min,
                                  const Eigen::Vector3d& bbox_max, double h, int min_cells = 2,
                                  long max_cells = kDefaultMaxGridCells);

/// Even-odd inside test with Z-axis rays. Shared triangle edges (same z within
/// eps) count once so coplanar face diagonals do not flip parity.
std::vector<bool> classify_cells_inside(const geom::TriSurface& surface,
                                        const CartesianGrid& grid);

/// Sample one h/2 child level, but retain the requested coarse lattice.
/// Only parents whose eight child samples mix solid and void are locally
/// refined by callers; uniform interior and exterior parents remain coarse.
/// `max_refinement_levels == 0` returns the original centre classification.
///
/// `even_cells` rounds each axis up to an even count so every bbox mid-plane
/// falls on a lattice plane. Tet fills built on the alternating 5-tet split
/// (mesh/lattice_split.hpp) require it — their checkerboard parity only mirrors
/// about a plane when the cell count crossed by that plane is even. The mixed
/// hex/pyramid fill does not: its cells are self-mirror, and its 2:1 closure was
/// tuned on the odd-permitting lattice.
FeatureAwareClassification classify_cells_feature_aware(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h,
    long max_cells = kDefaultMaxGridCells, double relative_volume_tolerance = 0.01,
    int max_refinement_levels = 4,
    const std::function<double(const Eigen::Vector3d&)>& size_field = {},
    bool even_cells = false);

/// Even-odd with rays along axis 0/1/2 (prism sweep uses longest axis).
std::vector<bool> classify_cells_inside_axis(const geom::TriSurface& surface,
                                             const CartesianGrid& grid, int ray_axis);

} // namespace polymesh::mesh
