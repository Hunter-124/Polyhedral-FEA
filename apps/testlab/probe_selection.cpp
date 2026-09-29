// SPDX-License-Identifier: BSD-3-Clause

// BC / load-face selection (box, normal rule, exact-CAD fallback), load
// assembly, selection-audit hashes, probes and solve-health gates.
//
// Anti-cheat: raw nodal max von Mises is a DIAGNOSTIC and never a score
// (ADR-0023); scoring reads element-centroid, face-mean and energy probes.

#include "fea/boundary_faces.hpp"
#include "fea/solve.hpp"
#include "fea/solve_health.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "geom/cad_topology.hpp"
#include "load_area.hpp"
#include "mesh/surface_project.hpp"
#include "probe_util.hpp"
#include "testlab_internal.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::testlab::detail {
namespace {

/// Unit normal from first triangle of a surface face (right-hand order of nodes).
/// Returns zero if the face is degenerate.
Eigen::Vector3d face_unit_normal(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    if (f.nodes.size() < 3) {
        return Eigen::Vector3d::Zero();
    }
    const Eigen::Vector3d& p0 = mesh.nodes[f.nodes[0]];
    const Eigen::Vector3d& p1 = mesh.nodes[f.nodes[1]];
    const Eigen::Vector3d& p2 = mesh.nodes[f.nodes[2]];
    Eigen::Vector3d n = (p1 - p0).cross(p2 - p0);
    const double nn = n.norm();
    if (!(nn > 1e-30)) {
        return Eigen::Vector3d::Zero();
    }
    return n / nn;
}

/// Legacy corner-only area. Keep this on the committed selection path so a
/// passing row cannot silently change its face ranking or load vector.
double legacy_surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    if (f.nodes.size() < 3) {
        return 0.0;
    }
    const Eigen::Vector3d& p0 = mesh.nodes[f.nodes[0]];
    const Eigen::Vector3d& p1 = mesh.nodes[f.nodes[1]];
    const Eigen::Vector3d& p2 = mesh.nodes[f.nodes[2]];
    double area = 0.5 * (p1 - p0).cross(p2 - p0).norm();
    if (f.nodes.size() >= 4 && f.nodes[2] != f.nodes[3]) {
        const Eigen::Vector3d& p3 = mesh.nodes[f.nodes[3]];
        area += 0.5 * (p2 - p0).cross(p3 - p0).norm();
    }
    return area;
}

/// Isoparametric face area, including tri6/quad8 mid-side geometry.
double surface_face_area(const fea::NodalMesh& mesh, const fea::SurfaceFace& face) {
    return fea::integrated_face_area(mesh, std::vector<fea::SurfaceFace>{face});
}

/// Boundary faces in `box`, optionally filtered so n·t̂ > min_dot when traction
/// is nonzero. Falls back to box-only if the normal filter empties the set.
std::vector<fea::SurfaceFace>
select_load_faces(const fea::NodalMesh& mesh, const Box3& box, const Eigen::Vector3d& traction,
                  double normal_min_dot, std::optional<double> expected_area = std::nullopt) {
    const auto all_faces = free_faces_as_surface(mesh);
    std::vector<fea::SurfaceFace> in_box;
    in_box.reserve(all_faces.size());
    for (const auto& face : all_faces) {
        if (box.contains(face_centroid(mesh, face))) {
            in_box.push_back(face);
        }
    }
    const double tnorm = traction.norm();
    const Eigen::Vector3d t_hat =
        tnorm > 1e-30 ? Eigen::Vector3d(traction / tnorm) : Eigen::Vector3d::Zero();
    if (in_box.empty() || !tlab::load_rule_filters(normal_min_dot, traction)) {
        return in_box;
    }
    std::vector<fea::SurfaceFace> filtered;
    filtered.reserve(in_box.size());
    // The normal test itself lives in tlab::load_rule_keeps_normal so the CAD-side
    // rule area cannot drift from it. |n·t̂| there tolerates inverted winding on
    // mixed/hex skins; box-only fallback below if nothing survives.
    for (const auto& face : in_box) {
        const Eigen::Vector3d n = face_unit_normal(mesh, face);
        if (n.norm() < 0.5) {
            continue; // degenerate
        }
        if (tlab::load_rule_keeps_normal(normal_min_dot, traction, n)) {
            filtered.push_back(face);
        }
    }
    if (filtered.empty()) {
        filtered = in_box;
    }
    // When CAD expected_area is known and the mesh free-skin overshoots (RVD
    // jagged tip, dual interfaces), keep the faces most aligned with traction
    // and farthest along t̂ until cumulative area is closest to expected.
    // Traction magnitude is still applied per-face; this only prunes which
    // free faces carry the load (same total force if area matches CAD).
    if (expected_area && *expected_area > 0.0 && filtered.size() > 1) {
        double total = 0.0;
        for (const auto& f : filtered) {
            total += legacy_surface_face_area(mesh, f);
        }
        const double exp = *expected_area;
        if (total > 1.05 * exp) {
            struct Ranked {
                fea::SurfaceFace face;
                double score = 0.0;
                double area = 0.0;
            };
            std::vector<Ranked> ranked;
            ranked.reserve(filtered.size());
            for (const auto& face : filtered) {
                const Eigen::Vector3d c = face_centroid(mesh, face);
                const Eigen::Vector3d n = face_unit_normal(mesh, face);
                Ranked r;
                r.face = face;
                r.area = legacy_surface_face_area(mesh, face);
                // Prefer outer tip: large c·t̂ and strong normal alignment.
                r.score = c.dot(t_hat) + 0.1 * std::abs(n.dot(t_hat));
                ranked.push_back(std::move(r));
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const Ranked& a, const Ranked& b) { return a.score > b.score; });
            std::vector<fea::SurfaceFace> kept;
            double acc = 0.0;
            double best_err = 1e300;
            std::size_t best_n = 0;
            for (std::size_t i = 0; i < ranked.size(); ++i) {
                acc += ranked[i].area;
                kept.push_back(ranked[i].face);
                const double err = std::abs(acc - exp);
                if (err < best_err) {
                    best_err = err;
                    best_n = kept.size();
                }
                // Stop once we are past expected and error is growing.
                if (acc > exp && err > best_err) {
                    break;
                }
            }
            if (best_n > 0 && best_n <= kept.size()) {
                kept.resize(best_n);
                return kept;
            }
        }
    }
    return filtered;
}

std::vector<fea::SurfaceFace>
select_exact_cad_load_faces(const fea::NodalMesh& mesh, const geom::CadModel& cad, double h,
                            std::span<const std::uint32_t> cad_face_ids) {
    if (cad_face_ids.empty()) {
        return {};
    }
    std::set<std::uint32_t> selected_ids(cad_face_ids.begin(), cad_face_ids.end());
    std::vector<mesh::BoundarySupport> provenance;
    mesh::BoundaryProjectionContext projection;
    if (!pipeline::make_boundary_projection(cad, h, &projection, &provenance)) {
        return {};
    }

    const auto all_faces = fea::boundary_surface_faces(mesh);
    provenance.resize(mesh.nodes.size());
    std::set<std::uint32_t> boundary_nodes;
    for (const auto& face : all_faces) {
        boundary_nodes.insert(face.nodes.begin(), face.nodes.end());
    }
    for (const auto node : boundary_nodes) {
        mesh::BoundarySupport owner;
        (void)projection.target(mesh.nodes[node], owner);
        provenance[node] = owner;
    }

    std::vector<fea::SurfaceFace> selected;
    selected.reserve(all_faces.size());
    for (const auto& face : all_faces) {
        std::size_t selected_votes = 0;
        std::size_t other_face_votes = 0;
        for (const auto node : face.nodes) {
            if (node >= provenance.size() ||
                provenance[node].kind != mesh::BoundarySupportKind::kCadFace) {
                continue;
            }
            if (selected_ids.contains(provenance[node].id)) {
                ++selected_votes;
            } else {
                ++other_face_votes;
            }
        }
        bool keep = selected_votes > 0 && selected_votes >= other_face_votes;
        if (!keep) {
            const auto exact = geom::project_point_on_surface(cad, face_centroid(mesh, face));
            keep = exact && exact->face_id != geom::kInvalidCadSupportId &&
                   selected_ids.contains(exact->face_id);
        }
        if (keep) {
            selected.push_back(face);
        }
    }
    return selected;
}

void hash_mix(std::uint64_t& hash, std::uint64_t value) {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (8 * byte)) & 0xffu;
        hash *= kPrime;
    }
}

int count_orphan_nodes(const fea::NodalMesh& mesh) {
    if (mesh.nodes.empty()) {
        return 0;
    }
    std::vector<char> used(mesh.nodes.size(), 0);
    for (const auto& el : mesh.elements) {
        for (const auto ni : el.nodes) {
            if (ni < used.size()) {
                used[ni] = 1;
            }
        }
    }
    int n = 0;
    for (const char u : used) {
        if (!u) {
            ++n;
        }
    }
    return n;
}

/// Unique nodes on the exact face set used to assemble the traction load.
std::vector<std::uint32_t> nodes_on_load_faces(const ResolvedLoadFaces& resolved,
                                               int* n_faces_out = nullptr) {
    std::set<std::uint32_t> unique;
    for (const auto& face : resolved.faces) {
        unique.insert(face.nodes.begin(), face.nodes.end());
    }
    if (n_faces_out != nullptr) {
        *n_faces_out = static_cast<int>(resolved.faces.size());
    }
    return std::vector<std::uint32_t>(unique.begin(), unique.end());
}

/// Fallback: unique nodes whose coordinates fall in `box`.
std::vector<std::uint32_t> nodes_in_box(const fea::NodalMesh& mesh, const Box3& box) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
        if (box.contains(mesh.nodes[i])) {
            out.push_back(i);
        }
    }
    return out;
}

/// Quality floor for p99 / face-mean stress (exclude slivers that invent 1e20 VM).
/// `ElementCentroidStress::quality` is fea::cell_quality for every element type
/// (0 = unmeasurable); 0.02 on the scaled-Jacobian / aspect scale ≈ a 50:1 cell.
constexpr double kStressQualityFloor = 0.02;

} // namespace

fea::Dirichlet make_dirichlet(const fea::NodalMesh& mesh, const std::vector<BcSpec>& bcs,
                              const geom::CadModel* cad, double h) {
    // Node-in-box plus free-face centroid-in-box (surface snap can pull end-face
    // nodes slightly off the CAD plane so pure node-in-box misses fixtures).
    fea::Dirichlet bc;
    auto fix_node = [&](std::uint32_t i, const BcSpec& b) {
        for (int a = 0; a < 3; ++a) {
            if (b.fix[static_cast<std::size_t>(a)]) {
                bc.dof_values[3 * static_cast<Eigen::Index>(i) + a] = 0.0;
            }
        }
    };
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
        const Eigen::Vector3d& p = mesh.nodes[i];
        for (const auto& b : bcs) {
            if (b.box.contains(p)) {
                fix_node(i, b);
            }
        }
    }
    const auto faces = fea::extract_boundary_faces(mesh);
    for (const auto& q : faces) {
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            // Degenerate tri quads use q[2]==q[3].
            if (k == 3 && q[2] == q[3]) {
                break;
            }
            c += mesh.nodes[q[static_cast<std::size_t>(k)]];
            ++n;
        }
        if (n <= 0) {
            continue;
        }
        c /= static_cast<double>(n);
        for (const auto& b : bcs) {
            if (!b.box.contains(c)) {
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                if (k == 3 && q[2] == q[3]) {
                    break;
                }
                fix_node(q[static_cast<std::size_t>(k)], b);
            }
        }
    }
    // Fallback: if still under-constrained, pin every node inside each BC box
    // expanded by 2% of mesh bbox diagonal (coarse graded meshes often leave
    // the CAD face plane by a fraction of h after snap).
    if (bc.dof_values.size() < 9 && !mesh.nodes.empty()) {
        Eigen::Vector3d lo = mesh.nodes.front();
        Eigen::Vector3d hi = lo;
        for (const auto& p : mesh.nodes) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        const double pad = 0.02 * std::max((hi - lo).norm(), 1e-9);
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
            const Eigen::Vector3d& p = mesh.nodes[i];
            for (const auto& b : bcs) {
                Box3 exp = b.box;
                exp.lo -= Eigen::Vector3d::Constant(pad);
                exp.hi += Eigen::Vector3d::Constant(pad);
                if (exp.contains(p)) {
                    fix_node(i, b);
                }
            }
        }
    }
    // Exact-support fallback is deliberately gated on total legacy failure:
    // every currently passing fixture set remains byte-for-byte unchanged.
    if (bc.dof_values.empty() && cad != nullptr) {
        for (const auto& b : bcs) {
            for (const auto& face :
                 select_exact_cad_load_faces(mesh, *cad, h, b.cad_face_ids)) {
                for (const auto node : face.nodes) {
                    fix_node(node, b);
                }
            }
        }
    }
    return bc;
}

std::vector<fea::SurfaceFace> free_faces_as_surface(const fea::NodalMesh& mesh) {
    const auto quads = fea::extract_boundary_faces(mesh);
    std::vector<fea::SurfaceFace> faces;
    faces.reserve(quads.size());
    for (const auto& q : quads) {
        fea::SurfaceFace f;
        if (q[2] == q[3]) {
            f.type = fea::FaceType::kTri3;
            f.nodes = {q[0], q[1], q[2]};
        } else {
            f.type = fea::FaceType::kQuad4;
            f.nodes = {q[0], q[1], q[2], q[3]};
        }
        faces.push_back(std::move(f));
    }
    return faces;
}

Eigen::Vector3d face_centroid(const fea::NodalMesh& mesh, const fea::SurfaceFace& f) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (auto n : f.nodes) {
        c += mesh.nodes[n];
    }
    return c / static_cast<double>(f.nodes.size());
}

std::vector<ResolvedLoadFaces> resolve_load_faces(const fea::NodalMesh& mesh,
                                                  const geom::CadModel* cad, double h,
                                                  const std::vector<LoadSpec>& loads) {
    std::vector<ResolvedLoadFaces> out;
    out.reserve(loads.size());
    for (const auto& load : loads) {
        ResolvedLoadFaces resolved;
        resolved.faces = select_load_faces(mesh, load.box, load.traction, load.normal_min_dot,
                                           load.expected_area);
        for (const auto& face : resolved.faces) {
            resolved.mesh_selected_area += legacy_surface_face_area(mesh, face);
        }

        bool legacy_failed_area_gate = false;
        if (load.expected_area && *load.expected_area > 0.0) {
            legacy_failed_area_gate =
                std::abs(resolved.mesh_selected_area - *load.expected_area) /
                    *load.expected_area >
                0.05;
        }
        if ((resolved.faces.empty() || legacy_failed_area_gate) && cad != nullptr &&
            load.cad_face_area && *load.cad_face_area > 0.0 && !load.cad_face_ids.empty()) {
            // Face REPLACEMENT: the box selection found nothing usable, so take the
            // faces the exact CAD ids resolve to instead.
            auto exact_faces = select_exact_cad_load_faces(mesh, *cad, h, load.cad_face_ids);
            double mesh_area = 0.0;
            for (const auto& face : exact_faces) {
                mesh_area += surface_face_area(mesh, face);
            }
            if (!exact_faces.empty() && mesh_area > 0.0) {
                resolved.faces = std::move(exact_faces);
                resolved.mesh_selected_area = mesh_area;
                resolved.used_exact_fallback = true;
            }
        }

        // Resultant-preserving rescale. Whichever face set we ended up with, the
        // applied resultant is made equal to traction x cad_rule_area, so it no
        // longer depends on how well this mesh happened to resolve a curved loaded
        // surface. A coarse mesh then solves the RIGHT problem badly instead of the
        // WRONG problem, and mesh_selected_area keeps the measured deficit so the
        // remaining distribution error stays visible in the row.
        if (load.cad_rule_area && *load.cad_rule_area > 0.0 &&
            resolved.mesh_selected_area > 0.0) {
            resolved.reported_area = *load.cad_rule_area;
            resolved.traction_scale = *load.cad_rule_area / resolved.mesh_selected_area;
        } else {
            resolved.reported_area = resolved.mesh_selected_area;
        }
        out.push_back(std::move(resolved));
    }
    return out;
}

Eigen::VectorXd make_loads(const fea::NodalMesh& mesh, const std::vector<LoadSpec>& loads,
                           const std::vector<ResolvedLoadFaces>& resolved_loads) {
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (std::size_t load_index = 0; load_index < loads.size(); ++load_index) {
        const auto& L = loads[load_index];
        const auto& resolved = resolved_loads[load_index];
        const auto& selected = resolved.faces;
        if (selected.empty()) {
            // Preserve the legacy node-lump fallback when neither selector can
            // identify a boundary face.
            std::vector<std::uint32_t> nodes;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(mesh.nodes.size()); ++i) {
                if (L.box.contains(mesh.nodes[i])) {
                    nodes.push_back(i);
                }
            }
            if (nodes.empty()) {
                continue;
            }
            Eigen::Vector3d lo = mesh.nodes[nodes.front()];
            Eigen::Vector3d hi = lo;
            for (auto n : nodes) {
                lo = lo.cwiseMin(mesh.nodes[n]);
                hi = hi.cwiseMax(mesh.nodes[n]);
            }
            const Eigen::Vector3d ext = (hi - lo).cwiseMax(1e-30);
            const double area =
                ext[0] * ext[1] * ext[2] / std::max({ext[0], ext[1], ext[2], 1e-30});
            const Eigen::Vector3d per = L.traction * area / static_cast<double>(nodes.size());
            for (auto n : nodes) {
                f.segment<3>(3 * static_cast<Eigen::Index>(n)) += per;
            }
            continue;
        }
        const Eigen::Vector3d t = resolved.traction_scale * L.traction;
        f += fea::assemble_traction_load(mesh, selected,
                                         [&](const Eigen::Vector3d&) { return t; });
    }
    return f;
}

std::uint64_t selected_face_set_hash(const std::vector<ResolvedLoadFaces>& selections) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t load_index = 0; load_index < selections.size(); ++load_index) {
        hash_mix(hash, load_index);
        std::vector<std::vector<std::uint32_t>> faces;
        faces.reserve(selections[load_index].faces.size());
        for (const auto& face : selections[load_index].faces) {
            auto nodes = face.nodes;
            std::sort(nodes.begin(), nodes.end());
            faces.push_back(std::move(nodes));
        }
        std::sort(faces.begin(), faces.end());
        hash_mix(hash, faces.size());
        for (const auto& face : faces) {
            hash_mix(hash, face.size());
            for (const auto node : face) {
                hash_mix(hash, node);
            }
        }
    }
    return hash;
}

std::uint64_t selected_node_set_hash(const std::vector<ResolvedLoadFaces>& selections) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t load_index = 0; load_index < selections.size(); ++load_index) {
        hash_mix(hash, load_index);
        std::set<std::uint32_t> nodes;
        for (const auto& face : selections[load_index].faces) {
            nodes.insert(face.nodes.begin(), face.nodes.end());
        }
        hash_mix(hash, nodes.size());
        for (const auto node : nodes) {
            hash_mix(hash, node);
        }
    }
    return hash;
}

std::uint64_t load_vector_hash(const Eigen::VectorXd& loads) {
    std::uint64_t hash = 1469598103934665603ull;
    hash_mix(hash, static_cast<std::uint64_t>(loads.size()));
    for (Eigen::Index i = 0; i < loads.size(); ++i) {
        hash_mix(hash, std::bit_cast<std::uint64_t>(loads[i]));
    }
    return hash;
}

std::uint64_t dirichlet_node_set_hash(const fea::Dirichlet& bc) {
    std::set<std::uint64_t> nodes;
    for (const auto& [dof, value] : bc.dof_values) {
        (void)value;
        nodes.insert(static_cast<std::uint64_t>(dof / 3));
    }
    std::uint64_t hash = 1469598103934665603ull;
    hash_mix(hash, nodes.size());
    for (const auto node : nodes) {
        hash_mix(hash, node);
    }
    return hash;
}

ProbeAnswers compute_probes(const fea::NodalMesh& mesh, const fea::Material& mat,
                            const Eigen::VectorXd& u, const std::vector<LoadSpec>& loads,
                            const std::vector<ResolvedLoadFaces>& resolved_loads,
                            const std::vector<MetricSpec>& metrics, const fea::Dirichlet& bc,
                            const Eigen::VectorXd& f) {
    ProbeAnswers a;
    a.n_orphan_nodes = count_orphan_nodes(mesh);
    a.tip_deflection_max = tlab::global_max_displacement_mag(u, mesh.nodes.size());
    a.strain_energy = fea::strain_energy(mesh, mat, u);

    // DIAGNOSTIC: nodal max (can spike on slivers — never the campaign score).
    const auto nodal = fea::recover_nodal_stress(mesh, mat, u);
    for (const auto& s : nodal) {
        a.sigma_max = std::max(a.sigma_max, fea::von_mises(s));
    }

    // Stress select box: first metric probe.select, else first load box.
    Box3 stress_box;
    bool have_stress_box = false;
    for (const auto& m : metrics) {
        if (m.probe.select) {
            stress_box = *m.probe.select;
            have_stress_box = true;
            break;
        }
    }
    if (!have_stress_box && !loads.empty()) {
        stress_box = loads.front().box;
        have_stress_box = true;
    }

    // Element-centroid (interior) stress samples — scoring path.
    const auto elem_s = fea::recover_element_centroid_stress(mesh, mat, u);
    std::vector<double> vm_quality;
    vm_quality.reserve(elem_s.size());
    for (const auto& es : elem_s) {
        if (es.quality < kStressQualityFloor) {
            ++a.n_quality_excluded;
            continue;
        }
        const double vm = fea::von_mises(es.stress);
        vm_quality.push_back(vm);
        if (have_stress_box && stress_box.contains(es.centroid)) {
            a.sigma_box_max = std::max(a.sigma_box_max, vm);
        }
    }
    if (!vm_quality.empty()) {
        std::sort(vm_quality.begin(), vm_quality.end());
        const std::size_t idx = std::min(
            vm_quality.size() - 1,
            static_cast<std::size_t>(0.99 * static_cast<double>(vm_quality.size() - 1)));
        a.sigma_p99 = vm_quality[idx];
    }

    // Area-weighted mean VM: for each boundary face in stress box, use nearest
    // quality-passing element-centroid sample (never nodal extrapolation).
    if (have_stress_box && !elem_s.empty()) {
        double wsum = 0.0;
        double vsum = 0.0;
        for (const auto& face : free_faces_as_surface(mesh)) {
            const Eigen::Vector3d c = face_centroid(mesh, face);
            if (!stress_box.contains(c)) {
                continue;
            }
            const double area = legacy_surface_face_area(mesh, face);
            if (!(area > 0.0)) {
                continue;
            }
            double best_d2 = std::numeric_limits<double>::infinity();
            double best_vm = 0.0;
            bool found = false;
            for (const auto& es : elem_s) {
                if (es.quality < kStressQualityFloor) {
                    continue;
                }
                const double d2 = (es.centroid - c).squaredNorm();
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best_vm = fea::von_mises(es.stress);
                    found = true;
                }
            }
            if (found) {
                wsum += area;
                vsum += area * best_vm;
            }
        }
        if (wsum > 0.0) {
            a.sigma_face_mean = vsum / wsum;
        }
    }

    // Tip / load face mean displacement + optional expected-area guard.
    // Uses the same box + normal-aligned face set as assemble_traction_load so
    // lateral wall faces near a tip slab are not counted as load area.
    std::vector<std::uint32_t> probe_nodes;
    if (!loads.empty() && !resolved_loads.empty()) {
        const LoadSpec& L0 = loads.front();
        const ResolvedLoadFaces& selected = resolved_loads.front();
        a.dominant_load_axis = tlab::dominant_axis(L0.traction);
        int n_faces = 0;
        probe_nodes = nodes_on_load_faces(selected, &n_faces);
        a.n_load_faces = n_faces;
        a.load_face_area = selected.reported_area;
        a.mesh_selected_area = selected.mesh_selected_area;

        // Expected area: the case rule on exact CAD (cad_rule_area, the rescale
        // target) when available, else the authored expected_area; never a 0.0
        // stand-in. Policy lives in load_area.hpp so it can be unit-tested.
        const tlab::LoadAreaAssessment area =
            tlab::assess_load_area(L0.expected_area, L0.cad_rule_area, a.mesh_selected_area);
        a.load_area_status = area.status;
        a.load_area_rel_err = area.rel_err;
        a.load_area_ok = area.ok;

        // Mesh-independent: does the authored case definition still agree with the
        // CAD? Reported separately from the deficit above, never folded into it.
        const tlab::AuthoredAreaCheck authored =
            tlab::check_authored_area(L0.expected_area, L0.cad_rule_area);
        a.authored_area_checked = authored.checked;
        a.authored_area_rel_diff = authored.rel_diff;
        a.authored_area_consistent = authored.consistent;
        if (probe_nodes.empty()) {
            probe_nodes = nodes_in_box(mesh, L0.box);
        }
    }
    a.n_probe_nodes = static_cast<int>(probe_nodes.size());
    if (!probe_nodes.empty()) {
        a.tip_deflection = tlab::face_mean_displacement_mag(u, probe_nodes);
        a.mean_u_component =
            tlab::face_mean_displacement_component(u, probe_nodes, a.dominant_load_axis);
        a.mean_ux = tlab::face_mean_displacement_component(u, probe_nodes, 0);
        a.mean_uz = tlab::face_mean_displacement_component(u, probe_nodes, 2);
    } else {
        a.tip_deflection = 0.0;
    }

    // Health gates for accuracy trust; thresholds live in run_one.cpp.
    const fea::SolveHealth health = fea::solve_health(mesh, mat, bc, f, u);
    a.free_residual_rel = health.free_residual_rel;
    a.reaction_sum_err = health.reaction_sum_err;
    a.n_bc_dofs = static_cast<int>(bc.dof_values.size());
    return a;
}

double evaluate_probe(const ProbeSpec& probe, const ProbeAnswers& a) {
    // Score path: face-mean stress, energy, tip face-mean. Raw max is diagnostic.
    if (probe.kind == "mean_vm" || probe.kind == "mean_von_mises" ||
        probe.kind == "face_mean_vm") {
        return a.sigma_face_mean;
    }
    if (probe.kind == "peak_vm") {
        return a.sigma_box_max;
    }
    if (probe.kind == "peak_vm_over_nominal") {
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("peak_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_box_max / probe.nominal;
    }
    if (probe.kind == "mean_vm_over_nominal" || probe.kind == "scf_mean" ||
        probe.kind == "scf") {
        // "scf" now means face-mean VM / nominal (not nodal max).
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("mean_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_face_mean / probe.nominal;
    }
    if (probe.kind == "max_von_mises" || probe.kind == "max_vm") {
        // Diagnostic only — references should not score this.
        return a.sigma_max;
    }
    if (probe.kind == "max_vm_over_nominal") {
        if (!(std::abs(probe.nominal) > 0.0)) {
            throw std::runtime_error("max_vm_over_nominal requires probe.nominal != 0");
        }
        return a.sigma_max / probe.nominal;
    }
    if (probe.kind == "sigma_p99" || probe.kind == "p99_vm") {
        return a.sigma_p99;
    }
    if (probe.kind == "strain_energy" || probe.kind == "energy") {
        return a.strain_energy;
    }
    if (probe.kind == "max_displacement" || probe.kind == "tip_deflection") {
        return a.tip_deflection;
    }
    if (probe.kind == "mean_ux_on_face") {
        return (a.dominant_load_axis == 0) ? a.mean_u_component : a.mean_ux;
    }
    if (probe.kind == "mean_uz_on_face") {
        return (a.dominant_load_axis == 2) ? a.mean_u_component : a.mean_uz;
    }
    throw std::runtime_error("unknown probe kind '" + probe.kind + "'");
}

PartCase with_exact_cad_selections(const pipeline::Model& model, const PartCase& source) {
    PartCase resolved = source;
    if (!model.cad) {
        return resolved;
    }
    const geom::CadTopology topology = geom::extract_topology(*model.cad, 4);
    const auto& surface = model.surface;
    for (auto& bc : resolved.bcs) {
        std::set<std::uint32_t> face_ids;
        Eigen::Index slab_axis = 0;
        (bc.box.hi - bc.box.lo).cwiseAbs().minCoeff(&slab_axis);
        Eigen::Vector3d slab_direction = Eigen::Vector3d::Zero();
        slab_direction[slab_axis] = 1.0;
        for (const auto& tri : surface.triangles) {
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!bc.box.contains(centroid)) {
                continue;
            }
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            if (!(cross.norm() > 0.0) ||
                std::abs(cross.normalized().dot(slab_direction)) <= 0.7) {
                continue;
            }
            const auto exact = geom::project_point_on_surface(*model.cad, centroid);
            if (exact && exact->face_id != geom::kInvalidCadSupportId) {
                face_ids.insert(exact->face_id);
            }
        }
        bc.cad_face_ids.assign(face_ids.begin(), face_ids.end());
    }
    for (auto& load : resolved.loads) {
        std::set<std::uint32_t> box_faces;
        std::set<std::uint32_t> aligned_faces;
        const double traction_norm = load.traction.norm();
        Eigen::Vector3d cap_direction = Eigen::Vector3d::Zero();
        if (traction_norm > 0.0) {
            cap_direction = load.traction / traction_norm;
        }
        double cap_min_dot = load.normal_min_dot;
        if (!(cap_min_dot > -1.0)) {
            // Normal filter disabled: the slab's thin axis (not the traction) picks
            // the CAD faces the face-replacement fallback may substitute. It must NOT
            // reach cad_rule_area, which uses the case's own normal_min_dot below.
            Eigen::Index slab_axis = 0;
            (load.box.hi - load.box.lo).cwiseAbs().minCoeff(&slab_axis);
            cap_direction = Eigen::Vector3d::Zero();
            cap_direction[slab_axis] = 1.0;
            cap_min_dot = 0.7;
        }
        // CAD face ids that resolve, so the rule area counts the same tessellation
        // the cap sets are built from and stays comparable with earlier runs.
        std::set<std::uint32_t> topology_ids;
        for (const auto& face : topology.faces) {
            topology_ids.insert(face.id);
        }
        // The case's rule as written, mirroring select_load_faces through the
        // shared predicate: box-only when normal_min_dot <= -1, else the filter,
        // with a fallback to box-only if the filter selects nothing.
        double box_rule_area = 0.0;
        double filtered_rule_area = 0.0;
        for (const auto& tri : surface.triangles) {
            const Eigen::Vector3d& a = surface.vertices[tri[0]];
            const Eigen::Vector3d& b = surface.vertices[tri[1]];
            const Eigen::Vector3d& c = surface.vertices[tri[2]];
            const Eigen::Vector3d centroid = (a + b + c) / 3.0;
            if (!load.box.contains(centroid)) {
                continue;
            }
            const auto exact = geom::project_point_on_surface(*model.cad, centroid);
            if (!exact || exact->face_id == geom::kInvalidCadSupportId) {
                continue;
            }
            box_faces.insert(exact->face_id);
            const Eigen::Vector3d cross = (b - a).cross(c - a);
            const double twice_area = cross.norm();
            const double tri_area = 0.5 * twice_area;
            // Cap set: heuristic direction, used ONLY to pick face ids for the
            // face-replacement fallback.
            if (cap_direction.norm() <= 0.0 ||
                (twice_area > 0.0 &&
                 std::abs((cross / twice_area).dot(cap_direction)) > cap_min_dot)) {
                aligned_faces.insert(exact->face_id);
            }
            // Rule area: the case's OWN normal_min_dot and traction, through the
            // same predicate the mesh selector uses.
            if (topology_ids.contains(exact->face_id)) {
                box_rule_area += tri_area;
                if (tlab::load_rule_keeps_normal(load.normal_min_dot, load.traction, cross)) {
                    filtered_rule_area += tri_area;
                }
            }
        }
        const bool authored_virtual_patch =
            resolved.loads.size() > 1 && load.expected_area && *load.expected_area > 0.0;
        const bool use_aligned = !aligned_faces.empty();
        const auto& selected = use_aligned ? aligned_faces : box_faces;
        double exact_area = 0.0;
        for (const auto face_id : selected) {
            const auto it =
                std::find_if(topology.faces.begin(), topology.faces.end(),
                             [&](const geom::CadFace& face) { return face.id == face_id; });
            if (it != topology.faces.end() && !authored_virtual_patch) {
                load.cad_face_ids.push_back(face_id);
                exact_area += it->area;
            }
        }
        if (exact_area > 0.0) {
            load.cad_face_area = exact_area;
        }
        // Mirror select_load_faces at the set level too: a filter that selects
        // nothing falls back to the whole in-box set.
        const double rule_area =
            (tlab::load_rule_filters(load.normal_min_dot, load.traction) &&
             filtered_rule_area > 0.0)
                ? filtered_rule_area
                : box_rule_area;
        if (authored_virtual_patch) {
            // A multi-region case cuts one CAD face into virtual patches. The
            // generator records their exact BRep-clipped areas; the coarse
            // display tessellation's centroid sum is only an approximation and
            // must not replace the authored continuum measure. Nor may the
            // whole-face fallback above erase the split.
            load.cad_rule_area = *load.expected_area;
        } else if (rule_area > 0.0) {
            // The case rule's continuum limit on the exact CAD tessellation (a mesh's
            // answer on a curved loaded surface is quantised to facet size), so it is
            // what the traction is rescaled onto.
            load.cad_rule_area = rule_area;
        }
        // Loud once per part: an authored expected_area that no longer matches the
        // CAD is a case-definition or geometry bug, and every row it produces is
        // scored against a load the reference did not assume.
        const auto authored =
            tlab::check_authored_area(load.expected_area, load.cad_rule_area);
        if (authored.checked && !authored.consistent) {
            std::fprintf(stderr,
                         "WARNING %s: authored select.expected_area %.9g disagrees with the "
                         "exact CAD rule area %.9g by %.3g%% (tol %.3g%%). This is a case "
                         "definition or geometry drift, not mesh quality: the reference "
                         "truth assumes a different loaded region than the run applies.\n",
                         resolved.part.c_str(), *load.expected_area, *load.cad_rule_area,
                         100.0 * *authored.rel_diff, 100.0 * tlab::kAuthoredAreaTol);
        }
    }
    return resolved;
}

} // namespace polymesh::testlab::detail
