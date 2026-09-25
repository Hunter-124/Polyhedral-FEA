// SPDX-License-Identifier: BSD-3-Clause
// Boundary smoothing, feature pinning, overlap carve and interior sliver
// relaxation of the graded tet fill.
#include "hybrid_fill_internal.hpp"
#include "topology_keys.hpp"

#include "mesh/cell_validity.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh::detail {

void smooth_boundary_and_pin_features(GradedFillState& s) {
    const geom::TriSurface& surface = s.surface;
    const std::span<const geom::SharpEdge> features = s.features;
    const BoundaryFit* const fit = s.fit;
    BoundaryProjectionContext* const projection = s.projection;
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    progress.set_phase("quality_smoothing");
    std::unordered_map<TriKey, std::array<std::uint32_t, 3>, TriKeyMixHash> once;
    std::unordered_map<TriKey, int, TriKeyMixHash> fcount;
    fcount.reserve(out.mesh.tets.size() * 2);
    for (const auto& t : out.mesh.tets) {
        fill_progress_poll();
        for (const auto& f : kTetFaces) {
            const auto k0 = t[static_cast<std::size_t>(f[0])];
            const auto k1 = t[static_cast<std::size_t>(f[1])];
            const auto k2 = t[static_cast<std::size_t>(f[2])];
            const auto key = sorted_tri_key(k0, k1, k2);
            if (++fcount[key] == 1) {
                once[key] = {k0, k1, k2};
            }
        }
    }
    // Deterministic order, and the reason is not tidiness: `smooth_boundary_nodes`
    // relaxes and re-projects node positions in the order it is handed the
    // faces, reverting moves that invert a tet, so the surviving coordinates
    // depend on that order. Iterating `once` directly would make the mesh a
    // function of the standard library's bucket layout (ADR-0032).
    std::vector<std::array<std::uint32_t, 4>> free_faces;
    free_faces.reserve(once.size() / 2);
    for (const auto& [key, tri] : once) {
        fill_progress_poll();
        if (fcount[key] == 1) {
            free_faces.push_back({tri[0], tri[1], tri[2], tri[2]});
        }
    }
    std::sort(free_faces.begin(), free_faces.end(), [](const auto& l, const auto& r) {
        return std::tie(l[0], l[1], l[2]) < std::tie(r[0], r[1], r[2]);
    });
    const double vol_eps = 1e-14 * hc * hc * hc;
    smooth_boundary_nodes(
        surface, out.mesh.nodes, free_faces, hc,
        [&](std::set<std::uint32_t>& offenders) {
            for (const auto& n : out.mesh.tets) {
                fill_progress_poll();
                const double v =
                    tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                      out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                if (v <= vol_eps) {
                    offenders.insert(n.begin(), n.end());
                }
            }
        },
        /*passes=*/3, /*relax=*/0.5, features, projection, mirror);
    for (auto& n : out.mesh.tets) {
        fill_progress_poll();
        const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                           out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
        if (v < 0.0) {
            std::swap(n[1], n[2]);
        }
    }
    // Hard-pin CAD vertices and sharp edge curves. Smoothing has just
    // evened the wall spacing, so this is where a crease becomes exact
    // rather than "as close as the nearest face point happens to be" — a
    // sharp edge, not a chamfer (ADR-0035).
    if (fit != nullptr && fit->can_pin()) {
        progress.set_phase("projection_feature_pin");
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> star;
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
            for (const auto ni : out.mesh.tets[ti]) {
                star[ni].push_back(ti);
            }
        }
        const auto node_offends = [&](std::uint32_t ni) {
            const auto it = star.find(ni);
            if (it == star.end()) {
                return false;
            }
            for (const auto ti : it->second) {
                fill_progress_poll();
                const auto& n = out.mesh.tets[ti];
                const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
                const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
                const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
                const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
                // Shape floor, not just a positive volume: a sign-only
                // gate lets the pin flatten a skin tet (ADR-0033).
                if (!(tet_signed_volume(a, b, c, d) > vol_eps) ||
                    validity::tet_shape_quality(a, b, c, d) < validity::kCellShapeFloor) {
                    return true;
                }
            }
            return false;
        };
        std::vector<std::uint32_t> bnodes;
        for (const auto& f : free_faces) {
            fill_progress_poll();
            bnodes.insert(bnodes.end(), f.begin(), f.end());
        }
        std::sort(bnodes.begin(), bnodes.end());
        bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
        out.mesh.pin = pin_feature_nodes(
            *fit->cad, *fit->topo, out.mesh.nodes, bnodes, hc, node_offends,
            projection != nullptr ? projection->provenance : nullptr, mirror);
        for (auto& n : out.mesh.tets) {
            fill_progress_poll();
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
    }
}

void carve_overlapped_sheets(GradedFillState& s) {
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    constexpr int kOverlapPasses = 48;
    const auto carve_to_clean = [&]() {
        for (int pass = 0; pass < kOverlapPasses; ++pass) {
            progress.set_phase("quality_overlap_pass", pass + 1, kOverlapPasses);
            const auto owners =
                buried_free_tet_face_owners(out.mesh.nodes, out.mesh.tets, hc);
            if (owners.empty()) {
                return;
            }
            std::vector<char> kill(out.mesh.tets.size(), 0);
            for (const auto ti : owners) {
                fill_progress_poll();
                kill[ti] = 1;
            }
            restrict_kill_to_shell(out.mesh.tets, kill);
            std::size_t n_kill = static_cast<std::size_t>(
                std::count(kill.begin(), kill.end(), static_cast<char>(1)));
            if (n_kill == 0) {
                // Every single-tet kill was vetoed: at a one-cell-thick
                // overlap band each deletion alone would pinch the shell.
                // Escalate to the whole node-neighbourhood of every stuck
                // owner so the guard judges the band, not a pinch.
                std::unordered_set<std::uint32_t> owner_nodes;
                for (const auto ti : owners) {
                    fill_progress_poll();
                    owner_nodes.insert(out.mesh.tets[ti].begin(), out.mesh.tets[ti].end());
                }
                std::fill(kill.begin(), kill.end(), static_cast<char>(0));
                for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                    fill_progress_poll(ti, out.mesh.tets.size());
                    for (const auto ni : out.mesh.tets[ti]) {
                        if (owner_nodes.count(ni)) {
                            kill[ti] = 1;
                            break;
                        }
                    }
                }
                restrict_kill_to_shell(out.mesh.tets, kill);
                n_kill = static_cast<std::size_t>(
                    std::count(kill.begin(), kill.end(), static_cast<char>(1)));
            }
            if (n_kill == 0 || n_kill >= out.mesh.tets.size()) {
                return;
            }
            std::size_t w = 0;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                fill_progress_poll(ti, out.mesh.tets.size());
                if (!kill[ti]) {
                    out.mesh.tets[w++] = out.mesh.tets[ti];
                }
            }
            out.mesh.tets.resize(w);
            // No snap inside the carve: re-projecting each freshly exposed
            // layer regenerates the tangle mid-carve.
        }
    };
    // Alternate carve -> snap toward the JOINT fixed point: no burial AND
    // a snapped boundary. Carving alone leaves the exposed layer at raw
    // positions, which costs real fidelity; snapping alone re-creates the
    // tangle. Most parts close the loop in one round; a near-tangent wedge
    // that will not close ships the divot instead of the self-intersection.
    constexpr int kAlternations = 3;
    for (int round = 0; round < kAlternations; ++round) {
        carve_to_clean();
        if (count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc).n_buried != 0) {
            break; // carve stranded: do not snap on top of a tangle
        }
        snap_round(s);
        if (count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc).n_buried == 0) {
            break; // snapped AND clean
        }
    }
    // The carve can expose sliver caps — one more collapse round cleans
    // them; it may itself re-expose or re-tangle, so it runs BEFORE the
    // final carve + pull and the census gate stays last.
    repair_round(s);
    carve_to_clean();
    progress.set_phase("quality_overlap_pull");
    pull_buried_free_faces(out.mesh.nodes, out.mesh.tets, hc, /*max_iters=*/8, mirror);
    // Final projection/pull is also a boundary-placement operation. The
    // ordinary repair precedes it; close near-coincident projected corners
    // here using the same topology/volume/quality guarded collapse only.
    repair_round(s, /*collisions_only=*/true);
    if (const auto st = count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc);
        st.n_buried != 0) {
        throw ValidityError(
            std::format("graded_tet_fill_surface: {} boundary faces remain buried inside "
                        "other cells after the overlap carve — self-intersecting boundary",
                        st.n_buried));
    }
}

void relax_interior_slivers(GradedFillState& s) {
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    progress.set_phase("quality_relaxation_setup");
    constexpr double kSliverFloor = 0.01; // ~half kCellShapeFloor: cure, not polish
    constexpr int kRelaxPasses = 6;
    const auto aspect = [&](const std::array<std::uint32_t, 4>& n) {
        const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
        const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
        const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
        const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
        return validity::tet_shape_quality(a, b, c, d);
    };
    std::vector<std::vector<std::uint32_t>> incident(out.mesh.nodes.size());
    for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
        fill_progress_poll(ti, out.mesh.tets.size());
        for (const auto ni : out.mesh.tets[ti]) {
            incident[ni].push_back(static_cast<std::uint32_t>(ti));
        }
    }
    // Boundary nodes are every node on a face used by exactly one tet.
    std::vector<char> frozen(out.mesh.nodes.size(), 0);
    {
        std::map<TriKey, int> face_use;
        for (const auto& t : out.mesh.tets) {
            fill_progress_poll();
            for (const auto& f : kTetFaces) {
                ++face_use[sorted_tri_key(t[static_cast<std::size_t>(f[0])],
                                          t[static_cast<std::size_t>(f[1])],
                                          t[static_cast<std::size_t>(f[2])])];
            }
        }
        for (std::size_t work_done = 0; const auto& [key, uses] : face_use) {
            if (active_fill_progress != nullptr) fill_progress_poll(work_done++, face_use.size());
            if (uses == 1) {
                for (const auto ni : key) {
                    frozen[ni] = 1;
                }
            }
        }
    }
    // Neighbour lists, ascending node id: the acceptance test reads the shared
    // node array, so visit order is mesh-level mutation state (ADR-0032).
    std::vector<std::vector<std::uint32_t>> nbrs(out.mesh.nodes.size());
    {
        std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
        for (const auto& t : out.mesh.tets) {
            fill_progress_poll();
            for (int i = 0; i < 4; ++i) {
                for (int j = i + 1; j < 4; ++j) {
                    const auto a = t[static_cast<std::size_t>(i)];
                    const auto b = t[static_cast<std::size_t>(j)];
                    if (a == b) {
                        continue;
                    }
                    const auto key = std::minmax(a, b);
                    if (!seen.insert({key.first, key.second}).second) {
                        continue;
                    }
                    nbrs[a].push_back(b);
                    nbrs[b].push_back(a);
                }
            }
        }
    }
    const auto worst_incident = [&](std::uint32_t ni) {
        double lo = 1.0;
        for (const auto ti : incident[ni]) {
            fill_progress_poll();
            lo = std::min(lo, aspect(out.mesh.tets[ti]));
        }
        return lo;
    };
    for (int pass = 0; pass < kRelaxPasses; ++pass) {
        progress.set_phase("quality_relaxation_pass", pass + 1, kRelaxPasses);
        // Nodes of every sliver tet, deduplicated, visited in mirror-canonical
        // order. This is a Gauss-Seidel sweep on the shared node array — an
        // accepted move changes whether the next node's move improves its own
        // star — so ascending node id gave a node and its mirror image
        // different predecessors here too (ADR-0036).
        std::set<std::uint32_t> target_set;
        for (const auto& t : out.mesh.tets) {
            fill_progress_poll();
            if (aspect(t) >= kSliverFloor) {
                continue;
            }
            for (const auto ni : t) {
                if (frozen[ni] == 0 && !nbrs[ni].empty()) {
                    target_set.insert(ni);
                }
            }
        }
        if (target_set.empty()) {
            break;
        }
        std::vector<std::uint32_t> targets(target_set.begin(), target_set.end());
        sort_mirror_canonical(out.mesh.nodes, targets);
        std::size_t n_moved = 0;
        for (std::size_t work_done = 0; const auto ni : targets) {
            if (active_fill_progress != nullptr) fill_progress_poll(work_done++, targets.size());
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            for (const auto other : nbrs[ni]) {
                centroid += out.mesh.nodes[other];
            }
            centroid /= static_cast<double>(nbrs[ni].size());
            const Eigen::Vector3d saved = out.mesh.nodes[ni];
            const double before = worst_incident(ni);
            bool kept = false;
            for (const double omega : {0.7, 0.4, 0.2}) {
                out.mesh.nodes[ni] = saved + omega * (centroid - saved);
                if (worst_incident(ni) > before) {
                    kept = true;
                    break;
                }
            }
            if (kept) {
                ++n_moved;
            } else {
                out.mesh.nodes[ni] = saved;
            }
        }
        if (n_moved == 0) {
            break;
        }
    }
}

} // namespace polymesh::mesh::detail
