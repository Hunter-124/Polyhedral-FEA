// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Convert face-based PolyMesh (G4 clipped Voronoi) → fea::NodalMesh of
// kPolyVem elements for the product VEM path (M5).

#include "fea/nodal_mesh.hpp"
#include "mesh/poly_mesh.hpp"

namespace polymesh::fea {

/// Build a NodalMesh where every PolyMesh cell becomes ElementType::kPolyVem.
/// Face loops are remapped to local node indices per element. Vertices are
/// shared globally. Empty mesh if `pm` has no cells.
[[nodiscard]] NodalMesh poly_mesh_to_vem(const mesh::PolyMesh& pm);

}  // namespace polymesh::fea
