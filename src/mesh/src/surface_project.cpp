// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace polymesh::mesh {
namespace {

// Ericson closest-point on triangle.
Eigen::Vector3d closest_on_triangle(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                                    const Eigen::Vector3d& b, const Eigen::Vector3d& c) {
    const Eigen::Vector3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return a;
    }
    const Eigen::Vector3d bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return a + ab * (d1 / (d1 - d3));
    }
    const Eigen::Vector3d cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return a + ac * (d2 / (d2 - d6));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

} // namespace

ClosestPoint closest_on_surface(const geom::TriSurface& surface, const Eigen::Vector3d& p) {
    ClosestPoint best;
    best.distance = std::numeric_limits<double>::infinity();
    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const auto& tri = surface.triangles[t];
        const Eigen::Vector3d q = closest_on_triangle(
            p, surface.vertices[tri[0]], surface.vertices[tri[1]], surface.vertices[tri[2]]);
        const double d = (p - q).norm();
        if (d < best.distance) {
            best.distance = d;
            best.point = q;
            best.triangle = t;
        }
    }
    return best;
}

ConformityStats surface_conformity(const geom::TriSurface& surface,
                                   const std::vector<Eigen::Vector3d>& points,
                                   const std::vector<std::uint32_t>& point_indices) {
    ConformityStats s;
    if (point_indices.empty()) {
        return s;
    }
    double sum = 0.0;
    for (auto i : point_indices) {
        const double d = closest_on_surface(surface, points[i]).distance;
        s.max_distance = std::max(s.max_distance, d);
        sum += d;
        ++s.count;
    }
    s.mean_distance = sum / static_cast<double>(s.count);
    return s;
}

SnapStats snap_boundary_nodes(const geom::TriSurface& surface,
                              std::vector<Eigen::Vector3d>& nodes,
                              const std::vector<std::uint32_t>& boundary_nodes, double h,
                              const CollectOffendersFn& collect_offenders, double max_move_frac,
                              int passes) {
    SnapStats stats;
    if (boundary_nodes.empty() || !(h > 0.0) || !std::isfinite(h) || !collect_offenders) {
        return stats;
    }
    max_move_frac = std::clamp(max_move_frac, 0.05, 0.85);
    passes = std::clamp(passes, 1, 6);
    const double max_total = max_move_frac * h;
    const double step_cap = max_total / static_cast<double>(passes);
    // Search radius: allow slightly beyond stair-case residual (~0.5√3 h).
    const double search_r = 1.5 * h;

    std::unordered_map<std::uint32_t, Eigen::Vector3d> original;
    original.reserve(boundary_nodes.size());
    std::unordered_map<std::uint32_t, double> moved;
    moved.reserve(boundary_nodes.size());

    stats.n_candidates = boundary_nodes.size();

    for (int pass = 0; pass < passes; ++pass) {
        for (auto ni : boundary_nodes) {
            if (ni >= nodes.size()) {
                continue;
            }
            const Eigen::Vector3d p = nodes[ni];
            const auto cp = closest_on_surface(surface, p);
            if (!(cp.distance > 1e-15) || cp.distance > search_r) {
                continue;
            }
            const double already = moved.count(ni) ? moved[ni] : 0.0;
            const double budget = max_total - already;
            if (budget <= 1e-15) {
                continue;
            }
            const double move = std::min({cp.distance, step_cap, budget});
            if (move <= 1e-15) {
                continue;
            }
            if (!original.count(ni)) {
                original.emplace(ni, p);
            }
            const Eigen::Vector3d delta = cp.point - p;
            nodes[ni] = p + delta * (move / cp.distance);
            moved[ni] = already + move;
        }
    }
    stats.n_moved = moved.size();

    // Unsnap offenders until the mesh is valid (or nothing left to unsnap).
    bool progressed = true;
    while (progressed && !original.empty()) {
        progressed = false;
        std::set<std::uint32_t> offenders;
        collect_offenders(offenders);
        for (const auto ni : offenders) {
            const auto it = original.find(ni);
            if (it == original.end()) {
                continue;
            }
            nodes[ni] = it->second;
            original.erase(it);
            moved.erase(ni);
            ++stats.n_unsnapped;
            progressed = true;
        }
    }

    stats.max_residual = 0.0;
    for (auto ni : boundary_nodes) {
        if (ni >= nodes.size()) {
            continue;
        }
        stats.max_residual =
            std::max(stats.max_residual, closest_on_surface(surface, nodes[ni]).distance);
    }
    return stats;
}

} // namespace polymesh::mesh
