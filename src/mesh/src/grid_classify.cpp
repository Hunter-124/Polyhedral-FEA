// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/grid_classify.hpp"

#include "mesh/poly_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <vector>

namespace polymesh::mesh {
namespace {

void validate_h_bbox(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                     double h, const char* where) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError(std::format("{}: h must be positive and finite", where));
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0 || !extent.allFinite()) {
        throw ValidityError(std::format("{}: empty or non-finite bounding box", where));
    }
}

int cells_for_extent(double extent, double h) {
    // ceil(extent/h) with a tiny slack so exact divisions stay exact (e.g. 1/0.1).
    return std::max(1, static_cast<int>(std::ceil(extent / h - 1e-14)));
}

/// Merge nearly-equal ray hits so a shared edge / face diagonal between two
/// coplanar triangles contributes one crossing, not two (parity flip → void).
void dedupe_crossings(std::vector<double>& crossings, double zeps) {
    if (crossings.empty()) {
        return;
    }
    std::sort(crossings.begin(), crossings.end());
    std::size_t w = 1;
    for (std::size_t r = 1; r < crossings.size(); ++r) {
        if (crossings[r] - crossings[w - 1] > zeps) {
            crossings[w++] = crossings[r];
        }
    }
    crossings.resize(w);
}

std::vector<bool> classify_impl(const geom::TriSurface& surface, const CartesianGrid& grid,
                                int ray_axis) {
    const int a0 = (ray_axis + 1) % 3;
    const int a1 = (ray_axis + 2) % 3;
    // Lattice is always (nx,ny,nz) in xyz. Plane axes a0/a1, ray along ray_axis.
    std::vector<bool> inside(static_cast<std::size_t>(grid.cell_count()), false);

    const double char_len = std::max({grid.cell[0], grid.cell[1], grid.cell[2], 1.0});
    const double zeps = 1e-12 * char_len + 1e-9 * char_len;
    const double area_eps = 1e-30 * char_len * char_len;

    const int n_axis[3] = {grid.nx, grid.ny, grid.nz};
    const int ni = n_axis[a0];
    const int nj = n_axis[a1];
    const int nk = n_axis[ray_axis];

    for (int j = 0; j < nj; ++j) {
        for (int i = 0; i < ni; ++i) {
            const double c0 = grid.origin[a0] + (static_cast<double>(i) + 0.5) * grid.cell[a0];
            const double c1 = grid.origin[a1] + (static_cast<double>(j) + 0.5) * grid.cell[a1];

            std::vector<double> crossings;
            crossings.reserve(surface.triangles.size() / 8 + 4);
            for (const auto& tri : surface.triangles) {
                const Eigen::Vector3d& A = surface.vertices[tri[0]];
                const Eigen::Vector3d& B = surface.vertices[tri[1]];
                const Eigen::Vector3d& C = surface.vertices[tri[2]];
                const double d1 =
                    (B[a0] - A[a0]) * (c1 - A[a1]) - (B[a1] - A[a1]) * (c0 - A[a0]);
                const double d2 =
                    (C[a0] - B[a0]) * (c1 - B[a1]) - (C[a1] - B[a1]) * (c0 - B[a0]);
                const double d3 =
                    (A[a0] - C[a0]) * (c1 - C[a1]) - (A[a1] - C[a1]) * (c0 - C[a0]);
                const bool has_neg = d1 < 0.0 || d2 < 0.0 || d3 < 0.0;
                const bool has_pos = d1 > 0.0 || d2 > 0.0 || d3 > 0.0;
                if (has_neg && has_pos) {
                    continue;
                }
                const double area = d1 + d2 + d3;
                if (std::abs(area) <= area_eps) {
                    continue;
                }
                crossings.push_back((d2 * A[ray_axis] + d3 * B[ray_axis] + d1 * C[ray_axis]) /
                                    area);
            }
            dedupe_crossings(crossings, zeps);

            for (int k = 0; k < nk; ++k) {
                const double cs = grid.origin[ray_axis] +
                                  (static_cast<double>(k) + 0.5) * grid.cell[ray_axis];
                const auto below = std::upper_bound(crossings.begin(), crossings.end(), cs) -
                                   crossings.begin();
                if (below % 2 != 1) {
                    continue;
                }
                // Map (i,j,k) on (a0,a1,ray) back to (ix,iy,iz).
                int ix = 0, iy = 0, iz = 0;
                int* comps[3] = {&ix, &iy, &iz};
                *comps[a0] = i;
                *comps[a1] = j;
                *comps[ray_axis] = k;
                inside[grid.index(ix, iy, iz)] = true;
            }
        }
    }
    return inside;
}

} // namespace

CartesianGrid make_bbox_grid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                             double h, long max_cells) {
    validate_h_bbox(bbox_min, bbox_max, h, "make_bbox_grid");
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    CartesianGrid g;
    g.origin = bbox_min;
    g.nx = cells_for_extent(extent[0], h);
    g.ny = cells_for_extent(extent[1], h);
    g.nz = cells_for_extent(extent[2], h);
    g.cell[0] = extent[0] / static_cast<double>(g.nx);
    g.cell[1] = extent[1] / static_cast<double>(g.ny);
    g.cell[2] = extent[2] / static_cast<double>(g.nz);
    if (g.cell_count() > max_cells) {
        throw ValidityError("make_bbox_grid: grid too fine; increase element size");
    }
    return g;
}

CartesianGrid make_bbox_grid_even(const Eigen::Vector3d& bbox_min,
                                  const Eigen::Vector3d& bbox_max, double h, int min_cells,
                                  long max_cells) {
    validate_h_bbox(bbox_min, bbox_max, h, "make_bbox_grid_even");
    if (min_cells < 2) {
        min_cells = 2;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    CartesianGrid g;
    g.origin = bbox_min;
    auto even_n = [&](double ext) {
        int n = std::max(min_cells, cells_for_extent(ext, h));
        if (n % 2) {
            ++n;
        }
        return n;
    };
    g.nx = even_n(extent[0]);
    g.ny = even_n(extent[1]);
    g.nz = even_n(extent[2]);
    g.cell[0] = extent[0] / static_cast<double>(g.nx);
    g.cell[1] = extent[1] / static_cast<double>(g.ny);
    g.cell[2] = extent[2] / static_cast<double>(g.nz);
    if (g.cell_count() > max_cells) {
        throw ValidityError("make_bbox_grid_even: grid too fine; increase element size");
    }
    return g;
}

std::vector<bool> classify_cells_inside(const geom::TriSurface& surface,
                                        const CartesianGrid& grid) {
    return classify_impl(surface, grid, /*ray_axis=*/2);
}

std::vector<bool> classify_cells_inside_axis(const geom::TriSurface& surface,
                                             const CartesianGrid& grid, int ray_axis) {
    if (ray_axis < 0 || ray_axis > 2) {
        throw ValidityError("classify_cells_inside_axis: ray_axis must be 0, 1, or 2");
    }
    return classify_impl(surface, grid, ray_axis);
}

} // namespace polymesh::mesh
