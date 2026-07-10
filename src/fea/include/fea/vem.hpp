// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Virtual Element Method k=1 for convex polyhedra (ADR-0003).
// Vertex DOFs only. Consistency = constant-strain B-bar via face integrals;
// stabilization vanishes on the 12-dimensional linear-displacement space
// (6 rigid-body + 6 constant-strain) so the patch test is exact.

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::fea {

/// A polyhedral cell: global node indices + oriented faces (each face is a
/// loop of *local* indices into `nodes`, outward from the cell).
struct PolyCell {
    std::vector<std::uint32_t> nodes;
    std::vector<std::vector<std::uint32_t>> faces; // local vertex indices
};

/// VEM k=1 stiffness, size 3n×3n, N/m. Throws FeaError on non-positive volume.
Eigen::MatrixXd vem_poly_stiffness(const std::vector<Eigen::Vector3d>& coords,
                                   const std::vector<std::vector<std::uint32_t>>& faces,
                                   const Material& material);

/// Same, from mesh coordinates via PolyCell.
Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material);

/// Convert a hex8 element to a PolyCell (6 quad faces, outward normals).
PolyCell hex8_as_poly(const NodalElement& hex);

/// Convert a tet4 element to a PolyCell (4 tri faces).
PolyCell tet4_as_poly(const NodalElement& tet);

/// Volume of a closed polyhedron (divergence theorem), m³.
double poly_volume(const std::vector<Eigen::Vector3d>& coords,
                   const std::vector<std::vector<std::uint32_t>>& faces);

} // namespace polymesh::fea
