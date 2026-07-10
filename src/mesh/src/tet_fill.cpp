// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/tet_fill.hpp"

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

double tet_signed_volume_impl(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                              const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

} // namespace

double tet_signed_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                         const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    return tet_signed_volume_impl(a, b, c, d);
}

void check_tet_fill_geometry(const TetFillOutput& out, double min_volume) {
    for (std::size_t e = 0; e < out.tets.size(); ++e) {
        const auto& n = out.tets[e];
        for (const auto idx : n) {
            if (idx >= out.nodes.size()) {
                throw ValidityError(
                    std::format("check_tet_fill_geometry: tet {} bad node index", e));
            }
            if (!out.nodes[idx].allFinite()) {
                throw ValidityError(
                    std::format("check_tet_fill_geometry: tet {} non-finite node", e));
            }
        }
        const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                out.nodes[n[2]], out.nodes[n[3]]);
        if (v <= min_volume) {
            throw ValidityError(std::format(
                "check_tet_fill_geometry: tet {} non-positive volume {:.3e}", e, v));
        }
    }
}

TetFillOutput tet_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h, bool snap_boundary) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("tet_fill_surface: h must be positive and finite");
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("tet_fill_surface: empty bounding box");
    }

    const auto cells = [&](int axis) {
        return std::max(1, static_cast<int>(std::ceil(extent[axis] / h)));
    };
    const int nx = cells(0), ny = cells(1), nz = cells(2);
    if (static_cast<long>(nx) * ny * nz > 512 * 1024) {
        throw ValidityError("tet_fill_surface: grid too fine; increase element size");
    }

    const Eigen::Vector3d origin = bbox_min;
    std::vector<bool> inside(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) *
                                 static_cast<std::size_t>(nz),
                             false);
    const auto cell_index = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    };

    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double cx = origin[0] + (i + 0.5) * h;
            const double cy = origin[1] + (j + 0.5) * h;
            std::vector<double> crossings;
            crossings.reserve(surface.triangles.size() / 8 + 4);
            for (const auto& tri : surface.triangles) {
                const Eigen::Vector3d& a = surface.vertices[tri[0]];
                const Eigen::Vector3d& b = surface.vertices[tri[1]];
                const Eigen::Vector3d& c = surface.vertices[tri[2]];
                const double d1 = (b[0] - a[0]) * (cy - a[1]) - (b[1] - a[1]) * (cx - a[0]);
                const double d2 = (c[0] - b[0]) * (cy - b[1]) - (c[1] - b[1]) * (cx - b[0]);
                const double d3 = (a[0] - c[0]) * (cy - c[1]) - (a[1] - c[1]) * (cx - c[0]);
                const bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
                const bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (has_neg && has_pos) {
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

    TetFillOutput out;
    out.h = h;
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.nodes.size()));
        if (fresh) {
            out.nodes.emplace_back(origin[0] + static_cast<double>(i) * h,
                                   origin[1] + static_cast<double>(j) * h,
                                   origin[2] + static_cast<double>(k) * h);
        }
        return it->second;
    };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[cell_index(i, j, k)];
    };

    // Corner numbering matches hex8 convention.
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

                static constexpr std::array<std::array<int, 4>, 6> tets{{
                    {{0, 1, 2, 6}},
                    {{0, 2, 3, 6}},
                    {{0, 1, 5, 6}},
                    {{0, 3, 7, 6}},
                    {{0, 4, 5, 6}},
                    {{0, 4, 7, 6}},
                }};
                for (const auto& t : tets) {
                    std::array<std::uint32_t, 4> n{{c[static_cast<std::size_t>(t[0])],
                                                    c[static_cast<std::size_t>(t[1])],
                                                    c[static_cast<std::size_t>(t[2])],
                                                    c[static_cast<std::size_t>(t[3])]}};
                    const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                            out.nodes[n[2]], out.nodes[n[3]]);
                    if (v < 0.0) {
                        std::swap(n[1], n[2]);
                    } else if (v == 0.0) {
                        continue;
                    }
                    out.tets.push_back(n);
                }

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

    if (out.tets.empty()) {
        throw ValidityError("tet_fill_surface: no interior cells (empty or open surface?)");
    }

    if (snap_boundary && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(q.begin(), q.end());
        }
        // Limited snap: pull toward the surface by at most 0.35 h. Full projection
        // can invert tets; Jacobian safety below unsnaps offenders (ADR-0015/B3).
        std::unordered_map<std::uint32_t, Eigen::Vector3d> pre_snap;
        pre_snap.reserve(bnodes.size());
        for (auto ni : bnodes) {
            const Eigen::Vector3d p = out.nodes[ni];
            const auto cp = closest_on_surface(surface, p);
            if (cp.distance > 0.0 && cp.distance < 1.25 * h) {
                const Eigen::Vector3d delta = cp.point - p;
                const double move = std::min(cp.distance, 0.35 * h);
                pre_snap.emplace(ni, p);
                out.nodes[ni] = p + delta * (move / cp.distance);
            }
        }

        // Unsnap any still-moved node that participates in a non-positive tet.
        // Revert iterates to a fixed point: undoing one node can rescue others.
        const double vol_eps = 1e-14 * h * h * h;
        bool progressed = true;
        while (progressed && !pre_snap.empty()) {
            progressed = false;
            std::set<std::uint32_t> offenders;
            for (const auto& n : out.tets) {
                const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                        out.nodes[n[2]], out.nodes[n[3]]);
                if (v > vol_eps) {
                    continue;
                }
                for (const auto idx : n) {
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

        // Orientation only (connectivity unchanged). Unsnap restores lattice
        // positivity; residual negatives should only be FP noise on orientation.
        for (auto& n : out.tets) {
            const double v = tet_signed_volume_impl(out.nodes[n[0]], out.nodes[n[1]],
                                                    out.nodes[n[2]], out.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
        check_tet_fill_geometry(out, vol_eps);
        return out;
    }

    check_tet_fill_geometry(out);
    return out;
}

} // namespace polymesh::mesh
