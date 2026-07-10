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

} // namespace polymesh::fea
