// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/cell_validity.hpp"
#include "mesh/hex_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/LU>

#include <cmath>
#include <format>
#include <map>
#include <set>

namespace polymesh::mesh {
namespace {

/// True when the cell must not ship: the trilinear map turns over anywhere the
/// FE assembly samples it (centre + the 2x2x2 rule `element_stiffness` uses).
///
/// The bar is deliberately the SAMPLED map, not `fea::cell_quality`'s corner
/// scaled Jacobian. A hex8 is assembled isoparametrically, so a corner-folded
/// cell whose sampled determinants stay positive is badly shaped but integrable,
/// and a hex has no conformity-free decomposition to escape into (unlike the
/// folded pyramids in the hybrid path, which ship as the two tets the assembly
/// already builds from them). Gating on the corner measure here buys shape by
/// abandoning the wall: measured 2026-08-15, M1max went 0.0007 -> 0.108 on the
/// sphere at h=0.15*extent and 9.6e-12 -> 2.559 on the hole plate at
/// h=0.10*extent -- half a cell off the surface -- because on a stair-stepped
/// lattice the nodes doing the snapping ARE the nodes of the folded cells.
/// The interior relaxation below is what actually improves their shape, and it
/// costs no fidelity.
bool hex_bad(const std::array<std::uint32_t, 8>& hx,
             const std::vector<Eigen::Vector3d>& nodes) {
    std::array<Eigen::Vector3d, 8> x{};
    for (int i = 0; i < 8; ++i) {
        x[static_cast<std::size_t>(i)] = nodes[hx[static_cast<std::size_t>(i)]];
    }
    return validity::hex8_min_jacobian(x) <= 0.0;
}

} // namespace

HexFillOutput hex_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h, bool snap_boundary) {
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h);
    const auto inside = classify_cells_inside(surface, grid);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;

    HexFillOutput out;
    out.h = grid.max_edge();
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[grid.index(i, j, k)];
    };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[grid.index(i, j, k)]) {
                    continue;
                }
                out.hexes.push_back({node_at(i, j, k), node_at(i + 1, j, k),
                                     node_at(i + 1, j + 1, k), node_at(i, j + 1, k),
                                     node_at(i, j, k + 1), node_at(i + 1, j, k + 1),
                                     node_at(i + 1, j + 1, k + 1), node_at(i, j + 1, k + 1)});
                struct FaceDef {
                    int di, dj, dk;
                    std::array<std::array<int, 3>, 4> corners;
                };
                const std::array<FaceDef, 6> faces{{
                    {-1, 0, 0, {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}}},
                    {1, 0, 0, {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}}},
                    {0, -1, 0, {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}}},
                    {0, 1, 0, {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}}},
                    {0, 0, -1, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}},
                    {0, 0, 1, {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}}},
                }};
                for (const auto& f : faces) {
                    if (is_inside(i + f.di, j + f.dj, k + f.dk)) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int q = 0; q < 4; ++q) {
                        const auto& c = f.corners[static_cast<std::size_t>(q)];
                        quad[static_cast<std::size_t>(q)] =
                            node_at(i + c[0], j + c[1], k + c[2]);
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }
    if (out.hexes.empty()) {
        throw ValidityError("hex_fill_surface: no interior cells");
    }

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        const std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const auto collect_offenders = [&](std::set<std::uint32_t>& offenders) {
            for (const auto& hx : out.hexes) {
                if (!hex_bad(hx, out.nodes)) {
                    continue;
                }
                offenders.insert(hx.begin(), hx.end());
            }
        };

        // Interior relaxation between snap rounds.
        //
        // A cube lattice cannot reach a curved or slanted wall without folding a
        // stair-step cell if only the wall may move: the nodes doing the snapping
        // ARE the nodes of the folded cells, so a validity gate on its own does
        // not remove the fold, it removes the snap. Measured 2026-08-15 with the
        // gate alone: M1max 0.0007 -> 0.108 (sphere h=0.15·extent) and 9.6e-12 ->
        // 2.559 (hole plate h=0.10·extent), i.e. half a cell off the surface.
        //
        // Giving the INTERIOR room fixes it at the source: each non-boundary node
        // moves toward the centroid of its lattice neighbours, the move is kept
        // only when every incident hex stays valid, and the wall is then snapped
        // again into the space that opened up. Interior nodes are unconstrained by
        // geometry, so this cannot cost boundary fidelity by construction.
        std::vector<std::vector<std::uint32_t>> nbrs(out.nodes.size());
        {
            static constexpr std::array<std::array<int, 2>, 12> kHexEdges{{
                {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}}, {{4, 5}}, {{5, 6}},
                {{6, 7}}, {{7, 4}}, {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
            }};
            std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
            for (const auto& hx : out.hexes) {
                for (const auto& e : kHexEdges) {
                    const auto a = hx[static_cast<std::size_t>(e[0])];
                    const auto b = hx[static_cast<std::size_t>(e[1])];
                    const auto key = std::minmax(a, b);
                    if (a == b || !seen.insert({key.first, key.second}).second) {
                        continue;
                    }
                    nbrs[a].push_back(b);
                    nbrs[b].push_back(a);
                }
            }
        }
        std::vector<std::vector<std::size_t>> node_hexes(out.nodes.size());
        for (std::size_t ci = 0; ci < out.hexes.size(); ++ci) {
            for (const auto ni : out.hexes[ci]) {
                node_hexes[ni].push_back(ci);
            }
        }
        std::vector<char> on_boundary(out.nodes.size(), 0);
        for (const auto ni : bnodes) {
            on_boundary[ni] = 1;
        }
        // Interior node ids in ascending order: the acceptance test reads the
        // shared node array, so visit order is mesh-level mutation state and must
        // not depend on container iteration order (ADR-0032).
        std::vector<std::uint32_t> interior;
        for (std::uint32_t ni = 0; ni < out.nodes.size(); ++ni) {
            if (on_boundary[ni] == 0 && !nbrs[ni].empty()) {
                interior.push_back(ni);
            }
        }
        const auto relax_interior = [&](int passes, double omega) {
            for (int pass = 0; pass < passes; ++pass) {
                for (const auto ni : interior) {
                    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                    for (const auto other : nbrs[ni]) {
                        centroid += out.nodes[other];
                    }
                    centroid /= static_cast<double>(nbrs[ni].size());
                    const Eigen::Vector3d saved = out.nodes[ni];
                    out.nodes[ni] = saved + omega * (centroid - saved);
                    for (const auto ci : node_hexes[ni]) {
                        if (hex_bad(out.hexes[ci], out.nodes)) {
                            out.nodes[ni] = saved; // reject: keep the valid state
                            break;
                        }
                    }
                }
            }
        };

        // Two rounds: snap what the lattice allows, open the interior, snap the
        // stragglers into the new space. A third round moved nothing measurable.
        out.boundary_max_distance =
            snap_boundary_nodes(surface, out.nodes, bnodes, out.h, collect_offenders,
                                /*max_move_frac=*/0.75, /*passes=*/4)
                .max_residual;
        relax_interior(/*passes=*/4, /*omega=*/0.5);
        out.boundary_max_distance =
            snap_boundary_nodes(surface, out.nodes, bnodes, out.h, collect_offenders,
                                /*max_move_frac=*/0.75, /*passes=*/4)
                .max_residual;
    }
    return out;
}

} // namespace polymesh::mesh
