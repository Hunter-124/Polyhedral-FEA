// SPDX-License-Identifier: BSD-3-Clause
// Boundary snap round of the graded tet fill.
#include "hybrid_fill_internal.hpp"

#include "mesh/cell_validity.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::mesh::detail {

void snap_round(GradedFillState& s) {
    const geom::TriSurface& surface = s.surface;
    const std::span<const geom::SharpEdge> features = s.features;
    BoundaryProjectionContext* const projection = s.projection;
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    progress.set_phase("projection_snap");
    std::vector<std::uint32_t> snap_nodes = tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
    if (snap_nodes.empty()) {
        return;
    }
    const double vol_eps = 1e-14 * hc * hc * hc;
    std::vector<char> on_boundary(out.mesh.nodes.size(), 0);
    for (std::size_t work_done = 0; const auto ni : snap_nodes) {
        if (active_fill_progress != nullptr)
            fill_progress_poll(work_done++, snap_nodes.size());
        on_boundary[ni] = 1;
    }
    // The bad-cell test: inverted is always bad; below the shared shape
    // floor is bad ONLY when every corner is on the boundary. A thin cell
    // with an interior corner is repairable — the downstream collapse and
    // relaxation rounds move the interior corner and lift it (stair cells
    // dip under the floor mid-snap and repair_round restores them). A thin
    // ALL-BOUNDARY cap has no interior corner to give, so no later pass can
    // lift it — it must block the move here.
    const auto tet_is_bad = [&](const std::array<std::uint32_t, 4>& n) {
        const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
        const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
        const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
        const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
        if (!(tet_signed_volume(a, b, c, d) > vol_eps)) {
            return true;
        }
        if (validity::tet_shape_quality(a, b, c, d) >= validity::kCellShapeFloor) {
            return false;
        }
        return on_boundary[n[0]] != 0 && on_boundary[n[1]] != 0 && on_boundary[n[2]] != 0 &&
               on_boundary[n[3]] != 0;
    };
    // Node -> incident tets, so the snap line-searches ONE node against its
    // own star instead of rescanning the mesh (same wiring as tet_fill).
    std::vector<std::vector<std::size_t>> incident(out.mesh.nodes.size());
    for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
        fill_progress_poll(ti, out.mesh.tets.size());
        for (const auto ni : out.mesh.tets[ti]) {
            incident[ni].push_back(ti);
        }
    }
    std::vector<std::vector<std::uint32_t>> nbrs(out.mesh.nodes.size());
    {
        std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
        for (const auto& n : out.mesh.tets) {
            fill_progress_poll();
            for (int a = 0; a < 4; ++a) {
                for (int b = a + 1; b < 4; ++b) {
                    const auto u = n[static_cast<std::size_t>(a)];
                    const auto v = n[static_cast<std::size_t>(b)];
                    const auto key = std::minmax(u, v);
                    if (u == v || !seen.insert({key.first, key.second}).second) {
                        continue;
                    }
                    nbrs[u].push_back(v);
                    nbrs[v].push_back(u);
                }
            }
        }
    }
    const auto node_offends = [&](std::uint32_t ni) {
        for (const auto ti : incident[ni]) {
            fill_progress_poll();
            if (tet_is_bad(out.mesh.tets[ti])) {
                return true;
            }
        }
        return false;
    };
    // Reflection orbit over the node set, so a nudge applies to a node and
    // its mirror images together or not at all: the nudges are a
    // Gauss-Seidel sweep on shared state, and accepting one on one side of
    // a symmetry plane changes what the other side's gate reads (ADR-0036).
    const mesh::MirrorNodeOrbit orbit(
        mirror != nullptr ? *mirror : mesh::MirrorFrame{}, out.mesh.nodes, [&] {
            const mesh::MirrorKeyFrame frame = mesh::mirror_key_frame(out.mesh.nodes);
            return frame.inv_quantum > 0.0 ? 1.0 / frame.inv_quantum : 0.0;
        }());
    const auto orbit_of = [&](std::uint32_t node) {
        std::vector<std::uint32_t> group{node};
        if (!orbit.active()) {
            return group;
        }
        for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
            const std::uint32_t other = orbit.reflected(node, mask);
            if (other == mesh::MirrorNodeOrbit::npos) {
                group.clear();
                return group;
            }
            if (std::find(group.begin(), group.end(), other) == group.end()) {
                group.push_back(other);
            }
        }
        return group;
    };
    // Interior room for a blocked node, mirroring tet_fill: the star's
    // interior neighbours are free, and its other wall nodes may slide
    // tangentially (re-projected through the same oracle, so their
    // placement never changes — only their spacing).
    const auto reproject = [&](std::uint32_t ni, const Eigen::Vector3d& p) {
        if (projection == nullptr) {
            return mirror_unfold(mirror,
                                 closest_on_surface(surface, mirror_fold(mirror, p)).point, p);
        }
        const auto target = boundary_projection_target(surface, p, ni, projection, mirror);
        return target ? target->point : p;
    };
    const auto relax_neighborhood = [&](std::uint32_t seed) {
        if (incident[seed].empty()) {
            return false;
        }
        std::vector<std::uint32_t> ring;
        std::vector<std::uint32_t> wall;
        for (const auto ti : incident[seed]) {
            fill_progress_poll();
            for (const auto ni : out.mesh.tets[ti]) {
                if (ni == seed || nbrs[ni].empty()) {
                    continue;
                }
                (on_boundary[ni] == 0 ? ring : wall).push_back(ni);
            }
        }
        auto dedup = [&](std::vector<std::uint32_t>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            // Mirror-canonical: each nudge is accepted against the shared
            // node array, so the sweep order decides the result (ADR-0036).
            sort_mirror_canonical(out.mesh.nodes, v);
        };
        dedup(ring);
        dedup(wall);
        bool moved_any = false;
        const double cap = 0.25 * hc;
        std::vector<char> done(out.mesh.nodes.size(), 0);
        const auto nudge = [&](std::uint32_t ni, bool tangential) {
            if (done[ni] != 0) {
                return;
            }
            const auto group = orbit_of(ni);
            if (group.empty()) {
                return; // incomplete orbit: no move is symmetric
            }
            for (const auto node : group) {
                done[node] = 1;
            }
            std::vector<Eigen::Vector3d> saved;
            std::vector<Eigen::Vector3d> moved;
            saved.reserve(group.size());
            moved.reserve(group.size());
            for (const auto node : group) {
                fill_progress_poll();
                Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                for (const auto other : nbrs[node]) {
                    centroid += out.mesh.nodes[other];
                }
                centroid /= static_cast<double>(nbrs[node].size());
                saved.push_back(out.mesh.nodes[node]);
                const Eigen::Vector3d step = 0.5 * (centroid - saved.back());
                const double len = step.norm();
                Eigen::Vector3d trial = saved.back() + (len > cap ? step * (cap / len) : step);
                if (tangential) {
                    trial = reproject(node, trial);
                    if ((trial - saved.back()).norm() > cap) {
                        return; // projection ran away; leave the wall alone
                    }
                }
                moved.push_back(trial);
            }
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                out.mesh.nodes[group[gi]] = moved[gi];
            }
            for (const auto node : group) {
                if (node_offends(node)) {
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] = saved[gi];
                    }
                    return;
                }
            }
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                if ((out.mesh.nodes[group[gi]] - saved[gi]).squaredNorm() > 0.0) {
                    moved_any = true;
                }
            }
        };
        for (const auto ni : ring) {
            fill_progress_poll();
            nudge(ni, /*tangential=*/false);
        }
        if (!moved_any) {
            for (const auto ni : wall) {
                fill_progress_poll();
                nudge(ni, /*tangential=*/true);
            }
        }
        return moved_any;
    };
    // Strong budget so LEB mid-edges on holes leave the Cartesian stair.
    // Use max(hc, ~cell diagonal) scale via frac>1; soft-unsnap keeps quality.
    //
    // Two stages. The bulk runs the compatibility (inversion-only) path:
    // its all-or-nothing semantics are what the downstream collapse and
    // relaxation rounds are built on — a stair cell may dip thin mid-snap
    // because repair_round lifts it afterwards, so gating THIS stage on the
    // shape floor would pin boundary nodes off the surface behind transient
    // stair caps.
    std::vector<std::size_t> skin_tets;
    skin_tets.reserve(snap_nodes.size() * 4);
    for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
        fill_progress_poll(ti, out.mesh.tets.size());
        const auto& n = out.mesh.tets[ti];
        if (on_boundary[n[0]] || on_boundary[n[1]] || on_boundary[n[2]] || on_boundary[n[3]]) {
            skin_tets.push_back(ti);
        }
    }
    auto collect_invert = [&](std::set<std::uint32_t>& offenders) {
        for (const auto ti : skin_tets) {
            fill_progress_poll();
            const auto& n = out.mesh.tets[ti];
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v > vol_eps) {
                continue;
            }
            offenders.insert(n.begin(), n.end());
        }
    };
    snap_boundary_nodes(surface, out.mesh.nodes, snap_nodes, hc, collect_invert,
                        /*max_move_frac=*/1.15, /*passes=*/7, features, {}, {},
                        /*defer_coupled=*/false, projection, {}, mirror);
    // Straggler rescue. The compatibility ladder's last resort is a full
    // retreat to the raw lattice site, and a node can stay there behind
    // all-boundary cap tets pinned at the shape floor, which neither the
    // exterior gate nor any smoother can move afterwards. Re-attempt exactly
    // those nodes with the culprit-aware path: bisected partial keeps,
    // floor-gated (an all-boundary cap has no interior corner, so no later
    // pass can lift it — it must block here), and neighbourhood relaxation to
    // open the room first. CAD-backed only: against a tessellation the exact
    // oracle does not exist and a node 0.01 hc off the facets is within facet
    // error, not stranded; rescuing it would move nodes onto facet noise.
    std::vector<std::uint32_t> stragglers;
    if (projection != nullptr) {
        for (std::size_t work_done = 0; const auto ni : snap_nodes) {
            if (active_fill_progress != nullptr)
                fill_progress_poll(work_done++, snap_nodes.size());
            if (ni >= out.mesh.nodes.size()) {
                continue;
            }
            const auto target = boundary_projection_target(surface, out.mesh.nodes[ni], ni,
                                                           projection, mirror);
            if (target && target->distance > 0.01 * hc && target->distance <= 2.5 * hc) {
                stragglers.push_back(ni);
            }
        }
    }
    if (!stragglers.empty()) {
        // Orbit-locked rescue: one fraction ladder for a whole reflection
        // orbit, accepted only when EVERY member's star accepts it. The
        // shared culprit-aware snap path cannot do this — its per-node
        // quality gates read values that tie across a mirror pair in exact
        // arithmetic and diverge in floating point, and a ladder that
        // stops one member at 0.5 and its image at 0.75 ships the
        // asymmetry (ADR-0036 §9.2).
        mesh::sort_mirror_canonical(out.mesh.nodes, stragglers);
        std::vector<char> rescued(out.mesh.nodes.size(), 0);
        for (std::size_t work_done = 0; const auto seed : stragglers) {
            if (active_fill_progress != nullptr)
                fill_progress_poll(work_done++, stragglers.size());
            if (rescued[seed] != 0) {
                continue;
            }
            const auto group = orbit_of(seed);
            if (group.empty()) {
                continue; // incomplete orbit: no move is symmetric
            }
            for (const auto node : group) {
                rescued[node] = 1;
            }
            std::vector<Eigen::Vector3d> saved;
            std::vector<Eigen::Vector3d> target_pt;
            saved.reserve(group.size());
            target_pt.reserve(group.size());
            bool have_all = true;
            for (const auto node : group) {
                fill_progress_poll();
                const auto target = boundary_projection_target(surface, out.mesh.nodes[node],
                                                               node, projection, mirror);
                if (!target) {
                    have_all = false;
                    break;
                }
                saved.push_back(out.mesh.nodes[node]);
                target_pt.push_back(target->point);
            }
            if (!have_all) {
                continue;
            }
            const auto star_clean = [&] {
                for (const auto node : group) {
                    if (node_offends(node)) {
                        return false;
                    }
                }
                return true;
            };
            // One fraction for the whole orbit, bisected; the ladder rungs
            // are shared with the snap's retreat ladder.
            double good = -1.0;
            double bad = -1.0;
            // Exact placement first, gated on inversion only — the
            // pipeline's standing contract is "inversion blocks, thinness
            // is repaired downstream" (repair_round / ship gate). The
            // floor-gated bisection would otherwise park a node a
            // bracket-width off the exact BRep, outside the fidelity tests'
            // 1e-12·h tolerance.
            {
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    out.mesh.nodes[group[gi]] = target_pt[gi];
                }
                bool invert_free = true;
                for (const auto node : group) {
                    for (const auto ti : incident[node]) {
                        const auto& n = out.mesh.tets[ti];
                        const double v =
                            tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                              out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                        if (!(v > vol_eps)) {
                            invert_free = false;
                            break;
                        }
                    }
                    if (!invert_free) {
                        break;
                    }
                }
                if (invert_free) {
                    good = 1.0;
                }
            }
            if (good < 0.0) {
                for (const double frac : {0.75, 0.5, 0.25}) {
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] =
                            saved[gi] + frac * (target_pt[gi] - saved[gi]);
                    }
                    if (star_clean()) {
                        good = frac;
                        break;
                    }
                    bad = frac;
                }
            }
            if (good < 0.0) {
                // No ladder rung is clean: open room (orbit-locked) and
                // retry once, exactly as the snap's relax rounds do.
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    out.mesh.nodes[group[gi]] = saved[gi];
                }
                if (relax_neighborhood(seed)) {
                    for (const double frac : {1.0, 0.75, 0.5, 0.25}) {
                        for (std::size_t gi = 0; gi < group.size(); ++gi) {
                            out.mesh.nodes[group[gi]] =
                                saved[gi] + frac * (target_pt[gi] - saved[gi]);
                        }
                        if (star_clean()) {
                            good = frac;
                            break;
                        }
                        bad = frac;
                    }
                }
            }
            if (good < 0.0) {
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    out.mesh.nodes[group[gi]] = saved[gi];
                }
                continue;
            }
            if (bad > good) {
                for (int step = 0; step < 6; ++step) {
                    const double mid = 0.5 * (good + bad);
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] =
                            saved[gi] + mid * (target_pt[gi] - saved[gi]);
                    }
                    if (star_clean()) {
                        good = mid;
                    } else {
                        bad = mid;
                    }
                }
            }
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                out.mesh.nodes[group[gi]] = saved[gi] + good * (target_pt[gi] - saved[gi]);
            }
        }
    }
    // Per-node accept/reject re-project for residual outliers (hole kinks).
    // Full projection; keep only if no skin tet inverts.
    {
        const double thr = 0.08 * hc;
        for (std::size_t work_done = 0; auto ni : snap_nodes) {
            if (active_fill_progress != nullptr)
                fill_progress_poll(work_done++, snap_nodes.size());
            if (ni >= out.mesh.nodes.size()) {
                continue;
            }
            const auto target = boundary_projection_target(surface, out.mesh.nodes[ni], ni,
                                                           projection, mirror);
            if (!target || !(target->distance > thr) || target->distance > 2.5 * hc) {
                continue;
            }
            const Eigen::Vector3d saved = out.mesh.nodes[ni];
            // Full projection first, then partial fractions — a partial move
            // still shrinks the residual when the full one inverts a tet.
            static constexpr double kFracs[] = {1.0, 0.6, 0.35};
            for (const double frac : kFracs) {
                out.mesh.nodes[ni] = saved + frac * (target->point - saved);
                bool ok = true;
                for (const auto ti : incident[ni]) {
                    const auto& n = out.mesh.tets[ti];
                    const double v =
                        tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                          out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                    if (v <= vol_eps) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    break;
                }
                out.mesh.nodes[ni] = saved;
            }
        }
    }
    for (auto& n : out.mesh.tets) {
        fill_progress_poll();
        const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                           out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
        if (v < 0.0) {
            std::swap(n[1], n[2]);
        }
    }
}

} // namespace polymesh::mesh::detail
