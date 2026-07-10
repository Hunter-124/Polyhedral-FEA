// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "fea/solve.hpp"
#include "geom/stl.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <queue>
#include <set>

namespace polymesh::pipeline {
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
    model.surface = geom::load_stl(path);
    model.surface.validate();
    model.name = path.substr(path.find_last_of('/') + 1);

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

VolumeMeshOutput volume_mesh(const Model& model, double h) {
    auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);

    VolumeMeshOutput out;
    out.mesh.nodes = std::move(fill.nodes);
    out.mesh.elements.reserve(fill.tets.size());
    for (const auto& tet : fill.tets) {
        out.mesh.elements.push_back(
            {fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
    }
    out.boundary_quads = std::move(fill.boundary_quads);
    out.mesher_note = std::format("tet grid fill v1: {} tet4, {} nodes, h = {:.4g} m",
                                  out.mesh.elements.size(), out.mesh.nodes.size(), fill.h);

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
        if (best <= 1.5 * h) {
            out.boundary_node_region[node] = best_region;
        }
    }
    out.mesh.check_validity();
    return out;
}

VolumeMeshOutput voxel_mesh(const Model& model, double h) { return volume_mesh(model, h); }

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
            auto vol = volume_mesh(model, h);
            set_status(std::format("solving… ({} tet4 elements, {} nodes)",
                                   vol.mesh.elements.size(), vol.mesh.nodes.size()));
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
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

            Eigen::VectorXd loads =
                Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
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

            const fea::Material material{.youngs_modulus = setup.youngs_modulus,
                                         .poissons_ratio = setup.poissons_ratio};
            const auto u = fea::solve_elastostatics(vol.mesh, material, bc, loads);
            const auto stress = fea::recover_nodal_stress(vol.mesh, material, u);

            SolveResult r;
            r.mesh_note = vol.mesher_note;
            r.volume_mesh = std::move(vol.mesh);
            r.boundary_quads = std::move(vol.boundary_quads);
            r.displacement = u;
            r.von_mises.resize(stress.size());
            r.u_magnitude.resize(stress.size());
            for (std::size_t i = 0; i < stress.size(); ++i) {
                r.von_mises[i] = fea::von_mises(stress[i]);
                r.u_magnitude[i] = u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
                r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
                r.max_displacement = std::max(r.max_displacement, r.u_magnitude[i]);
            }
            result_ = std::move(r);
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
