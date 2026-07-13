// SPDX-License-Identifier: BSD-3-Clause

#include "fea/poly_to_vem.hpp"
#include "fea/vem.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace polymesh::fea {

NodalMesh poly_mesh_to_vem(const mesh::PolyMesh& pm) {
    NodalMesh out;
    out.nodes = pm.vertices;
    out.elements.reserve(pm.cells.size());

    for (std::size_t ci = 0; ci < pm.cells.size(); ++ci) {
        const mesh::Cell& cell = pm.cells[ci];
        const mesh::CellId cid = static_cast<mesh::CellId>(ci);

        std::unordered_map<mesh::VertexId, std::uint32_t> g2l;
        std::vector<std::uint32_t> nodes;
        std::vector<std::vector<std::uint32_t>> faces;
        nodes.reserve(16);
        faces.reserve(cell.faces.size());

        auto ensure = [&](mesh::VertexId g) -> std::uint32_t {
            if (auto it = g2l.find(g); it != g2l.end()) {
                return it->second;
            }
            const auto loc = static_cast<std::uint32_t>(nodes.size());
            g2l.emplace(g, loc);
            nodes.push_back(static_cast<std::uint32_t>(g));
            return loc;
        };

        for (mesh::FaceId fid : cell.faces) {
            if (fid >= pm.faces.size()) {
                continue;
            }
            const mesh::Face& f = pm.faces[fid];
            if (f.owner != cid && !(f.neighbour && *f.neighbour == cid)) {
                continue;
            }
            std::vector<std::uint32_t> loop;
            loop.reserve(f.vertices.size());
            if (f.owner == cid) {
                for (mesh::VertexId g : f.vertices) {
                    loop.push_back(ensure(g));
                }
            } else {
                for (auto it = f.vertices.rbegin(); it != f.vertices.rend(); ++it) {
                    loop.push_back(ensure(*it));
                }
            }
            if (loop.size() >= 3) {
                faces.push_back(std::move(loop));
            }
        }

        if (nodes.size() < 4 || faces.size() < 4) {
            continue;
        }

        // Drop inverted / zero-volume debris (domain clip can leave slivers).
        std::vector<Eigen::Vector3d> coords(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            coords[i] = pm.vertices[nodes[i]];
        }
        double vol = poly_volume(coords, faces);
        if (vol < 0.0) {
            for (auto& loop : faces) {
                std::reverse(loop.begin(), loop.end());
            }
            vol = -vol;
        }
        Eigen::Vector3d bmin = coords[0], bmax = coords[0];
        for (const auto& p : coords) {
            bmin = bmin.cwiseMin(p);
            bmax = bmax.cwiseMax(p);
        }
        const double diag = std::max((bmax - bmin).norm(), 1e-30);
        const double vol_eps = 1e-14 * diag * diag * diag;
        if (!(vol > vol_eps)) {
            continue;
        }

        out.elements.emplace_back(ElementType::kPolyVem, std::move(nodes),
                                  std::move(faces));
    }
    return out;
}

}  // namespace polymesh::fea
