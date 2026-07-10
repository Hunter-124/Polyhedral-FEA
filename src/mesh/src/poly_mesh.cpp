// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mesh/poly_mesh.hpp"

#include <format>

namespace polymesh::mesh {

void PolyMesh::check_validity() const {
    const auto nv = static_cast<std::uint32_t>(vertices.size());
    const auto nc = static_cast<std::uint32_t>(cells.size());
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.vertices.size() < 3) {
            throw ValidityError(std::format("face {} has fewer than 3 vertices", f));
        }
        for (const auto v : face.vertices) {
            if (v >= nv) {
                throw ValidityError(
                    std::format("face {} references out-of-range vertex {}", f, v));
            }
        }
        if (face.owner >= nc) {
            throw ValidityError(
                std::format("face {} references out-of-range owner cell {}", f, face.owner));
        }
        if (face.neighbour && *face.neighbour >= nc) {
            throw ValidityError(std::format(
                "face {} references out-of-range neighbour cell {}", f, *face.neighbour));
        }
    }
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const Cell& cell = cells[c];
        if (cell.faces.size() < 4) {
            throw ValidityError(
                std::format("cell {} has fewer than 4 faces (cannot bound a volume)", c));
        }
        for (const auto f : cell.faces) {
            if (f >= faces.size()) {
                throw ValidityError(
                    std::format("cell {} references out-of-range face {}", c, f));
            }
            const Face& face = faces[f];
            const auto cid = static_cast<CellId>(c);
            if (face.owner != cid && face.neighbour != cid) {
                throw ValidityError(std::format(
                    "cell {} lists face {} which does not reference it back", c, f));
            }
        }
    }
}

} // namespace polymesh::mesh
