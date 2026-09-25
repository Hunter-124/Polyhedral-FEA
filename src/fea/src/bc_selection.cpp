// SPDX-License-Identifier: BSD-3-Clause

#include "fea/bc_selection.hpp"

#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"

#include <Eigen/Core>
#include <Eigen/Eigenvalues>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::fea {
namespace {

// A default BC slab must be broad enough to behave like a face. 12 nodes is
// ~4 boundary faces — the smallest patch that carries a traction instead of a
// point force — and 2% of the boundary nodes keeps that true on fine meshes,
// where even a few dozen nodes can still be a pinpoint. The 2% threshold
// separates curved closed parts (cone/sphere end slabs: <=1.6%) from the broad
// planar cantilever/plate ends (>=7.7%) on the default meshes. Below either
// bound the 0.51*h x-slab has degenerated and we switch to face selection.
constexpr std::size_t kMinSelNodes = 12;
constexpr double kMinSelFrac = 0.02;

} // namespace

std::size_t sane_selection_minimum(std::size_t n_boundary_nodes) {
    return std::max<std::size_t>(
        kMinSelNodes, static_cast<std::size_t>(
                          std::ceil(kMinSelFrac * static_cast<double>(n_boundary_nodes))));
}

std::size_t count_boundary_nodes(const std::vector<SurfaceFace>& faces) {
    return boundary_face_nodes(faces).size();
}

// Every branch selects only nodes on the boundary surface: a box names a region
// of the boundary, not a volume of material. Clamping interior nodes would embed
// a strain-free rigid inclusion (the pipeline's SolveJob collect_bcs() in
// solve_job.cpp follows the same rule).
BcSelection select_cantilever_end(const NodalMesh& mesh,
                                  const std::vector<SurfaceFace>& all_faces,
                                  std::size_t n_boundary_nodes,
                                  const std::optional<LoadRegion>& box, double xmin,
                                  double xmax, double tol, int end) {
    BcSelection sel;
    constexpr double kInf = std::numeric_limits<double>::infinity();
    if (box.has_value()) {
        sel.from_box = true;
        sel.region = LoadRegion{box->lo, box->hi};
        sel.nodes = boundary_nodes_within(mesh, all_faces, *sel.region);
        sel.faces = faces_touching(mesh, all_faces, *sel.region);
        return sel;
    }
    // The default slab as a region: one open end, so it is the same rule as a
    // box with two infinite bounds. `sel.region` stays unset — a slab is only
    // ever used to name an end FACE, so its faces come from `faces_within` and
    // no load is clipped to it.
    const LoadRegion slab =
        end < 0 ? LoadRegion{Eigen::Vector3d::Constant(-kInf), {xmin + tol, kInf, kInf}}
                : LoadRegion{{xmax - tol, -kInf, -kInf}, Eigen::Vector3d::Constant(kInf)};
    sel.nodes = boundary_nodes_within(mesh, all_faces, slab);
    sel.slab_nodes = sel.nodes.size();
    const auto need = sane_selection_minimum(n_boundary_nodes);
    if (sel.nodes.size() >= need) {
        // The slab's node set reaches 0.51·h up the side walls, so a plain
        // faces_within() also returns side-wall rim facets: as a fixture they
        // clamp an artificial patch boundary one slab deep into the wall, and as
        // a load they carry the end traction as in-plane shear. Keep only
        // end-facing facets (the fallback path's own |n·x̂| convention) and let
        // the node set be their closure: the end face including its perimeter
        // ring, nothing up the walls.
        const auto slab_faces = faces_within(all_faces, sel.nodes);
        for (const auto& f : slab_faces) {
            const Eigen::Vector3d n = surface_face_normal(mesh, f);
            if (n.squaredNorm() > 0.0 && std::abs(n.x()) >= kSelectionNormalMinDot) {
                sel.faces.push_back(f);
            }
        }
        if (!sel.faces.empty()) {
            sel.nodes = boundary_face_nodes(sel.faces);
            return sel;
        }
        sel.nodes.clear(); // nothing end-facing: widen via the band fallback
    }
    // Degenerate slab: take the ±x-facing boundary faces near this end instead.
    // |n·x̂| (not the signed dot) because mixed hex/pyramid skins do not
    // guarantee outward winding; the end band keeps the patch at the end rather
    // than over the whole half. Start tight and widen only while the patch is
    // still too small to act as a face.
    sel.face_fallback = true;
    const double extent = xmax - xmin;
    for (const double frac : {0.10, 0.25, 0.50}) {
        sel.faces.clear();
        sel.nodes.clear();
        sel.fallback_band = frac;
        const double cut = end < 0 ? xmin + frac * extent : xmax - frac * extent;
        for (const auto& f : all_faces) {
            const Eigen::Vector3d n = surface_face_normal(mesh, f);
            if (n.squaredNorm() <= 0.0 || std::abs(n.x()) < kSelectionNormalMinDot) {
                continue;
            }
            Eigen::Vector3d c = Eigen::Vector3d::Zero();
            for (const auto id : f.nodes) {
                c += mesh.nodes[id];
            }
            c /= static_cast<double>(f.nodes.size());
            if (end < 0 ? (c.x() > cut) : (c.x() < cut)) {
                continue;
            }
            sel.faces.push_back(f);
            sel.nodes.insert(sel.nodes.end(), f.nodes.begin(), f.nodes.end());
        }
        std::sort(sel.nodes.begin(), sel.nodes.end());
        sel.nodes.erase(std::unique(sel.nodes.begin(), sel.nodes.end()), sel.nodes.end());
        if (sel.nodes.size() >= need) {
            break;
        }
    }
    return sel;
}

// Pressure is a normal surface load: when a box includes a strip of adjacent
// side wall (for example z>=0.195 on a cylinder ending at z=0.2), keep only
// faces whose normal aligns with the requested pressure direction.
std::vector<SurfaceFace> pressure_aligned_faces(const NodalMesh& mesh,
                                                const std::vector<SurfaceFace>& box_faces,
                                                const Eigen::Vector3d& direction) {
    std::vector<SurfaceFace> out;
    out.reserve(box_faces.size());
    for (const auto& f : box_faces) {
        const Eigen::Vector3d n = surface_face_normal(mesh, f);
        if (std::abs(n.dot(direction)) >= kSelectionNormalMinDot) {
            out.push_back(f);
        }
    }
    return out;
}

// Fully fixing 3 non-collinear nodes gives 9 constraints and removes all six
// rigid-body modes; 2 nodes (or any collinear set) leaves the rotation about
// their axis free and the stiffness matrix singular.
std::string constraint_defect(const NodalMesh& mesh,
                              const std::vector<std::uint32_t>& fixed_nodes) {
    if (fixed_nodes.size() < 3) {
        return std::format("only {} fixed node(s) ({} constraints): fewer than the 3 "
                           "non-collinear nodes needed to remove all 6 rigid-body modes",
                           fixed_nodes.size(), 3 * fixed_nodes.size());
    }
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const auto n : fixed_nodes) {
        mean += mesh.nodes[n];
    }
    mean /= static_cast<double>(fixed_nodes.size());
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const auto n : fixed_nodes) {
        const Eigen::Vector3d d = mesh.nodes[n] - mean;
        cov += d * d.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    const Eigen::Vector3d ev = es.eigenvalues(); // ascending
    if (!(ev[2] > 0.0)) {
        return "all fixed nodes are coincident";
    }
    // Second spread axis relative to the first: below 1e-6 the cloud is a line.
    if (std::sqrt(ev[1] / ev[2]) < 1e-6) {
        return std::format("the {} fixed nodes are collinear (spread ratio {:.3g}), leaving "
                           "rotation about that axis unconstrained",
                           fixed_nodes.size(), std::sqrt(ev[1] / ev[2]));
    }
    return {};
}

// A box selection carries a `region`: the traction is then integrated over the
// part of `faces` inside the box, so the loaded patch ends on the box plane
// instead of on a staircase of element edges (consistent_region_load).
// Everything else integrates complete faces, or, when a legitimate coarse
// selection has nodes but no complete face, preserves the requested resultant
// with a documented per-node fallback.
Eigen::VectorXd assemble_selection_load(const NodalMesh& mesh,
                                        const std::vector<SurfaceFace>& faces,
                                        std::span<const std::uint32_t> fallback_nodes,
                                        const SurfaceLoadSpec& spec, const char* what,
                                        std::FILE* report,
                                        const std::optional<LoadRegion>& region,
                                        std::optional<double> exact_pressure_area) {
    const double mesh_area = region.has_value() ? integrated_region_area(mesh, faces, *region)
                                                : integrated_face_area(mesh, faces);
    if (fallback_nodes.empty() && !(mesh_area > 0.0)) {
        throw std::runtime_error(std::format(
            "{}: the load selection is empty — widen --load-box or refine with -h.", what));
    }
    if (spec.traction_mode && !(mesh_area > 0.0) && !exact_pressure_area.has_value()) {
        throw std::runtime_error(std::format(
            "{}: pressure selection has nodes but no integrable face or CAD area; widen "
            "--load-box or use --force for a known resultant.",
            what));
    }
    const double pressure_area =
        exact_pressure_area.has_value() ? *exact_pressure_area : mesh_area;
    const double magnitude =
        spec.traction_mode ? spec.traction_pa * pressure_area : spec.force;
    const Eigen::Vector3d total = magnitude * spec.dir;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    Eigen::Vector3d resultant = Eigen::Vector3d::Zero();
    double conservation_error = total.norm();
    const bool node_fallback = !(mesh_area > 0.0);
    if (!node_fallback) {
        auto applied = region.has_value() ? consistent_region_load(mesh, faces, *region, total)
                                          : consistent_face_load(mesh, faces, total);
        loads = std::move(applied.loads);
        resultant = applied.resultant;
        conservation_error = applied.conservation_error;
    } else {
        const Eigen::Vector3d per_node = total / static_cast<double>(fallback_nodes.size());
        for (const auto node : fallback_nodes) {
            loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
            resultant += per_node;
        }
        conservation_error = (resultant - total).norm();
    }
    const char* area_kind = region.has_value() ? "clipped area" : "area";
    const std::string area_note =
        node_fallback ? std::format("node fallback={}", fallback_nodes.size())
                      : (exact_pressure_area.has_value()
                             ? std::format("mesh {}={:.9g} m², CAD area={:.9g} m²", area_kind,
                                           mesh_area, pressure_area)
                             : std::format("{}={:.9g} m²", area_kind, mesh_area));
    std::fprintf(report,
                 "load: %zu faces, %s, %s → |F|=%.9g N along (%.4g %.4g %.4g) | "
                 "Σf=(%.9g %.9g %.9g) N, conservation err=%.3g N\n",
                 faces.size(), area_note.c_str(),
                 spec.traction_mode ? std::format("t={:.6g} Pa", spec.traction_pa).c_str()
                                    : "total force",
                 total.norm(), spec.dir.x(), spec.dir.y(), spec.dir.z(), resultant.x(),
                 resultant.y(), resultant.z(), conservation_error);
    if (conservation_error > 1e-9) {
        throw std::runtime_error(
            std::format("{}: load assembly lost {:.3g} N of the requested {:.6g} N resultant "
                        "(total-force conservation check failed)",
                        what, conservation_error, total.norm()));
    }
    return loads;
}

} // namespace polymesh::fea
