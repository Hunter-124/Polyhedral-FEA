// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Exterior-face extraction from a NodalMesh for mesh preview / VTU skin.
// Faces used by exactly one element are exterior. Triangles are stored as
// degenerate quads (v0,v1,v2,v2) so existing boundary_quads renderers work.

#include "fea/nodal_mesh.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace polymesh::fea {

/// Collect free surface faces of `mesh`.
/// Quads: (a,b,c,d). Triangles: (a,b,c,c). Empty if mesh has no solid elements.
std::vector<std::array<std::uint32_t, 4>> extract_boundary_faces(const NodalMesh& mesh);

/// Collect free surface faces of `mesh` as polygon loops (global node ids),
/// keeping polyhedral faces as true polygons instead of triangulating them.
/// For preview rendering of poly meshes: draw each polygon as one facet so the
/// wireframe shows real cell faces (not fan-triangulation diagonals). FEM
/// element faces come back as their 3- or 4-node loops. Empty if no solids.
std::vector<std::vector<std::uint32_t>> extract_boundary_polys(const NodalMesh& mesh);

} // namespace polymesh::fea
