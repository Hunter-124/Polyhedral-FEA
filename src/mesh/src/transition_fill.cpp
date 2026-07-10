// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/transition_fill.hpp"

#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <set>

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

} // namespace

TransitionFillOutput transition_fill_surface(const geom::TriSurface& surface,
                                             const Eigen::Vector3d& bbox_min,
                                             const Eigen::Vector3d& bbox_max, double h,
                                             bool snap_boundary) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("transition_fill_surface: h must be positive");
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("transition_fill_surface: empty bbox");
    }
    const auto cells = [&](int a) {
        return std::max(1, static_cast<int>(std::ceil(extent[a] / h)));
    };
    const int nx = cells(0), ny = cells(1), nz = cells(2);
    if (static_cast<long>(nx) * ny * nz > 512 * 1024) {
        throw ValidityError("transition_fill_surface: grid too fine");
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
                    const Eigen::Vector3d center =
                        origin + Eigen::Vector3d((i + 0.5) * h, (j + 0.5) * h, (k + 0.5) * h);
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
    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(q.begin(), q.end());
        }
        for (auto ni : bnodes) {
            const auto cp = closest_on_surface(surface, out.nodes[ni]);
            if (cp.distance > 0.0 && cp.distance < 1.25 * h) {
                const Eigen::Vector3d delta = cp.point - out.nodes[ni];
                const double move = std::min(cp.distance, 0.35 * h);
                out.nodes[ni] += delta * (move / cp.distance);
            }
        }
        // Residual distance after snap (metres).
        double max_d = 0.0;
        for (auto ni : bnodes) {
            max_d = std::max(max_d, closest_on_surface(surface, out.nodes[ni]).distance);
        }
        out.boundary_max_distance = max_d;
    }
    return out;
}

} // namespace polymesh::mesh
