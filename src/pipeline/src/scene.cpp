// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "fea/solve.hpp"
#include "geom/stl.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <queue>

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

VoxelMeshOutput voxel_mesh(const Model& model, double h) {
    const Eigen::Vector3d extent = model.bbox_max - model.bbox_min;
    const auto cells = [&](int axis) {
        return std::max(1, static_cast<int>(std::ceil(extent[axis] / h)));
    };
    const int nx = cells(0), ny = cells(1), nz = cells(2);
    if (static_cast<long>(nx) * ny * nz > 512 * 1024) {
        throw fea::FeaError("voxel_mesh: mesh size too small for the draft voxel "
                            "mesher; increase element size");
    }
    const Eigen::Vector3d origin = model.bbox_min;
    const auto& surf = model.surface;

    // Column-parity voxelization: one +z ray per (i,j) column of centers.
    std::vector<bool> inside(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) *
                                 static_cast<std::size_t>(nz),
                             false);
    const auto cell_index = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    };
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double cx = origin[0] + (i + 0.5) * h;
            const double cy = origin[1] + (j + 0.5) * h;
            std::vector<double> crossings;
            for (const auto& tri : surf.triangles) {
                // Ray (cx, cy, -inf) -> +z against the triangle (2D test in xy).
                const Eigen::Vector3d& a = surf.vertices[tri[0]];
                const Eigen::Vector3d& b = surf.vertices[tri[1]];
                const Eigen::Vector3d& c = surf.vertices[tri[2]];
                const double d1 = (b[0] - a[0]) * (cy - a[1]) - (b[1] - a[1]) * (cx - a[0]);
                const double d2 = (c[0] - b[0]) * (cy - b[1]) - (c[1] - b[1]) * (cx - b[0]);
                const double d3 = (a[0] - c[0]) * (cy - c[1]) - (a[1] - c[1]) * (cx - c[0]);
                const bool has_neg = d1 < 0 || d2 < 0 || d3 < 0;
                const bool has_pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (has_neg && has_pos) {
                    continue; // outside in 2D
                }
                // Interpolate z at (cx, cy) via barycentric coordinates.
                const double area = d1 + d2 + d3;
                if (area == 0.0) {
                    continue; // degenerate in projection
                }
                const double z = (d2 * a[2] + d3 * b[2] + d1 * c[2]) / area;
                crossings.push_back(z);
            }
            std::sort(crossings.begin(), crossings.end());
            for (int k = 0; k < nz; ++k) {
                const double cz = origin[2] + (k + 0.5) * h;
                const auto below = std::upper_bound(crossings.begin(), crossings.end(), cz) -
                                   crossings.begin();
                if (below % 2 == 1) {
                    inside[cell_index(i, j, k)] = true;
                }
            }
        }
    }

    // Emit shared lattice nodes for inside cells.
    VoxelMeshOutput out;
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.mesh.nodes.size()));
        if (fresh) {
            out.mesh.nodes.emplace_back(origin[0] + i * h, origin[1] + j * h,
                                        origin[2] + k * h);
        }
        return it->second;
    };
    const auto is_inside = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[cell_index(i, j, k)];
    };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[cell_index(i, j, k)]) {
                    continue;
                }
                out.mesh.elements.push_back(
                    {fea::ElementType::kHex8,
                     {node_at(i, j, k), node_at(i + 1, j, k), node_at(i + 1, j + 1, k),
                      node_at(i, j + 1, k), node_at(i, j, k + 1), node_at(i + 1, j, k + 1),
                      node_at(i + 1, j + 1, k + 1), node_at(i, j + 1, k + 1)}});
                // Boundary quads: faces whose neighbor cell is outside.
                struct FaceDef {
                    int di, dj, dk;
                    std::array<std::array<int, 3>, 4> corners;
                };
                const std::array<FaceDef, 6> faces{{
                    {-1, 0, 0, {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}}},
                    {1, 0, 0, {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}}},
                    {0, -1, 0, {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}}},
                    {0, 1, 0, {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}}},
                    {0, 0, -1, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}},
                    {0, 0, 1, {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}}},
                }};
                for (const auto& f : faces) {
                    if (is_inside(i + f.di, j + f.dj, k + f.dk)) {
                        continue;
                    }
                    std::array<std::uint32_t, 4> quad{};
                    for (int q = 0; q < 4; ++q) {
                        const auto& c = f.corners[static_cast<std::size_t>(q)];
                        quad[static_cast<std::size_t>(q)] =
                            node_at(i + c[0], j + c[1], k + c[2]);
                    }
                    out.boundary_quads.push_back(quad);
                }
            }
        }
    }

    // Map boundary nodes to STL regions via nearest triangle (brute force —
    // fine at draft-mesher sizes).
    std::set<std::uint32_t> boundary_nodes;
    for (const auto& quad : out.boundary_quads) {
        boundary_nodes.insert(quad.begin(), quad.end());
    }
    for (const auto node : boundary_nodes) {
        const auto& p = out.mesh.nodes[node];
        double best = std::numeric_limits<double>::max();
        int best_region = -1;
        for (std::size_t t = 0; t < surf.triangles.size(); ++t) {
            const auto& tri = surf.triangles[t];
            const double d = point_triangle_distance(
                p, surf.vertices[tri[0]], surf.vertices[tri[1]], surf.vertices[tri[2]]);
            if (d < best) {
                best = d;
                best_region = model.triangle_region[t];
            }
        }
        if (best <= 1.5 * h) {
            out.boundary_node_region[node] = best_region;
        }
    }
    return out;
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
            auto voxel = voxel_mesh(model, h);
            voxel.mesh.check_validity();
            set_status(std::format("solving… ({} hex8 elements, {} nodes)",
                                   voxel.mesh.elements.size(), voxel.mesh.nodes.size()));
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
            for (const auto& [node, region] : voxel.boundary_node_region) {
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
                Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(voxel.mesh.nodes.size()));
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
            const auto u = fea::solve_elastostatics(voxel.mesh, material, bc, loads);
            const auto stress = fea::recover_nodal_stress(voxel.mesh, material, u);

            SolveResult r;
            r.mesh_note = std::format("draft voxel mesh v0: {} hex8, {} nodes, h = {:.4g} m",
                                      voxel.mesh.elements.size(), voxel.mesh.nodes.size(), h);
            r.volume_mesh = std::move(voxel.mesh);
            r.boundary_quads = std::move(voxel.boundary_quads);
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
