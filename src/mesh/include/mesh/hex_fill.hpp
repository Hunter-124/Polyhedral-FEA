// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Deterministic hex8 interior fill of a closed surface (uniform Cartesian grid).
// Companion to tet_fill; useful for sweepable/boxy regions and hybrid co-design.
// Optional limited surface snap (≤0.75 h) with Jacobian safety — not Delaunay.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::mesh {

struct HexFillOutput {
    std::vector<Eigen::Vector3d> nodes; // metres
    std::vector<std::array<std::uint32_t, 8>> hexes;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0; // grid spacing used, metres
    double boundary_max_distance = 0.0; // metres; set when snap runs
};

/// Uniform Cartesian hex8 fill. `h` and bbox corners in metres.
/// @param snap_boundary Pull free-boundary nodes toward the STL (Jacobian-safe).
HexFillOutput hex_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h,
                               bool snap_boundary = true);

} // namespace polymesh::mesh
