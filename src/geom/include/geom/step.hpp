// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// STEP / B-rep loading via OpenCASCADE (ADR-0001).
//
// Always declared. When POLYMESH_WITH_OCC is not defined at compile time,
// load_step() throws GeomError("OpenCASCADE not enabled").

#include "geom/tri_surface.hpp"

#include <filesystem>

namespace polymesh::geom {

/// True when this build was configured with -DPOLYMESH_WITH_OCC=ON and
/// linked against OpenCASCADE.
[[nodiscard]] constexpr bool occ_enabled() noexcept {
#ifdef POLYMESH_WITH_OCC
    return true;
#else
    return false;
#endif
}

/// Loads a STEP file and returns a triangulated surface mesh.
///
/// Uses OpenCASCADE STEPControl_Reader + BRepMesh_IncrementalMesh.
/// Vertex coordinates are in metres (SI), matching the rest of geom.
/// Throws GeomError on I/O failure, malformed STEP, empty geometry, or when
/// OpenCASCADE support was not compiled in.
TriSurface load_step(const std::filesystem::path& path);

} // namespace polymesh::geom
