// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"
#include "scene_internal.hpp"

#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/element_validity.hpp"
#include "fea/nodal_mesh.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/brep_fidelity.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <ratio>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {

const char* geometry_volume_band(double relative_error) {
    if (relative_error > kGeometryVolumeHardLimit) {
        return "egregious";
    }
    if (relative_error >= kGeometryVolumeTruthLimit) {
        return "degraded";
    }
    return "clean";
}

std::string geometry_volume_note(std::string_view stage,
                                 const GeometryVolumeAssessment& assessment) {
    return std::format("geometry_{}_volume mesh={:.6g} cad={:.6g} rel_err={:.4g} band={}",
                       stage, assessment.mesh_volume, assessment.cad_volume,
                       assessment.relative_error,
                       geometry_volume_band(assessment.relative_error));
}

} // namespace

namespace detail {

std::size_t
relax_cells_below_shape_floor(fea::NodalMesh& mesh,
                              std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                              double floor_value, int rounds) {
    if (mesh.elements.empty() || mesh.nodes.empty()) {
        return 0;
    }
    std::vector<char> on_boundary(mesh.nodes.size(), 0);
    for (const auto& face : boundary_faces) {
        for (const auto ni : face) {
            if (ni < on_boundary.size()) {
                on_boundary[ni] = 1;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> incident(mesh.nodes.size());
    std::vector<std::vector<std::uint32_t>> neighbours(mesh.nodes.size());
    for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
        mesh::fill_progress_poll(ei, mesh.elements.size());
        const auto& nodes = mesh.elements[ei].nodes;
        for (const auto ni : nodes) {
            if (ni >= incident.size()) {
                continue;
            }
            incident[ni].push_back(static_cast<std::uint32_t>(ei));
            for (const auto other : nodes) {
                if (other != ni && other < mesh.nodes.size()) {
                    neighbours[ni].push_back(other);
                }
            }
        }
    }
    for (auto& list : neighbours) {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }

    // Accept a relaxation when it raises the WORST cell of the moved node's
    // star. Demanding the whole star clear the floor rejects every move on a
    // star that is below the floor to begin with — which is the only star this
    // sweep is ever called on.
    const auto star_min_quality = [&](std::uint32_t ni) {
        double worst = std::numeric_limits<double>::infinity();
        for (const auto ei : incident[ni]) {
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (std::isfinite(q)) {
                worst = std::min(worst, q);
            }
        }
        return worst;
    };

    std::size_t remaining = 0;
    for (int round = 0; round < rounds; ++round) {
        mesh::fill_progress_phase("ship_quality_pass", round + 1, rounds);
        // Ascending element index, then ascending node id: the acceptance test
        // reads the shared node array, so visit order is mutation state.
        std::vector<std::uint32_t> targets;
        for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
            mesh::fill_progress_poll(ei, mesh.elements.size());
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (!std::isfinite(q) || q >= floor_value) {
                continue;
            }
            for (const auto ni : mesh.elements[ei].nodes) {
                if (ni < on_boundary.size() && on_boundary[ni] == 0 &&
                    !neighbours[ni].empty()) {
                    targets.push_back(ni);
                }
            }
        }
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (targets.empty()) {
            break;
        }
        bool moved_any = false;
        std::size_t targets_done = 0;
        for (const auto ni : targets) {
            mesh::fill_progress_poll(targets_done++, targets.size());
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            for (const auto other : neighbours[ni]) {
                centroid += mesh.nodes[other];
            }
            centroid /= static_cast<double>(neighbours[ni].size());
            const Eigen::Vector3d saved = mesh.nodes[ni];
            const double before = star_min_quality(ni);
            mesh.nodes[ni] = saved + 0.5 * (centroid - saved);
            const double after = star_min_quality(ni);
            // Improvement is not enough: a move from -1e-3 to -9e-11 is an
            // improvement and still ships an inverted cell, which the solver
            // then refuses with "non-positive Jacobian". Never leave a star
            // non-positive, and never make a positive star worse.
            if (!(after > before) || !(after > 0.0)) {
                mesh.nodes[ni] = saved;
            } else {
                moved_any = true;
            }
        }
        if (!moved_any) {
            break;
        }
    }
    mesh::fill_progress_phase("ship_quality_census");
    std::size_t checked = 0;
    for (const auto& element : mesh.elements) {
        mesh::fill_progress_poll(checked++, mesh.elements.size());
        const double q = fea::cell_quality(mesh, element);
        if (std::isfinite(q) && q < floor_value) {
            ++remaining;
        }
    }
    return remaining;
}

ExteriorConformStats conform_true_exterior(
    fea::NodalMesh& mesh, std::span<const std::array<std::uint32_t, 4>> boundary_faces,
    mesh::BoundaryProjectionContext* projection, const mesh::BoundaryFit* fit, double h,
    double floor_value, const mesh::MirrorFrame* mirror) {
    ExteriorConformStats stats;
    if (mesh.elements.empty() || mesh.nodes.empty() || projection == nullptr || !(h > 0.0)) {
        return stats;
    }
    // Reflection orbit over the node set this pass receives. Every mesher stage
    // upstream is exactly mirror-symmetric by construction (mesh/mirror.hpp),
    // but moving nodes one at a time under a quality gate is order-dependent.
    // So each accepted move here is applied to a whole orbit or to none of it.
    const mesh::MirrorNodeOrbit orbit(
        mirror != nullptr ? *mirror : mesh::MirrorFrame{}, mesh.nodes, [&] {
            const mesh::MirrorKeyFrame frame = mesh::mirror_key_frame(mesh.nodes);
            return frame.inv_quantum > 0.0 ? 1.0 / frame.inv_quantum : 0.0;
        }());
    // Orbit copies of `node`, itself included, or empty when any copy is missing.
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
    std::vector<char> on_boundary(mesh.nodes.size(), 0);
    for (const auto& face : boundary_faces) {
        for (const auto ni : face) {
            if (ni < on_boundary.size()) {
                on_boundary[ni] = 1;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> incident;
    std::vector<std::vector<std::uint32_t>> neighbours;
    const auto rebuild_incidence = [&] {
        on_boundary.resize(mesh.nodes.size(), 0);
        incident.assign(mesh.nodes.size(), {});
        neighbours.assign(mesh.nodes.size(), {});
        for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
            mesh::fill_progress_poll(ei, mesh.elements.size());
            const auto& nodes = mesh.elements[ei].nodes;
            for (const auto ni : nodes) {
                if (ni >= incident.size()) {
                    continue;
                }
                incident[ni].push_back(static_cast<std::uint32_t>(ei));
                for (const auto other : nodes) {
                    if (other != ni && other < mesh.nodes.size()) {
                        neighbours[ni].push_back(other);
                    }
                }
            }
        }
        for (auto& list : neighbours) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
    };
    rebuild_incidence();
    const auto star_min_quality = [&](std::uint32_t ni) {
        double worst = std::numeric_limits<double>::infinity();
        for (const auto ei : incident[ni]) {
            const double q = fea::cell_quality(mesh, mesh.elements[ei]);
            if (std::isfinite(q)) {
                worst = std::min(worst, q);
            }
        }
        return worst;
    };
    // Two conditions, both necessary. `cell_quality` keeps the mesh solvable in
    // the shape sense the product reports; `element_jacobians_positive` is the
    // assembly's own integrability test, and it is the one that catches the
    // near-degenerate acceptances a quality floor lets through (det J < 0).
    const auto star_ok = [&](std::uint32_t ni) {
        const double q = star_min_quality(ni);
        if (std::isfinite(q) && q < floor_value) {
            return false;
        }
        return fea::star_jacobians_positive(mesh, incident[ni]);
    };
    // Room for one refused node. Interior star neighbours carry no geometry
    // constraint, so moving them costs no fidelity; the boundary neighbours may
    // only slide ALONG their own owner geometry, which changes spacing and not
    // placement. Both tiers are validated with the same rule the node move
    // uses — quality never worse, and always integrable, because a nudge that
    // only watches `cell_quality` can still ship det J < 0.
    // One node's Laplacian nudge. `try_nudge_orbit` below is what callers use:
    // a nudge accepted on one side of a symmetric part and refused on the other is
    // itself an asymmetry, and this pass runs on nodes the snap could not place,
    // which is exactly where a symmetric part has symmetric trouble.
    const auto try_nudge_one = [&](std::uint32_t ni, bool tangential) {
        if (neighbours[ni].empty()) {
            return false;
        }
        const double cap = 0.25 * h;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (const auto other : neighbours[ni]) {
            centroid += mesh.nodes[other];
        }
        centroid /= static_cast<double>(neighbours[ni].size());
        const Eigen::Vector3d saved = mesh.nodes[ni];
        const Eigen::Vector3d step = 0.5 * (centroid - saved);
        const double len = step.norm();
        const double before = star_min_quality(ni);
        Eigen::Vector3d moved = saved + (len > cap ? step * (cap / len) : step);
        if (tangential) {
            const auto back =
                mesh::owned_boundary_projection_target(moved, ni, projection, mirror);
            if (!back || (back->point - saved).norm() > cap) {
                return false;
            }
            moved = back->point;
        }
        mesh.nodes[ni] = moved;
        const double after = star_min_quality(ni);
        const bool ok = (!std::isfinite(after) || after >= std::min(before, floor_value)) &&
                        (std::isfinite(after) ? after > 0.0 : true) &&
                        fea::star_jacobians_positive(mesh, incident[ni]);
        if (!ok) {
            mesh.nodes[ni] = saved;
            return false;
        }
        return (mesh.nodes[ni] - saved).squaredNorm() > 0.0;
    };
    const auto try_nudge = [&](std::uint32_t ni, bool tangential) {
        const auto group = orbit_of(ni);
        if (group.empty()) {
            return false; // incomplete orbit: leave the node alone
        }
        std::vector<Eigen::Vector3d> saved;
        saved.reserve(group.size());
        for (const auto node : group) {
            saved.push_back(mesh.nodes[node]);
        }
        bool all_moved = true;
        for (const auto node : group) {
            if (!try_nudge_one(node, tangential)) {
                all_moved = false;
                break;
            }
        }
        if (!all_moved) {
            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                mesh.nodes[group[gi]] = saved[gi];
            }
            return false;
        }
        return true;
    };
    const auto open_room = [&](std::uint32_t seed) {
        bool moved_any = false;
        std::vector<std::uint32_t> ring = neighbours[seed];
        mesh::sort_mirror_canonical(mesh.nodes, ring);
        for (const auto ni : ring) {
            if (on_boundary[ni] == 0) {
                moved_any = try_nudge(ni, /*tangential=*/false) || moved_any;
            }
        }
        if (moved_any) {
            return true;
        }
        // A hex-blocked node has no interior corner to give, so the interior
        // tier alone cannot rescue it. The star's other WALL nodes are the only
        // degrees of freedom left, and sliding them along the surface is free.
        for (const auto ni : ring) {
            if (ni != seed && on_boundary[ni] != 0) {
                moved_any = try_nudge(ni, /*tangential=*/true) || moved_any;
            }
        }
        return moved_any;
    };

    std::vector<std::uint32_t> exterior;
    for (std::uint32_t ni = 0; ni < mesh.nodes.size(); ++ni) {
        if (on_boundary[ni] != 0 && !incident[ni].empty()) {
            exterior.push_back(ni);
        }
    }
    // Mirror-canonical order: the march below moves nodes and opens room in their
    // neighbourhoods, so an earlier node decides what a later one can do.
    mesh::sort_mirror_canonical(mesh.nodes, exterior);
    const double eps = 1e-9 * h;
    // Exact resolution only. `boundary_projection_target` falls back to the
    // TESSELLATION when a node has no latched owner, and a node can read ~0
    // residual against OCC's facets while sitting well off the true surface at
    // this deflection. A node with no owner is therefore projected freely onto
    // the BRep instead, which also latches an owner.
    const auto resolve = [&](std::uint32_t ni) -> std::optional<mesh::BoundaryTarget> {
        // Folded: owners recorded by the pin are the CANONICAL entity, so an
        // unfolded query against them answers in the wrong octant (ADR-0036 §9.2).
        auto target =
            mesh::owned_boundary_projection_target(mesh.nodes[ni], ni, projection, mirror);
        if (!target && fit != nullptr && fit->cad != nullptr) {
            if (const auto free_projection =
                    geom::project_point_on_surface(*fit->cad, mesh.nodes[ni])) {
                target =
                    mesh::BoundaryTarget{free_projection->point, free_projection->distance};
            }
        }
        return target;
    };
    // Constrained march instead of one all-or-nothing jump: take the largest
    // legal fraction of the remaining gap, re-project onto the same owner, and
    // repeat. Every accepted step strictly reduces the distance to the owner's
    // exact geometry, and the acceptance rule is the ship gate's — never make
    // the worst incident cell worse, never leave a star non-positive or below
    // the floor it started above. Returns the residual left.
    // The owner projection is not the measured quantity. A node whose latched
    // owner is a far face can be walked toward a point that IS on the BRep and
    // still end up FARTHER from the nearest surface, because the straight
    // segment leaves the local patch. So every step must also not increase the
    // free distance to the shape, which is exactly what the fidelity metric
    // reports.
    const auto free_distance = [&](std::uint32_t ni) {
        if (fit == nullptr || fit->cad == nullptr) {
            return 0.0;
        }
        const auto projected = geom::project_point_on_surface(*fit->cad, mesh.nodes[ni]);
        return projected ? projected->distance : 0.0;
    };
    // The march advances a whole reflection orbit in lockstep: one step fraction
    // for every member, accepted only when every member's own gate accepts it.
    // Marching each node separately is not equivalent — the members' gates read
    // quality values that tie across a mirror pair, so their fraction ladders can
    // stop at different rungs and place mirrored nodes differently (the same
    // failure mode the collapse round showed, ADR-0036 Section 9.2).
    const auto march_group = [&](const std::vector<std::uint32_t>& group, bool* relaxed) {
        for (int step = 0; step < 6; ++step) {
            std::vector<mesh::BoundaryTarget> now;
            now.reserve(group.size());
            bool have_targets = true;
            for (const auto node : group) {
                const auto target = resolve(node);
                if (!target || target->distance <= eps) {
                    have_targets = false;
                    break;
                }
                now.push_back(*target);
            }
            if (!have_targets) {
                break;
            }
            std::vector<Eigen::Vector3d> saved;
            std::vector<double> floor_here;
            std::vector<double> free_before;
            saved.reserve(group.size());
            floor_here.reserve(group.size());
            free_before.reserve(group.size());
            for (const auto node : group) {
                saved.push_back(mesh.nodes[node]);
                floor_here.push_back(std::min(star_min_quality(node), floor_value));
                free_before.push_back(free_distance(node));
            }
            bool advanced = false;
            for (const double frac : {1.0, 0.5, 0.25, 0.125}) {
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    mesh.nodes[group[gi]] = saved[gi] + frac * (now[gi].point - saved[gi]);
                }
                bool step_ok = true;
                for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                    const auto node = group[gi];
                    const double after = star_min_quality(node);
                    step_ok = (!std::isfinite(after) || after >= floor_here[gi]) &&
                              fea::star_jacobians_positive(mesh, incident[node]) &&
                              free_distance(node) <= free_before[gi];
                }
                if (step_ok) {
                    advanced = true;
                    break;
                }
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    mesh.nodes[group[gi]] = saved[gi];
                }
            }
            if (advanced) {
                continue;
            }
            bool opened = false;
            if (relaxed != nullptr && !*relaxed) {
                for (const auto node : group) {
                    opened = open_room(node) || opened;
                }
            }
            if (opened) {
                *relaxed = true;
                continue;
            }
            break;
        }
        return free_distance(group.front());
    };
    const auto march = [&](std::uint32_t ni, bool* relaxed) {
        const auto group = orbit_of(ni);
        if (group.empty()) {
            return free_distance(ni); // incomplete orbit: no move is symmetric
        }
        return march_group(group, relaxed);
    };

    // Whole-pass insurance. Every individual acceptance above is local — it
    // proves its own star did not get worse — and locality is not the same
    // promise as "the shipped mesh did not get worse". On cvt_poly, whose cells
    // are already degenerate (quality ~1e-14), the local rule let the count of
    // sub-floor cells drift up while the minimum stayed put. So the pass is also
    // judged as a whole, against the two numbers the product reports, and
    // reverted wholesale if either moved the wrong way.
    const auto mesh_quality_census = [&] {
        std::pair<double, std::size_t> census{std::numeric_limits<double>::infinity(), 0};
        for (const auto& element : mesh.elements) {
            const double q = fea::cell_quality(mesh, element);
            if (!std::isfinite(q)) {
                continue;
            }
            census.first = std::min(census.first, q);
            if (q < floor_value) {
                ++census.second;
            }
        }
        return census;
    };
    const auto entry_nodes = mesh.nodes;
    const auto entry_elements = mesh.elements;
    const auto entry_census = mesh_quality_census();

    std::vector<std::uint32_t> stuck;
    mesh::fill_progress_phase("exterior_projection");
    std::size_t exterior_done = 0;
    for (const auto ni : exterior) {
        mesh::fill_progress_poll(exterior_done++, exterior.size());
        const double start = free_distance(ni);
        if (start <= eps) {
            continue;
        }
        ++stats.n_candidates;
        bool relaxed_here = false;
        const double left = march(ni, &relaxed_here);
        if (left < start) {
            ++stats.n_moved;
            if (relaxed_here) {
                ++stats.n_relax_rescued;
            }
        }
        if (left > eps) {
            stuck.push_back(ni);
        }
    }

    // Conforming hex relief. What is left is blocked by a hexahedron already
    // saturated at the shape floor, where even a 0.125 step takes it under. A
    // hex has no interior corner and its neighbours are hexes too, so no
    // amount of relaxation helps.
    //
    // Fanning the hex into six pyramids over its own six quad faces changes no
    // face — the pyramid bases ARE the hex faces — so it is conforming with
    // every neighbour and the boundary shell is untouched. The apex is a new
    // interior node with full freedom, and a pyramid tolerates the wall motion
    // a hex refuses. The pipeline already ships pyramids from this mesher, so
    // nothing downstream is new.
    // The whole phase is judged as one unit and rolled back if it does not
    // pay: a relief that buys nothing must cost nothing.
    if (!stuck.empty()) {
        const auto mesh_worst_quality = [&] {
            double worst = std::numeric_limits<double>::infinity();
            for (const auto& element : mesh.elements) {
                const double q = fea::cell_quality(mesh, element);
                if (std::isfinite(q)) {
                    worst = std::min(worst, q);
                }
            }
            return worst;
        };
        const auto count_below_floor = [&] {
            std::size_t n = 0;
            for (const auto& element : mesh.elements) {
                const double q = fea::cell_quality(mesh, element);
                if (std::isfinite(q) && q < floor_value) {
                    ++n;
                }
            }
            return n;
        };
        const auto saved_nodes = mesh.nodes;
        const auto saved_elements = mesh.elements;
        const double quality_before = mesh_worst_quality();
        const std::size_t below_before = count_below_floor();
        double residual_before = 0.0;
        for (const auto ni : stuck) {
            residual_before += free_distance(ni);
        }
        static constexpr int kHexFaces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                                {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        std::set<std::uint32_t> hexes;
        for (const auto ni : stuck) {
            for (const auto ei : incident[ni]) {
                if (mesh.elements[ei].type == fea::ElementType::kHex8 &&
                    mesh.elements[ei].nodes.size() == 8) {
                    hexes.insert(ei);
                }
            }
        }
        for (const auto ei : hexes) {
            const auto corners = mesh.elements[ei].nodes;
            Eigen::Vector3d centre = Eigen::Vector3d::Zero();
            for (const auto ni : corners) {
                centre += mesh.nodes[ni];
            }
            centre /= 8.0;
            const auto apex = static_cast<std::uint32_t>(mesh.nodes.size());
            mesh.nodes.push_back(centre);
            std::array<fea::NodalElement, 6> fan{};
            bool ok = true;
            for (int f = 0; f < 6 && ok; ++f) {
                std::array<std::uint32_t, 4> base{
                    {corners[static_cast<std::size_t>(kHexFaces[f][0])],
                     corners[static_cast<std::size_t>(kHexFaces[f][1])],
                     corners[static_cast<std::size_t>(kHexFaces[f][2])],
                     corners[static_cast<std::size_t>(kHexFaces[f][3])]}};
                // Orientation is decided by measurement, not by trusting a face
                // table: whichever winding gives the pyramid a positive split
                // volume is the one that ships.
                if (mesh::validity::pyramid_min_split_volume(
                        mesh.nodes[base[0]], mesh.nodes[base[1]], mesh.nodes[base[2]],
                        mesh.nodes[base[3]], centre) <= 0.0) {
                    std::swap(base[1], base[3]);
                }
                if (mesh::validity::pyramid_min_split_volume(
                        mesh.nodes[base[0]], mesh.nodes[base[1]], mesh.nodes[base[2]],
                        mesh.nodes[base[3]], centre) <= 0.0) {
                    ok = false;
                    break;
                }
                fan[static_cast<std::size_t>(f)] = fea::NodalElement{
                    fea::ElementType::kPyramid5, {base[0], base[1], base[2], base[3], apex}};
                // A positive volume is not enough: fanning an already tight hex
                // can produce pyramids far under the floor, a worse mesh bought
                // with a better boundary. Every child must clear the floor and
                // be integrable, measured before anything is committed
                // (`cell_quality` reads only `mesh.nodes`, and the apex is
                // already in place).
                const auto& child = fan[static_cast<std::size_t>(f)];
                const double q = fea::cell_quality(mesh, child);
                ok = std::isfinite(q) && q >= floor_value &&
                     fea::element_jacobians_positive(mesh, child);
            }
            if (!ok) {
                mesh.nodes.pop_back();
                continue;
            }
            mesh.elements[ei] = fan[0];
            for (std::size_t f = 1; f < fan.size(); ++f) {
                mesh.elements.push_back(fan[f]);
            }
            ++stats.n_hex_fanned;
        }
        if (stats.n_hex_fanned > 0) {
            rebuild_incidence();
        }
        std::size_t moved_here = 0;
        double residual_after = 0.0;
        for (const auto ni : stuck) {
            const double before = free_distance(ni);
            bool relaxed_here = false;
            const double left = march(ni, &relaxed_here);
            residual_after += left;
            if (left < before) {
                ++moved_here;
            }
        }
        std::size_t nonintegrable = 0;
        for (const auto& element : mesh.elements) {
            if (!fea::element_jacobians_positive(mesh, element)) {
                ++nonintegrable;
            }
        }
        const bool paid = moved_here > 0 && residual_after < residual_before;
        // On a mesh that is already degenerate (cvt_poly ships quality ~1e-14)
        // a "worst quality did not drop" test is toothless — it let the count of
        // sub-floor cells grow 807 → 855 while the minimum stayed put. Count is
        // therefore part of the contract too.
        const bool kept_shape =
            mesh_worst_quality() >= std::min(quality_before, floor_value) &&
            nonintegrable == 0 && count_below_floor() <= below_before;
        if (!paid || !kept_shape) {
            mesh.nodes = saved_nodes;
            mesh.elements = saved_elements;
            stats.n_hex_fanned = 0;
            rebuild_incidence();
        } else {
            stats.n_moved += moved_here;
        }
        for (const auto ni : stuck) {
            const double left = free_distance(ni);
            if (left > eps) {
                ++stats.n_left;
                if (left > stats.worst_residual) {
                    stats.worst_residual = left;
                    stats.worst_node = ni;
                    stats.worst_position = mesh.nodes[ni];
                }
            }
        }
    }

    // Features last, on the same shipped node set and under the same gate: a
    // node exposed late can be a crease node nobody pinned.
    if (fit != nullptr && fit->can_pin()) {
        const auto node_offends = [&](std::uint32_t ni) { return !star_ok(ni); };
        const auto pin =
            mesh::pin_feature_nodes(*fit->cad, *fit->topo, mesh.nodes, exterior, h,
                                    node_offends, projection->provenance, mirror);
        stats.n_edge_pinned = pin.edge_pinned;
        stats.n_edge_chains = pin.chains;
        stats.n_pin_rejected = pin.rejected;
        const auto edge_pass_t0 = std::chrono::steady_clock::now();
        std::set<std::pair<std::uint32_t, std::uint32_t>> boundary_edges;
        for (const auto& face : boundary_faces) {
            const int n = face[3] == face[2] ? 3 : 4;
            for (int i = 0; i < n; ++i) {
                const auto a = face[static_cast<std::size_t>(i)];
                const auto b = face[static_cast<std::size_t>((i + 1) % n)];
                if (a != b) {
                    boundary_edges.insert(std::minmax(a, b));
                }
            }
        }
        const auto repair_edge =
            [&](std::uint32_t a, std::uint32_t b, const Eigen::Vector3d& target_a,
                const Eigen::Vector3d& target_b,
                std::vector<std::pair<std::uint32_t, Eigen::Vector3d>>* undo) {
                const Eigen::Vector3d saved_a = mesh.nodes[a];
                const Eigen::Vector3d saved_b = mesh.nodes[b];
                mesh.nodes[a] = target_a;
                mesh.nodes[b] = target_b;
                std::vector<std::uint32_t> candidates;
                std::vector<std::size_t> affected(incident[a].begin(), incident[a].end());
                affected.insert(affected.end(), incident[b].begin(), incident[b].end());
                std::sort(affected.begin(), affected.end());
                affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
                for (const auto ei : affected) {
                    for (const auto node : mesh.elements[ei].nodes) {
                        if (on_boundary[node] == 0) {
                            candidates.push_back(node);
                        }
                    }
                }
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()),
                                 candidates.end());
                // The pattern search below is a Gauss-Seidel sweep over these interior
                // nodes, so their visit order decides where each one lands. Ascending
                // node id does not mirror; the mirror key does.
                mesh::sort_mirror_canonical(mesh.nodes, candidates);
                std::vector<Eigen::Vector3d> saved;
                saved.reserve(candidates.size());
                for (const auto node : candidates) {
                    saved.push_back(mesh.nodes[node]);
                    affected.insert(affected.end(), incident[node].begin(),
                                    incident[node].end());
                }
                std::sort(affected.begin(), affected.end());
                affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
                // Branch-and-bound worst-quality: a candidate move can only be
                // accepted if it beats `bound`, so stop the scan the moment a cell
                // is at or below it. This keeps a per-edge pattern search
                // affordable on a full boundary.
                const auto objective = [&](double bound) {
                    double worst = std::numeric_limits<double>::infinity();
                    for (const auto ei : affected) {
                        const auto& element = mesh.elements[ei];
                        double quality = fea::cell_quality(mesh, element);
                        if (!std::isfinite(quality)) {
                            return -std::numeric_limits<double>::infinity();
                        }
                        if (!fea::element_jacobians_positive(mesh, element)) {
                            quality = -std::abs(quality);
                        }
                        worst = std::min(worst, quality);
                        if (worst <= bound) {
                            return worst;
                        }
                    }
                    return worst;
                };
                if (objective(floor_value) < floor_value) {
                    for (const double step_size : {0.5 * h, 0.25 * h, 0.125 * h}) {
                        for (int sweep = 0; sweep < 8; ++sweep) {
                            bool changed = false;
                            for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                                const auto node = candidates[ci];
                                double best_quality =
                                    objective(-std::numeric_limits<double>::infinity());
                                Eigen::Vector3d best_position = mesh.nodes[node];
                                // Trial directions are taken in the node's OWN folded
                                // frame: a node in a high octant tries the reflected
                                // step first, so a node and its mirror image walk
                                // mirrored paths through this greedy search. Fixed
                                // ±axis order would hand them different first
                                // improvements and place them asymmetrically.
                                Eigen::Vector3d fold_sign = Eigen::Vector3d::Ones();
                                if (mirror != nullptr) {
                                    for (int axis = 0; axis < 3; ++axis) {
                                        if (mirror->plane[static_cast<std::size_t>(axis)] &&
                                            mesh.nodes[node][axis] > mirror->center[axis]) {
                                            fold_sign[axis] = -1.0;
                                        }
                                    }
                                }
                                for (int axis = 0; axis < 3; ++axis) {
                                    for (const double sign : {-1.0, 1.0}) {
                                        Eigen::Vector3d trial = best_position;
                                        trial[axis] += sign * fold_sign[axis] * step_size;
                                        if (mirror != nullptr) {
                                            // A node on a plane may only move within
                                            // it; the normal step cancels and the
                                            // trial is a no-op.
                                            trial = mirror->clamp_to_planes(trial,
                                                                            mesh.nodes[node]);
                                        }
                                        mesh.nodes[node] = trial;
                                        const double quality = objective(best_quality);
                                        if (quality > best_quality + 1e-14) {
                                            best_quality = quality;
                                            best_position = trial;
                                            changed = true;
                                        }
                                        mesh.nodes[node] = best_position;
                                    }
                                }
                            }
                            if (!changed) {
                                break;
                            }
                            if (objective(floor_value) >= floor_value) {
                                break;
                            }
                        }
                        if (objective(floor_value) >= floor_value) {
                            break;
                        }
                    }
                }
                if (objective(floor_value) >= floor_value) {
                    if (undo != nullptr) {
                        undo->emplace_back(a, saved_a);
                        undo->emplace_back(b, saved_b);
                        for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                            undo->emplace_back(candidates[ci], saved[ci]);
                        }
                    }
                    return true;
                }
                mesh.nodes[a] = saved_a;
                mesh.nodes[b] = saved_b;
                for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                    mesh.nodes[candidates[ci]] = saved[ci];
                }
                return false;
            };
        std::map<std::size_t, std::size_t> connected_by_edge;
        // Per-node target cache: a boundary node belongs to ~6 boundary edges,
        // and both the topology query and the OCC curve projection are far more
        // expensive than the quality gate they feed.
        struct EdgeTarget {
            std::uint32_t edge_id = 0;
            Eigen::Vector3d point = Eigen::Vector3d::Zero();
            bool valid = false;
        };
        std::unordered_map<std::uint32_t, EdgeTarget> edge_target;
        const auto target_of = [&](std::uint32_t node) {
            const auto cached = edge_target.find(node);
            if (cached != edge_target.end()) {
                return cached->second;
            }
            EdgeTarget target;
            if (const auto near = geom::closest_edge(*fit->topo, mesh.nodes[node], true)) {
                if (near->distance <= 4.0 * h) {
                    if (const auto exact = geom::project_point_on_edge(
                            *fit->cad, near->edge_id, mesh.nodes[node])) {
                        if ((exact->point - mesh.nodes[node]).norm() <= 4.0 * h) {
                            target = EdgeTarget{near->edge_id, exact->point, true};
                        }
                    }
                }
            }
            edge_target.emplace(node, target);
            return target;
        };
        // Edges are visited in mirror-canonical order and repaired in whole
        // orbits: `repair_edge` moves interior nodes to buy the quality its own
        // gate demands, so a repair accepted on one side and refused on the other
        // leaves the two sides of a symmetric part genuinely different.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edge_order(boundary_edges.begin(),
                                                                        boundary_edges.end());
        {
            const mesh::MirrorKeyFrame ekey = mesh::mirror_key_frame(mesh.nodes);
            std::sort(edge_order.begin(), edge_order.end(), [&](const auto& x, const auto& y) {
                const auto kx = ekey.key(0.5 * (mesh.nodes[x.first] + mesh.nodes[x.second]));
                const auto ky = ekey.key(0.5 * (mesh.nodes[y.first] + mesh.nodes[y.second]));
                return kx != ky ? kx < ky : x < y;
            });
        }
        std::set<std::pair<std::uint32_t, std::uint32_t>> repaired;
        mesh::fill_progress_phase("exterior_edge_projection");
        std::size_t edges_done = 0;
        for (const auto& [a, b] : edge_order) {
            mesh::fill_progress_poll(edges_done++, edge_order.size());
            if (repaired.count(std::minmax(a, b)) != 0) {
                continue;
            }
            // Orbit copies of this edge, as node pairs.
            std::vector<std::pair<std::uint32_t, std::uint32_t>> copies{{a, b}};
            bool orbit_complete = true;
            if (orbit.active()) {
                for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
                    const std::uint32_t a2 = orbit.reflected(a, mask);
                    const std::uint32_t b2 = orbit.reflected(b, mask);
                    if (a2 == mesh::MirrorNodeOrbit::npos ||
                        b2 == mesh::MirrorNodeOrbit::npos) {
                        orbit_complete = false;
                        break;
                    }
                    if (boundary_edges.count(std::minmax(a2, b2)) == 0) {
                        orbit_complete = false;
                        break;
                    }
                    if (std::find(copies.begin(), copies.end(),
                                  std::pair<std::uint32_t, std::uint32_t>{a2, b2}) ==
                        copies.end()) {
                        copies.emplace_back(a2, b2);
                    }
                }
            }
            if (!orbit_complete) {
                continue;
            }
            bool all_valid = true;
            for (const auto& [a2, b2] : copies) {
                const auto ta = target_of(a2);
                const auto tb = target_of(b2);
                if (!ta.valid || !tb.valid || ta.edge_id != tb.edge_id) {
                    all_valid = false;
                    break;
                }
            }
            if (!all_valid) {
                continue;
            }
            std::vector<std::pair<std::uint32_t, Eigen::Vector3d>> undo;
            bool all_repaired = true;
            for (const auto& [a2, b2] : copies) {
                const auto ta = target_of(a2);
                const auto tb = target_of(b2);
                if (!repair_edge(a2, b2, ta.point, tb.point, &undo)) {
                    all_repaired = false;
                    break;
                }
            }
            if (!all_repaired) {
                for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
                    mesh.nodes[it->first] = it->second;
                }
                continue;
            }
            if (projection->provenance->size() < mesh.nodes.size()) {
                projection->provenance->resize(mesh.nodes.size());
            }
            for (const auto& [a2, b2] : copies) {
                const std::uint32_t edge_id = target_of(a2).edge_id;
                (*projection->provenance)[a2] =
                    mesh::BoundarySupport{mesh::BoundarySupportKind::kCadEdge, edge_id};
                (*projection->provenance)[b2] =
                    mesh::BoundarySupport{mesh::BoundarySupportKind::kCadEdge, edge_id};
                stats.n_edge_pinned += 2;
                ++connected_by_edge[edge_id];
                ++stats.n_connected_edges;
                repaired.insert(std::minmax(a2, b2));
            }
        }
        stats.edge_pass_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - edge_pass_t0)
                                 .count();
        for (const auto& [edge_id, count] : connected_by_edge) {
            if (!stats.connected_edge_census.empty()) {
                stats.connected_edge_census += ",";
            }
            stats.connected_edge_census += std::format("{}:{}", edge_id, count);
        }
    }

    // Facet-kink relief.
    //
    // With every boundary node exactly on the BRep the surface can still LOOK
    // wrong: grading transitions leave needle facets next to bulk ones, so
    // adjacent exact facets can meet at large kinks. A kink between two exact
    // facets is not a placement error, it is a *spacing* error, and spacing is
    // the one degree of freedom a node on a face still has.
    //
    // So: find the kinked boundary edges, slide the free-face nodes around them
    // along their own surface (re-projected through the owner oracle, so the
    // placement never changes), and keep only moves that lower the worst kink
    // in the node's own boundary neighbourhood. Crease and corner nodes are
    // never touched — their owner is an edge or a vertex, and sliding them is
    // exactly what would blunt the feature the pinning pass just made exact.
    {
        struct Facet {
            std::array<std::uint32_t, 4> nodes{};
            int count = 3;
        };
        std::vector<Facet> facets;
        facets.reserve(boundary_faces.size());
        for (const auto& face : boundary_faces) {
            Facet f;
            f.nodes = face;
            f.count = (face[3] == face[2]) ? 3 : 4;
            bool valid = true;
            for (int i = 0; i < f.count; ++i) {
                valid = valid && f.nodes[static_cast<std::size_t>(i)] < mesh.nodes.size();
            }
            if (valid) {
                facets.push_back(f);
            }
        }
        const auto facet_normal = [&](const Facet& f) {
            Eigen::Vector3d n = (mesh.nodes[f.nodes[1]] - mesh.nodes[f.nodes[0]])
                                    .cross(mesh.nodes[f.nodes[2]] - mesh.nodes[f.nodes[0]]);
            const double norm = n.norm();
            return norm > 1e-18 ? Eigen::Vector3d(n / norm) : Eigen::Vector3d::Zero().eval();
        };
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::size_t>>
            edge_facets;
        std::vector<std::vector<std::size_t>> node_facets(mesh.nodes.size());
        for (std::size_t fi = 0; fi < facets.size(); ++fi) {
            const auto& f = facets[fi];
            for (int i = 0; i < f.count; ++i) {
                const auto a = f.nodes[static_cast<std::size_t>(i)];
                const auto b = f.nodes[static_cast<std::size_t>((i + 1) % f.count)];
                if (a != b) {
                    edge_facets[{std::min(a, b), std::max(a, b)}].push_back(fi);
                }
                node_facets[a].push_back(fi);
            }
        }
        for (auto& list : node_facets) {
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        // Worst plane-to-plane angle among the manifold boundary edges of the
        // facets around one node — the quantity a move has to lower.
        const auto node_worst_kink = [&](std::uint32_t ni) {
            double worst = 0.0;
            for (const auto fi : node_facets[ni]) {
                const auto& f = facets[fi];
                for (int i = 0; i < f.count; ++i) {
                    const auto a = f.nodes[static_cast<std::size_t>(i)];
                    const auto b = f.nodes[static_cast<std::size_t>((i + 1) % f.count)];
                    if (a == b) {
                        continue;
                    }
                    const auto it = edge_facets.find({std::min(a, b), std::max(a, b)});
                    if (it == edge_facets.end() || it->second.size() != 2) {
                        continue;
                    }
                    const Eigen::Vector3d n0 = facet_normal(facets[it->second[0]]);
                    const Eigen::Vector3d n1 = facet_normal(facets[it->second[1]]);
                    if (n0.isZero() || n1.isZero()) {
                        worst = std::numbers::pi; // degenerate facet: always worse
                        continue;
                    }
                    worst =
                        std::max(worst, std::acos(std::clamp(std::abs(n0.dot(n1)), 0.0, 1.0)));
                }
            }
            return worst;
        };
        const auto* provenance = projection->provenance;
        // Face-owned nodes slide; edge- and vertex-owned nodes never do, because
        // sliding them is what would blunt the crease the pinning pass just made
        // exact. Unowned nodes do not slide either.
        const auto slidable = [&](std::uint32_t ni) {
            if (provenance == nullptr || ni >= provenance->size()) {
                return false;
            }
            return (*provenance)[ni].kind == mesh::BoundarySupportKind::kCadFace;
        };
        const double kink_threshold = 25.0 * std::numbers::pi / 180.0;
        for (int round = 0; round < 4; ++round) {
            std::vector<std::uint32_t> candidates;
            for (const auto& [edge, owners] : edge_facets) {
                if (owners.size() != 2) {
                    continue;
                }
                const Eigen::Vector3d n0 = facet_normal(facets[owners[0]]);
                const Eigen::Vector3d n1 = facet_normal(facets[owners[1]]);
                if (n0.isZero() || n1.isZero() ||
                    std::acos(std::clamp(std::abs(n0.dot(n1)), 0.0, 1.0)) >= kink_threshold) {
                    for (const auto fi : owners) {
                        const auto& f = facets[fi];
                        for (int i = 0; i < f.count; ++i) {
                            candidates.push_back(f.nodes[static_cast<std::size_t>(i)]);
                        }
                    }
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()),
                             candidates.end());
            // Mirror-canonical visit order: this is a Gauss-Seidel sweep on the
            // shared node array under a quality gate, so an accepted slide decides
            // whether the next one is legal.
            mesh::sort_mirror_canonical(mesh.nodes, candidates);
            std::size_t moved = 0;
            std::vector<char> done(mesh.nodes.size(), 0);
            for (const auto ni : candidates) {
                if (!slidable(ni) || done[ni] != 0) {
                    continue;
                }
                // The whole orbit slides together or not at all. A slide is a
                // tangential move under a local quality gate, so accepting it on
                // one side and refusing it on the other would break the mirror
                // symmetry.
                const auto group = orbit_of(ni);
                if (group.empty()) {
                    continue;
                }
                bool orbit_slidable = true;
                for (const auto node : group) {
                    orbit_slidable = orbit_slidable && slidable(node);
                }
                if (!orbit_slidable) {
                    continue;
                }
                std::vector<Eigen::Vector3d> group_saved;
                group_saved.reserve(group.size());
                for (const auto node : group) {
                    group_saved.push_back(mesh.nodes[node]);
                }
                // One relax value for the whole orbit, tested on every member
                // before any of them is kept. Letting each member walk its own
                // 0.5/0.25/0.125 ladder is not equivalent: the acceptance test
                // compares kink and quality values that tie in exact arithmetic
                // across a mirror pair, so the ladders could stop at different
                // rungs and slide mirrored nodes by different amounts.
                std::vector<Eigen::Vector3d> centroid(group.size());
                std::vector<double> kink_before(group.size());
                std::vector<double> quality_before(group.size());
                bool have_centroids = true;
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    const auto node = group[gi];
                    // Umbrella centroid over the boundary neighbours only: an
                    // interior neighbour would pull the node off its own face.
                    Eigen::Vector3d sum = Eigen::Vector3d::Zero();
                    std::size_t n_used = 0;
                    for (const auto fi : node_facets[node]) {
                        const auto& f = facets[fi];
                        for (int i = 0; i < f.count; ++i) {
                            const auto other = f.nodes[static_cast<std::size_t>(i)];
                            if (other != node) {
                                sum += mesh.nodes[other];
                                ++n_used;
                            }
                        }
                    }
                    if (n_used == 0) {
                        have_centroids = false;
                        break;
                    }
                    centroid[gi] = sum / static_cast<double>(n_used);
                    kink_before[gi] = node_worst_kink(node);
                    quality_before[gi] = star_min_quality(node);
                }
                if (!have_centroids) {
                    continue;
                }
                bool all_accepted = false;
                for (const double relax : {0.5, 0.25, 0.125}) {
                    bool step_ok = true;
                    for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                        const auto node = group[gi];
                        const Eigen::Vector3d slid =
                            group_saved[gi] + relax * (centroid[gi] - group_saved[gi]);
                        const auto back = mesh::owned_boundary_projection_target(
                            slid, node, projection, mirror);
                        if (!back) {
                            step_ok = false;
                            break;
                        }
                        mesh.nodes[node] = back->point;
                    }
                    if (step_ok) {
                        for (std::size_t gi = 0; gi < group.size() && step_ok; ++gi) {
                            const auto node = group[gi];
                            const double after = star_min_quality(node);
                            step_ok = node_worst_kink(node) < kink_before[gi] &&
                                      (!std::isfinite(after) ||
                                       after >= std::min(quality_before[gi], floor_value)) &&
                                      fea::star_jacobians_positive(mesh, incident[node]);
                        }
                    }
                    if (step_ok) {
                        all_accepted = true;
                        break;
                    }
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        mesh.nodes[group[gi]] = group_saved[gi];
                    }
                }
                if (!all_accepted) {
                    continue;
                }
                for (const auto node : group) {
                    done[node] = 1;
                    ++moved;
                }
            }
            stats.n_kink_relieved += moved;
            if (moved == 0) {
                break;
            }
        }
    }

    std::size_t nonintegrable_exit = 0;
    for (const auto& element : mesh.elements) {
        if (!fea::element_jacobians_positive(mesh, element)) {
            ++nonintegrable_exit;
        }
    }
    const auto exit_census = mesh_quality_census();
    if (nonintegrable_exit > 0 || exit_census.second > entry_census.second ||
        exit_census.first < std::min(entry_census.first, floor_value)) {
        mesh.nodes = entry_nodes;
        mesh.elements = entry_elements;
        stats.reverted = true;
    }
    return stats;
}

BoundaryShellTopology
boundary_shell_topology(const std::vector<std::array<std::uint32_t, 4>>& faces) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> use;
    for (const auto& face : faces) {
        // Triangles arrive as the degenerate quad (a,b,c,c).
        const std::size_t n = face[3] == face[2] ? 3 : 4;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % n];
            if (a == b) {
                continue;
            }
            ++use[a < b ? std::pair{a, b} : std::pair{b, a}];
        }
    }
    BoundaryShellTopology out;
    out.n_edges = use.size();
    for (const auto& [edge, count] : use) {
        (void)edge;
        if (count == 1) {
            ++out.n_open;
        } else if (count > 2) {
            ++out.n_nonmanifold;
        }
    }
    return out;
}

void replace_geometry_volume_note(std::string& note, std::string_view stage,
                                  const GeometryVolumeAssessment& assessment) {
    const std::string needle = std::format("geometry_{}_volume ", stage);
    const std::string replacement = geometry_volume_note(stage, assessment);
    const std::size_t token = note.find(needle);
    if (token == std::string::npos) {
        note += std::format(" | {}", replacement);
        return;
    }
    const std::size_t end = note.find(" | ", token);
    note.replace(token, end == std::string::npos ? std::string::npos : end - token,
                 replacement);
}

void enforce_feature_resolution(const Model& model, VolumeMeshOutput& output,
                                double requested_h, double delivered_h) {
    if (!model.cad || model.cad->empty() || output.boundary_quads.empty() ||
        !(requested_h > 0.0) || !(delivered_h > 0.0)) {
        return;
    }
    constexpr std::size_t kSamplesPerFace = 64;
    constexpr double kNormalMinDot = 0.5;
    const auto inspection = geom::inspect_brep(*model.cad);
    if (!inspection.available || inspection.face_count == 0) {
        return;
    }
    const auto topology = geom::extract_topology(*model.cad, 8);
    const auto samples =
        geom::sample_brep_surface(*model.cad, kSamplesPerFace * inspection.face_count);

    geom::TriSurface boundary;
    boundary.vertices = output.mesh.nodes;
    boundary.triangles.reserve(2 * output.boundary_quads.size());
    for (const auto& face : output.boundary_quads) {
        boundary.triangles.push_back({face[0], face[1], face[2]});
        if (face[3] != face[2]) {
            boundary.triangles.push_back({face[0], face[2], face[3]});
        }
    }

    const double limit = kGeometryFeatureResolutionOverH * requested_h;
    const double small_face_area = requested_h * requested_h;
    std::vector<bool> feature_face(inspection.face_count, false);
    for (std::size_t face_id = 0;
         face_id < inspection.face_count && face_id < topology.faces.size(); ++face_id) {
        feature_face[face_id] = topology.faces[face_id].kind != geom::CadSurfaceKind::kPlane ||
                                topology.faces[face_id].area <= small_face_area;
    }

    // A small/curved CAD face is present only when the delivered mesh contains
    // an actual nearby boundary patch with a compatible normal. Point distance
    // alone cannot detect a vanished through-hole in a thin plate: its bore is
    // still close to the plate's top/bottom faces, whose normals are orthogonal.
    std::vector<bool> face_has_patch(inspection.face_count, false);
    for (const auto& face : output.boundary_quads) {
        const int n = face[3] == face[2] ? 3 : 4;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int i = 0; i < n; ++i) {
            centroid += output.mesh.nodes[face[static_cast<std::size_t>(i)]];
        }
        centroid /= static_cast<double>(n);
        Eigen::Vector3d normal =
            (output.mesh.nodes[face[1]] - output.mesh.nodes[face[0]])
                .cross(output.mesh.nodes[face[2]] - output.mesh.nodes[face[0]]);
        const double normal_norm = normal.norm();
        if (!(normal_norm > 0.0) || !std::isfinite(normal_norm)) {
            continue;
        }
        normal /= normal_norm;
        for (std::size_t face_id = 0; face_id < feature_face.size(); ++face_id) {
            if (!feature_face[face_id] || face_has_patch[face_id]) {
                continue;
            }
            const auto exact = geom::project_point_on_face(
                *model.cad, static_cast<std::uint32_t>(face_id), centroid);
            if (exact && exact->distance <= limit &&
                std::abs(normal.dot(exact->normal)) >= kNormalMinDot) {
                face_has_patch[face_id] = true;
            }
        }
    }

    std::vector<double> face_distance(inspection.face_count, 0.0);
    std::vector<bool> sampled(inspection.face_count, false);
    for (std::size_t i = 0; i < samples.points.size(); ++i) {
        if (i >= samples.face_ids.size() || samples.face_ids[i] >= face_distance.size()) {
            continue;
        }
        const auto face_id = samples.face_ids[i];
        const double distance = mesh::closest_on_surface(boundary, samples.points[i]).distance;
        face_distance[face_id] = std::max(face_distance[face_id], distance);
        sampled[face_id] = true;
    }

    double worst_distance = 0.0;
    std::uint32_t worst_face = 0;
    bool unresolved = false;
    for (std::size_t face_id = 0; face_id < feature_face.size(); ++face_id) {
        if (!feature_face[face_id] || face_has_patch[face_id]) {
            continue;
        }
        // Projection collision repair cannot preserve a separate skin across
        // less than 5% of a fine lattice spacing. Account for that dimensional
        // absorption explicitly, rather than demanding an impossible normal
        // patch. Distance fidelity remains mandatory, and ordinary holes still
        // need aligned walls. A plane's intrinsic extent excludes its normal.
        const double absorption_limit = 0.05 * std::min(requested_h, delivered_h);
        if (face_id < topology.faces.size() && sampled[face_id] &&
            topology.faces[face_id].min_extent > 0.0 &&
            topology.faces[face_id].min_extent < absorption_limit &&
            face_distance[face_id] <= limit) {
            output.mesher_note += std::format(
                " | feature_absorbed face={} extent={:.6g} area={:.6g} "
                "scale={:.6g} distance={:.6g}",
                face_id, topology.faces[face_id].min_extent, topology.faces[face_id].area,
                absorption_limit, face_distance[face_id]);
            continue;
        }
        unresolved = true;
        if (sampled[face_id] && face_distance[face_id] >= worst_distance) {
            worst_distance = face_distance[face_id];
            worst_face = static_cast<std::uint32_t>(face_id);
        }
    }
    if (!unresolved) {
        return;
    }
    const auto& volume = output.fill_geometry_volume;
    throw GeometryVolumeLimitError(
        std::format("feature unresolved at h={:.6g} m: CAD face {} has no aligned delivered "
                    "boundary patch within {:.6g} m (sampled CAD-to-mesh max distance "
                    "{:.6g} m); mesh/BRep volume relative error is {:.4g}. This Cartesian "
                    "grid fill supports one local h/2 level, so a hole/void smaller than "
                    "that level can disappear; reduce -h to <= {:.6g} m or raise "
                    "--max-elems/--max-dof",
                    requested_h, worst_face, limit, worst_distance, volume.relative_error,
                    0.6 * requested_h),
        volume, false);
}

} // namespace detail

GeometryVolumeAssessment measure_geometry_volume(const Model& model,
                                                 const fea::NodalMesh& nodal) {
    GeometryVolumeAssessment out;
    if (!model.cad || model.cad->empty()) {
        return out;
    }
    const auto completeness =
        mesh::evaluate_geometry_completeness(*model.cad, fea::mesh_volume(nodal));
    if (!completeness.available) {
        return out;
    }
    out.available = true;
    out.mesh_volume = completeness.mesh_volume;
    out.cad_volume = completeness.brep_volume;
    out.relative_error = completeness.relative_volume_error;
    return out;
}

void update_solved_geometry_volume(const Model& model, VolumeMeshOutput& output) {
    output.solved_geometry_volume = measure_geometry_volume(model, output.mesh);
    if (!output.solved_geometry_volume.available) {
        return;
    }
    detail::replace_geometry_volume_note(output.mesher_note, "solved",
                                         output.solved_geometry_volume);
    if (output.solved_geometry_volume.relative_error > kGeometryVolumeHardLimit) {
        throw GeometryVolumeLimitError(
            std::format("geometry solved-stage guard failed: mesh/BRep volume relative error "
                        "{:.4g} exceeds hard limit {:.4g}; solved geometry is incomplete | {}",
                        output.solved_geometry_volume.relative_error, kGeometryVolumeHardLimit,
                        output.mesher_note),
            output.solved_geometry_volume, true);
    }
}

} // namespace polymesh::pipeline
