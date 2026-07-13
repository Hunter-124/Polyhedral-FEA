// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Wall tangential smooth + live BRep re-project (ADR-0024 Q2a / M10).
// Shared post-pass for varyhedron packing and future constrained CVT.

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// Result of a Jacobian-safe wall free-slide + OCC re-project pass.
struct WallProjectStats {
    std::size_t n_wall_nodes = 0;  // free-surface nodes far from sharp edges
    std::size_t n_moved = 0;       // accepted moves in the final state
    std::size_t n_reverted = 0;    // nodes reverted by tet volume guard
    int n_iters = 0;
    /// Mean |p − project(p)| over wall nodes after the pass (metres).
    double mean_surface_residual = 0.0;
    double max_surface_residual = 0.0;
};

/// Free-slide wall nodes on smooth faces: mild Laplacian on the free surface,
/// project displacement into the local tangent plane (OCC normal), re-project
/// onto the BRep via `geom::project_point_on_surface`, then revert any move
/// that flips a tet volume (same Jacobian safety as sharp edge snap).
///
/// Wall nodes = free-boundary nodes whose distance to the nearest **sharp**
/// CAD edge exceeds `sharp_guard_frac * h` (~0.4 h). Sharp / near-sharp nodes
/// are left alone so protected features stay put.
///
/// No-op (returns zeros) when `cad` is empty, OCC is unavailable, `iters` ≤ 0,
/// or there are no free-boundary wall nodes. STL-only callers simply omit this.
[[nodiscard]] WallProjectStats wall_tangential_project(
    const geom::CadModel& cad, const geom::CadTopology* topo,
    std::vector<Eigen::Vector3d>& nodes,
    std::span<const std::array<std::uint32_t, 4>> tets, double h, int iters = 4,
    double relax = 0.45, double sharp_guard_frac = 0.4);

} // namespace polymesh::mesh
