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

/// Loads a STEP file and returns a **derived** triangulated surface mesh.
///
/// Prefer `CadModel::load_step` + `tessellate()` for product paths that need
/// the live BRep (ADR-0020). This helper is equivalent to
/// `CadModel::load_step(path).tessellate()` and remains for legacy callers.
/// Throws GeomError on I/O failure, malformed STEP, empty geometry, or when
/// OpenCASCADE support was not compiled in.
TriSurface load_step(const std::filesystem::path& path);

} // namespace polymesh::geom
