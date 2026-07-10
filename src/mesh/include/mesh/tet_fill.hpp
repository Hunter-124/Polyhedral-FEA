// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Deterministic tet4 fill of a closed triangle surface (P2 v1 mesher).
// Cartesian grid over the bbox; each inside voxel is split into 6 tets along
// the space diagonal so shared faces match. Boundary is stair-cased for now
// (surface snap is a later refinement). Fully deterministic for (surface, h).
//
// Lives in mesh/ (not fea/) so library deps stay acyclic: mesh → geom only.
// pipeline converts TetFillOutput into fea::NodalMesh for the frozen solver.

#include "geom/tri_surface.hpp"
#include "mesh/poly_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::mesh {

struct TetFillOutput {
    std::vector<Eigen::Vector3d> nodes;
    /// Each tet: four node indices, positive orientation.
    std::vector<std::array<std::uint32_t, 4>> tets;
    /// Outer quads of the voxel lattice (region mapping / rendering).
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0;
};

/// Fill the interior of `surface` (assumed closed, outward CCW) at spacing h.
/// Throws ValidityError on empty volume or absurd grid size.
TetFillOutput tet_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h,
                               bool snap_boundary = true);

double tet_signed_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                         const Eigen::Vector3d& c, const Eigen::Vector3d& d);

/// Positive volumes and finite coordinates for every tet in `out`.
void check_tet_fill_geometry(const TetFillOutput& out, double min_volume = 0.0);

} // namespace polymesh::mesh
