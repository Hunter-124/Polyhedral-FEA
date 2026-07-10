// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/poly_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <queue>
#include <span>

namespace polymesh::mesh {
namespace {

std::vector<bool> inside_mask(const geom::TriSurface& surface, const Eigen::Vector3d& origin,
                              int nx, int ny, int nz, double h) {
    std::vector<bool> inside(static_cast<std::size_t>(nx) * ny * nz, false);
    const auto cell_index = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * ny + j) * nx + i;
    };
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double cx = origin[0] + (i + 0.5) * h;
            const double cy = origin[1] + (j + 0.5) * h;
            std::vector<double> crossings;
            for (const auto& tri : surface.triangles) {
                const auto& a = surface.vertices[tri[0]];
                const auto& b = surface.vertices[tri[1]];
                const auto& c = surface.vertices[tri[2]];
                const double d1 = (b[0] - a[0]) * (cy - a[1]) - (b[1] - a[1]) * (cx - a[0]);
                const double d2 = (c[0] - b[0]) * (cy - b[1]) - (c[1] - b[1]) * (cx - b[0]);
                const double d3 = (a[0] - c[0]) * (cy - c[1]) - (a[1] - c[1]) * (cx - c[0]);
                if ((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)) {
                    continue;
                }
                const double area = d1 + d2 + d3;
                if (area == 0.0) {
                    continue;
                }
                crossings.push_back((d2 * a[2] + d3 * b[2] + d1 * c[2]) / area);
            }
            std::sort(crossings.begin(), crossings.end());
            for (int k = 0; k < nz; ++k) {
                const double cz = origin[2] + (k + 0.5) * h;
                const auto below = std::upper_bound(crossings.begin(), crossings.end(), cz) -
                                   crossings.begin();
                if (below % 2 == 1) {
                    inside[cell_index(i, j, k)] = true;
                }
            }
        }
    }
    return inside;
}

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

    // Work on the fine lattice (h/2) so coarse cells are 2×2×2 fine cells.
    const double hf = h * 0.5;
    const auto n_axis = [&](int a) {
        return std::max(2, static_cast<int>(std::ceil(extent[a] / hf)));
    };
    // Even counts so coarse grouping is clean.
    int nxf = n_axis(0), nyf = n_axis(1), nzf = n_axis(2);
    if (nxf % 2) {
        ++nxf;
    }
    if (nyf % 2) {
        ++nyf;
    }
    if (nzf % 2) {
        ++nzf;
    }
    if (static_cast<long>(nxf) * nyf * nzf > 512 * 1024) {
        throw ValidityError("graded_tet_fill_surface: grid too fine");
    }

    const Eigen::Vector3d origin = bbox_min;
    const auto inside = inside_mask(surface, origin, nxf, nyf, nzf, hf);
    const auto idx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * nyf + j) * nxf + i;
    };
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
                const Eigen::Vector3d center =
                    origin +
                    Eigen::Vector3d((2 * ic + 1) * hf, (2 * jc + 1) * hf, (2 * kc + 1) * hf);
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
    out.h_coarse = h;
    out.h_fine = hf;
    out.skin_layers = skin_layers;
    out.mesh.h = hf;

    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.mesh.nodes.size()));
        if (fresh) {
            out.mesh.nodes.emplace_back(origin[0] + i * hf, origin[1] + j * hf,
                                        origin[2] + k * hf);
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

    // Boundary quads on the fine lattice (for region mapping).
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

    check_tet_fill_geometry(out.mesh);
    return out;
}

} // namespace polymesh::mesh
