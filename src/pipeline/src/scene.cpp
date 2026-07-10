// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "adapt/error.hpp"
#include "adapt/loop.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/features.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "mesh/hex_fill.hpp"
#include "mesh/hybrid_fill.hpp"
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

VolumeMeshOutput volume_mesh(const Model& model, double h, VolumeMesher mesher,
                             int skin_layers, bool feature_refine) {
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
        // Hex core + pyramid skin as native element types (ADR-0013). GATE-1
        // hex is isoparametric; pyramid is tet-split. Hybrid constant-strain
        // patch is not exact across hex–pyramid faces; pure-hex and pure-
        // pyramid patches are.
        auto fill =
            mesh::transition_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            if (cell.kind == mesh::TransitionCellKind::kHex8) {
                out.mesh.elements.push_back(fea::NodalElement{
                    fea::ElementType::kHex8,
                    {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4],
                     cell.nodes[5], cell.nodes[6], cell.nodes[7]}});
            } else {
                out.mesh.elements.push_back(
                    fea::NodalElement{fea::ElementType::kPyramid5,
                                      {cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                       cell.nodes[3], cell.nodes[4]}});
            }
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "hex+pyramid transition v1: {} hex, {} pyramids, {} nodes, h={:.4g} m, "
            "boundary max|d|={:.3g} m",
            fill.n_hex, fill.n_pyramid, out.mesh.nodes.size(), fill_h,
            fill.boundary_max_distance);
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
            feature_band);
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
            "graded tet v1: {} tets ({} coarse blocks, {} fine cells, {} feature blocks), "
            "h={:.4g}/{:.4g} m, boundary max|d|={:.3g} m mean|d|={:.3g} m",
            out.mesh.elements.size(), graded.n_coarse_cells, graded.n_fine_cells,
            graded.n_feature_cells, graded.h_coarse, graded.h_fine, conf.max_distance,
            conf.mean_distance);
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
        out.mesher_note = std::format("tet grid fill v1: {} tet4, {} nodes, h={:.4g} m, "
                                      "minQ={:.3f}, meanQ={:.3f}, slivers={}",
                                      out.mesh.elements.size(), out.mesh.nodes.size(), fill_h,
                                      q.min_aspect, q.mean_aspect, q.n_sliver);
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

void SolveJob::start(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    state_ = State::kMeshing;
    set_status("meshing…");
    // Copy inputs by value into the worker.
    worker_ = std::thread([this, model, setup] {
        try {
            const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
            const double h = setup.mesh_size > 0.0 ? setup.mesh_size : extent / 24.0;
            double h_use = h;
            if (setup.use_feature_grading) {
                const auto edges = geom::detect_sharp_edges(model.surface, 30.0);
                if (!edges.empty()) {
                    // A priori: tighten global h toward feature min size so the
                    // skin / transition layers resolve creases (FeatureSizing
                    // available for local queries; meshers still take scalar h).
                    const auto field =
                        adapt::make_feature_sizing(h * 0.5, h, 2.0 * h, model.surface, edges);
                    const Eigen::Vector3d mid =
                        0.5 * (model.bbox_min + model.bbox_max);
                    h_use = std::min(h_use, field->size_at(mid));
                    // Sample bbox corners (feature-rich on CAD boxes).
                    for (int mask = 0; mask < 8; ++mask) {
                        Eigen::Vector3d c;
                        c[0] = (mask & 1) ? model.bbox_max[0] : model.bbox_min[0];
                        c[1] = (mask & 2) ? model.bbox_max[1] : model.bbox_min[1];
                        c[2] = (mask & 4) ? model.bbox_max[2] : model.bbox_min[2];
                        h_use = std::min(h_use, field->size_at(c));
                    }
                }
            }
            auto vol = volume_mesh(model, h_use, setup.mesher, setup.skin_layers,
                                   setup.use_feature_grading);
            set_status(std::format("solving… ({} elements, {} nodes)",
                                   vol.mesh.elements.size(), vol.mesh.nodes.size()));
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
            Eigen::VectorXd loads;
            const fea::Material material{.youngs_modulus = setup.youngs_modulus,
                                         .poissons_ratio = setup.poissons_ratio};

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

            for (int pass = 0; pass <= setup.adapt_passes; ++pass) {
                if (pass > 0) {
                    vol = volume_mesh(model, h_use, setup.mesher, setup.skin_layers,
                                      setup.use_feature_grading);
                    apply_bcs();
                    set_status(std::format("adapt pass {}… ({} elems)", pass,
                                           vol.mesh.elements.size()));
                }
                const auto u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads);
                const auto zz_try = fea::recover_zz(vol.mesh, material, u_try);
                if (pass < setup.adapt_passes) {
                    const auto sug = adapt::suggest_uniform_refine(zz_try.element_eta, h_use,
                                                                   0.3, 0.75, h * 0.35);
                    if (sug.h_next >= h_use * 0.98) {
                        // No further refinement — accept this solve as final.
                        const auto& stress = zz_try.nodal_stress;
                        SolveResult r;
                        r.mesh_note = std::format("{} | adapt early-stop h={:.4g}",
                                                  vol.mesher_note, h_use);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        r.displacement = u_try;
                        r.global_eta = zz_try.global_eta;
                        r.von_mises.resize(stress.size());
                        r.u_magnitude.resize(stress.size());
                        for (std::size_t i = 0; i < stress.size(); ++i) {
                            r.von_mises[i] = fea::von_mises(stress[i]);
                            r.u_magnitude[i] =
                                u_try.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
                            r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
                            r.max_displacement =
                                std::max(r.max_displacement, r.u_magnitude[i]);
                        }
                        result_ = std::move(r);
                        break;
                    }
                    h_use = sug.h_next;
                    continue;
                }
                // Final pass results.
                const auto& stress = zz_try.nodal_stress;
                SolveResult r;
                r.mesh_note = std::format("{} | adapt_passes={} h={:.4g}", vol.mesher_note,
                                          setup.adapt_passes, h_use);
                r.volume_mesh = std::move(vol.mesh);
                r.boundary_quads = std::move(vol.boundary_quads);
                r.displacement = u_try;
                r.global_eta = zz_try.global_eta;
                r.von_mises.resize(stress.size());
                r.u_magnitude.resize(stress.size());
                for (std::size_t i = 0; i < stress.size(); ++i) {
                    r.von_mises[i] = fea::von_mises(stress[i]);
                    r.u_magnitude[i] =
                        u_try.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
                    r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
                    r.max_displacement = std::max(r.max_displacement, r.u_magnitude[i]);
                }
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
    if (worker_.joinable()) {
        worker_.join();
    }
    state_ = State::kIdle;
    return std::move(result_);
}

SolveJob::~SolveJob() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace polymesh::pipeline
