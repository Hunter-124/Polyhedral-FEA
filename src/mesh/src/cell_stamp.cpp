// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/cell_stamp.hpp"

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

} // namespace polymesh::mesh
