// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <unordered_set>
#include <vector>

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

// Mark coarse blocks within `band` of any seed by rasterizing balls on the
// coarse index grid — O(seeds · ball_cells) instead of O(blocks · seeds).
void mark_seed_blocks(std::vector<bool>& coarse_fine, std::vector<bool>& coarse_seed,
                      int nxc, int nyc, int nzc, const CartesianGrid& fine,
                      std::span<const Eigen::Vector3d> seeds, double seed_band) {
    if (!(seed_band > 0.0) || seeds.empty() || nxc < 1 || nyc < 1 || nzc < 1) {
        return;
    }
    const double band2 = seed_band * seed_band;
    // Coarse cell size ≈ 2 fine cells.
    const Eigen::Vector3d hc = 2.0 * fine.cell;
    const double h_ref = std::max({hc[0], hc[1], hc[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(seed_band / h_ref)) + 1);

    const auto cidx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(nyc) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nxc) +
               static_cast<std::size_t>(i);
    };

    for (const auto& seed : seeds) {
        // Continuous fine-lattice index of the seed, then coarse parent.
        const Eigen::Vector3d local = seed - fine.origin;
        const int ifine = static_cast<int>(std::floor(local[0] / fine.cell[0]));
        const int jfine = static_cast<int>(std::floor(local[1] / fine.cell[1]));
        const int kfine = static_cast<int>(std::floor(local[2] / fine.cell[2]));
        const int ic0 = ifine / 2;
        const int jc0 = jfine / 2;
        const int kc0 = kfine / 2;
        for (int dk = -r; dk <= r; ++dk) {
            for (int dj = -r; dj <= r; ++dj) {
                for (int di = -r; di <= r; ++di) {
                    const int ic = ic0 + di, jc = jc0 + dj, kc = kc0 + dk;
                    if (ic < 0 || ic >= nxc || jc < 0 || jc >= nyc || kc < 0 ||
                        kc >= nzc) {
                        continue;
                    }
                    // Coarse-block center on the fine lattice (same as emit path).
                    const Eigen::Vector3d center =
                        fine.node(2 * ic + 1, 2 * jc + 1, 2 * kc + 1);
                    if ((center - seed).squaredNorm() <= band2) {
                        const auto id = cidx(ic, jc, kc);
                        coarse_fine[id] = true;
                        coarse_seed[id] = true;
                    }
                }
            }
        }
    }
}

// Feature band: sample sharp-edge segments onto the coarse grid and expand.
// Avoids O(blocks · n_edges) distance_to_features scans.
void mark_feature_blocks(std::vector<bool>& coarse_fine, std::vector<bool>& coarse_feature,
                         int nxc, int nyc, int nzc, const CartesianGrid& fine,
                         const geom::TriSurface& surface,
                         std::span<const geom::SharpEdge> features, double feature_band) {
    if (!(feature_band > 0.0) || features.empty() || nxc < 1) {
        return;
    }
    const Eigen::Vector3d hc = 2.0 * fine.cell;
    const double h_ref = std::max({hc[0], hc[1], hc[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(feature_band / h_ref)) + 1);
    const double band2 = feature_band * feature_band;

    const auto cidx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(nyc) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nxc) +
               static_cast<std::size_t>(i);
    };

    auto stamp = [&](const Eigen::Vector3d& p) {
        const Eigen::Vector3d local = p - fine.origin;
        const int ifine = static_cast<int>(std::floor(local[0] / fine.cell[0]));
        const int jfine = static_cast<int>(std::floor(local[1] / fine.cell[1]));
        const int kfine = static_cast<int>(std::floor(local[2] / fine.cell[2]));
        const int ic0 = ifine / 2;
        const int jc0 = jfine / 2;
        const int kc0 = kfine / 2;
        for (int dk = -r; dk <= r; ++dk) {
            for (int dj = -r; dj <= r; ++dj) {
                for (int di = -r; di <= r; ++di) {
                    const int ic = ic0 + di, jc = jc0 + dj, kc = kc0 + dk;
                    if (ic < 0 || ic >= nxc || jc < 0 || jc >= nyc || kc < 0 ||
                        kc >= nzc) {
                        continue;
                    }
                    const Eigen::Vector3d center =
                        fine.node(2 * ic + 1, 2 * jc + 1, 2 * kc + 1);
                    if ((center - p).squaredNorm() <= band2) {
                        const auto id = cidx(ic, jc, kc);
                        coarse_fine[id] = true;
                        coarse_feature[id] = true;
                    }
                }
            }
        }
    };

    for (const auto& e : features) {
        if (e.v0 >= surface.vertices.size() || e.v1 >= surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d& a = surface.vertices[e.v0];
        const Eigen::Vector3d& b = surface.vertices[e.v1];
        const double len = (b - a).norm();
        // Sample along the crease so long edges still densify a continuous band.
        const int n_samp = std::max(2, static_cast<int>(std::ceil(len / h_ref)) + 1);
        for (int s = 0; s <= n_samp; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n_samp);
            stamp((1.0 - t) * a + t * b);
        }
    }
}

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

    // Always 2:1: fine lattice ≈ h/2, coarse blocks span 2 fine cells ≈ h.
    // (Former subdiv=4 built a global h/4 lattice whenever features/seeds were
    // active — 8× cells, bulk still only h/2, and thin plates went fully fine.)
    constexpr int subdiv = 2;
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

    // Boundary distance in cell hops (0 = shares a face with exterior).
    // Face-only (6-connected) — 26-connected over-marked corner shells and
    // inflated the fine band on thin plates / near holes.
    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
    const int face_nbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    for (int k = 0; k < nzf; ++k) {
        for (int j = 0; j < nyf; ++j) {
            for (int i = 0; i < nxf; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (const auto& o : face_nbr) {
                    if (!inb(i + o[0], j + o[1], k + o[2])) {
                        boundary = true;
                        break;
                    }
                }
                if (boundary) {
                    dist[idx(i, j, k)] = 0;
                    q.push({i, j, k});
                }
            }
        }
    }
    int max_dist = 0;
    while (!q.empty()) {
        const auto c = q.front();
        q.pop();
        const int d0 = dist[idx(c[0], c[1], c[2])];
        max_dist = std::max(max_dist, d0);
        for (const auto& o : face_nbr) {
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

    // Cap skin depth so thin plates keep a coarse core when possible.
    // Requested skin is 2*skin_layers fine cells; never eat more than half the
    // interior thickness (else "graded" becomes uniform fine on plates/shells).
    const int requested_thresh = std::max(1, 2 * skin_layers);
    const int thickness_cap = std::max(1, (max_dist + 1) / 2);
    const int fine_dist_thresh = std::min(requested_thresh, thickness_cap);

    const int nxc = nxf / 2, nyc = nyf / 2, nzc = nzf / 2;
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
                if (need_fine) {
                    coarse_fine[cidx(ic, jc, kc)] = true;
                }
            }
        }
    }

    // Feature / seed bands force fine without rebuilding a finer global lattice.
    mark_feature_blocks(coarse_fine, coarse_feature, nxc, nyc, nzc, fine, surface, features,
                        feature_band);
    mark_seed_blocks(coarse_fine, coarse_seed, nxc, nyc, nzc, fine, refine_seeds, seed_band);

    GradedTetFillOutput out;
    out.h_coarse = 2.0 * hf; // one coarse Kuhn cube spans 2 fine cells ≈ target h
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
                    emit_cube_tets(2 * ic, 2 * jc, 2 * kc, 2);
                    ++out.n_coarse_cells;
                } else {
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

        // Only tets that touch a boundary node can invert from snap — skip the
        // interior (was O(passes · n_tets) and dominated wall time).
        std::vector<std::size_t> skin_tets;
        skin_tets.reserve(bnodes.size() * 4);
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

        const double vol_eps = 1e-14 * hf * hf * hf;
        snap_boundary_nodes(
            surface, out.mesh.nodes, bnodes, hf,
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
            /*max_move_frac=*/0.85, /*passes=*/4);
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
