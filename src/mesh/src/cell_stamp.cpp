// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/cell_stamp.hpp"

#include "geom/indicators.hpp"

#include <algorithm>
#include <cmath>

namespace polymesh::mesh {
namespace {

std::size_t flat_idx(int i, int j, int k, int nx, int ny) {
    return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
            static_cast<std::size_t>(j)) *
               static_cast<std::size_t>(nx) +
           static_cast<std::size_t>(i);
}

void stamp_ball(std::vector<char>& is_marked, std::vector<char>* tag_out, int nx, int ny,
                int nz, const CartesianGrid& grid, const Eigen::Vector3d& p, double band2,
                int r) {
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
                    const auto id = flat_idx(i, j, k, nx, ny);
                    is_marked[id] = 1;
                    if (tag_out) {
                        (*tag_out)[id] = 1;
                    }
                }
            }
        }
    }
}

} // namespace

void stamp_seed_cells(std::vector<char>& is_marked, std::vector<char>* is_seed_out, int nx,
                      int ny, int nz, const CartesianGrid& grid,
                      std::span<const Eigen::Vector3d> seeds, double seed_band) {
    if (!(seed_band > 0.0) || seeds.empty() || nx < 1) {
        return;
    }
    const double band2 = seed_band * seed_band;
    const double h_ref = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(seed_band / h_ref)) + 1);
    for (const auto& seed : seeds) {
        stamp_ball(is_marked, is_seed_out, nx, ny, nz, grid, seed, band2, r);
    }
}

void stamp_feature_cells(std::vector<char>& is_marked, std::vector<char>* is_feature_out,
                         int nx, int ny, int nz, const CartesianGrid& grid,
                         const geom::TriSurface& surface,
                         std::span<const geom::SharpEdge> features, double feature_band) {
    if (!(feature_band > 0.0) || features.empty() || nx < 1) {
        return;
    }
    const double band2 = feature_band * feature_band;
    const double h_ref = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1e-30});
    const int r = std::max(1, static_cast<int>(std::ceil(feature_band / h_ref)) + 1);
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
            stamp_ball(is_marked, is_feature_out, nx, ny, nz, grid, (1.0 - t) * a + t * b,
                       band2, r);
        }
    }
}

void stamp_curvature_cells(std::vector<char>& is_l1, std::vector<char>* is_l2,
                           std::vector<char>* is_tag_out, int nx, int ny, int nz,
                           const CartesianGrid& grid, const geom::TriSurface& surface,
                           double max_turn_rad) {
    if (!(max_turn_rad > 0.0) || nx < 1 || surface.triangles.empty()) {
        return;
    }
    const auto curv = geom::estimate_vertex_curvature(surface);
    if (curv.kappa.size() < surface.vertices.size()) {
        return;
    }
    const double h_ref = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1e-30});

    // Mark the 3³ cell block around p — one-cell band each side of the wall so
    // the refined shell has thickness comparable to the old 1.6–2 h seed balls.
    const auto mark_block = [&](const Eigen::Vector3d& p, bool l2) {
        const Eigen::Vector3d local = p - grid.origin;
        const int i0 = static_cast<int>(std::floor(local[0] / grid.cell[0]));
        const int j0 = static_cast<int>(std::floor(local[1] / grid.cell[1]));
        const int k0 = static_cast<int>(std::floor(local[2] / grid.cell[2]));
        for (int dk = -1; dk <= 1; ++dk) {
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    const int i = i0 + di, j = j0 + dj, k = k0 + dk;
                    if (i < 0 || i >= nx || j < 0 || j >= ny || k < 0 || k >= nz) {
                        continue;
                    }
                    const auto id = flat_idx(i, j, k, nx, ny);
                    is_l1[id] = 1;
                    if (l2 && is_l2) {
                        (*is_l2)[id] = 1;
                    }
                    if (is_tag_out) {
                        (*is_tag_out)[id] = 1;
                    }
                }
            }
        }
    };

    // κ ≈ 2|H|: |H| averages the two principal curvatures, so a cylinder wall
    // (κ_max, 0) reads half its true bending — double to drive by κ_max.
    const auto turn_of = [&](double kappa_mean_abs) { return h_ref * 2.0 * kappa_mean_abs; };

    for (const auto& tri : surface.triangles) {
        const Eigen::Vector3d& a = surface.vertices[tri[0]];
        const Eigen::Vector3d& b = surface.vertices[tri[1]];
        const Eigen::Vector3d& c = surface.vertices[tri[2]];
        const double ka = curv.kappa[tri[0]];
        const double kb = curv.kappa[tri[1]];
        const double kc = curv.kappa[tri[2]];
        // Cheap reject: even the sharpest corner of this facet turns too little.
        if (turn_of(std::max({ka, kb, kc})) <= max_turn_rad) {
            continue;
        }
        // Barycentric sampling at ~half-cell spacing (min 1 interior sample).
        const double emax =
            std::max({(a - b).norm(), (a - c).norm(), (b - c).norm(), 1e-30});
        const int n = std::clamp(static_cast<int>(std::ceil(emax / (0.5 * h_ref))), 1, 64);
        for (int u = 0; u <= n; ++u) {
            for (int v = 0; v <= n - u; ++v) {
                const double fu = static_cast<double>(u) / static_cast<double>(n);
                const double fv = static_cast<double>(v) / static_cast<double>(n);
                const double fw = 1.0 - fu - fv;
                const double kappa = fu * ka + fv * kb + fw * kc;
                const double turn = turn_of(kappa);
                if (turn <= max_turn_rad) {
                    continue;
                }
                mark_block(fu * a + fv * b + fw * c, turn > 2.0 * max_turn_rad);
            }
        }
    }
}

} // namespace polymesh::mesh
