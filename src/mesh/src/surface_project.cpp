// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

    int flat(int i, int j, int k) const { return (k * ny + j) * nx + i; }

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
        const Eigen::Vector3d extent =
            (bmax - bmin).cwiseMax(Eigen::Vector3d::Constant(1e-12));
        const double pad = 1e-6 * extent.norm() + 1e-12;
        bmin.array() -= pad;
        bmax.array() += pad;

        // Target ~2 triangles per bin; clamp resolution. Fine parts (10^5 tris
        // on real CAD) need more bins to keep per-query candidate lists short.
        const std::size_t ntri = surface.triangles.size();
        const double ntri_d = static_cast<double>(std::max<std::size_t>(1, ntri / 2));
        const int target = std::max(4, static_cast<int>(std::cbrt(ntri_d)));
        const int res = std::clamp(target, 4, 128);
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
            const Eigen::Vector3d q =
                closest_on_triangle(p, surface.vertices[tri[0]], surface.vertices[tri[1]],
                                    surface.vertices[tri[2]]);
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
                    const double next_lb =
                        static_cast<double>(r) * std::min({cell[0], cell[1], cell[2]});
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

ClosestPoint closest_on_surface_brute(const geom::TriSurface& surface,
                                      const Eigen::Vector3d& p) {
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

std::optional<BoundaryTarget>
owned_boundary_projection_target(const Eigen::Vector3d& p, std::uint32_t node,
                                 BoundaryProjectionContext* context,
                                 const MirrorFrame* mirror) {
    if (context == nullptr || !context->target) {
        return std::nullopt;
    }
    BoundarySupport transient;
    BoundarySupport* support = &transient;
    if (context->provenance != nullptr) {
        if (context->provenance->size() <= node) {
            context->provenance->resize(static_cast<std::size_t>(node) + 1);
        }
        support = &(*context->provenance)[node];
    }
    const BoundarySupport owner = *support;
    // The oracle sees the folded query, so ownership is classified in the
    // canonical octant: a node and its mirror image latch the SAME face/edge/
    // vertex id and are then projected onto mirrored points of it. Classifying
    // each in its own octant is what let a sphere's seam edge own one node and
    // its mirror image own the face (ADR-0036 §7).
    const Eigen::Vector3d query = mirror_fold(mirror, p);
    auto target = context->target(query, *support);
    // A classified owner is immutable. In particular, vertex and protected
    // edge ownership can never silently fall back to a face.
    if (owner.kind != BoundarySupportKind::kUnknown &&
        (support->kind != owner.kind || support->id != owner.id)) {
        *support = owner;
        return std::nullopt;
    }
    if (target && target->point.allFinite()) {
        target->point = mirror_unfold(mirror, target->point, p);
        target->distance = (target->point - p).norm();
        if (std::isfinite(target->distance)) {
            return target;
        }
    }
    return std::nullopt;
}

std::optional<BoundaryTarget> boundary_projection_target(const geom::TriSurface& surface,
                                                         const Eigen::Vector3d& p,
                                                         std::uint32_t node,
                                                         BoundaryProjectionContext* context,
                                                         const MirrorFrame* mirror) {
    if (context != nullptr && context->target) {
        return owned_boundary_projection_target(p, node, context, mirror);
    }
    const ClosestPoint cp = closest_on_surface(surface, mirror_fold(mirror, p));
    if (!std::isfinite(cp.distance)) {
        return std::nullopt;
    }
    // Reflection is an isometry, so the folded distance is the real distance.
    return BoundaryTarget{mirror_unfold(mirror, cp.point, p), cp.distance};
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

MirrorKeyFrame mirror_key_frame(const std::vector<Eigen::Vector3d>& nodes) {
    MirrorKeyFrame f;
    if (nodes.empty()) {
        return f;
    }
    Eigen::Vector3d lo = nodes.front();
    Eigen::Vector3d hi = lo;
    for (const auto& p : nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    f.center = 0.5 * (lo + hi);
    const double quantum = 1e-9 * (hi - lo).norm();
    f.inv_quantum = quantum > 0.0 ? 1.0 / quantum : 0.0;
    return f;
}

void sort_mirror_canonical(const std::vector<Eigen::Vector3d>& nodes,
                          std::vector<std::uint32_t>& ids) {
    if (ids.size() < 2) {
        return;
    }
    MirrorKeyFrame frame;
    {
        Eigen::Vector3d lo = nodes[ids.front()];
        Eigen::Vector3d hi = lo;
        for (const auto ni : ids) {
            lo = lo.cwiseMin(nodes[ni]);
            hi = hi.cwiseMax(nodes[ni]);
        }
        frame.center = 0.5 * (lo + hi);
        const double quantum = 1e-9 * (hi - lo).norm();
        frame.inv_quantum = quantum > 0.0 ? 1.0 / quantum : 0.0;
    }
    // Key array over the whole node range: the comparator must be cheap and must
    // not recompute a key per comparison.
    std::vector<std::array<long long, 3>> key(nodes.size());
    for (const auto ni : ids) {
        key[ni] = frame.key(nodes[ni]);
    }
    std::sort(ids.begin(), ids.end(), [&](std::uint32_t a, std::uint32_t b) {
        return key[a] != key[b] ? key[a] < key[b] : a < b;
    });
}

SnapStats
snap_boundary_nodes(const geom::TriSurface& surface, std::vector<Eigen::Vector3d>& nodes,
                    const std::vector<std::uint32_t>& boundary_nodes, double h,
                    const CollectOffendersFn& collect_offenders, double max_move_frac,
                    int passes, std::span<const geom::SharpEdge> feature_edges,
                    const RepairInteriorFn& repair_interior, const NodeOffendsFn& node_offends,
                    bool defer_coupled, BoundaryProjectionContext* projection,
                    const RelaxNeighborhoodFn& relax_neighborhood, const MirrorFrame* mirror) {
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
            Eigen::Vector3d target = p;
            double dist = 0.0;
            bool have = false;
            // Exact CAD ownership wins. Unknown nodes are classified once by
            // the callback; known vertex/edge/face ids persist across passes.
            // Once an exact oracle is installed, never fall back to a triangle
            // or sampled feature target: doing so could move an owned node
            // across a trimmed face or sharp edge when its exact projection
            // temporarily fails.
            if (projection != nullptr && projection->target) {
                const auto exact =
                    boundary_projection_target(surface, p, ni, projection, mirror);
                if (exact && exact->distance > 1e-15 && exact->distance <= search_r) {
                    target = exact->point;
                    dist = exact->distance;
                    have = true;
                }
                if (!have) {
                    continue;
                }
            } else {
                // Legacy tessellation heuristic for contexts without a live
                // CAD projection oracle. Both queries run on the folded point:
                // the tessellation is the one input that is measurably NOT
                // mirror-symmetric even on a symmetric part, and the sampled
                // feature set inherits that (ADR-0036 §7).
                const Eigen::Vector3d q = mirror_fold(mirror, p);
                const auto cp = closest_on_surface(surface, q);
                if (!feature_edges.empty()) {
                    const auto cf = geom::closest_on_features(q, surface, feature_edges);
                    if (std::isfinite(cf.distance) && cf.distance > 1e-15 &&
                        cf.distance <= edge_prefer_r &&
                        cf.distance <= cp.distance + 0.08 * h) {
                        target = mirror_unfold(mirror, cf.point, p);
                        dist = cf.distance;
                        have = true;
                    }
                }
                if (!have && cp.distance > 1e-15 && cp.distance <= search_r) {
                    target = mirror_unfold(mirror, cp.point, p);
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
    // Some product meshes expose private fan apexes that can be re-placed
    // after the wall reaches its actual endpoint. Repair them before deciding
    // that a boundary node must retreat.
    if (repair_interior) {
        repair_interior();
    }
    stats.n_moved = moved.size();

    // Line-search projected nodes back toward their original lattice sites.
    //
    // The culprit-aware implementation used to call the GLOBAL offender
    // collector at every scan and every one of twelve bisection steps. On the
    // hybrid sphere that is ~O(boundary nodes × 16 × 47k cells): 3.1 s became
    // 240.5 s, and icecream hybrid-VEM did not finish in 19 minutes.
    //
    // Callers that provide `node_offends` inspect only the cells incident to
    // the trial node. A cached global snapshot selects candidates; it is
    // refreshed only after coupled restores and at the final proof, preserving
    // whole-mesh validity without the quadratic global-rescan loop.
    if (node_offends) {
        std::unordered_map<std::uint32_t, Eigen::Vector3d> snapped;
        snapped.reserve(original.size());
        std::unordered_map<std::uint32_t, double> fraction;
        fraction.reserve(original.size());
        for (const auto& [ni, _] : original) {
            snapped.emplace(ni, nodes[ni]);
            fraction.emplace(ni, 1.0);
        }
        struct RestoredMove {
            std::uint32_t node;
            Eigen::Vector3d original;
            Eigen::Vector3d snapped;
        };
        std::vector<RestoredMove> recover;
        recover.reserve(original.size());
        std::unordered_set<std::uint32_t> deferred;
        deferred.reserve(original.size());

        constexpr int kBisectSteps = 6;
        const double bracket_tol = 1e-6 * h;
        // One global offender snapshot drives local trials until a coupled
        // full restore changes the neighbourhood. Stale entries are cheap:
        // `node_offends` discards them in O(node degree). A successful trial
        // cannot create an unlisted bad cell because its complete incident
        // star was validated before commit.
        std::set<std::uint32_t> offenders;
        collect_offenders(offenders);
        const std::size_t max_steps = 8 * original.size() + 1;
        // Retreat order decides the result: freeing one node routinely legalises
        // its neighbour. On a symmetric mesh a node and its mirror image offend by
        // the same amount, so a node-id tie-break retreats one of the pair and
        // keeps the other (ADR-0036).
        const MirrorKeyFrame mkey = mirror_key_frame(nodes);
        for (std::size_t step = 0; step < max_steps && !original.empty(); ++step) {
            const auto pick_worst = [&](bool skip_deferred) {
                std::uint32_t picked = 0xffffffffu;
                double picked_move = -1.0;
                std::array<long long, 3> picked_key{};
                for (const auto ni : offenders) {
                    const auto it = moved.find(ni);
                    if (it == moved.end() || (skip_deferred && deferred.count(ni) != 0) ||
                        !node_offends(ni)) {
                        continue;
                    }
                    const double move = it->second;
                    const double eps = 1e-12 * std::max(std::abs(move), std::abs(picked_move));
                    if (move > picked_move + eps) {
                        picked = ni;
                        picked_move = move;
                        picked_key = mkey.key(nodes[ni]);
                        continue;
                    }
                    if (move < picked_move - eps) {
                        continue;
                    }
                    const auto key = mkey.key(nodes[ni]);
                    if (key < picked_key || (key == picked_key && ni < picked)) {
                        picked = ni;
                        picked_move = move;
                        picked_key = key;
                    }
                }
                return picked;
            };

            std::uint32_t worst = pick_worst(/*skip_deferred=*/true);
            if (worst == 0xffffffffu) {
                // Refresh only at the coupled boundary: either the cached set
                // is fully stale/clean or every live candidate is deferred.
                offenders.clear();
                collect_offenders(offenders);
                worst = pick_worst(/*skip_deferred=*/true);
            }
            if (worst == 0xffffffffu) {
                worst = pick_worst(/*skip_deferred=*/false);
                if (worst == 0xffffffffu) {
                    break; // globally clean or only pre-existing offenders
                }
                const Eigen::Vector3d orig = original.at(worst);
                recover.push_back({worst, orig, snapped.at(worst)});
                nodes[worst] = orig;
                original.erase(worst);
                snapped.erase(worst);
                fraction.erase(worst);
                moved.erase(worst);
                deferred.clear();
                ++stats.n_unsnapped;
                offenders.clear();
                collect_offenders(offenders);
                continue;
            }

            const Eigen::Vector3d orig = original.at(worst);
            const Eigen::Vector3d full = snapped.at(worst);
            const double from = fraction.at(worst);
            const double span = (full - orig).norm();
            double bad = from;
            double good = -1.0;
            static constexpr double kKeep[] = {0.75, 0.5, 0.25, 0.0};
            const auto run_ladder = [&](bool include_full) {
                bad = from;
                good = -1.0;
                if (include_full) {
                    // Only on a retry: the node is currently AT `from` and was
                    // picked because it offends there, so testing 1.0 first is
                    // wasted work — unless relaxation has since opened room.
                    nodes[worst] = orig + from * (full - orig);
                    if (!node_offends(worst)) {
                        good = from;
                        return;
                    }
                }
                for (const double keep : kKeep) {
                    const double f = from * keep;
                    nodes[worst] = orig + f * (full - orig);
                    if (!node_offends(worst)) {
                        good = f;
                        return;
                    }
                    bad = f;
                }
            };
            run_ladder(/*include_full=*/false);
            // Relax whenever the projection cannot be kept WHOLE, not only
            // when every fraction fails. The common case on a curved wall is
            // not a full retreat (measured: 9 of 584 nodes on icecream_cone)
            // but a partial keep — the ladder settles at 0.25 of the move and
            // leaves the node 0.6 h off the CAD while reporting nothing. The
            // cell that blocks it is a stair fold whose other corners are
            // interior and unconstrained, so open that room and retry; the
            // relaxation is validity-gated and touches interior nodes only,
            // which cannot cost boundary fidelity (ADR-0035).
            for (int round = 0; good < from && round < 3 && relax_neighborhood; ++round) {
                if (!relax_neighborhood(worst)) {
                    break;
                }
                const double before = good;
                run_ladder(/*include_full=*/true);
                if (good > before) {
                    ++stats.n_relax_rescued;
                }
            }
            if (good < 0.0) {
                if (!defer_coupled) {
                    // Hex/poly stars do not need fan-corner deferral. Restore
                    // immediately; this keeps curved pure-hex cases linear.
                    nodes[worst] = orig;
                    recover.push_back({worst, orig, full});
                    original.erase(worst);
                    snapped.erase(worst);
                    fraction.erase(worst);
                    moved.erase(worst);
                    deferred.clear();
                    ++stats.n_unsnapped;
                    offenders.clear();
                    collect_offenders(offenders);
                    continue;
                }
                nodes[worst] = orig + from * (full - orig);
                deferred.insert(worst);
                continue;
            }
            for (int i = 0; i < kBisectSteps && (bad - good) * span > bracket_tol; ++i) {
                const double mid = 0.5 * (good + bad);
                nodes[worst] = orig + mid * (full - orig);
                if (node_offends(worst)) {
                    bad = mid;
                } else {
                    good = mid;
                }
            }
            nodes[worst] = orig + good * (full - orig);
            if (good <= 0.0) {
                recover.push_back({worst, orig, full});
                original.erase(worst);
                snapped.erase(worst);
                fraction.erase(worst);
                moved.erase(worst);
                ++stats.n_unsnapped;
            } else {
                fraction[worst] = good;
                moved[worst] = good * span;
            }
            deferred.clear();
        }

        offenders.clear();
        collect_offenders(offenders);
        for (int cleanup = 0; cleanup < 2 && !offenders.empty(); ++cleanup) {
            bool restored = false;
            for (const auto ni : offenders) {
                const auto oit = original.find(ni);
                if (oit == original.end()) {
                    continue;
                }
                nodes[ni] = oit->second;
                recover.push_back({ni, oit->second, snapped.at(ni)});
                original.erase(oit);
                snapped.erase(ni);
                fraction.erase(ni);
                moved.erase(ni);
                ++stats.n_unsnapped;
                restored = true;
            }
            if (!restored) {
                break;
            }
            offenders.clear();
            collect_offenders(offenders);
        }

        // Recover as much surface projection as each fully restored node can
        // keep in the now-clean coupled neighbourhood.
        //
        // Sequential and coupled: a node is re-pushed while its own star is
        // valid, and that decides whether its neighbour can be. `recover` was
        // filled by scanning an ascending-id offender set, which does not mirror,
        // so a node and its mirror image were replayed in different company and
        // kept different fractions. Replay on the mirror key instead (ADR-0036).
        if (offenders.empty()) {
            std::stable_sort(recover.begin(), recover.end(),
                             [&](const RestoredMove& a, const RestoredMove& b) {
                                 const auto ka = mkey.key(a.original);
                                 const auto kb = mkey.key(b.original);
                                 return ka != kb ? ka < kb : a.node < b.node;
                             });
            for (const auto& r : recover) {
                if (node_offends(r.node)) {
                    nodes[r.node] = r.original;
                    continue;
                }
                const double span = (r.snapped - r.original).norm();
                if (!(span > 0.0)) {
                    continue;
                }
                double good = 0.0;
                double bad = 1.0;
                nodes[r.node] = r.snapped;
                if (!node_offends(r.node)) {
                    good = 1.0;
                } else {
                    static constexpr double kKeep[] = {0.75, 0.5, 0.25};
                    for (const double f : kKeep) {
                        nodes[r.node] = r.original + f * (r.snapped - r.original);
                        if (!node_offends(r.node)) {
                            good = f;
                            break;
                        }
                        bad = f;
                    }
                    for (int i = 0; i < kBisectSteps && (bad - good) * span > bracket_tol;
                         ++i) {
                        const double mid = 0.5 * (good + bad);
                        nodes[r.node] = r.original + mid * (r.snapped - r.original);
                        if (node_offends(r.node)) {
                            bad = mid;
                        } else {
                            good = mid;
                        }
                    }
                }
                nodes[r.node] = r.original + good * (r.snapped - r.original);
                if (good > 0.0) {
                    moved[r.node] = good * span;
                }
            }
            // The recovery above is a sequence of LOCAL decisions: each node is
            // re-pushed while its own incident star is valid, but a later node's
            // push can re-break a cell an earlier node shares, and that earlier
            // node is never revisited. This whole-mesh sweep used to be computed
            // and then thrown away — "mandatory final whole-mesh proof" that
            // proved nothing, because no caller of this function reads the
            // offender set. Measured 2026-08-15 on ellipsoid_boss_s1 hybrid at
            // auto h: 4 hex8 cells left the snap at fea::cell_quality -0.99 with
            // 7 of 8 nodes recovered to 0.5-0.7 h of travel.
            //
            // So act on it: retreat every recovered node that still participates
            // in a bad cell, all the way back, and re-prove. Each iteration
            // permanently drops at least one node from `recovered_span`, so this
            // terminates in at most one pass per recovered node.
            std::unordered_map<std::uint32_t, Eigen::Vector3d> recovered_origin;
            recovered_origin.reserve(recover.size());
            for (const auto& r : recover) {
                recovered_origin.emplace(r.node, r.original);
            }
            offenders.clear();
            collect_offenders(offenders);
            while (!offenders.empty()) {
                bool retreated = false;
                for (const auto ni : offenders) {
                    const auto it = recovered_origin.find(ni);
                    if (it == recovered_origin.end()) {
                        continue; // not something this recovery pass moved
                    }
                    nodes[ni] = it->second;
                    moved.erase(ni);
                    recovered_origin.erase(it);
                    ++stats.n_unsnapped;
                    retreated = true;
                }
                if (!retreated) {
                    break; // remaining offenders predate this snap; not ours to fix
                }
                offenders.clear();
                collect_offenders(offenders);
            }
        }
    } else {
        // Compatibility path for legacy fill callers: the old bounded
        // 0.75/0.5/0.25 ladder (at most four global scans per restored node).
        // It is intentionally boring; the former 12-step culprit loop made
        // every existing callsite pay for an incident map it did not have.
        while (!original.empty()) {
            std::set<std::uint32_t> offenders;
            collect_offenders(offenders);
            std::uint32_t worst = 0xffffffffu;
            double worst_move = -1.0;
            for (const auto ni : offenders) {
                const auto it = moved.find(ni);
                if (it != moved.end() && it->second > worst_move) {
                    worst_move = it->second;
                    worst = ni;
                }
            }
            if (worst == 0xffffffffu) {
                break;
            }
            const auto oit = original.find(worst);
            const Eigen::Vector3d cur = nodes[worst];
            const Eigen::Vector3d orig = oit->second;
            bool fixed = false;
            static constexpr double kKeep[] = {0.75, 0.5, 0.25};
            for (const double keep : kKeep) {
                nodes[worst] = orig + keep * (cur - orig);
                std::set<std::uint32_t> still;
                collect_offenders(still);
                if (still.count(worst) == 0) {
                    moved[worst] = (nodes[worst] - orig).norm();
                    fixed = true;
                    break;
                }
            }
            if (fixed) {
                continue;
            }
            nodes[worst] = orig;
            original.erase(oit);
            moved.erase(worst);
            ++stats.n_unsnapped;
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

SmoothStats smooth_boundary_nodes(const geom::TriSurface& surface,
                                  std::vector<Eigen::Vector3d>& nodes,
                                  std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                                  double h, const CollectOffendersFn& collect_offenders,
                                  int passes, double relax,
                                  std::span<const geom::SharpEdge> feature_edges,
                                  BoundaryProjectionContext* projection,
                                  const MirrorFrame* mirror) {
    SmoothStats stats;
    if (boundary_faces.empty() || !(h > 0.0) || !std::isfinite(h) || !collect_offenders) {
        return stats;
    }
    passes = std::clamp(passes, 1, 10);
    relax = std::clamp(relax, 0.05, 1.0);
    (void)grid_for(surface);

    // Boundary graph: unique undirected edges of the free-surface faces
    // (quads, or tris encoded with a duplicated last node).
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> nbr;
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
        };
        for (const auto& q : boundary_faces) {
            for (int e = 0; e < 4; ++e) {
                add_edge(q[static_cast<std::size_t>(e)],
                         q[static_cast<std::size_t>((e + 1) % 4)]);
            }
        }
    }

    // Crease classification: node sits on a sharp CAD edge. Near-crease wall
    // nodes must not be smoothed across the crease, so anything within the
    // guard band that is not *on* the crease is frozen.
    const double on_crease_r = 0.10 * h;
    const double crease_guard_r = 0.50 * h;
    enum class Kind : std::uint8_t { kFree, kCrease, kFrozen };
    std::unordered_map<std::uint32_t, Kind> kind;
    kind.reserve(nbr.size());
    // Visit order for the whole smoothing pass. The classification query below
    // and the re-projection at the Jacobi step both write shared per-node
    // provenance through the exact oracle, so the visit sequence is mesh-level
    // mutation state rather than a private scan: `nbr` bucket order differs
    // between libstdc++ and MSVC (measured 2026-08-14: 5 of 24 corpus pairs
    // disagreed, worst 264 vs 200 elements). Ascending node id fixed that but is
    // not mirror-equivariant, and this pass was the single largest symmetry loss
    // in the graded fill — see `sort_mirror_canonical` (ADR-0036).
    std::vector<std::uint32_t> nbr_ids;
    nbr_ids.reserve(nbr.size());
    for (const auto& [ni, _] : nbr) {
        nbr_ids.push_back(ni);
    }
    // Each adjacency list is sorted on the same key, so a node and its mirror
    // image accumulate their neighbour centroid in mirrored order and the two
    // sums round identically. Left in bucket order the two differ by an ulp,
    // which is harmless for the centroid but not for the comparisons downstream
    // of it.
    for (auto& [ni, list] : nbr) {
        (void)ni;
        sort_mirror_canonical(nodes, list);
    }
    sort_mirror_canonical(nodes, nbr_ids);
    const bool exact_owners = projection != nullptr && projection->target;
    for (const auto ni : nbr_ids) {
        Kind k = Kind::kFree;
        if (exact_owners) {
            // Classification is a side effect of the first exact target query;
            // the point itself is not moved during this setup pass.
            (void)boundary_projection_target(surface, nodes[ni], ni, projection, mirror);
            if (projection->provenance != nullptr && ni < projection->provenance->size()) {
                const BoundarySupport owner = (*projection->provenance)[ni];
                if (owner.kind == BoundarySupportKind::kCadVertex) {
                    k = Kind::kFrozen;
                } else if (owner.kind == BoundarySupportKind::kCadEdge) {
                    k = Kind::kCrease;
                }
            }
        } else if (!feature_edges.empty()) {
            const double df =
                geom::closest_on_features(mirror_fold(mirror, nodes[ni]), surface,
                                          feature_edges)
                    .distance;
            if (df <= on_crease_r) {
                k = Kind::kCrease;
            } else if (df <= crease_guard_r) {
                k = Kind::kFrozen;
            }
        }
        kind.emplace(ni, k);
    }
    if (!exact_owners && feature_edges.empty()) {
        // No CAD crease info: protect sharp geometry intrinsically — freeze
        // nodes whose incident boundary-face normals disagree strongly (box
        // edges/corners). Smoothly curved patches keep a tight normal cone.
        std::unordered_map<std::uint32_t, Eigen::Vector3d> first_n;
        std::unordered_set<std::uint32_t> frozen;
        for (const auto& q : boundary_faces) {
            const Eigen::Vector3d e1 = nodes[q[1]] - nodes[q[0]];
            const Eigen::Vector3d e2 = nodes[q[2]] - nodes[q[0]];
            const Eigen::Vector3d n = e1.cross(e2);
            const double len = n.norm();
            if (len <= 0.0) {
                continue;
            }
            const Eigen::Vector3d nn = n / len;
            for (int e = 0; e < 4; ++e) {
                const auto ni = q[static_cast<std::size_t>(e)];
                const auto [it, fresh] = first_n.try_emplace(ni, nn);
                if (!fresh && std::abs(it->second.dot(nn)) < 0.7071) {
                    frozen.insert(ni); // > ~45° spread: geometric crease
                }
            }
        }
        for (const auto ni : frozen) {
            kind[ni] = Kind::kFrozen;
        }
    }

    std::unordered_map<std::uint32_t, Eigen::Vector3d> moved; // pre-pass position
    for (int pass = 0; pass < passes; ++pass) {
        // Jacobi targets from the current state.
        std::vector<std::pair<std::uint32_t, Eigen::Vector3d>> targets;
        targets.reserve(nbr.size());
        for (const auto ni : nbr_ids) {
            const auto& nb = nbr.at(ni);
            const Kind k = kind[ni];
            if (k == Kind::kFrozen || nb.empty()) {
                continue;
            }
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            std::size_t n_used = 0;
            if (k == Kind::kCrease) {
                // Relax along the crease chain only; corners/junctions
                // (≠2 crease neighbors) and kinked chains stay put.
                std::vector<std::uint32_t> cn;
                for (const auto o : nb) {
                    if (kind[o] == Kind::kCrease) {
                        cn.push_back(o);
                    }
                }
                if (cn.size() != 2) {
                    continue;
                }
                const Eigen::Vector3d d0 = (nodes[cn[0]] - nodes[ni]).normalized();
                const Eigen::Vector3d d1 = (nodes[cn[1]] - nodes[ni]).normalized();
                if (d0.dot(d1) > -0.5) {
                    continue; // sharper than 120° in-chain: corner, keep
                }
                centroid = 0.5 * (nodes[cn[0]] + nodes[cn[1]]);
                n_used = 2;
            } else {
                for (const auto o : nb) {
                    centroid += nodes[o];
                    ++n_used;
                }
                centroid /= static_cast<double>(n_used);
            }
            if (n_used == 0) {
                continue;
            }
            Eigen::Vector3d p = nodes[ni] + relax * (centroid - nodes[ni]);
            // Re-project so travel is tangential. Exact owners are always
            // resolved through the same constrained oracle used by snap.
            if (exact_owners) {
                const auto target =
                    boundary_projection_target(surface, p, ni, projection, mirror);
                if (!target || target->distance > 0.75 * h) {
                    continue;
                }
                p = target->point;
            } else if (k == Kind::kCrease) {
                const auto cf = geom::closest_on_features(mirror_fold(mirror, p), surface,
                                                          feature_edges);
                if (!std::isfinite(cf.distance)) {
                    continue;
                }
                p = mirror_unfold(mirror, cf.point, p);
            } else {
                const auto cp = closest_on_surface(surface, mirror_fold(mirror, p));
                if (cp.distance > 0.75 * h) {
                    continue; // projection ran away — keep the node
                }
                p = mirror_unfold(mirror, cp.point, p);
            }
            if ((p - nodes[ni]).squaredNorm() <= 1e-30) {
                continue;
            }
            targets.push_back({ni, p});
        }
        if (targets.empty()) {
            break;
        }
        for (const auto& [ni, p] : targets) {
            moved.try_emplace(ni, nodes[ni]);
            nodes[ni] = p;
        }
        // Inversion guard: revert moved offenders until the mesh is clean.
        // The cascade MUST run to a fixed point: at a concave crease, reverting
        // one node routinely inverts a neighbour's tet, and an iteration cap
        // here once left 920 inverted tets behind on icecream_cone.step — which
        // the caller then "fixed" by re-winding them, producing watertight,
        // positive-volume, mutually OVERLAPPING tets (9 boundary faces buried
        // inside other cells). Termination is guaranteed: every iteration
        // erases at least one node from `moved`, and each node reverts once.
        while (true) {
            std::set<std::uint32_t> offenders;
            collect_offenders(offenders);
            bool reverted = false;
            for (const auto ni : offenders) {
                const auto it = moved.find(ni);
                if (it == moved.end()) {
                    continue;
                }
                nodes[ni] = it->second;
                moved.erase(it);
                kind[ni] = Kind::kFrozen; // do not retry in later passes
                ++stats.n_reverted;
                reverted = true;
            }
            if (!reverted) {
                break;
            }
        }
    }
    stats.n_moved = moved.size();
    for (const auto& [ni, _] : nbr) {
        stats.max_residual =
            std::max(stats.max_residual, closest_on_surface(surface, nodes[ni]).distance);
    }
    return stats;
}

} // namespace polymesh::mesh
