// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Test-support structured mesh generators for the verification suite.
// These are deliberately NOT part of the product mesher (that's Phase P2+);
// they exist so Tier-0/Tier-1 tests can run on deterministic meshes without
// external files.

#include "fea/nodal_mesh.hpp"

#include <cstdint>

namespace polymesh::test_support {

/// Structured hex8 grid over the box [0,size] with nx*ny*nz cells.
fea::NodalMesh box_hex_mesh(int nx, int ny, int nz, const Eigen::Vector3d& size);

/// Structured tet4 mesh: each hex cell of the grid split into 6 tets
/// (Kuhn triangulation — conforming across cells).
fea::NodalMesh box_tet_mesh(int nx, int ny, int nz, const Eigen::Vector3d& size);

/// Promotes a tet4 mesh to tet10 / hex8 to hex20 by inserting shared
/// mid-edge nodes at edge midpoints (straight-edged quadratic elements).
fea::NodalMesh promote_to_quadratic(const fea::NodalMesh& mesh);

/// Deterministically perturbs interior corner nodes by up to `amplitude`
/// times the local spacing `h`, using a seeded LCG. Nodes on the bounding
/// box of the mesh are left untouched, so boundary planes stay flat.
/// Call BEFORE promote_to_quadratic so mid-edge nodes stay at midpoints.
void distort_interior(fea::NodalMesh& mesh, double amplitude, double h, std::uint64_t seed);

} // namespace polymesh::test_support
