// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/octa_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

// Face corner offsets relative to cell (i,j,k) for each of 6 faces.
// face: 0=-x 1=+x 2=-y 3=+y 4=-z 5=+z
void face_corners(int f, int i, int j, int k, std::array<std::array<int, 3>, 4>& out) {
    switch (f) {
    case 0:
        out = {{{i, j, k}, {i, j + 1, k}, {i, j + 1, k + 1}, {i, j, k + 1}}};
        break;
    case 1:
        out = {{{i + 1, j, k}, {i + 1, j, k + 1}, {i + 1, j + 1, k + 1}, {i + 1, j + 1, k}}};
        break;
    case 2:
        out = {{{i, j, k}, {i, j, k + 1}, {i + 1, j, k + 1}, {i + 1, j, k}}};
        break;
    case 3:
        out = {{{i, j + 1, k}, {i + 1, j + 1, k}, {i + 1, j + 1, k + 1}, {i, j + 1, k + 1}}};
        break;
    case 4:
        out = {{{i, j, k}, {i + 1, j, k}, {i + 1, j + 1, k}, {i, j + 1, k}}};
        break;
    default:
        out = {{{i, j, k + 1}, {i, j + 1, k + 1}, {i + 1, j + 1, k + 1}, {i + 1, j, k + 1}}};
        break;
    }
}

void push_tet(std::vector<std::array<std::uint32_t, 4>>& tets,
              const std::vector<Eigen::Vector3d>& nodes, std::array<std::uint32_t, 4> n) {
    double v = tet_signed_volume(nodes[n[0]], nodes[n[1]], nodes[n[2]], nodes[n[3]]);
    if (v < 0.0) {
        std::swap(n[1], n[2]);
        v = -v;
    }
    if (v > 0.0) {
        tets.push_back(n);
    }
}

} // namespace

OctaFillOutput octa_fill_surface(const geom::TriSurface& surface,
                                 const Eigen::Vector3d& bbox_min,
                                 const Eigen::Vector3d& bbox_max, double h,
                                 bool snap_boundary) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("octa_fill_surface: h must be positive");
    }
    // Experimental path: hard cell budget so auto-h never builds multi-million
    // tet floods (product GUI must stay interactive).
    constexpr std::size_t kOctaMaxCells = 24 * 1024;
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kOctaMaxCells, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h_use);
    const auto inside = classify_cells_inside(surface, grid);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz && inside[idx(i, j, k)];
    };

    OctaFillOutput out;
    out.h = grid.max_edge();
    out.mesh.h = out.h;

    std::map<std::array<int, 3>, std::uint32_t> corner_ids;
    const auto corner_at = [&](int i, int j, int k) {
        const auto [it, fresh] = corner_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.mesh.nodes.size()));
        if (fresh) {
            out.mesh.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };

    // Cell centre node per interior cell (flat index → node id, -1 if none).
    std::vector<std::int32_t> center_of(inside.size(), -1);
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                center_of[idx(i, j, k)] = static_cast<std::int32_t>(out.mesh.nodes.size());
                out.mesh.nodes.push_back(grid.cell_center(i, j, k));
            }
        }
    }

    // Interior face octahedra: only +x/+y/+z neighbours to avoid double-count.
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto c0 = static_cast<std::uint32_t>(center_of[idx(i, j, k)]);
                // Faces: 1=+x, 3=+y, 5=+z
                for (int f : {1, 3, 5}) {
                    const int ni = i + kFaceNbr[f][0], nj = j + kFaceNbr[f][1],
                              nk = k + kFaceNbr[f][2];
                    if (!inb(ni, nj, nk)) {
                        continue;
                    }
                    const auto c1 = static_cast<std::uint32_t>(center_of[idx(ni, nj, nk)]);
                    std::array<std::array<int, 3>, 4> offs{};
                    face_corners(f, i, j, k, offs);
                    std::array<std::uint32_t, 4> fc{};
                    for (int q = 0; q < 4; ++q) {
                        fc[static_cast<std::size_t>(q)] =
                            corner_at(offs[static_cast<std::size_t>(q)][0],
                                      offs[static_cast<std::size_t>(q)][1],
                                      offs[static_cast<std::size_t>(q)][2]);
                    }
                    // Four tets: c0–edge–c1 around the face loop.
                    push_tet(out.mesh.tets, out.mesh.nodes, {c0, fc[0], fc[1], c1});
                    push_tet(out.mesh.tets, out.mesh.nodes, {c0, fc[1], fc[2], c1});
                    push_tet(out.mesh.tets, out.mesh.nodes, {c0, fc[2], fc[3], c1});
                    push_tet(out.mesh.tets, out.mesh.nodes, {c0, fc[3], fc[0], c1});
                    ++out.n_octahedra;
                }
            }
        }
    }

    // Boundary half-octahedra: pyramid from centre to exterior face → 2 tets.
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto c0 = static_cast<std::uint32_t>(center_of[idx(i, j, k)]);
                for (int f = 0; f < 6; ++f) {
                    if (inb(i + kFaceNbr[f][0], j + kFaceNbr[f][1], k + kFaceNbr[f][2])) {
                        continue;
                    }
                    std::array<std::array<int, 3>, 4> offs{};
                    face_corners(f, i, j, k, offs);
                    std::array<std::uint32_t, 4> fc{};
                    for (int q = 0; q < 4; ++q) {
                        fc[static_cast<std::size_t>(q)] =
                            corner_at(offs[static_cast<std::size_t>(q)][0],
                                      offs[static_cast<std::size_t>(q)][1],
                                      offs[static_cast<std::size_t>(q)][2]);
                    }
                    // Two tets with diagonal 0–2 (match pyramid convention).
                    push_tet(out.mesh.tets, out.mesh.nodes, {fc[0], fc[1], fc[2], c0});
                    push_tet(out.mesh.tets, out.mesh.nodes, {fc[0], fc[2], fc[3], c0});
                    out.mesh.boundary_quads.push_back(fc);
                    ++out.n_boundary_pyramids;
                }
            }
        }
    }

    if (out.mesh.tets.empty()) {
        throw ValidityError("octa_fill_surface: no interior cells");
    }

    if (snap_boundary && !out.mesh.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.mesh.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        // Do not snap cell centres (not on free surface lattice).
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        std::vector<std::size_t> skin_tets;
        {
            std::unordered_set<std::uint32_t> bset(bnodes.begin(), bnodes.end());
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                const auto& n = out.mesh.tets[ti];
                if (bset.count(n[0]) || bset.count(n[1]) || bset.count(n[2]) ||
                    bset.count(n[3])) {
                    skin_tets.push_back(ti);
                }
            }
        }
        const double vol_eps = 1e-14 * out.h * out.h * out.h;
        snap_boundary_nodes(
            surface, out.mesh.nodes, bnodes, out.h,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto ti : skin_tets) {
                    const auto& n = out.mesh.tets[ti];
                    const double v =
                        tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                          out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                    if (v > vol_eps) {
                        continue;
                    }
                    offenders.insert(n.begin(), n.end());
                }
            },
            /*max_move_frac=*/0.75, /*passes=*/3);
        for (auto& n : out.mesh.tets) {
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
    }

    check_tet_fill_geometry(out.mesh);
    return out;
}

} // namespace polymesh::mesh
