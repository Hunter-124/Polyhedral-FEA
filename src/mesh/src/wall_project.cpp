// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/wall_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace {

struct FaceKey {
    std::uint32_t a, b, c;
    bool operator==(const FaceKey& o) const { return a == o.a && b == o.b && c == o.c; }
};

struct FaceHash {
    std::size_t operator()(const FaceKey& f) const {
        return (static_cast<std::size_t>(f.a) * 73856093u) ^
               (static_cast<std::size_t>(f.b) * 19349663u) ^
               (static_cast<std::size_t>(f.c) * 83492791u);
    }
};

FaceKey sorted_face(std::uint32_t i, std::uint32_t j, std::uint32_t k) {
    if (i > j) {
        std::swap(i, j);
    }
    if (j > k) {
        std::swap(j, k);
    }
    if (i > j) {
        std::swap(i, j);
    }
    return FaceKey{i, j, k};
}

double tet_vol6(const std::vector<Eigen::Vector3d>& nodes,
                const std::array<std::uint32_t, 4>& t) {
    const Eigen::Vector3d a = nodes[t[0]];
    const Eigen::Vector3d b = nodes[t[1]];
    const Eigen::Vector3d c = nodes[t[2]];
    const Eigen::Vector3d d = nodes[t[3]];
    // Materialize diffs: Eigen expression templates do not nest .cross() safely.
    const Eigen::Vector3d ba = b - a;
    const Eigen::Vector3d ca = c - a;
    const Eigen::Vector3d da = d - a;
    return ba.dot(ca.cross(da));
}

} // namespace

WallProjectStats wall_tangential_project(
    const geom::CadModel& cad, const geom::CadTopology* topo,
    std::vector<Eigen::Vector3d>& nodes,
    std::span<const std::array<std::uint32_t, 4>> tets, double h, int iters,
    double relax, double sharp_guard_frac) {
    WallProjectStats stats;
    if (cad.empty() || !cad.has_brep() || tets.empty() || nodes.empty() || !(h > 0.0) ||
        !std::isfinite(h) || iters <= 0) {
        return stats;
    }
    iters = std::clamp(iters, 1, 8);
    relax = std::clamp(relax, 0.05, 1.0);
    sharp_guard_frac = std::clamp(sharp_guard_frac, 0.05, 2.0);
    const double hh = std::max(h, 1e-12);
    const double sharp_band = sharp_guard_frac * hh;
    const double max_step = 0.5 * hh;

    // Free-face map (count == 1) and undirected boundary graph.
    std::unordered_map<FaceKey, int, FaceHash> fcounts;
    std::unordered_map<FaceKey, std::array<std::uint32_t, 3>, FaceHash> forient;
    const int quads[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : tets) {
        for (const auto& f : quads) {
            const std::uint32_t i0 = t[static_cast<std::size_t>(f[0])];
            const std::uint32_t i1 = t[static_cast<std::size_t>(f[1])];
            const std::uint32_t i2 = t[static_cast<std::size_t>(f[2])];
            if (i0 >= nodes.size() || i1 >= nodes.size() || i2 >= nodes.size()) {
                continue;
            }
            const FaceKey sk = sorted_face(i0, i1, i2);
            fcounts[sk]++;
            forient[sk] = {i0, i1, i2};
        }
    }

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> nbr;
    std::unordered_set<std::uint32_t> bset;
    {
        std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
        auto add_edge = [&](std::uint32_t a, std::uint32_t b) {
            if (a == b || a >= nodes.size() || b >= nodes.size()) {
                return;
            }
            const auto key = std::minmax(a, b);
            if (!seen.insert({key.first, key.second}).second) {
                return;
            }
            nbr[a].push_back(b);
            nbr[b].push_back(a);
            bset.insert(a);
            bset.insert(b);
        };
        for (const auto& [key, count] : fcounts) {
            if (count != 1) {
                continue;
            }
            const auto& tri = forient[key];
            add_edge(tri[0], tri[1]);
            add_edge(tri[1], tri[2]);
            add_edge(tri[2], tri[0]);
        }
    }
    if (bset.empty()) {
        return stats;
    }

    // Wall nodes: free boundary, not near a sharp CAD edge.
    std::vector<std::uint32_t> wall;
    wall.reserve(bset.size());
    for (std::uint32_t ni : bset) {
        if (topo != nullptr && !topo->edges.empty()) {
            const auto q = geom::closest_edge(*topo, nodes[ni], /*sharp_only=*/true);
            if (q && q->distance <= sharp_band) {
                continue; // sharp / near-sharp — leave protected
            }
        }
        wall.push_back(ni);
    }
    stats.n_wall_nodes = wall.size();
    if (wall.empty()) {
        return stats;
    }
    std::unordered_set<std::uint32_t> wall_set(wall.begin(), wall.end());

    // Tets that touch any wall node (volume guard domain).
    std::vector<const std::array<std::uint32_t, 4>*> guard_tets;
    guard_tets.reserve(tets.size());
    for (const auto& t : tets) {
        bool touch = false;
        for (int k = 0; k < 4; ++k) {
            if (wall_set.count(t[static_cast<std::size_t>(k)])) {
                touch = true;
                break;
            }
        }
        if (touch) {
            guard_tets.push_back(&t);
        }
    }

    std::unordered_map<std::uint32_t, Eigen::Vector3d> accepted; // last good pos
    for (int it = 0; it < iters; ++it) {
        // Jacobi targets from current state.
        std::vector<std::pair<std::uint32_t, Eigen::Vector3d>> targets;
        targets.reserve(wall.size());
        for (std::uint32_t ni : wall) {
            const auto nit = nbr.find(ni);
            if (nit == nbr.end() || nit->second.empty()) {
                continue;
            }
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            std::size_t n_used = 0;
            for (std::uint32_t o : nit->second) {
                // Prefer free-surface neighbors (all nbr are boundary).
                centroid += nodes[o];
                ++n_used;
            }
            if (n_used == 0) {
                continue;
            }
            centroid /= static_cast<double>(n_used);
            Eigen::Vector3d d = centroid - nodes[ni];

            // Local normal from BRep at current node; project d onto tangent.
            Eigen::Vector3d n = Eigen::Vector3d::Zero();
            if (const auto pr = geom::project_point_on_surface(cad, nodes[ni])) {
                n = pr->normal;
            }
            if (n.squaredNorm() > 1e-20) {
                n.normalize();
                d = d - d.dot(n) * n;
            }

            Eigen::Vector3d p = nodes[ni] + relax * d;
            // Cap step so a single iter cannot leap across elements.
            {
                const Eigen::Vector3d step = p - nodes[ni];
                const double slen = step.norm();
                if (slen > max_step) {
                    p = nodes[ni] + (max_step / slen) * step;
                }
            }

            // Re-project onto live BRep (core of free-slide wall).
            const auto proj = geom::project_point_on_surface(cad, p);
            if (!proj) {
                continue;
            }
            // Reject runaway projections (e.g. wrong face of a thin solid).
            if (proj->distance > 0.75 * hh && (proj->point - nodes[ni]).norm() > 0.75 * hh) {
                continue;
            }
            p = proj->point;
            if ((p - nodes[ni]).squaredNorm() <= 1e-30) {
                continue;
            }
            targets.push_back({ni, p});
        }
        if (targets.empty()) {
            break;
        }

        // Apply batch; remember pre-move for revert.
        std::unordered_map<std::uint32_t, Eigen::Vector3d> batch_saved;
        for (const auto& [ni, p] : targets) {
            batch_saved.emplace(ni, nodes[ni]);
            nodes[ni] = p;
        }

        // Jacobian guard: revert offenders participating in flipped tets.
        for (int guard = 0; guard < 32; ++guard) {
            std::unordered_set<std::uint32_t> offenders;
            for (const auto* tp : guard_tets) {
                if (tet_vol6(nodes, *tp) <= 0.0) {
                    for (int k = 0; k < 4; ++k) {
                        offenders.insert((*tp)[static_cast<std::size_t>(k)]);
                    }
                }
            }
            bool reverted = false;
            for (std::uint32_t ni : offenders) {
                const auto it_s = batch_saved.find(ni);
                if (it_s == batch_saved.end()) {
                    continue;
                }
                nodes[ni] = it_s->second;
                batch_saved.erase(it_s);
                // Do not retry this node in later outer iters.
                wall_set.erase(ni);
                ++stats.n_reverted;
                reverted = true;
            }
            if (!reverted) {
                break;
            }
        }

        // Drop frozen nodes from wall list for subsequent iters.
        if (wall_set.size() != wall.size()) {
            wall.erase(std::remove_if(wall.begin(), wall.end(),
                                      [&](std::uint32_t ni) { return !wall_set.count(ni); }),
                       wall.end());
        }
        for (const auto& [ni, _] : batch_saved) {
            accepted[ni] = nodes[ni];
        }
        ++stats.n_iters;
    }

    stats.n_moved = accepted.size();

    // Residual of wall nodes vs BRep.
    double sum = 0.0;
    std::size_t n_res = 0;
    for (std::uint32_t ni : wall_set) {
        if (const auto pr = geom::project_point_on_surface(cad, nodes[ni])) {
            sum += pr->distance;
            stats.max_surface_residual = std::max(stats.max_surface_residual, pr->distance);
            ++n_res;
        }
    }
    if (n_res > 0) {
        stats.mean_surface_residual = sum / static_cast<double>(n_res);
    }
    return stats;
}

} // namespace polymesh::mesh
