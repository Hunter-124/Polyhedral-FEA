// SPDX-License-Identifier: BSD-3-Clause
#include "geom/step.hpp"

#include "geom/cad_model.hpp"

// Historical API: load_step returns a derived triangulation. Prefer CadModel
// (ADR-0020) for product paths that need the live BRep.

namespace polymesh::geom {

TriSurface load_step(const std::filesystem::path& path) {
    return CadModel::load_step(path).tessellate();
}

} // namespace polymesh::geom
