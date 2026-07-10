// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/mixed_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <queue>
#include <set>

namespace polymesh::mesh {
namespace {

constexpr std::array<std::array<int, 4>, 6> kHexFaces{{
    {{0, 3, 2, 1}},
    {{4, 5, 6, 7}},
    {{0, 1, 5, 4}},
    {{2, 3, 7, 6}},
    {{0, 4, 7, 3}},
    {{1, 2, 6, 5}},
}};

constexpr std::array<std::array<int, 3>, 6> kFaceNbr{{
    {{0, 0, -1}},
    {{0, 0, 1}},
    {{0, -1, 0}},
    {{0, 1, 0}},
    {{-1, 0, 0}},
    {{1, 0, 0}},
}};

constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
    {{0, 1, 2, 6}},
    {{0, 2, 3, 6}},
    {{0, 1, 5, 6}},
    {{0, 3, 7, 6}},
    {{0, 4, 5, 6}},
    {{0, 4, 7, 6}},
}};

constexpr std::array<std::array<double, 3>, 8> kHexCornerSigns{{
    {{-1, -1, -1}}, {{1, -1, -1}}, {{1, 1, -1}}, {{-1, 1, -1}},
    {{-1, -1, 1}},  {{1, -1, 1}},  {{1, 1, 1}},  {{-1, 1, 1}},
}};

double tet_vol(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
               const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

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

bool tet_inverted(const std::array<std::uint32_t, 4>& n,
                  const std::vector<Eigen::Vector3d>& nodes, double vol_eps) {
    return tet_vol(nodes[n[0]], nodes[n[1]], nodes[n[2]], nodes[n[3]]) <= vol_eps;
}

} // namespace

MixedFillOutput mixed_fill_surface(const geom::TriSurface& surface,
                                   const Eigen::Vector3d& bbox_min,
                                   const Eigen::Vector3d& bbox_max, double h,
                                   int skin_layers, std::span<const geom::SharpEdge> features,
                                   double feature_band,
                                   std::span<const Eigen::Vector3d> curvature_seeds,
                                   double seed_band, bool snap_boundary) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("mixed_fill_surface: h must be positive");
    }
    if (skin_layers < 1) {
        skin_layers = 1;
    }
    if (!(feature_band > 0.0) || features.empty()) {
        feature_band = 0.0;
    }
    if (!(seed_band > 0.0) || curvature_seeds.empty()) {
        seed_band = 0.0;
    }

    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h);
    const auto inside = classify_cells_inside(surface, grid);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double h_cell = grid.max_edge();
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz && inside[idx(i, j, k)];
    };

    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
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

    MixedFillOutput out;
    out.h = h_cell;
    out.skin_layers = skin_layers;
    std::vector<char> is_skin(inside.size(), 0);
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const int d = dist[idx(i, j, k)];
                bool skin = (d >= 0 && d < skin_layers);
                const Eigen::Vector3d center = grid.cell_center(i, j, k);
                if (!skin && feature_band > 0.0) {
                    if (geom::distance_to_features(center, surface, features) <= feature_band) {
                        skin = true;
                        ++out.n_feature_skin_cells;
                    }
                }
                if (!skin && seed_band > 0.0) {
                    for (const auto& s : curvature_seeds) {
                        if ((center - s).squaredNorm() <= seed_band * seed_band) {
                            skin = true;
                            ++out.n_feature_skin_cells;
                            break;
                        }
                    }
                }
                is_skin[idx(i, j, k)] = skin ? 1 : 0;
            }
        }
    }

    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };

    auto corners = [&](int i, int j, int k) -> std::array<std::uint32_t, 8> {
        return {{
            node_at(i, j, k),
            node_at(i + 1, j, k),
            node_at(i + 1, j + 1, k),
            node_at(i, j + 1, k),
            node_at(i, j, k + 1),
            node_at(i + 1, j, k + 1),
            node_at(i + 1, j + 1, k + 1),
            node_at(i, j + 1, k + 1),
        }};
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto c = corners(i, j, k);
                if (!is_skin[idx(i, j, k)]) {
                    MixedCell hx;
                    hx.kind = MixedCellKind::kHex8;
                    hx.n_nodes = 8;
                    hx.nodes = c;
                    out.cells.push_back(hx);
                    ++out.n_hex;
                } else {
                    for (const auto& t : kCubeTets) {
                        MixedCell tet;
                        tet.kind = MixedCellKind::kTet4;
                        tet.n_nodes = 4;
                        tet.nodes[0] = c[static_cast<std::size_t>(t[0])];
                        tet.nodes[1] = c[static_cast<std::size_t>(t[1])];
                        tet.nodes[2] = c[static_cast<std::size_t>(t[2])];
                        tet.nodes[3] = c[static_cast<std::size_t>(t[3])];
                        double v =
                            tet_vol(out.nodes[tet.nodes[0]], out.nodes[tet.nodes[1]],
                                    out.nodes[tet.nodes[2]], out.nodes[tet.nodes[3]]);
                        if (v < 0.0) {
                            std::swap(tet.nodes[1], tet.nodes[2]);
                            v = -v;
                        }
                        if (v > 0.0) {
                            out.cells.push_back(tet);
                            ++out.n_tet;
                        }
                    }
                }

                for (std::size_t f = 0; f < 6; ++f) {
                    const auto& o = kFaceNbr[f];
                    if (inb(i + o[0], j + o[1], k + o[2])) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int qn = 0; qn < 4; ++qn) {
                        quad[static_cast<std::size_t>(qn)] = c[static_cast<std::size_t>(
                            kHexFaces[f][static_cast<std::size_t>(qn)])];
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }

    if (out.cells.empty()) {
        throw ValidityError("mixed_fill_surface: no interior cells");
    }

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
        const double vol_eps = 1e-14 * h_cell * h_cell * h_cell;
        out.boundary_max_distance =
            snap_boundary_nodes(
                surface, out.nodes, bnodes, h_cell,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : out.cells) {
                        bool bad = false;
                        if (cell.kind == MixedCellKind::kTet4) {
                            bad = tet_inverted({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                                cell.nodes[3]},
                                               out.nodes, vol_eps);
                        } else {
                            bad = hex_inverted(
                                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3],
                                 cell.nodes[4], cell.nodes[5], cell.nodes[6], cell.nodes[7]},
                                out.nodes);
                        }
                        if (!bad) {
                            continue;
                        }
                        for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                            offenders.insert(cell.nodes[m]);
                        }
                    }
                },
                /*max_move_frac=*/0.9, /*passes=*/6)
                .max_residual;
        for (auto& cell : out.cells) {
            if (cell.kind != MixedCellKind::kTet4) {
                continue;
            }
            const double v = tet_vol(out.nodes[cell.nodes[0]], out.nodes[cell.nodes[1]],
                                     out.nodes[cell.nodes[2]], out.nodes[cell.nodes[3]]);
            if (v < 0.0) {
                std::swap(cell.nodes[1], cell.nodes[2]);
            }
        }
    }
    return out;
}

MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill) {
    MixedFillOutput out;
    out.h = fill.h;
    out.boundary_quads = fill.boundary_quads;
    out.boundary_max_distance = fill.boundary_max_distance;
    out.skin_layers = fill.skin_layers;
    out.n_feature_skin_cells = fill.n_feature_skin_cells;
    out.nodes = fill.nodes;
    out.n_hex = 0;
    out.n_pyramid = 0;
    out.n_tet = 0;
    out.cells.reserve(fill.cells.size() + 5 * fill.n_hex);

    for (const auto& cell : fill.cells) {
        if (cell.kind == MixedCellKind::kTet4) {
            out.cells.push_back(cell);
            ++out.n_tet;
            continue;
        }
        if (cell.kind == MixedCellKind::kPyramid5) {
            out.cells.push_back(cell);
            ++out.n_pyramid;
            continue;
        }
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (int i = 0; i < 8; ++i) {
            center += out.nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        center /= 8.0;
        const auto apex = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(center);
        for (const auto& face : kHexFaces) {
            MixedCell pyr;
            pyr.kind = MixedCellKind::kPyramid5;
            pyr.n_nodes = 5;
            const Eigen::Vector3d& p0 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[0])]];
            const Eigen::Vector3d& p1 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[1])]];
            const Eigen::Vector3d& p2 =
                out.nodes[cell.nodes[static_cast<std::size_t>(face[2])]];
            const Eigen::Vector3d n = (p1 - p0).cross(p2 - p0);
            const bool apex_on_positive = n.dot(center - p0) > 0.0;
            if (apex_on_positive) {
                pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
                pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[1])];
                pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
                pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[3])];
            } else {
                pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
                pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[3])];
                pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
                pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[1])];
            }
            pyr.nodes[4] = apex;
            out.cells.push_back(pyr);
            ++out.n_pyramid;
        }
    }
    return out;
}

} // namespace polymesh::mesh
