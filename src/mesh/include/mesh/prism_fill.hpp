// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Deterministic prism6 fill of a closed triangle surface (ROADMAP C3).
// Cartesian lattice over the AABB; each inside voxel is split into two
// wedges (right-triangle bases extruded along the dominant axis). Stair-cased
// boundary like hex/tet fills — NOT CAD extrusion detection / sweep meshing.
// See ADR-0015. Real prism6 connectivity for the FE zoo.
//
// Lives in mesh/ (not fea/) so library deps stay acyclic: mesh → geom only.

#include "geom/tri_surface.hpp"
#include "mesh/poly_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::mesh {

struct PrismFillOutput {
    std::vector<Eigen::Vector3d> nodes; // metres
    /// Each prism: six node indices, bottom tri then top tri (fea prism6 order).
    std::vector<std::array<std::uint32_t, 6>> prisms;
    /// Outer quads of the voxel lattice (region mapping / rendering).
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0;     // grid spacing used, metres
    int sweep_axis = 2; // 0=x, 1=y, 2=z — extrusion direction
};

/// Uniform Cartesian prism6 fill. `h` and bbox corners in metres.
/// Sweep axis = longest bbox extent (ties prefer z, then y, then x).
PrismFillOutput prism_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h);

/// Signed prism volume, m³ (positive for right-handed bottom tri + extrusion).
double prism_signed_volume(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                           const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                           const Eigen::Vector3d& p4, const Eigen::Vector3d& p5);

/// Positive volumes and finite coordinates for every prism in `out`.
/// @param min_volume Lower bound on |V|, m³ (0 = any positive).
void check_prism_fill_geometry(const PrismFillOutput& out, double min_volume = 0.0);

} // namespace polymesh::mesh
