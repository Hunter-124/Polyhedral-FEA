// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/poly_mesh.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <set>

namespace polymesh::mesh {
namespace {

Eigen::Vector3d face_centroid(const PolyMesh& m, const Face& face) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (const auto v : face.vertices) {
        c += m.vertices[v];
    }
    return c / static_cast<double>(face.vertices.size());
}

double tet_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
                  const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

} // namespace

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

void PolyMesh::check_geometry() const {
    check_validity();

    // Closed manifold shell: each undirected edge of boundary faces appears
    // in exactly two boundary faces. Interior faces (with neighbour) skipped.
    std::map<std::pair<VertexId, VertexId>, int> boundary_edge_count;
    for (const Face& face : faces) {
        if (face.neighbour) {
            continue;
        }
        const auto n = face.vertices.size();
        for (std::size_t i = 0; i < n; ++i) {
            VertexId a = face.vertices[i];
            VertexId b = face.vertices[(i + 1) % n];
            if (a > b) {
                std::swap(a, b);
            }
            boundary_edge_count[{a, b}] += 1;
        }
    }
    for (const auto& [e, count] : boundary_edge_count) {
        if (count != 2) {
            throw ValidityError(std::format(
                "boundary edge ({},{}) appears {} times (want 2 for closed manifold)", e.first,
                e.second, count));
        }
    }

    // Positive tet volume for tet cells (four triangular faces).
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const Cell& cell = cells[c];
        if (cell.kind != CellKind::kTet) {
            continue;
        }
        std::set<VertexId> vids;
        for (const auto fid : cell.faces) {
            for (const auto v : faces[fid].vertices) {
                vids.insert(v);
            }
        }
        if (vids.size() != 4) {
            throw ValidityError(
                std::format("tet cell {} does not reference exactly 4 unique vertices", c));
        }
        // Reconstruct oriented volume from owner-outward faces via divergence:
        // V = (1/3) sum_f centroid_f · area_normal_f (outward).
        double vol = 0.0;
        for (const auto fid : cell.faces) {
            const Face& face = faces[fid];
            if (face.vertices.size() != 3) {
                throw ValidityError(
                    std::format("tet cell {} face {} is not a triangle", c, fid));
            }
            const auto& a = vertices[face.vertices[0]];
            const auto& b = vertices[face.vertices[1]];
            const auto& c0 = vertices[face.vertices[2]];
            Eigen::Vector3d n = (b - a).cross(c0 - a) * 0.5;
            // Face loop is outward from owner; if this cell is neighbour, flip.
            if (face.owner != static_cast<CellId>(c)) {
                n = -n;
            }
            vol += face_centroid(*this, face).dot(n) / 3.0;
        }
        if (vol <= 0.0) {
            throw ValidityError(
                std::format("tet cell {} has non-positive volume {:.3e}", c, vol));
        }
        (void)tet_volume; // helper retained for future hex/prism checks
    }
}

} // namespace polymesh::mesh
