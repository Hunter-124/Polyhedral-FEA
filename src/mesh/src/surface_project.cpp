// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

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

/// Uniform grid hash over triangle AABBs for accelerated closest-point (S0).
struct SurfaceGrid {
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector3d cell = Eigen::Vector3d::Ones();
    int nx = 1, ny = 1, nz = 1;
    std::vector<std::vector<std::size_t>> bins;

    int flat(int i, int j, int k) const {
        return (k * ny + j) * nx + i;
    }

    void build(const geom::TriSurface& surface) {
        if (surface.triangles.empty() || surface.vertices.empty()) {
            bins.clear();
            return;
        }
        Eigen::Vector3d bmin = surface.vertices[0];
        Eigen::Vector3d bmax = surface.vertices[0];
        for (const auto& v : surface.vertices) {
            bmin = bmin.cwiseMin(v);
            bmax = bmax.cwiseMax(v);
        }
        // Pad slightly so boundary queries land inside the hash.
        const Eigen::Vector3d extent = (bmax - bmin).cwiseMax(Eigen::Vector3d::Constant(1e-12));
        const double pad = 1e-6 * extent.norm() + 1e-12;
        bmin.array() -= pad;
        bmax.array() += pad;

        // Target ~8 triangles per bin; clamp resolution.
        const std::size_t ntri = surface.triangles.size();
        const double ntri_d = static_cast<double>(std::max<std::size_t>(1, ntri / 8));
        const int target = std::max(4, static_cast<int>(std::cbrt(ntri_d)));
        const int res = std::clamp(target, 4, 64);
        nx = ny = nz = res;
        origin = bmin;
        cell = (bmax - bmin).cwiseQuotient(Eigen::Vector3d(nx, ny, nz));
        cell = cell.cwiseMax(Eigen::Vector3d::Constant(1e-30));
        bins.assign(static_cast<std::size_t>(nx * ny * nz), {});

        for (std::size_t t = 0; t < ntri; ++t) {
            const auto& tri = surface.triangles[t];
            const Eigen::Vector3d& A = surface.vertices[tri[0]];
            const Eigen::Vector3d& B = surface.vertices[tri[1]];
            const Eigen::Vector3d& C = surface.vertices[tri[2]];
            const Eigen::Vector3d tmin = A.cwiseMin(B).cwiseMin(C);
            const Eigen::Vector3d tmax = A.cwiseMax(B).cwiseMax(C);
            const Eigen::Vector3d lomin = (tmin - origin).cwiseQuotient(cell);
            const Eigen::Vector3d lomax = (tmax - origin).cwiseQuotient(cell);
            const int i0 = std::clamp(static_cast<int>(std::floor(lomin[0])), 0, nx - 1);
            const int j0 = std::clamp(static_cast<int>(std::floor(lomin[1])), 0, ny - 1);
            const int k0 = std::clamp(static_cast<int>(std::floor(lomin[2])), 0, nz - 1);
            const int i1 = std::clamp(static_cast<int>(std::floor(lomax[0])), 0, nx - 1);
            const int j1 = std::clamp(static_cast<int>(std::floor(lomax[1])), 0, ny - 1);
            const int k1 = std::clamp(static_cast<int>(std::floor(lomax[2])), 0, nz - 1);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        bins[static_cast<std::size_t>(flat(i, j, k))].push_back(t);
                    }
                }
            }
        }
    }

    ClosestPoint query(const geom::TriSurface& surface, const Eigen::Vector3d& p) const {
        ClosestPoint best;
        best.distance = std::numeric_limits<double>::infinity();
        if (bins.empty()) {
            return best;
        }

        auto consider = [&](std::size_t t) {
            const auto& tri = surface.triangles[t];
            const Eigen::Vector3d q = closest_on_triangle(
                p, surface.vertices[tri[0]], surface.vertices[tri[1]], surface.vertices[tri[2]]);
            const double d = (p - q).norm();
            if (d < best.distance) {
                best.distance = d;
                best.point = q;
                best.triangle = t;
            }
        };

        const Eigen::Vector3d local = (p - origin).cwiseQuotient(cell);
        const int ic = std::clamp(static_cast<int>(std::floor(local[0])), 0, nx - 1);
        const int jc = std::clamp(static_cast<int>(std::floor(local[1])), 0, ny - 1);
        const int kc = std::clamp(static_cast<int>(std::floor(local[2])), 0, nz - 1);

        // Expanding shell until the best distance cannot improve.
        const int max_r = std::max({nx, ny, nz});
        for (int r = 0; r <= max_r; ++r) {
            const int i0 = std::max(0, ic - r), i1 = std::min(nx - 1, ic + r);
            const int j0 = std::max(0, jc - r), j1 = std::min(ny - 1, jc + r);
            const int k0 = std::max(0, kc - r), k1 = std::min(nz - 1, kc + r);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        // Only the shell at radius r (avoid re-scanning inner cubes).
                        if (r > 0) {
                            const bool on_shell = (i == i0 || i == i1 || j == j0 || j == j1 ||
                                                   k == k0 || k == k1);
                            if (!on_shell) {
                                continue;
                            }
                        }
                        for (std::size_t t : bins[static_cast<std::size_t>(flat(i, j, k))]) {
                            consider(t);
                        }
                    }
                }
            }
            if (std::isfinite(best.distance)) {
                // Lower bound to any unvisited bin: distance to shell outside r.
                // Conservative: cell diagonal * (r+0.5) approx; stop if best is
                // closer than the nearest unexplored cell face.
                const double cell_diag = cell.norm();
                if (best.distance <= (static_cast<double>(r) + 0.5) * cell_diag * 0.5 ||
                    r >= 2) {
                    // After a few shells, also do a small safety ring once more then stop
                    // if distance is finite. For correctness under AABB over-approx,
                    // expand until best.distance^2 cannot beat next shell.
                    const double next_lb = static_cast<double>(r) * std::min({cell[0], cell[1], cell[2]});
                    if (r > 0 && best.distance <= next_lb) {
                        break;
                    }
                    if (r >= 4 && best.distance < cell_diag) {
                        break;
                    }
                }
            }
        }

        // Fallback brute force if hash failed (empty bins / degenerate).
        if (!std::isfinite(best.distance)) {
            for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
                consider(t);
            }
        }
        return best;
    }
};

// Thread-local cache: rebuild when surface pointer / triangle count changes.
const SurfaceGrid& grid_for(const geom::TriSurface& surface) {
    thread_local const geom::TriSurface* cached_ptr = nullptr;
    thread_local std::size_t cached_ntri = 0;
    thread_local std::size_t cached_nv = 0;
    thread_local SurfaceGrid cached;
    if (cached_ptr != &surface || cached_ntri != surface.triangles.size() ||
        cached_nv != surface.vertices.size()) {
        cached.build(surface);
        cached_ptr = &surface;
        cached_ntri = surface.triangles.size();
        cached_nv = surface.vertices.size();
    }
    return cached;
}

ClosestPoint closest_on_surface_brute(const geom::TriSurface& surface, const Eigen::Vector3d& p) {
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

} // namespace

ClosestPoint closest_on_surface(const geom::TriSurface& surface, const Eigen::Vector3d& p) {
    if (surface.triangles.size() < 32) {
        return closest_on_surface_brute(surface, p);
    }
    // Grid hash; for absolute correctness on rare hash misses, compare is not
    // needed when the expanding-shell termination is conservative. If the grid
    // is empty, brute force.
    const auto& g = grid_for(surface);
    if (g.bins.empty()) {
        return closest_on_surface_brute(surface, p);
    }
    auto best = g.query(surface, p);
    // Safety: if result looks wrong (non-finite), brute.
    if (!std::isfinite(best.distance)) {
        return closest_on_surface_brute(surface, p);
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
                              int passes, std::span<const geom::SharpEdge> feature_edges) {
    SnapStats stats;
    if (boundary_nodes.empty() || !(h > 0.0) || !std::isfinite(h) || !collect_offenders) {
        return stats;
    }
    // Allow up to ~1 cell diagonal so LEB mid-edges on cylinders can leave the stair.
    max_move_frac = std::clamp(max_move_frac, 0.05, 1.25);
    passes = std::clamp(passes, 1, 10);
    const double max_total = max_move_frac * h;
    const double step_cap = max_total / static_cast<double>(passes);
    // Search radius: full cell diagonal (~√3 h) plus margin for coarse facets.
    const double search_r = 2.5 * h;
    // Prefer CAD crease only for true rim/crease nodes (see below).
    const double edge_prefer_r = 0.55 * h;

    // Warm the surface grid once.
    (void)grid_for(surface);

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
            Eigen::Vector3d target = p;
            double dist = 0.0;
            bool have = false;
            // Feature prefer only when the node is *as close* to a crease as to
            // the surface (true rim). Hole-wall / free-face nodes often sit near
            // a rim in 3D (thin walls) but their closest surface is the wall —
            // those must project to the wall, not collapse onto the rim edge.
            if (!feature_edges.empty()) {
                const auto cf = geom::closest_on_features(p, surface, feature_edges);
                if (std::isfinite(cf.distance) && cf.distance > 1e-15 &&
                    cf.distance <= edge_prefer_r &&
                    cf.distance <= cp.distance + 0.08 * h) {
                    target = cf.point;
                    dist = cf.distance;
                    have = true;
                }
            }
            if (!have) {
                if (cp.distance > 1e-15 && cp.distance <= search_r) {
                    target = cp.point;
                    dist = cp.distance;
                    have = true;
                }
            }
            if (!have) {
                continue;
            }
            const double already = moved.count(ni) ? moved[ni] : 0.0;
            const double budget = max_total - already;
            if (budget <= 1e-15) {
                continue;
            }
            const double move = std::min({dist, step_cap, budget});
            if (move <= 1e-15) {
                continue;
            }
            if (!original.count(ni)) {
                original.emplace(ni, p);
            }
            const Eigen::Vector3d delta = target - p;
            nodes[ni] = p + delta * (move / dist);
            moved[ni] = already + move;
        }
    }
    stats.n_moved = moved.size();

    // Line-search unsnap: try keeping as much projection as possible (0.75→0.5→
    // 0.25→full restore). Soft half-restore alone left cylinder/hole residual.
    while (!original.empty()) {
        std::set<std::uint32_t> offenders;
        collect_offenders(offenders);
        std::uint32_t worst = 0xffffffffu;
        double worst_move = -1.0;
        for (const auto ni : offenders) {
            const auto it = moved.find(ni);
            if (it == moved.end()) {
                continue;
            }
            if (it->second > worst_move) {
                worst_move = it->second;
                worst = ni;
            }
        }
        if (worst == 0xffffffffu || worst_move < 0.0) {
            break; // inverted cell not caused by a still-snapped node
        }
        const auto oit = original.find(worst);
        if (oit == original.end()) {
            break;
        }
        const Eigen::Vector3d cur = nodes[worst];
        const Eigen::Vector3d orig = oit->second;
        bool fixed = false;
        static constexpr double kFracs[] = {0.75, 0.5, 0.25};
        for (const double f : kFracs) {
            // f = fraction of *remaining* snap kept (toward surface from orig).
            nodes[worst] = orig + f * (cur - orig);
            std::set<std::uint32_t> still;
            collect_offenders(still);
            if (!still.count(worst)) {
                moved[worst] = (nodes[worst] - orig).norm();
                fixed = true;
                break;
            }
        }
        if (fixed) {
            continue;
        }
        // Full restore.
        nodes[worst] = orig;
        original.erase(oit);
        moved.erase(worst);
        ++stats.n_unsnapped;
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
