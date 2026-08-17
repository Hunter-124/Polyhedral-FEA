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

FeaturePinReport pin_feature_nodes(const geom::CadModel& cad, const geom::CadTopology& topo,
                                   std::vector<Eigen::Vector3d>& nodes,
                                   const std::vector<std::uint32_t>& boundary_nodes, double h,
                                   const NodeOffendsFn& node_offends,
                                   std::vector<BoundarySupport>* provenance) {
    FeaturePinReport report;
    if (boundary_nodes.empty() || topo.empty() || cad.empty() || !(h > 0.0) ||
        !std::isfinite(h)) {
        return report;
    }

    // Ascending node order everywhere: the acceptance test reads the shared
    // node array, so visit order is mesh-level mutation state (ADR-0032).
    std::vector<std::uint32_t> candidates(boundary_nodes.begin(), boundary_nodes.end());
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [&](std::uint32_t n) { return n >= nodes.size(); }),
                     candidates.end());
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
        if (!try_pin(nodes, best, exact->point, node_offends)) {
            ++report.rejected;
            continue;
        }
        claimed.insert(best);
        set_owner(provenance, best, BoundarySupportKind::kCadVertex, vertex.id);
        ++report.vertex_pinned;
    }

    // ---- 2. Sharp edge chains -------------------------------------------
    // Capture radius 0.5 h: wide enough that the lattice node straddling a
    // crease is caught, narrow enough that a wall node one cell away is not
    // dragged onto the edge (which would chamfer the crease from the other
    // side).
    const double capture_r = 0.5 * h;
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

        bool used = false;
        for (const auto& pin : chain) {
            const Eigen::Vector3d seed = polyline_point(edge.samples, stations, pin.t);
            const auto exact = geom::project_point_on_edge(cad, edge.id, seed);
            if (!exact) {
                continue;
            }
            // A re-spaced target must not teleport a node: cap travel at one
            // cell so a chain with a bad parameter estimate degrades to the
            // plain nearest-point pin instead of shuffling the crease.
            if ((exact->point - nodes[pin.node]).norm() > 1.0 * h) {
                const auto direct = geom::project_point_on_edge(cad, edge.id, nodes[pin.node]);
                if (!direct) {
                    continue;
                }
                if (!try_pin(nodes, pin.node, direct->point, node_offends)) {
                    ++report.rejected;
                    continue;
                }
            } else {
                if (!try_pin(nodes, pin.node, exact->point, node_offends)) {
                    ++report.rejected;
                    continue;
                }
            }
            claimed.insert(pin.node);
            set_owner(provenance, pin.node, BoundarySupportKind::kCadEdge, edge.id);
            ++report.edge_pinned;
            used = true;
            const auto residual = geom::project_point_on_edge(cad, edge.id, nodes[pin.node]);
            if (residual) {
                report.max_edge_residual =
                    std::max(report.max_edge_residual, residual->distance);
            }
        }
        if (used) {
            ++report.chains;
        }
    }

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
