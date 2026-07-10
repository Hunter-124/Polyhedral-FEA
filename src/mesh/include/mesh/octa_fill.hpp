// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Experimental BCC octahedral lattice fill (ADR-0019 / Track H O1).
//
// For every face-adjacent pair of interior Cartesian cells, emit one
// octahedron (two cell centres + four face corners) split into 4 tet4.
// Boundary cells also get pyramids (half-octa) on free faces → 2 tets each.
// Not a production mesher — experimental comparison only.

#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

namespace polymesh::mesh {

struct OctaFillOutput {
    TetFillOutput mesh; // nodes + tets + boundary quads
    std::size_t n_octahedra = 0;
    std::size_t n_boundary_pyramids = 0;
    double h = 0.0;
};

/// Experimental BCC octa → tet4 fill. Throws ValidityError on empty volume.
OctaFillOutput octa_fill_surface(const geom::TriSurface& surface,
                                 const Eigen::Vector3d& bbox_min,
                                 const Eigen::Vector3d& bbox_max, double h,
                                 bool snap_boundary = true);

} // namespace polymesh::mesh
