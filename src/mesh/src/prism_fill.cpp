// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/prism_fill.hpp"

#include "mesh/grid_classify.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>

namespace polymesh::mesh {
namespace {

double tet_signed_volume_impl(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                              const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

// Linear prism 0,1,2 bottom / 3,4,5 top → three tets (positive when base is CCW
// from the top and extrusion is outward from the base plane).
double prism_signed_volume_impl(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                                const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                                const Eigen::Vector3d& p4, const Eigen::Vector3d& p5) {
    return tet_signed_volume_impl(p0, p1, p2, p4) + tet_signed_volume_impl(p0, p2, p3, p4) +
           tet_signed_volume_impl(p2, p3, p4, p5);
}

int pick_sweep_axis(const Eigen::Vector3d& extent) {
    // Longest side; ties prefer z (2), then y (1), then x (0).
    int axis = 2;
    double best = extent[2];
    if (extent[1] > best + 1e-15) {
        best = extent[1];
        axis = 1;
    }
    if (extent[0] > best + 1e-15) {
        axis = 0;
    }
    return axis;
}

} // namespace

double prism_signed_volume(const Eigen::Vector3d& p0, const Eigen::Vector3d& p1,
                           const Eigen::Vector3d& p2, const Eigen::Vector3d& p3,
                           const Eigen::Vector3d& p4, const Eigen::Vector3d& p5) {
    return prism_signed_volume_impl(p0, p1, p2, p3, p4, p5);
}

void check_prism_fill_geometry(const PrismFillOutput& out, double min_volume) {
    for (std::size_t e = 0; e < out.prisms.size(); ++e) {
        const auto& n = out.prisms[e];
        for (const auto idx : n) {
            if (idx >= out.nodes.size()) {
                throw ValidityError(
                    std::format("check_prism_fill_geometry: prism {} bad node index", e));
            }
            if (!out.nodes[idx].allFinite()) {
                throw ValidityError(
                    std::format("check_prism_fill_geometry: prism {} non-finite node", e));
            }
        }
        const double v =
            prism_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]], out.nodes[n[2]],
                                     out.nodes[n[3]], out.nodes[n[4]], out.nodes[n[5]]);
        if (v <= min_volume) {
            throw ValidityError(std::format(
                "check_prism_fill_geometry: prism {} non-positive volume {:.3e}", e, v));
        }
    }
}

PrismFillOutput prism_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h) {
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h);
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    const int sweep = pick_sweep_axis(extent);
    const int a0 = (sweep + 1) % 3; // base axis 0
    const int a1 = (sweep + 2) % 3; // base axis 1

    // Classify in xyz, then view lattice as (i,j,k)=(a0,a1,sweep).
    const auto inside_xyz = classify_cells_inside_axis(surface, grid, sweep);
    const int n_xyz[3] = {grid.nx, grid.ny, grid.nz};
    const int ni = n_xyz[a0], nj = n_xyz[a1], nk = n_xyz[sweep];

    const auto to_xyz = [&](int i, int j, int k) {
        int ix = 0, iy = 0, iz = 0;
        int* comps[3] = {&ix, &iy, &iz};
        *comps[a0] = i;
        *comps[a1] = j;
        *comps[sweep] = k;
        return std::array<int, 3>{ix, iy, iz};
    };
    const auto cell_index = [&](int i, int j, int k) {
        const auto xyz = to_xyz(i, j, k);
        return grid.index(xyz[0], xyz[1], xyz[2]);
    };
    const auto is_inside = [&](int i, int j, int k) {
        if (i < 0 || i >= ni || j < 0 || j >= nj || k < 0 || k >= nk) {
            return false;
        }
        return inside_xyz[cell_index(i, j, k)];
    };

    PrismFillOutput out;
    out.h = grid.max_edge();
    out.sweep_axis = sweep;
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            const auto xyz = to_xyz(i, j, k);
            out.nodes.push_back(grid.node(xyz[0], xyz[1], xyz[2]));
        }
        return it->second;
    };

    // Each inside voxel → 2 prisms (base quad diagonal i,j → i+1,j+1).
    // Hex corners in (i,j,k) lattice:
    //   0 (i,j,k)  1 (i+1,j,k)  2 (i+1,j+1,k)  3 (i,j+1,k)
    //   4 (i,j,k+1) 5 (i+1,j,k+1) 6 (i+1,j+1,k+1) 7 (i,j+1,k+1)
    // Prism A: bottom 0,1,2  top 4,5,6
    // Prism B: bottom 0,2,3  top 4,6,7
    for (int k = 0; k < nk; ++k) {
        for (int j = 0; j < nj; ++j) {
            for (int i = 0; i < ni; ++i) {
                if (!is_inside(i, j, k)) {
                    continue;
                }
                const std::array<std::uint32_t, 8> c{{
                    node_at(i, j, k),
                    node_at(i + 1, j, k),
                    node_at(i + 1, j + 1, k),
                    node_at(i, j + 1, k),
                    node_at(i, j, k + 1),
                    node_at(i + 1, j, k + 1),
                    node_at(i + 1, j + 1, k + 1),
                    node_at(i, j + 1, k + 1),
                }};

                auto push_prism = [&](std::array<std::uint32_t, 6> n) {
                    double v = prism_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                        out.nodes[n[2]], out.nodes[n[3]],
                                                        out.nodes[n[4]], out.nodes[n[5]]);
                    if (v < 0.0) {
                        // Flip base and top triangle orientation.
                        std::swap(n[1], n[2]);
                        std::swap(n[4], n[5]);
                        v = -v;
                    }
                    if (v > 0.0) {
                        out.prisms.push_back(n);
                    }
                };
                push_prism({c[0], c[1], c[2], c[4], c[5], c[6]});
                push_prism({c[0], c[2], c[3], c[4], c[6], c[7]});

                // Boundary quads on the voxel lattice (same as hex fill).
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
                        const auto& corner = f.corners[static_cast<std::size_t>(q)];
                        quad[static_cast<std::size_t>(q)] =
                            node_at(i + corner[0], j + corner[1], k + corner[2]);
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }

    if (out.prisms.empty()) {
        throw ValidityError("prism_fill_surface: no interior cells (empty or open surface?)");
    }
    return out;
}

} // namespace polymesh::mesh
