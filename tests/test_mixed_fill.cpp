// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "geom/stl.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/surface_project.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Geometry>
#include "support/box_model.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

using namespace polymesh::mesh;
namespace pipeline = polymesh::pipeline;
namespace fea = polymesh::fea;

namespace {

polymesh::geom::TriSurface unit_box() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    s.triangles = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
        {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    return s;
}

} // namespace

TEST_CASE("mixed_fill unit box: hex bulk + pyramid skin") {
    const auto s = unit_box();
    auto fill = mixed_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.2, /*skin_layers=*/1);
    REQUIRE(fill.n_hex > 0);
    REQUIRE(fill.n_pyramid > 0);
    REQUIRE(fill.n_tet == 0);
    REQUIRE_FALSE(fill.boundary_quads.empty());
}

TEST_CASE("pipeline hybrid zoo emits all-pyramid product FE") {
    pipeline::Model m;
    m.surface = unit_box();
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipeline::volume_mesh(m, 0.2, pipeline::VolumeMesher::kHybrid, 1, false);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);

    std::size_t n_pyr = 0, n_other = 0;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == fea::ElementType::kPyramid5) {
            ++n_pyr;
        } else {
            ++n_other;
        }
    }
    REQUIRE(n_pyr > 0);
    REQUIRE(n_other == 0);
}

TEST_CASE("hybrid zoo expanded product path patch test: constant strain") {
    auto raw = mixed_fill_surface(unit_box(), {-0.05, -0.05, -0.05}, {1.05, 1.05, 1.05}, 0.2,
                                  /*skin_layers=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    REQUIRE(raw.n_hex > 0);
    REQUIRE(raw.n_pyramid > 0);
    auto fill = expand_mixed_hex_to_pyramids(raw);
    REQUIRE(fill.n_hex == 0);
    REQUIRE(fill.n_pyramid > 0);

    fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& cell : fill.cells) {
        REQUIRE(cell.kind == MixedCellKind::kPyramid5);
        mesh.elements.push_back(fea::NodalElement{
            fea::ElementType::kPyramid5,
            {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
    }

    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;

    std::set<std::uint32_t> bnodes;
    for (const auto& q : fill.boundary_quads) {
        bnodes.insert(q.begin(), q.end());
    }
    fea::Dirichlet bc;
    for (auto i : bnodes) {
        bc.fix_node(i, g * mesh.nodes[i]);
    }
    const fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto u = fea::solve_elastostatics(mesh, mat, bc, loads);

    double max_error = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d exact = g * mesh.nodes[i];
        const Eigen::Vector3d fem = u.segment<3>(3 * static_cast<Eigen::Index>(i));
        max_error = std::max(max_error, (fem - exact).norm());
    }
    REQUIRE(max_error < 1e-10);
}

TEST_CASE("hybrid zoo cylinder_prism smoke: pyramid FE + snap") {
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.12 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);

    bool has_pyr = false;
    for (const auto& el : vol.mesh.elements) {
        has_pyr = has_pyr || el.type == fea::ElementType::kPyramid5;
    }
    REQUIRE(has_pyr);
}

TEST_CASE("hybrid zoo 2:1 size adaptivity on feature band") {
    // Open box with a sharp crease band: feature grading must refine to h/2.
    auto model = polymesh::testsupport::model_from_surface(polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.15 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto plain = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                    /*skin=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    auto edges = polymesh::geom::detect_sharp_edges(model.surface, 30.0);
    REQUIRE_FALSE(edges.empty());
    auto graded = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                     /*skin=*/1, edges, 2.0 * h, {}, 0.0,
                                     /*snap=*/false);
    REQUIRE(graded.n_fine_cells > 0);
    REQUIRE(graded.n_transition_cells > 0);
    REQUIRE(graded.h_fine == Catch::Approx(0.5 * graded.h).margin(1e-9));
    // Fine path should produce more cells than plain (2×2×2 in feature bands).
    REQUIRE(graded.cells.size() > plain.cells.size());
}

namespace {

/// Faces of each FE cell type, as local node indices.
std::vector<std::vector<std::uint32_t>> cell_faces(const fea::NodalElement& el) {
    static const std::vector<std::vector<std::uint32_t>> kHex{
        {0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    static const std::vector<std::vector<std::uint32_t>> kPyr{
        {0, 1, 2, 3}, {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
    static const std::vector<std::vector<std::uint32_t>> kTet{
        {0, 1, 2}, {0, 1, 3}, {1, 2, 3}, {0, 2, 3}};
    const auto& local = el.type == fea::ElementType::kHex8    ? kHex
                        : el.type == fea::ElementType::kPyramid5 ? kPyr
                                                                 : kTet;
    std::vector<std::vector<std::uint32_t>> out;
    out.reserve(local.size());
    for (const auto& f : local) {
        std::vector<std::uint32_t> g;
        g.reserve(f.size());
        for (const auto i : f) {
            g.push_back(el.nodes[i]);
        }
        out.push_back(std::move(g));
    }
    return out;
}

} // namespace

// A hanging node is invisible to check_validity() and to the displacement
// solution (the assembly simply ignores the constraint), but it wrecks stress
// recovery and the ZZ estimator. The coarse end of the hybrid ladder is where
// the element-budget fallback rewrites the fine/transition sets, so pin the
// invariant there: every face is shared by at most two cells, and no node of
// the mesh lies strictly inside a face that only one cell owns.
TEST_CASE("hybrid zoo meshes are conforming across the budget-fallback ladder") {
    auto model = polymesh::testsupport::model_from_surface(
        polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    for (const double frac : {0.08, 0.10, 0.12, 0.16, 0.20}) {
        const double h = frac * extent;
        auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid, 2, true);
        REQUIRE_FALSE(vol.mesh.elements.empty());

        std::map<std::vector<std::uint32_t>, int> face_use;
        std::map<std::vector<std::uint32_t>, std::vector<std::uint32_t>> face_loop;
        std::vector<std::vector<std::uint32_t>> free_faces;
        for (const auto& el : vol.mesh.elements) {
            for (auto f : cell_faces(el)) {
                auto key = f;
                std::sort(key.begin(), key.end());
                ++face_use[key];
                face_loop.try_emplace(key, std::move(f));
            }
        }
        int max_face_use = 0;
        for (const auto& [key, uses] : face_use) {
            max_face_use = std::max(max_face_use, uses);
            if (frac != 0.08) {
                continue; // face-use hash is the sweep gate; deep T-junction sweep once
            }
            if (uses != 1) {
                continue;
            }
            free_faces.push_back(face_loop.at(key)); // preserve face winding
        }
        INFO("h/extent = " << frac << ", max face use " << max_face_use);
        REQUIRE(max_face_use <= 2);
        if (frac != 0.08) {
            continue;
        }
        // A real exterior face can of course have other surface nodes in its
        // geometric interior (a curved CAD facet after snapping). A hanging
        // 2:1 interface is different: it is a pair of unpaired faces in the
        // volume, at least one fine layer from the surface. Cache one closest-
        // surface query per unique free node, then keep only INTERNAL faces.
        std::set<std::uint32_t> free_nodes;
        for (const auto& f : free_faces) {
            free_nodes.insert(f.begin(), f.end());
        }
        std::map<std::uint32_t, double> surface_distance;
        for (const auto g : free_nodes) {
            surface_distance.emplace(g,
                                     closest_on_surface(model.surface, vol.mesh.nodes[g]).distance);
        }
        std::vector<std::vector<std::uint32_t>> internal_faces;
        for (const auto& f : free_faces) {
            double max_distance = 0.0;
            for (const auto g : f) {
                max_distance = std::max(max_distance, surface_distance.at(g));
            }
            if (max_distance > 0.10 * h) {
                internal_faces.push_back(f);
            }
        }
        free_faces = std::move(internal_faces);
        free_nodes.clear();
        for (const auto& f : free_faces) {
            free_nodes.insert(f.begin(), f.end());
        }
        const double bin = 0.5 * h;
        const auto bin_key = [&](const Eigen::Vector3d& p) {
            return std::array<int, 3>{{static_cast<int>(std::floor(p[0] / bin)),
                                       static_cast<int>(std::floor(p[1] / bin)),
                                       static_cast<int>(std::floor(p[2] / bin))}};
        };
        std::map<std::array<int, 3>, std::vector<std::uint32_t>> buckets;
        for (const auto g : free_nodes) {
            buckets[bin_key(vol.mesh.nodes[g])].push_back(g);
        }
        std::size_t hanging = 0;
        for (const auto& f : free_faces) {
            Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
            for (const auto g : f) {
                ctr += vol.mesh.nodes[g];
            }
            ctr /= static_cast<double>(f.size());
            double radius = 0.0;
            for (const auto g : f) {
                radius = std::max(radius, (vol.mesh.nodes[g] - ctr).norm());
            }
            const Eigen::Vector3d nrm =
                (vol.mesh.nodes[f[1]] - vol.mesh.nodes[f[0]])
                    .cross(vol.mesh.nodes[f[2]] - vol.mesh.nodes[f[0]])
                    .normalized();
            const auto lo = bin_key(ctr - Eigen::Vector3d::Constant(radius));
            const auto hi = bin_key(ctr + Eigen::Vector3d::Constant(radius));
            for (int k = lo[2]; k <= hi[2]; ++k) {
                for (int j = lo[1]; j <= hi[1]; ++j) {
                    for (int i = lo[0]; i <= hi[0]; ++i) {
                        const auto it = buckets.find({{i, j, k}});
                        if (it == buckets.end()) {
                            continue;
                        }
                        for (const auto g : it->second) {
                            if (std::find(f.begin(), f.end(), g) != f.end()) {
                                continue;
                            }
                            const Eigen::Vector3d d = vol.mesh.nodes[g] - ctr;
                            if (d.norm() > 0.99 * radius ||
                                std::abs(d.dot(nrm)) > 1e-9 * radius) {
                                continue;
                            }
                            bool inside = true;
                            for (std::size_t e = 0; e < f.size() && inside; ++e) {
                                const Eigen::Vector3d a = vol.mesh.nodes[f[e]];
                                const Eigen::Vector3d b =
                                    vol.mesh.nodes[f[(e + 1) % f.size()]];
                                inside = (b - a).cross(vol.mesh.nodes[g] - a).dot(nrm) > -1e-12;
                            }
                            hanging += inside ? 1 : 0;
                        }
                    }
                }
            }
        }
        INFO("h/extent = " << frac << ", " << vol.mesh.elements.size() << " elements");
        REQUIRE(hanging == 0);
    }
}
