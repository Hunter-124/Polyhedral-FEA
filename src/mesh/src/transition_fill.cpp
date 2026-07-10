// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/transition_fill.hpp"

#include "mesh/grid_classify.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <unordered_map>

namespace polymesh::mesh {
namespace {

// Hex face corners (local 0..7) outward for right-handed hex8.
constexpr std::array<std::array<int, 4>, 6> kHexFaces{{
    {{0, 3, 2, 1}}, // -z
    {{4, 5, 6, 7}}, // +z
    {{0, 1, 5, 4}}, // -y
    {{2, 3, 7, 6}}, // +y
    {{0, 4, 7, 3}}, // -x
    {{1, 2, 6, 5}}, // +x
}};

// Neighbor offsets matching face order above.
constexpr std::array<std::array<int, 3>, 6> kFaceNbr{{
    {{0, 0, -1}},
    {{0, 0, 1}},
    {{0, -1, 0}},
    {{0, 1, 0}},
    {{-1, 0, 0}},
    {{1, 0, 0}},
}};

// Hex8 corner (xi,eta,zeta) signs for trilinear shape functions.
constexpr std::array<std::array<double, 3>, 8> kHexCornerSigns{{{-1, -1, -1},
                                                                {1, -1, -1},
                                                                {1, 1, -1},
                                                                {-1, 1, -1},
                                                                {-1, -1, 1},
                                                                {1, -1, 1},
                                                                {1, 1, 1},
                                                                {-1, 1, 1}}};

// 2×2×2 Gauss points (1/√3) matching the product rule used by hex assembly.
constexpr double kG = 0.5773502691896257;
constexpr std::array<std::array<double, 3>, 8> kHexGaussPts{{{-kG, -kG, -kG},
                                                             {kG, -kG, -kG},
                                                             {-kG, kG, -kG},
                                                             {kG, kG, -kG},
                                                             {-kG, -kG, kG},
                                                             {kG, -kG, kG},
                                                             {-kG, kG, kG},
                                                             {kG, kG, kG}}};

double tet_signed_vol(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                      const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

/// Isoparametric hex8 Jacobian det at reference `xi` (fea convention:
/// J(r,c) = d x_c / d xi_r).
double hex8_jacobian_det(const std::array<Eigen::Vector3d, 8>& x, const Eigen::Vector3d& xi) {
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

/// True if cell would produce non-positive Jacobian / collapsed volume.
/// Hex: det J at centre + assembly Gauss points. Pyramid: |tet-split vols| > ε
/// (assembly may flip orientation).
bool cell_inverted(const TransitionCell& cell, const std::vector<Eigen::Vector3d>& nodes,
                   double vol_eps) {
    if (cell.kind == TransitionCellKind::kHex8) {
        std::array<Eigen::Vector3d, 8> x{};
        for (int i = 0; i < 8; ++i) {
            x[static_cast<std::size_t>(i)] = nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        if (hex8_jacobian_det(x, Eigen::Vector3d::Zero()) <= 0.0) {
            return true;
        }
        for (const auto& gp : kHexGaussPts) {
            if (hex8_jacobian_det(x, Eigen::Vector3d(gp[0], gp[1], gp[2])) <= 0.0) {
                return true;
            }
        }
        return false;
    }
    const auto& n = cell.nodes;
    const double v1 = tet_signed_vol(nodes[n[0]], nodes[n[1]], nodes[n[2]], nodes[n[4]]);
    const double v2 = tet_signed_vol(nodes[n[0]], nodes[n[2]], nodes[n[3]], nodes[n[4]]);
    return std::abs(v1) <= vol_eps || std::abs(v2) <= vol_eps;
}

} // namespace

TransitionFillOutput transition_fill_surface(const geom::TriSurface& surface,
                                             const Eigen::Vector3d& bbox_min,
                                             const Eigen::Vector3d& bbox_max, double h,
                                             bool snap_boundary) {
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h);
    const auto inside = classify_cells_inside(surface, grid);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double h_cell = grid.max_edge();

    const auto cell_index = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[cell_index(i, j, k)];
    };

    // Boundary cell = inside cell with any face neighbor outside.
    std::vector<bool> boundary(inside.size(), false);
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[cell_index(i, j, k)]) {
                    continue;
                }
                for (const auto& o : kFaceNbr) {
                    if (!is_inside(i + o[0], j + o[1], k + o[2])) {
                        boundary[cell_index(i, j, k)] = true;
                        break;
                    }
                }
            }
        }
    }

    TransitionFillOutput out;
    out.h = h_cell;
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[cell_index(i, j, k)]) {
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

                if (!boundary[cell_index(i, j, k)]) {
                    TransitionCell cell;
                    cell.kind = TransitionCellKind::kHex8;
                    cell.n_nodes = 8;
                    cell.nodes = c;
                    out.cells.push_back(cell);
                    ++out.n_hex;
                } else {
                    // Pyramid skin: apex at cell center (new node).
                    const Eigen::Vector3d center = grid.cell_center(i, j, k);
                    const auto apex = static_cast<std::uint32_t>(out.nodes.size());
                    out.nodes.push_back(center);
                    for (const auto& face : kHexFaces) {
                        TransitionCell pyr;
                        pyr.kind = TransitionCellKind::kPyramid5;
                        pyr.n_nodes = 5;
                        // Order base so the apex lies on the +normal side of the
                        // right-handed base winding. That matches the isoparametric
                        // pyramid reference (base zeta=-1, apex zeta=+1) and yields
                        // positive Jacobians for element_stiffness.
                        const Eigen::Vector3d& p0 =
                            out.nodes[c[static_cast<std::size_t>(face[0])]];
                        const Eigen::Vector3d& p1 =
                            out.nodes[c[static_cast<std::size_t>(face[1])]];
                        const Eigen::Vector3d& p2 =
                            out.nodes[c[static_cast<std::size_t>(face[2])]];
                        const Eigen::Vector3d n = (p1 - p0).cross(p2 - p0);
                        const bool apex_on_positive = n.dot(center - p0) > 0.0;
                        if (apex_on_positive) {
                            pyr.nodes[0] = c[static_cast<std::size_t>(face[0])];
                            pyr.nodes[1] = c[static_cast<std::size_t>(face[1])];
                            pyr.nodes[2] = c[static_cast<std::size_t>(face[2])];
                            pyr.nodes[3] = c[static_cast<std::size_t>(face[3])];
                        } else {
                            pyr.nodes[0] = c[static_cast<std::size_t>(face[0])];
                            pyr.nodes[1] = c[static_cast<std::size_t>(face[3])];
                            pyr.nodes[2] = c[static_cast<std::size_t>(face[2])];
                            pyr.nodes[3] = c[static_cast<std::size_t>(face[1])];
                        }
                        pyr.nodes[4] = apex;
                        out.cells.push_back(pyr);
                        ++out.n_pyramid;
                    }
                }

                // Boundary quads for BC mapping (outer faces of the lattice).
                for (std::size_t f = 0; f < 6; ++f) {
                    const auto& o = kFaceNbr[f];
                    if (is_inside(i + o[0], j + o[1], k + o[2])) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int q = 0; q < 4; ++q) {
                        quad[static_cast<std::size_t>(q)] = c[static_cast<std::size_t>(
                            kHexFaces[f][static_cast<std::size_t>(q)])];
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }

    if (out.cells.empty()) {
        throw ValidityError("transition_fill_surface: no interior cells");
    }

    // Limited surface snap on free-boundary lattice nodes (not pyramid apices).
    // Jacobian safety (B3): unsnap any moved node that inverts a hex or pyramid
    // so no non-positive Jacobian element reaches the solver.
    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(q.begin(), q.end());
        }
        std::unordered_map<std::uint32_t, Eigen::Vector3d> pre_snap;
        pre_snap.reserve(bnodes.size());
        for (auto ni : bnodes) {
            const Eigen::Vector3d p = out.nodes[ni];
            const auto cp = closest_on_surface(surface, p);
            if (cp.distance > 0.0 && cp.distance < 1.25 * h_cell) {
                const Eigen::Vector3d delta = cp.point - p;
                const double move = std::min(cp.distance, 0.35 * h_cell);
                pre_snap.emplace(ni, p);
                out.nodes[ni] = p + delta * (move / cp.distance);
            }
        }

        const double vol_eps = 1e-14 * h_cell * h_cell * h_cell;
        bool progressed = true;
        while (progressed && !pre_snap.empty()) {
            progressed = false;
            std::set<std::uint32_t> offenders;
            for (const auto& cell : out.cells) {
                if (!cell_inverted(cell, out.nodes, vol_eps)) {
                    continue;
                }
                for (std::uint8_t i = 0; i < cell.n_nodes; ++i) {
                    const auto idx = cell.nodes[i];
                    if (pre_snap.find(idx) != pre_snap.end()) {
                        offenders.insert(idx);
                    }
                }
            }
            for (const auto ni : offenders) {
                const auto it = pre_snap.find(ni);
                if (it == pre_snap.end()) {
                    continue;
                }
                out.nodes[ni] = it->second;
                pre_snap.erase(it);
                progressed = true;
            }
        }

        // Residual distance after snap / unsnap (metres).
        double max_d = 0.0;
        for (auto ni : bnodes) {
            max_d = std::max(max_d, closest_on_surface(surface, out.nodes[ni]).distance);
        }
        out.boundary_max_distance = max_d;

        // Hard gate: still-inverted cells must not leave the mesher.
        for (std::size_t e = 0; e < out.cells.size(); ++e) {
            if (cell_inverted(out.cells[e], out.nodes, vol_eps)) {
                throw ValidityError(std::format(
                    "transition_fill_surface: cell {} non-positive Jacobian after snap", e));
            }
        }
    }
    return out;
}

TransitionFillOutput expand_hex_core_to_pyramids(const TransitionFillOutput& fill) {
    TransitionFillOutput out;
    out.h = fill.h;
    out.boundary_quads = fill.boundary_quads;
    out.boundary_max_distance = fill.boundary_max_distance;
    out.nodes = fill.nodes;
    out.cells.reserve(fill.n_pyramid + 6 * fill.n_hex);
    out.n_hex = 0;
    out.n_pyramid = 0;

    for (const auto& cell : fill.cells) {
        if (cell.kind == TransitionCellKind::kPyramid5) {
            out.cells.push_back(cell);
            ++out.n_pyramid;
            continue;
        }
        // Interior hex → six pyramids, same face order / orientation as the skin.
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (int i = 0; i < 8; ++i) {
            center += out.nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        center /= 8.0;
        const auto apex = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(center);
        for (const auto& face : kHexFaces) {
            TransitionCell pyr;
            pyr.kind = TransitionCellKind::kPyramid5;
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
