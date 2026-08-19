// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/feature_pin.hpp"

#include "geom/signal_fft.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace polymesh::mesh {

namespace {

/// Spectral energy kept when re-spacing a closed crease chain. The same
/// fraction the sizing pipeline uses (ADR-0034): high enough that a real
/// feature mode is never dropped, low enough to strip lattice noise.
constexpr double kChainEnergyFraction = 0.995;

/// Move `node` exactly onto `target`, or not at all.
///
/// A partial pin is worse than no pin: the node ends up neither on its feature
/// curve nor on the surface it came from, which is exactly the off-CAD
/// outlier this pass exists to remove (measured on plate_hole: partial pins
/// left the worst boundary node 0.31 h off the BRep, three times the
/// pre-pin figure). Features are all-or-nothing.
bool try_pin(std::vector<Eigen::Vector3d>& nodes, std::uint32_t node,
             const Eigen::Vector3d& target, const NodeOffendsFn& node_offends) {
    const Eigen::Vector3d from = nodes[node];
    nodes[node] = target;
    if (!node_offends || !node_offends(node)) {
        return true;
    }
    nodes[node] = from;
    return false;
}

void set_owner(std::vector<BoundarySupport>* provenance, std::uint32_t node,
               BoundarySupportKind kind, std::uint32_t id) {
    if (provenance == nullptr) {
        return;
    }
    if (provenance->size() <= node) {
        provenance->resize(static_cast<std::size_t>(node) + 1);
    }
    (*provenance)[node] = BoundarySupport{kind, id};
}

/// Cumulative chord stations of a polyline, normalized to [0, 1].
std::vector<double> chord_stations(const std::vector<Eigen::Vector3d>& pts) {
    std::vector<double> s(pts.size(), 0.0);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        s[i] = s[i - 1] + (pts[i] - pts[i - 1]).norm();
    }
    const double total = s.back();
    if (total > 0.0) {
        for (auto& v : s) {
            v /= total;
        }
    }
    return s;
}

/// Arclength parameter in [0,1] of `p` along the sampled polyline `samples`.
double polyline_parameter(const std::vector<Eigen::Vector3d>& samples,
                          const std::vector<double>& stations, const Eigen::Vector3d& p) {
    double best = std::numeric_limits<double>::infinity();
    double best_t = 0.0;
    for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
        const Eigen::Vector3d d = samples[i + 1] - samples[i];
        const double len2 = d.squaredNorm();
        double u = 0.0;
        if (len2 > 0.0) {
            u = std::clamp((p - samples[i]).dot(d) / len2, 0.0, 1.0);
        }
        const Eigen::Vector3d q = samples[i] + u * d;
        const double dist = (p - q).squaredNorm();
        if (dist < best) {
            best = dist;
            best_t = stations[i] + u * (stations[i + 1] - stations[i]);
        }
    }
    return best_t;
}

/// Point on the sampled polyline at arclength parameter `t` in [0,1].
Eigen::Vector3d polyline_point(const std::vector<Eigen::Vector3d>& samples,
                               const std::vector<double>& stations, double t) {
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 0; i + 1 < samples.size(); ++i) {
        if (t <= stations[i + 1] || i + 2 == samples.size()) {
            const double span = stations[i + 1] - stations[i];
            const double u = span > 0.0 ? (t - stations[i]) / span : 0.0;
            return samples[i] + std::clamp(u, 0.0, 1.0) * (samples[i + 1] - samples[i]);
        }
    }
    return samples.back();
}

} // namespace
FeaturePinReport pin_feature_nodes(
    const geom::CadModel& cad, const geom::CadTopology& topo,
    std::vector<Eigen::Vector3d>& nodes,
    const std::vector<std::uint32_t>& boundary_nodes, double h,
    const NodeOffendsFn& node_offends,
    std::vector<BoundarySupport>* provenance, const MirrorFrame* mirror) {
    FeaturePinReport report;
    if (boundary_nodes.empty() || topo.empty() || cad.empty() || !(h > 0.0) ||
        !std::isfinite(h)) {
        return report;
    }

    // Visit order is mesh-level mutation state, not tidiness: `try_pin` accepts
    // or reverts against the shared node array, so an earlier pin decides
    // whether a later one is legal (ADR-0032). Ascending node id made that
    // platform-independent but not mirror-equivariant — node ids do not mirror —
    // so the order runs on the mirror key, with the id only as the final
    // tie-break (ADR-0036).
    std::vector<std::uint32_t> candidates(boundary_nodes.begin(), boundary_nodes.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](std::uint32_t n) { return n >= nodes.size(); }),
                     candidates.end());
    sort_mirror_canonical(nodes, candidates);
    // Orbit map over the node array as this pass finds it: every earlier stage is
    // exactly symmetric by construction, so this is the symmetry the pins must
    // preserve. Node indices never change here, only positions, so the map stays
    // valid while pins are applied.
    const MirrorNodeOrbit orbit_storage(
        mirror != nullptr ? *mirror : MirrorFrame{}, nodes,
        [&] {
            const MirrorKeyFrame frame = mirror_key_frame(nodes);
            return frame.inv_quantum > 0.0 ? 1.0 / frame.inv_quantum : 0.0;
        }());
    const MirrorNodeOrbit* orbit = orbit_storage.active() ? &orbit_storage : nullptr;
    if (candidates.empty()) {
        return report;
    }

    // ---- 1. CAD vertices ------------------------------------------------
    // One node per vertex, the nearest within 0.75 h. A vertex owns its node
    // outright: every later pass must treat it as frozen, so claiming a node
    // that a sharp edge would also want is correct precedence.
    std::unordered_set<std::uint32_t> claimed;
    claimed.reserve(candidates.size());
    for (const auto& vertex : topo.vertices) {
        std::uint32_t best = 0;
        double best_d = 0.75 * h;
        bool found = false;
        for (const auto ni : candidates) {
            if (claimed.count(ni) != 0) {
                continue;
            }
            const double d = (nodes[ni] - vertex.position).norm();
            if (d < best_d) {
                best_d = d;
                best = ni;
                found = true;
            }
        }
        if (!found) {
            continue;
        }
        const auto exact = geom::project_point_on_vertex(cad, vertex.id, nodes[best]);
        if (!exact) {
            continue;
        }
        const Eigen::Vector3d vertex_target =
            mirror != nullptr ? mirror->clamp_to_planes(exact->point, nodes[best])
                              : exact->point;
        // The whole orbit of the claimed node is pinned together. A CAD vertex and
        // its mirror image are two separate topology entries visited in kernel
        // order, so pinning each one when its own turn came let the validity gate
        // accept one and refuse the other. Measured on plate_hole at h = 6 mm with
        // every other stage exact: two box-corner/edge nodes lost their mirror
        // image and took 8 tets with them.
        std::vector<std::uint32_t> group{best};
        if (orbit != nullptr) {
            for (unsigned mask = 1; mask <= orbit->reflection_count(); ++mask) {
                const std::uint32_t other = orbit->reflected(best, mask);
                if (other == MirrorNodeOrbit::npos) {
                    group.clear();
                    break;
                }
                if (other != best && claimed.count(other) == 0 &&
                    std::find(group.begin(), group.end(), other) == group.end()) {
                    group.push_back(other);
                }
            }
        }
        if (group.empty()) {
            continue; // incomplete orbit: leave the node where the snap put it
        }
        // Each copy is pinned onto ITS OWN nearest exact CAD vertex, which for a
        // symmetric solid is the mirror image of this one.
        std::vector<std::pair<Eigen::Vector3d, std::uint32_t>> group_target;
        group_target.reserve(group.size());
        bool have_targets = true;
        for (const auto node : group) {
            if (node == best) {
                group_target.emplace_back(vertex_target, vertex.id);
                continue;
            }
            const geom::CadVertex* nearest = nullptr;
            double nearest_d = std::numeric_limits<double>::infinity();
            for (const auto& other_vertex : topo.vertices) {
                const double d = (nodes[node] - other_vertex.position).norm();
                if (d < nearest_d) {
                    nearest_d = d;
                    nearest = &other_vertex;
                }
            }
            if (nearest == nullptr || nearest_d > 0.75 * h) {
                have_targets = false;
                break;
            }
            const auto other_exact =
                geom::project_point_on_vertex(cad, nearest->id, nodes[node]);
            if (!other_exact) {
                have_targets = false;
                break;
            }
            group_target.emplace_back(
                mirror != nullptr
                    ? mirror->clamp_to_planes(other_exact->point, nodes[node])
                    : other_exact->point,
                nearest->id);
        }
        if (!have_targets) {
            continue;
        }
        std::vector<Eigen::Vector3d> group_saved;
        group_saved.reserve(group.size());
        for (const auto node : group) {
            group_saved.push_back(nodes[node]);
        }
        bool all_pinned = true;
        for (std::size_t gi = 0; gi < group.size(); ++gi) {
            if (!try_pin(nodes, group[gi], group_target[gi].first, node_offends)) {
                all_pinned = false;
                break;
            }
        }
        if (!all_pinned) {
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                nodes[group[gi]] = group_saved[gi];
            }
            ++report.rejected;
            continue;
        }
        // Owner ids are canonical, not per-node: every later projection folds its
        // query into the low octant (mesh/mirror.hpp), so an owner recorded as the
        // node's OWN nearest CAD vertex would be projected from a folded query and
        // answer with a point in the wrong octant. Measured on plate_hole at
        // h = 6 mm: the very next snap round pulled such a node 2.4 mm — 0.4 h —
        // off the corner it had just been pinned to, while its mirror image stayed,
        // and those two nodes were the last 8 unmirrored tets in the part.
        std::uint32_t canonical_owner = group_target.front().second;
        if (orbit != nullptr) {
            const auto [source, mask] = orbit->canonical(best);
            (void)mask;
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                if (group[gi] == source) {
                    canonical_owner = group_target[gi].second;
                    break;
                }
            }
        }
        for (std::size_t gi = 0; gi < group.size(); ++gi) {
            claimed.insert(group[gi]);
            set_owner(provenance, group[gi], BoundarySupportKind::kCadVertex,
                      canonical_owner);
            ++report.vertex_pinned;
        }
    }

    // ---- 2. Sharp edge chains -------------------------------------------
    // Two radii, and they do different jobs.
    //
    // `capture_r` decides who is *considered* a crease node: wide enough that
    // the lattice node straddling a crease is caught.
    //
    // `travel_r` decides who is actually *moved*. Without it the pass drags
    // wall nodes half a cell onto the rim and their incident faces come with
    // them — visible in the compare_meshers tet tile as triangular flaps
    // standing off the hole. A node further than this from the curve is not a
    // crease node that drifted; it is a wall node, and its own face owns it.
    const double capture_r = 0.5 * h;
    const double travel_r = 0.35 * h;
    // Targets for EVERY sharp edge are collected first, then symmetrised across
    // reflection orbits, and only then applied.
    //
    // Collecting globally is what makes the symmetrisation possible at all: a
    // node's reflection orbit routinely spans several CAD edges. The two rim
    // circles of a cylinder are one orbit under the z mirror but two topological
    // edges, and the four vertical edges of a plate are one orbit under x and y.
    // Symmetrising inside a single edge's chain therefore refused almost every
    // pin it should have made — measured on cylinder.step at h = 8 mm, rim chains
    // pinned dropped to zero and the shipped facet-normal p99 rose from 0.35° to
    // 1.28°.
    //
    // Two properties are wanted from the symmetrisation, and both come from using
    // the canonical orbit member as the single source of truth:
    //   * The Fourier re-spacing is a GLOBAL operation on a closed chain, and it
    //     does not commute with reflecting that chain (reflection reverses the
    //     parameterisation, and a low-pass of the reversed signal against the
    //     uniform ramp is not the reverse of the low-passed signal). Re-spacing one
    //     octant and reflecting the result keeps the regularity the pass exists for
    //     and makes it exact.
    //   * The recorded OWNER must be the canonical member's edge, because every
    //     later projection folds its query into the low octant: an owner recorded
    //     as the node's own edge would then be projected from a folded query and
    //     answer in the wrong octant. Measured on plate_hole at h = 6 mm, that
    //     inconsistency let the next snap round pull a freshly pinned box-corner
    //     node 2.4 mm (0.4 h) off its corner while its mirror image stayed put.
    struct ChainPin {
        Eigen::Vector3d point = Eigen::Vector3d::Zero();
        std::uint32_t edge_id = 0;
    };
    std::unordered_map<std::uint32_t, ChainPin> chain_target;
    std::size_t n_chains = 0;
    for (const auto& edge : topo.edges) {
        if (edge.feature != geom::CadEdgeFeature::kSharp || edge.samples.size() < 2) {
            continue;
        }
        const std::vector<double> stations = chord_stations(edge.samples);
        if (!(stations.back() > 0.0)) {
            continue;
        }

        struct Pinned {
            std::uint32_t node;
            double t;
        };
        std::vector<Pinned> chain;
        for (const auto ni : candidates) {
            if (claimed.count(ni) != 0) {
                continue;
            }
            const double t = polyline_parameter(edge.samples, stations, nodes[ni]);
            const Eigen::Vector3d on_curve = polyline_point(edge.samples, stations, t);
            if ((nodes[ni] - on_curve).norm() > capture_r) {
                continue;
            }
            chain.push_back({ni, t});
        }
        if (chain.empty()) {
            continue;
        }
        std::sort(chain.begin(), chain.end(),
                  [](const Pinned& a, const Pinned& b) { return a.t < b.t; });

        // Fourier re-spacing for closed chains. A closed sharp edge (a bore
        // rim, a sphere-cap seam) has a periodic coordinate signal; the
        // lattice samples it unevenly, and that unevenness is the visible
        // sawtooth. Low-passing the *parameter* signal against a uniform
        // ramp removes the lattice's own frequency content while leaving the
        // curve untouched — the pin target below is still the exact OCC
        // projection, only its parameter has been regularized.
        const bool closed = edge.v0 == edge.v1 &&
                            (edge.samples.front() - edge.samples.back()).norm() <=
                                1e-9 * std::max(1.0, edge.length);
        if (closed && chain.size() >= 8) {
            std::vector<double> idx(chain.size());
            std::vector<double> param(chain.size());
            for (std::size_t i = 0; i < chain.size(); ++i) {
                idx[i] = static_cast<double>(i);
                // Deviation of the measured parameter from a perfectly
                // uniform chain: a zero-mean periodic signal whose content is
                // lattice noise plus any genuine crowding the geometry needs.
                param[i] = chain[i].t - static_cast<double>(i) / static_cast<double>(
                                                                    chain.size());
            }
            geom::FilterReport filter;
            const auto smoothed = geom::lowpass_signal_periodic(idx, param,
                                                                kChainEnergyFraction, &filter);
            for (std::size_t i = 0; i < chain.size(); ++i) {
                double t = smoothed[i] + static_cast<double>(i) /
                                             static_cast<double>(chain.size());
                chain[i].t = std::clamp(t, 0.0, 1.0);
            }
        }

        bool contributed = false;
        for (const auto& pin : chain) {
            const Eigen::Vector3d seed = polyline_point(edge.samples, stations, pin.t);
            const auto exact = geom::project_point_on_edge(cad, edge.id, seed);
            if (!exact) {
                continue;
            }
            // Two guards on the actual move. A re-spaced target must not
            // teleport a node along the curve (fall back to the plain nearest
            // point), and no pin may drag a node further than `travel_r`
            // across the wall.
            Eigen::Vector3d target = exact->point;
            if ((target - nodes[pin.node]).norm() > travel_r) {
                const auto direct = geom::project_point_on_edge(cad, edge.id, nodes[pin.node]);
                if (!direct) {
                    continue;
                }
                target = direct->point;
            }
            if ((target - nodes[pin.node]).norm() > travel_r) {
                continue; // a wall node, not a crease node that drifted
            }
            // A node sitting on a mirror plane stays on it: the exact curve
            // projection is free to leave the plane by a rounding error, and that
            // error is a broken symmetry all by itself.
            if (mirror != nullptr) {
                target = mirror->clamp_to_planes(target, nodes[pin.node]);
            }
            chain_target.insert_or_assign(pin.node, ChainPin{target, edge.id});
            contributed = true;
        }
        if (contributed) {
            ++n_chains;
        }
    }
    if (orbit != nullptr) {
        std::unordered_map<std::uint32_t, ChainPin> mirrored;
        mirrored.reserve(chain_target.size());
        for (const auto& [node, pin] : chain_target) {
            const auto [source, mask] = orbit->canonical(node);
            if (source == MirrorNodeOrbit::npos) {
                continue;
            }
            const auto it = chain_target.find(source);
            if (it == chain_target.end()) {
                continue; // canonical member is not pinned: refuse the copy
            }
            mirrored.insert_or_assign(
                node, ChainPin{orbit->reflect(it->second.point, mask), it->second.edge_id});
        }
        chain_target = std::move(mirrored);
    }
    // Apply in orbit groups so a validity refusal takes the whole group.
    std::unordered_set<std::uint32_t> applied;
    for (const auto ni : candidates) {
        const auto target_it = chain_target.find(ni);
        if (target_it == chain_target.end() || applied.count(ni) != 0) {
            continue;
        }
        std::vector<std::uint32_t> group{ni};
        if (orbit != nullptr) {
            for (unsigned mask = 1; mask <= orbit->reflection_count(); ++mask) {
                const std::uint32_t other = orbit->reflected(ni, mask);
                if (other == MirrorNodeOrbit::npos || other == ni) {
                    continue;
                }
                if (chain_target.find(other) == chain_target.end()) {
                    group.clear();
                    break;
                }
                if (std::find(group.begin(), group.end(), other) == group.end()) {
                    group.push_back(other);
                }
            }
        }
        if (group.empty()) {
            ++report.rejected;
            continue;
        }
        std::vector<Eigen::Vector3d> saved;
        saved.reserve(group.size());
        for (const auto node : group) {
            saved.push_back(nodes[node]);
        }
        bool all_pinned = true;
        for (const auto node : group) {
            if (!try_pin(nodes, node, chain_target.at(node).point, node_offends)) {
                all_pinned = false;
                break;
            }
        }
        if (!all_pinned) {
            for (std::size_t i = 0; i < group.size(); ++i) {
                nodes[group[i]] = saved[i];
            }
            ++report.rejected;
            continue;
        }
        for (const auto node : group) {
            applied.insert(node);
            claimed.insert(node);
            const std::uint32_t owner_edge = chain_target.at(node).edge_id;
            set_owner(provenance, node, BoundarySupportKind::kCadEdge, owner_edge);
            ++report.edge_pinned;
            const auto residual = geom::project_point_on_edge(cad, owner_edge, nodes[node]);
            if (residual) {
                report.max_edge_residual =
                    std::max(report.max_edge_residual, residual->distance);
            }
        }
    }
    report.chains = n_chains;

    // Post-pin census: which boundary node is worst against the exact BRep,
    // and who owns it. This is the number the fidelity metric will report, so
    // measuring it here with the owner attached is what turns "the mesh is off
    // the CAD" into "this owner class could not be reached".
    for (const auto ni : candidates) {
        const auto exact = geom::project_point_on_surface(cad, nodes[ni]);
        if (!exact || exact->distance <= report.worst_node_distance) {
            continue;
        }
        report.worst_node_distance = exact->distance;
        report.worst_node = ni;
        report.worst_node_owner = (provenance != nullptr && ni < provenance->size())
                                      ? (*provenance)[ni].kind
                                      : BoundarySupportKind::kUnknown;
    }

    return report;
}

} // namespace polymesh::mesh
