// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "adapt/error.hpp"
#include "adapt/loop.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/features.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "mesh/hex_fill.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "mesh/transition_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <queue>
#include <set>
#include <span>

namespace polymesh::pipeline {
namespace adapt = polymesh::adapt;

namespace {

Eigen::Vector3d triangle_normal(const geom::TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    return ab.cross(ac).normalized();
}

/// Closest distance from a point to a triangle (Ericson, Real-Time
/// Collision Detection).
double point_triangle_distance(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                               const Eigen::Vector3d& b, const Eigen::Vector3d& c) {
    const Eigen::Vector3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return (p - a).norm();
    }
    const Eigen::Vector3d bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return (p - b).norm();
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return (p - (a + ab * (d1 / (d1 - d3)))).norm();
    }
    const Eigen::Vector3d cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return (p - c).norm();
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return (p - (a + ac * (d2 / (d2 - d6)))).norm();
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return (p - (b + (c - b) * w)).norm();
    }
    const double denom = 1.0 / (va + vb + vc);
    const Eigen::Vector3d closest = a + ab * (vb * denom) + ac * (vc * denom);
    return (p - closest).norm();
}

} // namespace

Model Model::load(const std::string& path, double sharp_angle_deg) {
    Model model;
    const auto lower = [&] {
        std::string s = path;
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    if (lower.ends_with(".step") || lower.ends_with(".stp")) {
        model.surface = geom::load_step(path);
    } else {
        model.surface = geom::load_stl(path);
    }
    model.surface.validate();
    const auto slash = path.find_last_of("/\\");
    model.name = slash == std::string::npos ? path : path.substr(slash + 1);

    model.bbox_min = model.surface.vertices.front();
    model.bbox_max = model.surface.vertices.front();
    for (const auto& v : model.surface.vertices) {
        model.bbox_min = model.bbox_min.cwiseMin(v);
        model.bbox_max = model.bbox_max.cwiseMax(v);
    }

    // CAD-style face regions: grow across edges whose dihedral angle is
    // below the sharp threshold.
    const std::size_t n_tris = model.surface.triangles.size();
    std::vector<Eigen::Vector3d> normals(n_tris);
    for (std::size_t t = 0; t < n_tris; ++t) {
        normals[t] = triangle_normal(model.surface, t);
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edge_tris;
    for (std::size_t t = 0; t < n_tris; ++t) {
        const auto& tri = model.surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                         tri[static_cast<std::size_t>((e + 1) % 3)]);
            edge_tris[key].push_back(static_cast<std::uint32_t>(t));
        }
    }
    const double cos_sharp = std::cos(sharp_angle_deg * std::numbers::pi / 180.0);
    model.triangle_region.assign(n_tris, -1);
    for (std::size_t seed = 0; seed < n_tris; ++seed) {
        if (model.triangle_region[seed] >= 0) {
            continue;
        }
        const int region = model.region_count++;
        std::queue<std::uint32_t> frontier;
        frontier.push(static_cast<std::uint32_t>(seed));
        model.triangle_region[seed] = region;
        while (!frontier.empty()) {
            const auto t = frontier.front();
            frontier.pop();
            const auto& tri = model.surface.triangles[t];
            for (int e = 0; e < 3; ++e) {
                const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                             tri[static_cast<std::size_t>((e + 1) % 3)]);
                for (const auto other : edge_tris.at(key)) {
                    if (model.triangle_region[other] >= 0) {
                        continue;
                    }
                    if (normals[t].dot(normals[other]) > cos_sharp) {
                        model.triangle_region[other] = region;
                        frontier.push(other);
                    }
                }
            }
        }
    }
    return model;
}

ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg) {
    ResolvedMeshSize out;
    if (requested_h > 0.0) {
        out.h = requested_h;
        out.auto_chosen = false;
        out.note = std::format("h={:.4g} m (user)", out.h);
        return out;
    }

    const Eigen::Vector3d extent_vec = model.bbox_max - model.bbox_min;
    const double extent = extent_vec.maxCoeff();
    const double diagonal = extent_vec.norm();
    // Primary scale: max edge of AABB / 16 (practical zero-tune; former CLI
    // mesh default). Secondary: diagonal / 28 — keeps long thin parts from
    // exploding along the short axes while still resolving the long span.
    double h_geom = extent / 16.0;
    if (diagonal > 0.0) {
        h_geom = std::min(h_geom, diagonal / 28.0);
    }
    if (!(h_geom > 0.0) || !std::isfinite(h_geom)) {
        h_geom = 0.05; // last-resort fallback for degenerate bbox
    }

    const auto edges = geom::detect_sharp_edges(model.surface, sharp_angle_deg);
    out.n_sharp_edges = edges.size();
    double min_feature = std::numeric_limits<double>::infinity();
    for (const auto& e : edges) {
        const double len =
            (model.surface.vertices[e.v0] - model.surface.vertices[e.v1]).norm();
        if (len > 1e-15) {
            min_feature = std::min(min_feature, len);
        }
    }
    if (!std::isfinite(min_feature)) {
        min_feature = 0.0;
    }
    out.min_feature_length = min_feature;

    // Feature density: more creases than a simple brick (12 edges) → mild refine.
    double density_scale = 1.0;
    if (out.n_sharp_edges > 24) {
        density_scale = 0.85;
    }
    if (out.n_sharp_edges > 80) {
        density_scale = 0.7;
    }
    if (out.n_sharp_edges > 200) {
        density_scale = 0.55;
    }

    double h0 = h_geom * density_scale;
    // Aim for ≥ ~2 elements across the shortest crease when that is smaller
    // than the geometric default — but never below h_geom/4 (CI runtime).
    if (min_feature > 0.0) {
        const double h_feat = 0.5 * min_feature;
        if (h_feat < h0) {
            h0 = std::max(h_feat, h_geom * 0.25);
        }
    }

    // Absolute clamps from diagonal so pathological geometry stays meshable.
    if (diagonal > 0.0) {
        h0 = std::clamp(h0, diagonal / 200.0, diagonal / 6.0);
    }
    out.h = h0;
    out.auto_chosen = true;
    out.note = std::format(
        "auto h={:.4g} m (extent/16∩diag/28, n_sharp={}, min_feat={:.3g} m, dens×{:.2f})",
        out.h, out.n_sharp_edges, out.min_feature_length, density_scale);
    return out;
}

VolumeMeshOutput volume_mesh(const Model& model, double h, VolumeMesher mesher,
                             int skin_layers, bool feature_refine,
                             std::span<const Eigen::Vector3d> refine_seeds, double seed_band) {
    VolumeMeshOutput out;
    double fill_h = h;
    if (mesher == VolumeMesher::kHexFill || mesher == VolumeMesher::kHexVem) {
        auto fill = mesh::hex_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.hexes.size());
        for (const auto& hx : fill.hexes) {
            fea::NodalElement el{fea::ElementType::kHex8,
                                 {hx[0], hx[1], hx[2], hx[3], hx[4], hx[5], hx[6], hx[7]}};
            if (mesher == VolumeMesher::kHexVem) {
                auto poly = fea::hex8_as_poly(el);
                el.type = fea::ElementType::kPolyVem;
                el.faces = std::move(poly.faces);
            }
            out.mesh.elements.push_back(std::move(el));
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format("{} grid fill v1: {} cells, {} nodes, h={:.4g} m",
                                      mesher == VolumeMesher::kHexVem ? "hex-VEM" : "hex",
                                      out.mesh.elements.size(), out.mesh.nodes.size(), fill_h);
    } else if (mesher == VolumeMesher::kHexPyramid) {
        // Topology: hex core + pyramid skin (ADR-0013). Product FE path expands
        // each interior hex to six pyramids (centroid apex) so face diagonals
        // match the tet-split pyramid skin — constant-strain patch exact.
        auto raw =
            mesh::transition_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_skin = raw.n_pyramid;
        auto fill = mesh::expand_hex_core_to_pyramids(raw);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPyramid5,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "hex+pyramid product FE (Cartesian grid, not Delaunay; all-pyramid expand): "
            "{} lattice hex → {} pyramids ({} skin + {} from hex), {} nodes, h={:.4g} m, "
            "boundary max|d|={:.3g} m",
            n_hex_lattice, fill.n_pyramid, n_pyr_skin, fill.n_pyramid - n_pyr_skin,
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kGradedTet) {
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                feature_band = 2.0 * h; // one coarse cell radius around creases
            }
        }
        auto graded = mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, refine_seeds, seed_band);
        fill_h = graded.h_fine;
        out.mesh.nodes = std::move(graded.mesh.nodes);
        out.mesh.elements.reserve(graded.mesh.tets.size());
        for (const auto& tet : graded.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(graded.mesh.boundary_quads);
        std::vector<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(bnodes.end(), q.begin(), q.end());
        }
        std::sort(bnodes.begin(), bnodes.end());
        bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, bnodes);
        out.mesher_note = std::format(
            "graded tet v1: {} tets ({} coarse, {} fine, {} feature, {} seed blocks), "
            "h={:.4g}/{:.4g} m, boundary max|d|={:.3g} m mean|d|={:.3g} m",
            out.mesh.elements.size(), graded.n_coarse_cells, graded.n_fine_cells,
            graded.n_feature_cells, graded.n_seed_cells, graded.h_coarse, graded.h_fine,
            conf.max_distance, conf.mean_distance);
    } else {
        auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.tets.size());
        for (const auto& tet : fill.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        std::vector<std::array<std::uint32_t, 4>> tet_ids;
        tet_ids.reserve(out.mesh.elements.size());
        for (const auto& el : out.mesh.elements) {
            if (el.nodes.size() == 4) {
                tet_ids.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
            }
        }
        const auto q = mesh::summarize_tet4_quality(out.mesh.nodes, tet_ids);
        out.mesher_note = std::format(
            "tet grid fill v1 (Cartesian, not Delaunay): {} tet4, {} nodes, h={:.4g} m, "
            "minQ={:.3f}, meanQ={:.3f}, slivers={}",
            out.mesh.elements.size(), out.mesh.nodes.size(), fill_h, q.min_aspect,
            q.mean_aspect, q.n_sliver);
    }

    const auto& surf = model.surface;
    std::set<std::uint32_t> boundary_nodes;
    for (const auto& quad : out.boundary_quads) {
        boundary_nodes.insert(quad.begin(), quad.end());
    }
    for (const auto node : boundary_nodes) {
        const auto& pt = out.mesh.nodes[node];
        double best = std::numeric_limits<double>::max();
        int best_region = -1;
        for (std::size_t ti = 0; ti < surf.triangles.size(); ++ti) {
            const auto& tri = surf.triangles[ti];
            const double d = point_triangle_distance(
                pt, surf.vertices[tri[0]], surf.vertices[tri[1]], surf.vertices[tri[2]]);
            if (d < best) {
                best = d;
                best_region = model.triangle_region[ti];
            }
        }
        if (best <= 1.5 * fill_h) {
            out.boundary_node_region[node] = best_region;
        }
    }
    out.mesh.check_validity();
    return out;
}

VolumeMeshOutput voxel_mesh(const Model& model, double h) {
    return volume_mesh(model, h, VolumeMesher::kTetFill, 2);
}

void SolveJob::set_status(const std::string& s) {
    const std::lock_guard lock(status_mutex_);
    status_ = s;
}

std::string SolveJob::status_text() const {
    const std::lock_guard lock(status_mutex_);
    return status_;
}

void SolveJob::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SolveJob::clear_failure() {
    if (state_ == State::kFailed) {
        join_worker();
        state_ = State::kIdle;
        set_status("idle");
    }
}

void SolveJob::start_mesh(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    state_ = State::kMeshing;
    set_status("meshing…");
    worker_ = std::thread([this, model, setup] {
        try {
            const auto resolved = resolve_mesh_size(model, setup.mesh_size);
            const double h = resolved.h;
            double h_use = h;
            if (setup.use_feature_grading) {
                // C2: curvature + thin-wall + sharp edges (geometry sizing).
                const auto edges = geom::detect_sharp_edges(model.surface, 30.0);
                const auto field =
                    adapt::make_geometry_sizing(h * 0.5, h, 2.0 * h, model.surface, edges);
                h_use =
                    std::min(h_use, field->size_at(0.5 * (model.bbox_min + model.bbox_max)));
                for (int mask = 0; mask < 8; ++mask) {
                    Eigen::Vector3d c;
                    c[0] = (mask & 1) ? model.bbox_max[0] : model.bbox_min[0];
                    c[1] = (mask & 2) ? model.bbox_max[1] : model.bbox_min[1];
                    c[2] = (mask & 4) ? model.bbox_max[2] : model.bbox_min[2];
                    h_use = std::min(h_use, field->size_at(c));
                }
            }
            mesh_only_ = volume_mesh(model, h_use, setup.mesher, setup.skin_layers,
                                     setup.use_feature_grading);
            // Prefix auto/user h note so GUI mesh_note shows resolved size (D5).
            mesh_only_.mesher_note =
                std::format("{} | {}", resolved.note, mesh_only_.mesher_note);
            set_status(std::format("mesh ready — {} elems, {} nodes | {}",
                                   mesh_only_.mesh.elements.size(),
                                   mesh_only_.mesh.nodes.size(), mesh_only_.mesher_note));
            state_ = State::kMeshDone;
        } catch (const std::exception& e) {
            set_status(std::format("mesh failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

namespace {
void fill_result_fields(SolveResult& r, const fea::ZzRecovery& zz, const Eigen::VectorXd& u) {
    r.displacement = u;
    r.global_eta = zz.global_eta;
    r.element_eta = zz.element_eta;
    const auto n_nodes = r.volume_mesh.nodes.size();
    r.nodal_eta.assign(n_nodes, 0.0);
    std::vector<int> counts(n_nodes, 0);
    for (std::size_t e = 0; e < r.volume_mesh.elements.size() && e < zz.element_eta.size();
         ++e) {
        for (auto n : r.volume_mesh.elements[e].nodes) {
            r.nodal_eta[n] += zz.element_eta[e];
            ++counts[n];
        }
    }
    r.max_nodal_eta = 0.0;
    for (std::size_t i = 0; i < n_nodes; ++i) {
        if (counts[i] > 0) {
            r.nodal_eta[i] /= static_cast<double>(counts[i]);
        }
        r.max_nodal_eta = std::max(r.max_nodal_eta, r.nodal_eta[i]);
    }
    const auto& stress = zz.nodal_stress;
    r.von_mises.resize(stress.size());
    r.u_magnitude.resize(stress.size());
    r.max_von_mises = 0.0;
    r.max_displacement = 0.0;
    for (std::size_t i = 0; i < stress.size(); ++i) {
        r.von_mises[i] = fea::von_mises(stress[i]);
        r.u_magnitude[i] = u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
        r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
        r.max_displacement = std::max(r.max_displacement, r.u_magnitude[i]);
    }
}
} // namespace

void SolveJob::start(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    state_ = State::kMeshing;
    set_status("meshing…");
    // Copy inputs by value into the worker.
    worker_ = std::thread([this, model, setup] {
        try {
            const auto resolved = resolve_mesh_size(model, setup.mesh_size);
            const double h = resolved.h;
            double h_use = h;
            if (setup.use_feature_grading) {
                // A priori: geometry sizing (sharp edges + curvature + thin-wall)
                // tightens global h so skin/transition resolve creases, fillets,
                // and thin regions (meshers still take a scalar h).
                const auto edges = geom::detect_sharp_edges(model.surface, 30.0);
                const auto field =
                    adapt::make_geometry_sizing(h * 0.5, h, 2.0 * h, model.surface, edges);
                const Eigen::Vector3d mid = 0.5 * (model.bbox_min + model.bbox_max);
                h_use = std::min(h_use, field->size_at(mid));
                for (int mask = 0; mask < 8; ++mask) {
                    Eigen::Vector3d c;
                    c[0] = (mask & 1) ? model.bbox_max[0] : model.bbox_min[0];
                    c[1] = (mask & 2) ? model.bbox_max[1] : model.bbox_min[1];
                    c[2] = (mask & 4) ? model.bbox_max[2] : model.bbox_min[2];
                    h_use = std::min(h_use, field->size_at(c));
                }
            }
            std::vector<Eigen::Vector3d> adapt_seeds;
            double adapt_seed_band = 0.0;
            // D4: Dörfler element indices for optional local LEB before remesh.
            std::vector<std::size_t> adapt_marked;
            auto vol = volume_mesh(model, h_use, setup.mesher, setup.skin_layers,
                                   setup.use_feature_grading, adapt_seeds, adapt_seed_band);
            // Keep resolved-h note on mesher_note for solve mesh_note (GUI/CLI).
            vol.mesher_note = std::format("{} | {}", resolved.note, vol.mesher_note);
            set_status(std::format("solving… ({} elements, {} nodes)",
                                   vol.mesh.elements.size(), vol.mesh.nodes.size()));
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
            Eigen::VectorXd loads;
            const fea::Material material{.youngs_modulus = setup.youngs_modulus,
                                         .poissons_ratio = setup.poissons_ratio};

            auto assign_boundary_regions = [&](double band) {
                vol.boundary_node_region.clear();
                const auto& surf = model.surface;
                for (std::uint32_t node = 0;
                     node < static_cast<std::uint32_t>(vol.mesh.nodes.size()); ++node) {
                    const auto& pt = vol.mesh.nodes[node];
                    double best = std::numeric_limits<double>::max();
                    int best_region = -1;
                    for (std::size_t ti = 0; ti < surf.triangles.size(); ++ti) {
                        const auto& tri = surf.triangles[ti];
                        const double d = point_triangle_distance(pt, surf.vertices[tri[0]],
                                                                 surf.vertices[tri[1]],
                                                                 surf.vertices[tri[2]]);
                        if (d < best) {
                            best = d;
                            best_region = model.triangle_region[ti];
                        }
                    }
                    if (best <= band) {
                        vol.boundary_node_region[node] = best_region;
                    }
                }
            };

            auto apply_bcs = [&]() {
                bc = fea::Dirichlet{};
                region_nodes.clear();
                for (const auto& [node, region] : vol.boundary_node_region) {
                    region_nodes[region].push_back(node);
                }
                for (const int region : setup.fixtures) {
                    if (const auto it = region_nodes.find(region); it != region_nodes.end()) {
                        for (const auto node : it->second) {
                            bc.fix_node(node);
                        }
                    }
                }
                if (bc.dof_values.empty()) {
                    throw fea::FeaError("no fixtures: fix at least one face before solving");
                }
                loads = Eigen::VectorXd::Zero(
                    3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
                for (const auto& [region, load] : setup.loads) {
                    const auto it = region_nodes.find(region);
                    if (it == region_nodes.end() || it->second.empty()) {
                        continue;
                    }
                    const Eigen::Vector3d per_node =
                        load.force / static_cast<double>(it->second.size());
                    for (const auto node : it->second) {
                        loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
                    }
                }
            };
            apply_bcs();

            auto element_centroids = [&](const fea::NodalMesh& m) {
                std::vector<Eigen::Vector3d> cents;
                cents.reserve(m.elements.size());
                for (const auto& el : m.elements) {
                    Eigen::Vector3d c = Eigen::Vector3d::Zero();
                    for (auto n : el.nodes) {
                        c += m.nodes[n];
                    }
                    cents.push_back(c / static_cast<double>(el.nodes.size()));
                }
                return cents;
            };

            // D3: p-elevate smooth linear elems after last h-pass (or single solve).
            // Explicit flag or auto when adapt_passes > 0.
            const bool do_p_elevate = setup.p_elevate || setup.adapt_passes > 0;

            // After mid-edge insertion, assign boundary regions to new nodes that
            // sit between two existing boundary nodes of the same region so
            // fixtures/loads still cover face mid-edge DOFs.
            auto extend_boundary_regions = [&]() {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edge_mid;
                for (const auto& el : vol.mesh.elements) {
                    if (el.type != fea::ElementType::kTet10 &&
                        el.type != fea::ElementType::kHex20) {
                        continue;
                    }
                    const int n_corner = (el.type == fea::ElementType::kTet10) ? 4 : 8;
                    const int n_mid = (el.type == fea::ElementType::kTet10) ? 6 : 12;
                    // Canonical edge order matches p_elevate mid-edge append order.
                    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
                        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
                    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
                                                                                   {1, 2},
                                                                                   {2, 3},
                                                                                   {3, 0},
                                                                                   {4, 5},
                                                                                   {5, 6},
                                                                                   {6, 7},
                                                                                   {7, 4},
                                                                                   {0, 4},
                                                                                   {1, 5},
                                                                                   {2, 6},
                                                                                   {3, 7}}};
                    for (int m = 0; m < n_mid; ++m) {
                        const auto& epair = (el.type == fea::ElementType::kTet10)
                                                ? kTetEdges[static_cast<std::size_t>(m)]
                                                : kHexEdges[static_cast<std::size_t>(m)];
                        const auto a = el.nodes[static_cast<std::size_t>(epair[0])];
                        const auto b = el.nodes[static_cast<std::size_t>(epair[1])];
                        const auto mid = el.nodes[static_cast<std::size_t>(n_corner + m)];
                        edge_mid[std::minmax(a, b)] = mid;
                    }
                }
                for (const auto& [ab, mid] : edge_mid) {
                    if (vol.boundary_node_region.contains(mid)) {
                        continue;
                    }
                    const auto ita = vol.boundary_node_region.find(ab.first);
                    const auto itb = vol.boundary_node_region.find(ab.second);
                    if (ita != vol.boundary_node_region.end() &&
                        itb != vol.boundary_node_region.end() && ita->second == itb->second) {
                        vol.boundary_node_region[mid] = ita->second;
                    }
                }
            };

            auto maybe_p_elevate = [&](const std::vector<double>& element_eta,
                                       std::string& note_suffix) {
                if (!do_p_elevate || element_eta.empty()) {
                    return false;
                }
                const auto smooth = adapt::mark_smooth(element_eta, 0.3);
                if (smooth.empty()) {
                    note_suffix = " p-elev=0";
                    return false;
                }
                const auto before = vol.mesh.nodes.size();
                vol.mesh = fea::p_elevate(vol.mesh, smooth);
                extend_boundary_regions();
                apply_bcs();
                const auto counts = fea::count_element_types(vol.mesh);
                note_suffix =
                    std::format(" p-elev={} n+{} (tet10={} hex20={})", smooth.size(),
                                vol.mesh.nodes.size() - before, counts.tet10, counts.hex20);
                set_status(std::format("p-elevate… ({} smooth → quadratic)", smooth.size()));
                return true;
            };

            auto mesh_is_all_tet4 = [](const fea::NodalMesh& m) {
                if (m.elements.empty()) {
                    return false;
                }
                for (const auto& el : m.elements) {
                    if (el.type != fea::ElementType::kTet4 || el.nodes.size() != 4) {
                        return false;
                    }
                }
                return true;
            };

            /// ADR-0016: try Rivara LEB on marked tets; returns true if mesh grew.
            auto try_local_leb = [&](std::span<const std::size_t> marks) -> bool {
                if (marks.empty() || !mesh_is_all_tet4(vol.mesh)) {
                    return false;
                }
                std::vector<std::array<std::uint32_t, 4>> tets;
                tets.reserve(vol.mesh.elements.size());
                for (const auto& el : vol.mesh.elements) {
                    tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
                }
                const std::size_t n0 = tets.size();
                try {
                    mesh::LocalRefineStats st;
                    auto refined =
                        mesh::local_refine_tets(vol.mesh.nodes, std::move(tets), marks, &st);
                    if (refined.tets.size() <= n0) {
                        return false;
                    }
                    vol.mesh.nodes = std::move(refined.nodes);
                    vol.mesh.elements.clear();
                    vol.mesh.elements.reserve(refined.tets.size());
                    for (const auto& tet : refined.tets) {
                        vol.mesh.elements.push_back(fea::NodalElement{
                            fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
                    }
                    vol.mesh.check_validity();
                    // Lattice boundary quads no longer match midpoints; drop them.
                    vol.boundary_quads.clear();
                    assign_boundary_regions(1.5 * h_use);
                    vol.mesher_note = std::format(
                        "{} | local LEB (ADR-0016): +{} tets, +{} nodes, {} bisections",
                        vol.mesher_note, refined.tets.size() - n0, st.n_new_nodes,
                        st.n_bisections);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            };

            for (int pass = 0; pass <= setup.adapt_passes; ++pass) {
                if (pass > 0) {
                    // D4: prefer true local LEB on tet meshes when marks exist.
                    const bool tet_path = setup.mesher == VolumeMesher::kTetFill ||
                                          setup.mesher == VolumeMesher::kGradedTet;
                    bool did_local = false;
                    if (tet_path && !adapt_marked.empty()) {
                        did_local = try_local_leb(adapt_marked);
                    }
                    if (!did_local) {
                        // Prefer graded mesher when local seeds are available so
                        // a posteriori balls can refine without global h→0.
                        const auto mesher_adapt =
                            (!adapt_seeds.empty() && setup.mesher == VolumeMesher::kTetFill)
                                ? VolumeMesher::kGradedTet
                                : setup.mesher;
                        vol = volume_mesh(model, h_use, mesher_adapt, setup.skin_layers,
                                          setup.use_feature_grading, adapt_seeds,
                                          adapt_seed_band);
                    }
                    apply_bcs();
                    set_status(std::format("adapt pass {}… ({} elems, {} seeds{})", pass,
                                           vol.mesh.elements.size(), adapt_seeds.size(),
                                           did_local ? ", local LEB" : ""));
                }
                auto u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads);
                auto zz_try = fea::recover_zz(vol.mesh, material, u_try);
                // D2: global η target — stop when η is small enough (0 = disabled).
                if (setup.eta_target > 0.0 && zz_try.global_eta <= setup.eta_target) {
                    std::string pnote;
                    if (maybe_p_elevate(zz_try.element_eta, pnote)) {
                        u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads);
                        zz_try = fea::recover_zz(vol.mesh, material, u_try);
                    }
                    SolveResult r;
                    r.mesh_note = std::format(
                        "{} | eta-target stop η={:.4g}≤{:.4g} pass={}/{} h={:.4g}{}",
                        vol.mesher_note, zz_try.global_eta, setup.eta_target, pass,
                        setup.adapt_passes, h_use, pnote);
                    r.volume_mesh = std::move(vol.mesh);
                    r.boundary_quads = std::move(vol.boundary_quads);
                    fill_result_fields(r, zz_try, u_try);
                    result_ = std::move(r);
                    break;
                }
                if (pass < setup.adapt_passes) {
                    const auto cents = element_centroids(vol.mesh);
                    const auto sug = adapt::suggest_refine(cents, zz_try.element_eta, h_use,
                                                           0.3, 0.75, h * 0.35);
                    if (sug.n_marked == 0 && sug.h_next >= h_use * 0.98) {
                        std::string pnote;
                        if (maybe_p_elevate(zz_try.element_eta, pnote)) {
                            u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads);
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                        }
                        SolveResult r;
                        r.mesh_note = std::format("{} | adapt early-stop h={:.4g}{}",
                                                  vol.mesher_note, h_use, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        fill_result_fields(r, zz_try, u_try);
                        result_ = std::move(r);
                        break;
                    }
                    h_use = sug.h_next;
                    adapt_seeds = sug.refine_seeds;
                    adapt_seed_band = sug.seed_band;
                    adapt_marked = adapt::dorfler_mark(zz_try.element_eta, 0.3);
                    continue;
                }
                std::string pnote;
                if (maybe_p_elevate(zz_try.element_eta, pnote)) {
                    u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads);
                    zz_try = fea::recover_zz(vol.mesh, material, u_try);
                }
                SolveResult r;
                r.mesh_note =
                    std::format("{} | adapt_passes={} h={:.4g} seeds={}{}", vol.mesher_note,
                                setup.adapt_passes, h_use, adapt_seeds.size(), pnote);
                r.volume_mesh = std::move(vol.mesh);
                r.boundary_quads = std::move(vol.boundary_quads);
                fill_result_fields(r, zz_try, u_try);
                result_ = std::move(r);
            } // adapt passes
            set_status(std::format("done — max von Mises {:.4g} MPa, max deflection {:.4g} mm",
                                   result_.max_von_mises / 1e6,
                                   result_.max_displacement * 1e3));
            state_ = State::kDone;
        } catch (const std::exception& e) {
            set_status(std::format("solve failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

std::optional<SolveResult> SolveJob::take_result() {
    if (state_ != State::kDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(result_);
}

std::optional<VolumeMeshOutput> SolveJob::take_mesh() {
    if (state_ != State::kMeshDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(mesh_only_);
}

SolveJob::~SolveJob() { join_worker(); }

} // namespace polymesh::pipeline
