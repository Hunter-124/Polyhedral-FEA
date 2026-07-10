// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <queue>
#include <set>
#include <span>

namespace polymesh::mesh {
namespace {

// Kuhn 6-tet split of a unit cube with corners numbered as hex8.
constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
    {{0, 1, 2, 6}},
    {{0, 2, 3, 6}},
    {{0, 1, 5, 6}},
    {{0, 3, 7, 6}},
    {{0, 4, 5, 6}},
    {{0, 4, 7, 6}},
}};

} // namespace

GradedTetFillOutput
graded_tet_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                        const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                        std::span<const geom::SharpEdge> features, double feature_band,
                        std::span<const Eigen::Vector3d> refine_seeds, double seed_band) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("graded_tet_fill_surface: h must be positive");
    }
    if (skin_layers < 1) {
        skin_layers = 1;
    }
    if (!(feature_band > 0.0) || features.empty()) {
        feature_band = 0.0;
    }
    if (!(seed_band > 0.0) || refine_seeds.empty()) {
        seed_band = 0.0;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("graded_tet_fill_surface: empty bbox");
    }

    // Fine lattice: h/2 by default. With feature or seed bands, target h/4 so
    // curved edges / error seeds get much smaller elements (skin also denser).
    // Coarse blocks remain 2×2×2 of fine cells → bulk spacing ≈ 2·h_fine.
    // Bbox-fitted even grid + robust ray parity (shared-edge dedupe).
    // Pre-floor h so the fine lattice fits the cell budget; make_bbox_grid_even
    // also auto-coarsens as a backstop (avoids "grid too fine" on small user h).
    const bool ultra_fine = (feature_band > 0.0) || (seed_band > 0.0);
    const int subdiv = ultra_fine ? 4 : 2; // requested h / fine-target ratio
    const double h_budget = min_h_for_cell_budget(bbox_min, bbox_max, kDefaultMaxGridCells,
                                                  /*subdivision=*/subdiv);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const double hf_target = h_use / static_cast<double>(subdiv);
    const CartesianGrid fine = make_bbox_grid_even(bbox_min, bbox_max, hf_target, 2);
    const int nxf = fine.nx, nyf = fine.ny, nzf = fine.nz;
    const double hf = fine.max_edge();
    const auto inside = classify_cells_inside(surface, fine);
    const auto idx = [&](int i, int j, int k) { return fine.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nxf && j >= 0 && j < nyf && k >= 0 && k < nzf &&
               inside[idx(i, j, k)];
    };

    // Boundary distance in cell hops (0 = touches exterior).
    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
    for (int k = 0; k < nzf; ++k) {
        for (int j = 0; j < nyf; ++j) {
            for (int i = 0; i < nxf; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (int dk = -1; dk <= 1 && !boundary; ++dk) {
                    for (int dj = -1; dj <= 1 && !boundary; ++dj) {
                        for (int di = -1; di <= 1; ++di) {
                            if (di == 0 && dj == 0 && dk == 0) {
                                continue;
                            }
                            if (!inb(i + di, j + dj, k + dk)) {
                                boundary = true;
                                break;
                            }
                        }
                    }
                }
                if (boundary) {
                    dist[idx(i, j, k)] = 0;
                    q.push({i, j, k});
                }
            }
        }
    }
    const int dijk[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                            {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    while (!q.empty()) {
        const auto c = q.front();
        q.pop();
        const int d0 = dist[idx(c[0], c[1], c[2])];
        for (const auto& o : dijk) {
            const int ni = c[0] + o[0], nj = c[1] + o[1], nk = c[2] + o[2];
            if (!inb(ni, nj, nk)) {
                continue;
            }
            auto& dn = dist[idx(ni, nj, nk)];
            if (dn < 0 || dn > d0 + 1) {
                dn = d0 + 1;
                q.push({ni, nj, nk});
            }
        }
    }

    // Coarse skin: a 2×2×2 block is "fine" if any fine cell has dist < 2*skin_layers,
    // or the block center is near a sharp edge / error seed.
    const int nxc = nxf / 2, nyc = nyf / 2, nzc = nzf / 2;
    const int fine_dist_thresh = std::max(1, 2 * skin_layers);
    std::vector<bool> coarse_fine(static_cast<std::size_t>(nxc) * nyc * nzc, false);
    std::vector<bool> coarse_feature(static_cast<std::size_t>(nxc) * nyc * nzc, false);
    std::vector<bool> coarse_seed(static_cast<std::size_t>(nxc) * nyc * nzc, false);
    const auto cidx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * nyc + j) * nxc + i;
    };
    for (int kc = 0; kc < nzc; ++kc) {
        for (int jc = 0; jc < nyc; ++jc) {
            for (int ic = 0; ic < nxc; ++ic) {
                bool any_in = false;
                bool need_fine = false;
                for (int dk = 0; dk < 2; ++dk) {
                    for (int dj = 0; dj < 2; ++dj) {
                        for (int di = 0; di < 2; ++di) {
                            const int i = 2 * ic + di, j = 2 * jc + dj, k = 2 * kc + dk;
                            if (!inside[idx(i, j, k)]) {
                                continue;
                            }
                            any_in = true;
                            if (dist[idx(i, j, k)] >= 0 &&
                                dist[idx(i, j, k)] < fine_dist_thresh) {
                                need_fine = true;
                            }
                        }
                    }
                }
                if (!any_in) {
                    continue;
                }
                const Eigen::Vector3d center = fine.node(2 * ic + 1, 2 * jc + 1, 2 * kc + 1);
                if (feature_band > 0.0) {
                    const double dfeat = geom::distance_to_features(center, surface, features);
                    if (dfeat <= feature_band) {
                        need_fine = true;
                        coarse_feature[cidx(ic, jc, kc)] = true;
                    }
                }
                if (seed_band > 0.0) {
                    for (const auto& seed : refine_seeds) {
                        if ((center - seed).norm() <= seed_band) {
                            need_fine = true;
                            coarse_seed[cidx(ic, jc, kc)] = true;
                            break;
                        }
                    }
                }
                if (need_fine) {
                    coarse_fine[cidx(ic, jc, kc)] = true;
                }
            }
        }
    }

    GradedTetFillOutput out;
    // Report realized spacings (may exceed targets after cell-budget floor).
    out.h_coarse = 2.0 * hf; // one coarse Kuhn cube spans 2 fine cells
    out.h_fine = hf;
    out.skin_layers = skin_layers;
    out.subdivision = subdiv;
    out.mesh.h = hf;

    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.mesh.nodes.size()));
        if (fresh) {
            out.mesh.nodes.push_back(fine.node(i, j, k));
        }
        return it->second;
    };

    auto emit_cube_tets = [&](int i0, int j0, int k0, int step) {
        // Cube from (i0,j0,k0) to +step in each axis on fine lattice.
        const std::array<std::uint32_t, 8> c{{
            node_at(i0, j0, k0),
            node_at(i0 + step, j0, k0),
            node_at(i0 + step, j0 + step, k0),
            node_at(i0, j0 + step, k0),
            node_at(i0, j0, k0 + step),
            node_at(i0 + step, j0, k0 + step),
            node_at(i0 + step, j0 + step, k0 + step),
            node_at(i0, j0 + step, k0 + step),
        }};
        for (const auto& t : kCubeTets) {
            std::array<std::uint32_t, 4> n{
                {c[static_cast<std::size_t>(t[0])], c[static_cast<std::size_t>(t[1])],
                 c[static_cast<std::size_t>(t[2])], c[static_cast<std::size_t>(t[3])]}};
            double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                         out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
                v = -v;
            }
            if (v > 0.0) {
                out.mesh.tets.push_back(n);
            }
        }
    };

    for (int kc = 0; kc < nzc; ++kc) {
        for (int jc = 0; jc < nyc; ++jc) {
            for (int ic = 0; ic < nxc; ++ic) {
                // Is the whole 2×2×2 block interior?
                bool all_in = true;
                bool any_in = false;
                for (int dk = 0; dk < 2 && all_in; ++dk) {
                    for (int dj = 0; dj < 2 && all_in; ++dj) {
                        for (int di = 0; di < 2; ++di) {
                            const int i = 2 * ic + di, j = 2 * jc + dj, k = 2 * kc + dk;
                            if (inside[idx(i, j, k)]) {
                                any_in = true;
                            } else {
                                all_in = false;
                            }
                        }
                    }
                }
                if (!any_in) {
                    continue;
                }
                if (all_in && !coarse_fine[cidx(ic, jc, kc)]) {
                    // Coarse cube = one Kuhn split spanning 2 fine cells.
                    emit_cube_tets(2 * ic, 2 * jc, 2 * kc, 2);
                    ++out.n_coarse_cells;
                } else {
                    // Fine: every occupied fine cell becomes 6 tets.
                    if (coarse_feature[cidx(ic, jc, kc)]) {
                        ++out.n_feature_cells;
                    }
                    if (coarse_seed[cidx(ic, jc, kc)]) {
                        ++out.n_seed_cells;
                    }
                    for (int dk = 0; dk < 2; ++dk) {
                        for (int dj = 0; dj < 2; ++dj) {
                            for (int di = 0; di < 2; ++di) {
                                const int i = 2 * ic + di, j = 2 * jc + dj, k = 2 * kc + dk;
                                if (!inside[idx(i, j, k)]) {
                                    continue;
                                }
                                emit_cube_tets(i, j, k, 1);
                                ++out.n_fine_cells;
                            }
                        }
                    }
                }
            }
        }
    }

    if (out.mesh.tets.empty()) {
        throw ValidityError("graded_tet_fill_surface: no interior cells");
    }

    // Boundary quads on the fine lattice (for region mapping + snap).
    for (int k = 0; k < nzf; ++k) {
        for (int j = 0; j < nyf; ++j) {
            for (int i = 0; i < nxf; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
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
                    if (inb(i + f.di, j + f.dj, k + f.dk)) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int q = 0; q < 4; ++q) {
                        const auto& c = f.corners[static_cast<std::size_t>(q)];
                        quad[static_cast<std::size_t>(q)] =
                            node_at(i + c[0], j + c[1], k + c[2]);
                    }
                    out.mesh.boundary_quads.push_back(quad);
                }
            }
        }
    }

    // Multi-pass surface snap so curved walls (cylinders/holes) are not pure
    // stair-cases. Jacobian safety unsnaps offenders (ADR-0015/B3).
    if (!out.mesh.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.mesh.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const double vol_eps = 1e-14 * hf * hf * hf;
        snap_boundary_nodes(
            surface, out.mesh.nodes, bnodes, hf,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto& n : out.mesh.tets) {
                    const double v =
                        tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                          out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                    if (v > vol_eps) {
                        continue;
                    }
                    offenders.insert(n.begin(), n.end());
                }
            },
            /*max_move_frac=*/0.55, /*passes=*/3);
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
