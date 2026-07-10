// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hex_fill.hpp"
#include "mesh/poly_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace polymesh::mesh {

HexFillOutput hex_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("hex_fill_surface: h must be positive and finite");
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("hex_fill_surface: empty bounding box");
    }
    const auto cells = [&](int axis) {
        return std::max(1, static_cast<int>(std::ceil(extent[axis] / h)));
    };
    const int nx = cells(0), ny = cells(1), nz = cells(2);
    if (static_cast<long>(nx) * ny * nz > 512 * 1024) {
        throw ValidityError("hex_fill_surface: grid too fine; increase element size");
    }
    const Eigen::Vector3d origin = bbox_min;
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
                if ((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0))
                    continue;
                const double area = d1 + d2 + d3;
                if (area == 0.0)
                    continue;
                crossings.push_back((d2 * a[2] + d3 * b[2] + d1 * c[2]) / area);
            }
            std::sort(crossings.begin(), crossings.end());
            for (int k = 0; k < nz; ++k) {
                const double cz = origin[2] + (k + 0.5) * h;
                const auto below = std::upper_bound(crossings.begin(), crossings.end(), cz) -
                                   crossings.begin();
                if (below % 2 == 1)
                    inside[cell_index(i, j, k)] = true;
            }
        }
    }
    HexFillOutput out;
    out.h = h;
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.emplace_back(origin[0] + i * h, origin[1] + j * h, origin[2] + k * h);
        }
        return it->second;
    };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[cell_index(i, j, k)];
    };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[cell_index(i, j, k)])
                    continue;
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
                    if (is_inside(i + f.di, j + f.dj, k + f.dk))
                        continue;
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
    return out;
}

} // namespace polymesh::mesh
