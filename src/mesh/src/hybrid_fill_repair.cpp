// SPDX-License-Identifier: BSD-3-Clause
// Sliver-cap collapse and void/flake carve round of the graded tet fill.
#include "hybrid_fill_internal.hpp"
#include "topology_keys.hpp"

#include "mesh/fill_progress.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh::detail {
namespace {

/// Ordering key for a geometric scalar that must treat a mirrored pair's values
/// as EQUAL, so the tie falls through to the mirror key rather than being
/// decided by rounding noise (ADR-0036).
///
/// A cap and its mirror image tie in exact arithmetic — same aspect, same six
/// edge lengths, same collapse scores — but their coordinates are reflections
/// computed in floating point, so each quantity differs in the last ulp or two.
///
/// Quantising at 1e-9 of the quantity's own scale is nine orders above the noise
/// and nine below any difference this mesher acts on. It is a quantisation and
/// not an epsilon comparison on purpose: an epsilon comparison is not transitive,
/// and `std::sort` requires a strict weak ordering.
[[nodiscard]] long long tie_key(double value, double scale) {
    if (!std::isfinite(value)) {
        return value > 0.0 ? std::numeric_limits<long long>::max()
                           : std::numeric_limits<long long>::min();
    }
    const double quantum = 1e-9 * (scale > 0.0 ? scale : 1.0);
    return static_cast<long long>(std::llround(value / quantum));
}

} // namespace

void repair_round(GradedFillState& s, bool collisions_only) {
    const geom::TriSurface& surface = s.surface;
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    const std::vector<double>& original_spacing = s.original_spacing;
    const std::unordered_set<std::uint32_t>& initial_juts = s.initial_juts;
    const auto projected_collision = [&](const std::array<std::uint32_t, 4>& tet) {
        constexpr double kProjectionTravelFraction = 0.95;
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                const double spacing =
                    std::min(original_spacing[tet[a]], original_spacing[tet[b]]);
                if ((out.mesh.nodes[tet[a]] - out.mesh.nodes[tet[b]]).norm() <
                    (1.0 - kProjectionTravelFraction) * spacing)
                    return true;
            }
        }
        return false;
    };
    if (collisions_only &&
        std::none_of(out.mesh.tets.begin(), out.mesh.tets.end(), projected_collision))
        return;
    constexpr double kCapAspect = 0.05;  // caps live far below Kuhn ~0.27
    constexpr double kKeepAspect = 0.04; // incident tets must stay above
    constexpr int kCollapsePasses = 5;
    const double vol_eps = 1e-14 * hc * hc * hc;
    const auto aspect_of = [&](const std::array<std::uint32_t, 4>& n) {
        const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
        const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
        const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
        const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
        const double v = std::abs(tet_signed_volume(a, b, c, d));
        const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                      (b - c).norm(), (b - d).norm(), (c - d).norm()});
        if (emax <= 0.0) {
            return 0.0;
        }
        return std::min(1.0, 6.0 * 1.4142135623730951 * v / (emax * emax * emax));
    };
    std::vector<std::uint32_t> node_remap(out.mesh.nodes.size());
    for (std::uint32_t ni = 0; ni < node_remap.size(); ++ni) {
        node_remap[ni] = ni;
    }
    bool collapsed_any = false;
    for (int pass = 0; pass < kCollapsePasses; ++pass) {
        progress.set_cells(0, out.mesh.tets.size());
        progress.set_phase(collisions_only ? "quality_collision_pass"
                                           : "quality_collapse_pass",
                           pass + 1, kCollapsePasses);
        // `try_collapse` checks that no incident tet inverts or degrades,
        // which is necessary and not sufficient: an edge collapse also has
        // to satisfy the link condition, or it welds the complex to itself
        // and slits the skin. Rather than evaluate that condition -- which
        // is subtle in 3D and easy to get subtly wrong -- the pass is run
        // and then checked, and reverted whole if it tore anything. A
        // sliver that survives is a quality problem; a torn skin is a
        // correctness one, and the two are not tradeable.
        const auto tets_before = out.mesh.tets;
        const auto remap_before = node_remap;
        const std::size_t torn_before = tet_shell_topology(out.mesh.tets).n_torn_edges;
        // Slivers are, by definition, almost no material: a pass that
        // cleans them up loses a rounding error of volume. A pass that
        // loses real volume is not cleaning up, it is eating the part.
        const auto total_volume = [&]() {
            double v = 0.0;
            for (const auto& t : out.mesh.tets) {
                fill_progress_poll();
                v += std::abs(tet_signed_volume(out.mesh.nodes[t[0]], out.mesh.nodes[t[1]],
                                                out.mesh.nodes[t[2]], out.mesh.nodes[t[3]]));
            }
            return v;
        };
        const double volume_before = total_volume();
        const auto bvec = tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
        const std::unordered_set<std::uint32_t> bset(bvec.begin(), bvec.end());
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> incident;
        incident.reserve(out.mesh.nodes.size());
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
            for (const auto ni : out.mesh.tets[ti]) {
                incident[ni].push_back(ti);
            }
        }
        std::vector<char> removed(out.mesh.tets.size(), 0);
        std::size_t removed_count = 0;
        FillProgressElementsScope live_elements(out.mesh.tets, &removed_count);
        bool any = false;
        // Viability/quality of merging `dead` into `surv`, as the worst
        // post-collapse aspect over the incident star, or -infinity when the
        // merge is not legal. Pure: reads the mesh, never mutates it, so a
        // collapse direction can be *chosen* on quality before it is applied.
        const auto collapse_score = [&](std::uint32_t dead, std::uint32_t surv) {
            // Never pull the surface inward: a boundary node may only merge
            // into another boundary node.
            if (bset.count(dead) && !bset.count(surv)) {
                return -std::numeric_limits<double>::infinity();
            }
            const auto it = incident.find(dead);
            if (it == incident.end()) {
                return -std::numeric_limits<double>::infinity();
            }
            double worst = std::numeric_limits<double>::infinity();
            for (const auto tj : it->second) {
                fill_progress_poll();
                if (removed[tj]) {
                    continue;
                }
                auto probe = out.mesh.tets[tj];
                const double aspect_before = aspect_of(probe);
                bool has_surv = false;
                for (auto& nn : probe) {
                    if (nn == surv) {
                        has_surv = true;
                    }
                    if (nn == dead) {
                        nn = surv;
                    }
                }
                if (has_surv) {
                    continue; // degenerates away with the collapse
                }
                const double v =
                    tet_signed_volume(out.mesh.nodes[probe[0]], out.mesh.nodes[probe[1]],
                                      out.mesh.nodes[probe[2]], out.mesh.nodes[probe[3]]);
                // Reject inversions outright; otherwise accept when the
                // neighbor stays healthy — or at least does not get worse
                // (sliver clusters heal stepwise).
                const double aspect_after = aspect_of(probe);
                const bool was_healthy = aspect_before >= kKeepAspect;
                if (v <= 0.0 || (was_healthy && v <= vol_eps) ||
                    (aspect_after < kKeepAspect && aspect_after < aspect_before)) {
                    return -std::numeric_limits<double>::infinity();
                }
                worst = std::min(worst, aspect_after);
            }
            return worst;
        };
        const auto try_collapse = [&](std::uint32_t dead, std::uint32_t surv) {
            if (!std::isfinite(collapse_score(dead, surv))) {
                return false;
            }
            const auto it = incident.find(dead);
            for (const auto tj : it->second) {
                fill_progress_poll();
                if (removed[tj]) {
                    continue;
                }
                auto& t = out.mesh.tets[tj];
                bool has_surv = false;
                for (const auto nn : t) {
                    if (nn == surv) {
                        has_surv = true;
                        break;
                    }
                }
                if (has_surv) {
                    removed[tj] = 1;
                    ++removed_count;
                    continue;
                }
                for (auto& nn : t) {
                    if (nn == dead) {
                        nn = surv;
                    }
                }
                incident[surv].push_back(tj);
            }
            node_remap[dead] = surv;
            any = true;
            return true;
        };

        // Collapse decisions are sequential and their inputs tie constantly on
        // a symmetric mesh (equal edge lengths, equal aspects), so every
        // ordering below runs through this frame instead of node/tet indices,
        // which do not mirror (ADR-0036).
        const MirrorKeyFrame mkey = mirror_key_frame(out.mesh.nodes);

        // Collapse the whole reflection orbit or none of it.
        //
        // Ordering the decisions equivariantly is not enough: along a ring of
        // caps a greedy sweep that merges adjacent wall nodes commits a
        // matching, and a matching chosen one cap at a time need not be
        // mirror-symmetric even when every individual choice is (ADR-0036).
        //
        // So a collapse is applied to every reflected copy of itself at once,
        // and refused unless
        //   * every reflected copy of both endpoints exists and is still live,
        //   * every copy is legal on its own (`collapse_score` finite), and
        //   * the copies' incident stars are pairwise DISJOINT.
        // The disjointness requirement is what makes applying them in sequence
        // equivalent to applying them simultaneously: with disjoint stars no
        // copy can change another's legality. It refuses collapses within a
        // cell of a mid-plane, and a node ON a plane — whose two candidate
        // survivors are reflections of each other — can never collapse, which
        // is correct: either choice would break the symmetry it sits on.
        const double orbit_tol = mkey.inv_quantum > 0.0 ? 1.0 / mkey.inv_quantum : 0.0;
        const MirrorNodeOrbit orbit =
            mirror != nullptr ? MirrorNodeOrbit(*mirror, out.mesh.nodes, orbit_tol)
                              : MirrorNodeOrbit(MirrorFrame{}, out.mesh.nodes, 0.0);
        const auto collapse_orbit = [&](std::uint32_t dead, std::uint32_t surv) {
            if (!orbit.active()) {
                return try_collapse(dead, surv);
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> copies;
            copies.reserve(8);
            copies.emplace_back(dead, surv);
            for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
                const std::uint32_t d2 = orbit.reflected(dead, mask);
                const std::uint32_t s2 = orbit.reflected(surv, mask);
                if (d2 == MirrorNodeOrbit::npos || s2 == MirrorNodeOrbit::npos) {
                    return false;
                }
                if (d2 == dead && s2 == surv) {
                    continue; // this reflection fixes the edge
                }
                if (d2 == dead || s2 == surv || d2 == surv || s2 == dead) {
                    return false; // edge meets its own reflection
                }
                if (std::find(copies.begin(), copies.end(),
                              std::pair<std::uint32_t, std::uint32_t>{d2, s2}) !=
                    copies.end()) {
                    continue;
                }
                copies.emplace_back(d2, s2);
            }
            // Disjointness is required BETWEEN copies, never within one: a
            // copy's own two endpoints necessarily share the tets on the edge
            // being collapsed.
            std::unordered_set<std::size_t> other_stars;
            for (const auto& [d2, s2] : copies) {
                if (node_remap[d2] != d2 || node_remap[s2] != s2) {
                    return false;
                }
                if (!std::isfinite(collapse_score(d2, s2))) {
                    return false;
                }
                std::vector<std::size_t> star;
                for (const auto node : {d2, s2}) {
                    const auto it = incident.find(node);
                    if (it == incident.end()) {
                        return false;
                    }
                    for (const auto tj : it->second) {
                        if (!removed[tj]) {
                            star.push_back(tj);
                        }
                    }
                }
                for (const auto tj : star) {
                    if (other_stars.count(tj) != 0) {
                        return false; // copies interact: not independent
                    }
                }
                other_stars.insert(star.begin(), star.end());
            }
            bool applied = false;
            for (const auto& [d2, s2] : copies) {
                applied = try_collapse(d2, s2) || applied;
            }
            return applied;
        };
        // Phase A — void juts: boundary nodes whose projection the snap had
        // to reject (hole-rim stair chords poking into the void) merge into
        // an adjacent on-surface boundary node instead of leaving a spike.
        if (!collisions_only)
            for (std::size_t work_done = 0; const auto ni : bvec) {
                if (active_fill_progress != nullptr)
                    fill_progress_poll(work_done++, bvec.size());
                if (node_remap[ni] != ni || !initial_juts.contains(ni)) {
                    continue;
                }
                const double resid = surface_distance(surface, mirror, out.mesh.nodes[ni]);
                if (resid <= 0.15 * hc) {
                    continue;
                }
                const auto it = incident.find(ni);
                if (it == incident.end()) {
                    continue;
                }
                std::vector<std::pair<double, std::uint32_t>> cand;
                for (const auto tj : it->second) {
                    fill_progress_poll();
                    if (removed[tj]) {
                        continue;
                    }
                    for (const auto nn : out.mesh.tets[tj]) {
                        if (nn == ni || !bset.count(nn)) {
                            continue;
                        }
                        cand.push_back({(out.mesh.nodes[nn] - out.mesh.nodes[ni]).norm(), nn});
                    }
                }
                // Distance ties break on the mirror key, not the node id, so a
                // jut and its mirror image merge toward mirrored neighbours. The
                // length itself is compared through `tie_key`: mirrored lengths
                // agree only to the last ulp, and ordering on that noise is what
                // sent mirrored juts to unmirrored survivors (ADR-0036).
                std::sort(cand.begin(), cand.end(), [&](const auto& x, const auto& y) {
                    const auto lx = tie_key(x.first, hc);
                    const auto ly = tie_key(y.first, hc);
                    if (lx != ly) {
                        return lx < ly;
                    }
                    const auto kx = mkey.key(out.mesh.nodes[x.second]);
                    const auto ky = mkey.key(out.mesh.nodes[y.second]);
                    return kx != ky ? kx < ky : x.second < y.second;
                });
                cand.erase(std::unique(cand.begin(), cand.end(),
                                       [](const auto& x, const auto& y) {
                                           return x.second == y.second;
                                       }),
                           cand.end());
                for (const auto& [len, surv] : cand) {
                    fill_progress_poll();
                    if (surface_distance(surface, mirror, out.mesh.nodes[surv]) > 0.05 * hc) {
                        continue;
                    }
                    if (collapse_orbit(ni, surv)) {
                        break;
                    }
                }
            }

        // Phase B — sliver caps: collapse the shortest viable edge of the
        // worst cap first. Index order fails the sphere residual and
        // centre-out radial order breaks the hole plate (it starts at the
        // hole rim, the one boundary that must not move first); quality, not
        // geometry, is the only visit order that respects both.
        const auto& mkey_b = mkey;
        const auto tet_center_b = [&](std::size_t ti) {
            const auto& n = out.mesh.tets[ti];
            return 0.25 * (out.mesh.nodes[n[0]] + out.mesh.nodes[n[1]] + out.mesh.nodes[n[2]] +
                           out.mesh.nodes[n[3]]);
        };
        std::vector<std::size_t> cap_order;
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
            if (!removed[ti] && aspect_of(out.mesh.tets[ti]) < kCapAspect &&
                (!collisions_only || projected_collision(out.mesh.tets[ti]))) {
                cap_order.push_back(ti);
            }
        }
        std::sort(cap_order.begin(), cap_order.end(), [&](std::size_t a, std::size_t b) {
            const auto qa = tie_key(aspect_of(out.mesh.tets[a]), 1.0);
            const auto qb = tie_key(aspect_of(out.mesh.tets[b]), 1.0);
            if (qa != qb) {
                return qa < qb;
            }
            const auto ka = mkey_b.key(tet_center_b(a));
            const auto kb = mkey_b.key(tet_center_b(b));
            return ka != kb ? ka < kb : a < b;
        });
        for (std::size_t work_done = 0; const auto ti : cap_order) {
            if (active_fill_progress != nullptr)
                fill_progress_poll(work_done++, cap_order.size());
            if (removed[ti] || aspect_of(out.mesh.tets[ti]) >= kCapAspect) {
                continue;
            }
            const auto tet = out.mesh.tets[ti];
            // Candidate edges by ascending length; the six edges of a lattice
            // cap tie on length constantly, and the tie-break decides which
            // edge collapses — keys, not ids, so a mirrored cap tries its
            // edges in mirrored order (ADR-0036).
            std::array<std::pair<double, std::array<std::uint32_t, 2>>, 6> edges_len{};
            int ne = 0;
            for (int p = 0; p < 4; ++p) {
                for (int q2 = p + 1; q2 < 4; ++q2) {
                    const std::uint32_t a = tet[static_cast<std::size_t>(p)];
                    const std::uint32_t b = tet[static_cast<std::size_t>(q2)];
                    edges_len[static_cast<std::size_t>(ne++)] = {
                        (out.mesh.nodes[a] - out.mesh.nodes[b]).norm(), {a, b}};
                }
            }
            // A cap's six edges must be tried in mirrored order, so the
            // tie-break has to identify an EDGE mirror-invariantly, not just
            // its lower endpoint: two edges sharing that endpoint tie, and a
            // node-id tie-break does not mirror. The sorted pair of endpoint
            // mirror keys is unique per edge here: no lattice edge joins two
            // nodes of the same reflection orbit, because even cell counts keep
            // every cell — and therefore every tet and every edge — off the
            // mid-planes.
            using EdgeMirrorKey = std::array<std::array<long long, 3>, 2>;
            const auto edge_mirror_key = [&](std::array<std::uint32_t, 2> e) {
                const auto k0 = mkey.key(out.mesh.nodes[e[0]]);
                const auto k1 = mkey.key(out.mesh.nodes[e[1]]);
                return k1 < k0 ? EdgeMirrorKey{{k1, k0}} : EdgeMirrorKey{{k0, k1}};
            };
            std::sort(edges_len.begin(), edges_len.end(), [&](const auto& x, const auto& y) {
                const auto lx = tie_key(x.first, hc);
                const auto ly = tie_key(y.first, hc);
                if (lx != ly) {
                    return lx < ly;
                }
                const auto kx = edge_mirror_key(x.second);
                const auto ky = edge_mirror_key(y.second);
                return kx != ky ? kx < ky
                                : std::min(x.second[0], x.second[1]) <
                                      std::min(y.second[0], y.second[1]);
            });
            bool done = false;
            for (int e = 0; e < ne && !done; ++e) {
                const std::uint32_t a = edges_len[static_cast<std::size_t>(e)].second[0];
                const std::uint32_t b = edges_len[static_cast<std::size_t>(e)].second[1];
                if (node_remap[a] != a || node_remap[b] != b) {
                    continue;
                }
                // When both directions are legal, keep the one whose incident
                // star survives in better shape. Aspect is mirror-invariant,
                // so a cap and its mirror image collapse in mirrored
                // directions — but only when the two scores are compared
                // through `tie_key`. On a symmetric lattice the scores tie
                // exactly in exact arithmetic and differ in the last ulp in
                // floating point, so a raw comparison picked the direction
                // from that noise and mirrored caps collapsed opposite ways.
                // A genuine tie falls back to farther-from-centre.
                const double sa = collapse_score(a, b); // a dies
                const double sb = collapse_score(b, a); // b dies
                if (!std::isfinite(sa) && !std::isfinite(sb)) {
                    continue;
                }
                const auto qa = tie_key(sa, 1.0);
                const auto qb = tie_key(sb, 1.0);
                std::uint32_t dead;
                std::uint32_t surv;
                if (qa > qb) {
                    dead = a;
                    surv = b;
                } else if (qb > qa) {
                    dead = b;
                    surv = a;
                } else {
                    const auto ka = mkey.key(out.mesh.nodes[a]);
                    const auto kb = mkey.key(out.mesh.nodes[b]);
                    const bool a_dies = ka < kb || (ka == kb && a < b);
                    dead = a_dies ? a : b;
                    surv = a_dies ? b : a;
                }
                done = collapse_orbit(dead, surv);
            }
        }

        if (!any) {
            break;
        }
        std::size_t w = 0;
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
            if (!removed[ti]) {
                out.mesh.tets[w++] = out.mesh.tets[ti];
            }
        }
        out.mesh.tets.resize(w);
        removed_count = 0;
        // 0.5% of the part per pass: three orders of magnitude more than a
        // sliver sweep needs, and far below the fill guard's 10% limit, so
        // a pass has to be visibly destructive to trip it.
        constexpr double kMaxPassVolumeLoss = 0.005;
        const double volume_after = total_volume();
        if (tet_shell_topology(out.mesh.tets).n_torn_edges > torn_before ||
            volume_after < (1.0 - kMaxPassVolumeLoss) * volume_before) {
            out.mesh.tets = tets_before;
            node_remap = remap_before;
            break;
        }
        collapsed_any = true;
    }
    if (collapsed_any) {
        const auto resolve = [&](std::uint32_t ni) {
            while (node_remap[ni] != ni) {
                ni = node_remap[ni];
            }
            return ni;
        };
        for (auto& q : out.mesh.boundary_quads) {
            for (auto& ni : q) {
                ni = resolve(ni);
            }
        }
    }

    // S5 void carve: nodes the snap could not place on the surface (their
    // projection inverts skin tets — hole-void stair chords) poke into CAD
    // holes. Peel the tets of those *pre-identified* jut nodes as they gain
    // free faces, until the juts drop out of the mesh. Newly exposed nodes
    // are not juts, so the peel cannot run away into the bulk.
    // Also peels *flat caps the collapse could not cure* (aspect below the
    // scorecard floor): a cap that survives S4 is wedged between healthy
    // tets; with a free face it is a zero-thickness skin flake — removing
    // it exposes those healthy faces with negligible volume change.
    if (!collisions_only) {
        constexpr int kCarvePasses = 4;
        constexpr double kPeelAspect = 0.03; // < kKeepAspect: only true flakes
        const auto aspect_peel = [&](const std::array<std::uint32_t, 4>& n) {
            const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
            const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
            const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
            const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
            const double v = std::abs(tet_signed_volume(a, b, c, d));
            const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                          (b - c).norm(), (b - d).norm(), (c - d).norm()});
            if (emax <= 0.0) {
                return true;
            }
            return 6.0 * 1.4142135623730951 * v / (emax * emax * emax) < kPeelAspect;
        };
        const auto& jut = initial_juts;
        for (int pass = 0; pass < kCarvePasses; ++pass) {
            progress.set_phase("quality_carve_pass", pass + 1, kCarvePasses);
            // Free faces per tet (faces appearing once across the mesh).
            std::unordered_map<TriKey, int, TriKeyMixHash> fcount;
            fcount.reserve(out.mesh.tets.size() * 2);
            for (const auto& t : out.mesh.tets) {
                fill_progress_poll();
                for (const auto& f : kTetFaces) {
                    ++fcount[sorted_tri_key(t[static_cast<std::size_t>(f[0])],
                                            t[static_cast<std::size_t>(f[1])],
                                            t[static_cast<std::size_t>(f[2])])];
                }
            }
            std::vector<char> kill(out.mesh.tets.size(), 0);
            std::size_t n_kill = 0;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                fill_progress_poll(ti, out.mesh.tets.size());
                const auto& t = out.mesh.tets[ti];
                bool has_jut = false;
                for (const auto ni : t) {
                    if (jut.count(ni)) {
                        has_jut = true;
                        break;
                    }
                }
                if (!has_jut && !aspect_peel(t)) {
                    continue;
                }
                bool has_free_face = false;
                for (const auto& f : kTetFaces) {
                    if (fcount[sorted_tri_key(t[static_cast<std::size_t>(f[0])],
                                              t[static_cast<std::size_t>(f[1])],
                                              t[static_cast<std::size_t>(f[2])])] == 1) {
                        has_free_face = true;
                        break;
                    }
                }
                if (has_free_face) {
                    kill[ti] = 1;
                    ++n_kill;
                }
            }
            // Deleting a tet that has a free face exposes its other three,
            // which can strand a neighbour with two exposed faces of its
            // own. Same class as the child carve above, same remedy.
            if (n_kill > 0 && n_kill < out.mesh.tets.size()) {
                restrict_kill_to_shell(out.mesh.tets, kill);
                n_kill = static_cast<std::size_t>(
                    std::count(kill.begin(), kill.end(), static_cast<char>(1)));
            }
            if (n_kill == 0 || n_kill >= out.mesh.tets.size()) {
                break;
            }
            std::size_t w = 0;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                fill_progress_poll(ti, out.mesh.tets.size());
                if (!kill[ti]) {
                    out.mesh.tets[w++] = out.mesh.tets[ti];
                }
            }
            out.mesh.tets.resize(w);
        }
    }
}

} // namespace polymesh::mesh::detail
