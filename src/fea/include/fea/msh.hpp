// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Gmsh .msh format version 2.2 (ASCII) import for the P1 reference solver.
//
// P1 freezes the baseline against externally generated meshes; this reader is
// the bridge. Only the element types in the P1 zoo (tet4/tet10, hex8/hex20)
// and their surface faces (tri3/tri6, quad4/quad8) are accepted. Physical
// group tags on surface elements are preserved so tests can select traction
// and Dirichlet boundaries by group id.
//
// Node numbering in Gmsh v2.2 for these types matches the canonical order in
// fea/nodal_mesh.hpp (verified against the Gmsh reference manual).

#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace polymesh::fea {

/// Result of loading a .msh file: volume mesh plus tagged boundary faces.
struct MshModel {
    NodalMesh mesh;
    /// Boundary faces keyed by Gmsh physical group tag (first element tag).
    std::map<int, std::vector<SurfaceFace>> physical_faces;
    /// Optional physical-group names from $PhysicalNames (tag -> name).
    std::map<int, std::string> physical_names;
};

/// Loads an ASCII Gmsh mesh format 2.2 file. Throws FeaError on I/O failure,
/// unsupported format version/type, unknown element types in the volume set,
/// or inconsistent node/element indexing.
MshModel load_msh(const std::filesystem::path& path);

/// Parses ASCII .msh 2.2 text (exposed for unit tests).
MshModel parse_msh(const std::string& text);

} // namespace polymesh::fea
