// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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

constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

// Stamp seeds onto coarse cells — O(seeds · ball) not O(cells · seeds).
void mark_seed_cells(std::vector<char>& is_fine, std::vector<char>& is_seed, int nx, int ny,
                     int nz, const CartesianGrid& grid, std::span<const Eigen::Vector3d> seeds,
                     double seed_band) {
    if (!(seed_band > 0.0) || seeds.empty() || nx < 1) {
        return;
    }
    const double band2 = seed_band * seed_band;
    const double h_ref = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(seed_band / h_ref)) + 1);
    const auto idx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    };
    for (const auto& seed : seeds) {
        const Eigen::Vector3d local = seed - grid.origin;
        const int i0 = static_cast<int>(std::floor(local[0] / grid.cell[0]));
        const int j0 = static_cast<int>(std::floor(local[1] / grid.cell[1]));
        const int k0 = static_cast<int>(std::floor(local[2] / grid.cell[2]));
        for (int dk = -r; dk <= r; ++dk) {
            for (int dj = -r; dj <= r; ++dj) {
                for (int di = -r; di <= r; ++di) {
                    const int i = i0 + di, j = j0 + dj, k = k0 + dk;
                    if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) {
                        continue;
                    }
                    const Eigen::Vector3d c = grid.cell_center(i, j, k);
                    if ((c - seed).squaredNorm() <= band2) {
                        const auto id = idx(i, j, k);
                        is_fine[id] = 1;
                        is_seed[id] = 1;
                    }
                }
            }
        }
    }
}

void mark_feature_cells(std::vector<char>& is_fine, std::vector<char>& is_feature, int nx,
                        int ny, int nz, const CartesianGrid& grid,
                        const geom::TriSurface& surface,
                        std::span<const geom::SharpEdge> features, double feature_band) {
    if (!(feature_band > 0.0) || features.empty() || nx < 1) {
        return;
    }
    const double band2 = feature_band * feature_band;
    const double h_ref = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(feature_band / h_ref)) + 1);
    const auto idx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    };
    auto stamp = [&](const Eigen::Vector3d& p) {
        const Eigen::Vector3d local = p - grid.origin;
        const int i0 = static_cast<int>(std::floor(local[0] / grid.cell[0]));
        const int j0 = static_cast<int>(std::floor(local[1] / grid.cell[1]));
        const int k0 = static_cast<int>(std::floor(local[2] / grid.cell[2]));
        for (int dk = -r; dk <= r; ++dk) {
            for (int dj = -r; dj <= r; ++dj) {
                for (int di = -r; di <= r; ++di) {
                    const int i = i0 + di, j = j0 + dj, k = k0 + dk;
                    if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) {
                        continue;
                    }
                    const Eigen::Vector3d c = grid.cell_center(i, j, k);
                    if ((c - p).squaredNorm() <= band2) {
                        const auto id = idx(i, j, k);
                        is_fine[id] = 1;
                        is_feature[id] = 1;
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

    // Coarse-primary lattice at target h (classify cost matches tet/hybrid).
    // Local 2×2×2 only on fine-marked cells → bulk ≈ h, skin/feature ≈ h/2.
    constexpr int subdiv = 2;
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kDefaultMaxGridCells, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h_use);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double hc = grid.max_edge();
    const double hf = 0.5 * hc;
    const auto inside = classify_cells_inside(surface, grid);
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz && inside[idx(i, j, k)];
    };

    // Face-only boundary distance (coarse hops).
    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
    int max_dist = 0;
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (const auto& o : kFaceNbr) {
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
    while (!q.empty()) {
        const auto c = q.front();
        q.pop();
        const int d0 = dist[idx(c[0], c[1], c[2])];
        max_dist = std::max(max_dist, d0);
        for (const auto& o : kFaceNbr) {
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

    // Skin depth in coarse cells; never eat more than half the interior.
    const int skin_cap = std::max(1, (max_dist + 1) / 2);
    const int skin_thresh = std::min(skin_layers, skin_cap);

    std::vector<char> is_fine(inside.size(), 0);
    std::vector<char> is_feature(inside.size(), 0);
    std::vector<char> is_seed(inside.size(), 0);
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (dist[c] >= 0 && dist[c] < skin_thresh) {
            is_fine[c] = 1;
        }
    }
    mark_feature_cells(is_fine, is_feature, nx, ny, nz, grid, surface, features, feature_band);
    mark_seed_cells(is_fine, is_seed, nx, ny, nz, grid, refine_seeds, seed_band);
    // Only interior cells may be refined.
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            is_fine[c] = 0;
            is_feature[c] = 0;
            is_seed[c] = 0;
        }
    }

    GradedTetFillOutput out;
    out.h_coarse = hc;
    out.h_fine = hf;
    out.skin_layers = skin_layers;
    out.subdivision = subdiv;
    out.mesh.h = hf;

    // Dense fine-index node table: fine indices (0..2*n) map half-cell steps.
    // Coarse cell (i,j,k) occupies fine corners (2i,2j,2k) … (2i+2,2j+2,2k+2).
    const int nfx = 2 * nx + 1, nfy = 2 * ny + 1, nfz = 2 * nz + 1;
    const std::size_t n_slot =
        static_cast<std::size_t>(nfx) * static_cast<std::size_t>(nfy) * static_cast<std::size_t>(nfz);
    std::vector<std::int32_t> node_of(n_slot, -1);
    const auto flat = [&](int fi, int fj, int fk) {
        return (static_cast<std::size_t>(fk) * static_cast<std::size_t>(nfy) +
                static_cast<std::size_t>(fj)) *
                   static_cast<std::size_t>(nfx) +
               static_cast<std::size_t>(fi);
    };
    const auto node_at = [&](int fi, int fj, int fk) -> std::uint32_t {
        auto& slot = node_of[flat(fi, fj, fk)];
        if (slot >= 0) {
            return static_cast<std::uint32_t>(slot);
        }
        const Eigen::Vector3d p = grid.origin + Eigen::Vector3d(0.5 * static_cast<double>(fi) *
                                                                   grid.cell[0],
                                                               0.5 * static_cast<double>(fj) *
                                                                   grid.cell[1],
                                                               0.5 * static_cast<double>(fk) *
                                                                   grid.cell[2]);
        slot = static_cast<std::int32_t>(out.mesh.nodes.size());
        out.mesh.nodes.push_back(p);
        return static_cast<std::uint32_t>(slot);
    };

    auto emit_cube_tets = [&](int fi0, int fj0, int fk0, int step) {
        const std::array<std::uint32_t, 8> c{{
            node_at(fi0, fj0, fk0),
            node_at(fi0 + step, fj0, fk0),
            node_at(fi0 + step, fj0 + step, fk0),
            node_at(fi0, fj0 + step, fk0),
            node_at(fi0, fj0, fk0 + step),
            node_at(fi0 + step, fj0, fk0 + step),
            node_at(fi0 + step, fj0 + step, fk0 + step),
            node_at(fi0, fj0 + step, fk0 + step),
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

    // Face corner offsets in fine indices for step-size `s` cell at (fi,fj,fk).
    // Order matches prior lattice quad convention (outward not required for snap).
    auto emit_face_quads = [&](int fi, int fj, int fk, int s, int face) {
        // face: 0=-x 1=+x 2=-y 3=+y 4=-z 5=+z
        std::array<std::array<int, 3>, 4> corners{};
        switch (face) {
        case 0: // -x
            corners = {{{0, 0, 0}, {0, s, 0}, {0, s, s}, {0, 0, s}}};
            break;
        case 1: // +x
            corners = {{{s, 0, 0}, {s, 0, s}, {s, s, s}, {s, s, 0}}};
            break;
        case 2: // -y
            corners = {{{0, 0, 0}, {0, 0, s}, {s, 0, s}, {s, 0, 0}}};
            break;
        case 3: // +y
            corners = {{{0, s, 0}, {s, s, 0}, {s, s, s}, {0, s, s}}};
            break;
        case 4: // -z
            corners = {{{0, 0, 0}, {s, 0, 0}, {s, s, 0}, {0, s, 0}}};
            break;
        default: // +z
            corners = {{{0, 0, s}, {0, s, s}, {s, s, s}, {s, 0, s}}};
            break;
        }
        std::array<std::uint32_t, 4> quad{};
        for (int q = 0; q < 4; ++q) {
            quad[static_cast<std::size_t>(q)] =
                node_at(fi + corners[static_cast<std::size_t>(q)][0],
                        fj + corners[static_cast<std::size_t>(q)][1],
                        fk + corners[static_cast<std::size_t>(q)][2]);
        }
        out.mesh.boundary_quads.push_back(quad);
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto id = idx(i, j, k);
                if (!is_fine[id]) {
                    // Coarse Kuhn cube spanning fine indices step 2.
                    emit_cube_tets(2 * i, 2 * j, 2 * k, 2);
                    ++out.n_coarse_cells;
                    for (int f = 0; f < 6; ++f) {
                        if (!inb(i + kFaceNbr[f][0], j + kFaceNbr[f][1],
                                 k + kFaceNbr[f][2])) {
                            emit_face_quads(2 * i, 2 * j, 2 * k, 2, f);
                        }
                    }
                } else {
                    if (is_feature[id]) {
                        ++out.n_feature_cells;
                    }
                    if (is_seed[id]) {
                        ++out.n_seed_cells;
                    }
                    // Local 2×2×2 fine Kuhn cubes.
                    for (int dk = 0; dk < 2; ++dk) {
                        for (int dj = 0; dj < 2; ++dj) {
                            for (int di = 0; di < 2; ++di) {
                                emit_cube_tets(2 * i + di, 2 * j + dj, 2 * k + dk, 1);
                                ++out.n_fine_cells;
                            }
                        }
                    }
                    // Exterior faces of the parent coarse cell → 2×2 fine quads.
                    for (int f = 0; f < 6; ++f) {
                        if (inb(i + kFaceNbr[f][0], j + kFaceNbr[f][1],
                                k + kFaceNbr[f][2])) {
                            continue;
                        }
                        // Sub-faces on the exterior side of the coarse cell.
                        for (int a = 0; a < 2; ++a) {
                            for (int b = 0; b < 2; ++b) {
                                int fi = 2 * i, fj = 2 * j, fk = 2 * k;
                                switch (f) {
                                case 0: // -x face: di fixed 0
                                    fi = 2 * i;
                                    fj = 2 * j + a;
                                    fk = 2 * k + b;
                                    break;
                                case 1: // +x
                                    fi = 2 * i + 1;
                                    fj = 2 * j + a;
                                    fk = 2 * k + b;
                                    break;
                                case 2: // -y
                                    fi = 2 * i + a;
                                    fj = 2 * j;
                                    fk = 2 * k + b;
                                    break;
                                case 3: // +y
                                    fi = 2 * i + a;
                                    fj = 2 * j + 1;
                                    fk = 2 * k + b;
                                    break;
                                case 4: // -z
                                    fi = 2 * i + a;
                                    fj = 2 * j + b;
                                    fk = 2 * k;
                                    break;
                                default: // +z
                                    fi = 2 * i + a;
                                    fj = 2 * j + b;
                                    fk = 2 * k + 1;
                                    break;
                                }
                                emit_face_quads(fi, fj, fk, 1, f);
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

    // Multi-pass surface snap (Jacobian-safe). Only boundary-touching tets.
    if (!out.mesh.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.mesh.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
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
