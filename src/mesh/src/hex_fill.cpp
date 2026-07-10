// SPDX-License-Identifier: BSD-3-Clause
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

// Isoparametric hex8 corner signs (fea reference: ±1 cube).
constexpr std::array<std::array<double, 3>, 8> kHexCornerSigns{{
    {{-1, -1, -1}},
    {{1, -1, -1}},
    {{1, 1, -1}},
    {{-1, 1, -1}},
    {{-1, -1, 1}},
    {{1, -1, 1}},
    {{1, 1, 1}},
    {{-1, 1, 1}},
}};

double hex8_jac_det(const std::array<Eigen::Vector3d, 8>& x, const Eigen::Vector3d& xi) {
    Eigen::Matrix3d jac = Eigen::Matrix3d::Zero();
    for (int a = 0; a < 8; ++a) {
        const double sx = kHexCornerSigns[static_cast<std::size_t>(a)][0];
        const double sy = kHexCornerSigns[static_cast<std::size_t>(a)][1];
        const double sz = kHexCornerSigns[static_cast<std::size_t>(a)][2];
        const double dxi = 0.125 * sx * (1.0 + sy * xi[1]) * (1.0 + sz * xi[2]);
        const double deta = 0.125 * sy * (1.0 + sx * xi[0]) * (1.0 + sz * xi[2]);
        const double dzeta = 0.125 * sz * (1.0 + sx * xi[0]) * (1.0 + sy * xi[1]);
        const auto& xa = x[static_cast<std::size_t>(a)];
        jac(0, 0) += dxi * xa[0];
        jac(0, 1) += dxi * xa[1];
        jac(0, 2) += dxi * xa[2];
        jac(1, 0) += deta * xa[0];
        jac(1, 1) += deta * xa[1];
        jac(1, 2) += deta * xa[2];
        jac(2, 0) += dzeta * xa[0];
        jac(2, 1) += dzeta * xa[1];
        jac(2, 2) += dzeta * xa[2];
    }
    return jac.determinant();
}

bool hex_inverted(const std::array<std::uint32_t, 8>& hx,
                  const std::vector<Eigen::Vector3d>& nodes) {
    std::array<Eigen::Vector3d, 8> x{};
    for (int i = 0; i < 8; ++i) {
        x[static_cast<std::size_t>(i)] = nodes[hx[static_cast<std::size_t>(i)]];
    }
    if (hex8_jac_det(x, Eigen::Vector3d::Zero()) <= 0.0) {
        return true;
    }
    // 2×2×2 Gauss points (±1/√3) — same safety net as transition_fill.
    static constexpr double g = 0.5773502691896257;
    static constexpr std::array<std::array<double, 3>, 8> gps{{
        {{-g, -g, -g}}, {{g, -g, -g}}, {{-g, g, -g}}, {{g, g, -g}},
        {{-g, -g, g}},  {{g, -g, g}},  {{-g, g, g}},  {{g, g, g}},
    }};
    for (const auto& gp : gps) {
        if (hex8_jac_det(x, Eigen::Vector3d(gp[0], gp[1], gp[2])) <= 0.0) {
            return true;
        }
    }
    return false;
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
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        out.boundary_max_distance =
            snap_boundary_nodes(
                surface, out.nodes, bnodes, out.h,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& hx : out.hexes) {
                        if (!hex_inverted(hx, out.nodes)) {
                            continue;
                        }
                        offenders.insert(hx.begin(), hx.end());
                    }
                },
                /*max_move_frac=*/0.75, /*passes=*/4)
                .max_residual;
    }
    return out;
}

} // namespace polymesh::mesh
