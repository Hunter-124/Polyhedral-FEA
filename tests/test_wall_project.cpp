// SPDX-License-Identifier: BSD-3-Clause
// M10: wall tangential smooth + OCC surface project (ADR-0024 Q2a).
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "mesh/varyhedron_fill.hpp"
#include "mesh/wall_project.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Eigen/Core>

#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using polymesh::geom::CadModel;
using polymesh::geom::extract_topology;
using polymesh::geom::occ_enabled;
using polymesh::geom::project_point_on_surface;
using polymesh::mesh::varyhedron_fill_surface;
using polymesh::mesh::wall_tangential_project;
using polymesh::pipeline::Model;
using polymesh::pipeline::VolumeMesher;
using polymesh::pipeline::volume_mesh;

namespace {

/// Mean |sqrt(x²+y²) − R| for free-boundary nodes on the cylinder lateral wall
/// (away from end caps), using a z-band so rim-protected nodes are excluded.
double mean_lateral_radial_residual(const std::vector<Eigen::Vector3d>& nodes,
                                    const std::vector<std::array<std::uint32_t, 4>>& tets,
                                    double R, double z_lo, double z_hi) {
    // Boundary nodes from unpaired faces.
    struct FaceKey {
        std::uint32_t a, b, c;
        bool operator==(const FaceKey& o) const { return a == o.a && b == o.b && c == o.c; }
    };
    struct FaceHash {
        std::size_t operator()(const FaceKey& f) const {
            return (static_cast<std::size_t>(f.a) * 73856093u) ^
                   (static_cast<std::size_t>(f.b) * 19349663u) ^
                   (static_cast<std::size_t>(f.c) * 83492791u);
        }
    };
    auto sorted = [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        if (i > j) {
            std::swap(i, j);
        }
        if (j > k) {
            std::swap(j, k);
        }
        if (i > j) {
            std::swap(i, j);
        }
        return FaceKey{i, j, k};
    };
    std::unordered_map<FaceKey, int, FaceHash> faces;
    const int quads[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : tets) {
        for (const auto& f : quads) {
            faces[sorted(t[static_cast<std::size_t>(f[0])], t[static_cast<std::size_t>(f[1])],
                         t[static_cast<std::size_t>(f[2])])]++;
        }
    }
    std::unordered_set<std::uint32_t> bset;
    for (const auto& [key, count] : faces) {
        if (count == 1) {
            bset.insert(key.a);
            bset.insert(key.b);
            bset.insert(key.c);
        }
    }
    double sum = 0.0;
    std::size_t n = 0;
    for (std::uint32_t ni : bset) {
        if (ni >= nodes.size()) {
            continue;
        }
        const auto& p = nodes[ni];
        if (p.z() < z_lo || p.z() > z_hi) {
            continue;
        }
        const double r = std::hypot(p.x(), p.y());
        // Lateral wall: near radius R (exclude axis-interior boundary if any).
        if (r < 0.5 * R) {
            continue;
        }
        sum += std::abs(r - R);
        ++n;
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

} // namespace

TEST_CASE("wall_tangential_project no-ops without CadModel BRep") {
    CadModel empty;
    std::vector<Eigen::Vector3d> nodes = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<std::array<std::uint32_t, 4>> tets = {{{0, 1, 2, 3}}};
    const auto st = wall_tangential_project(empty, nullptr, nodes, tets, 0.1, 3);
    CHECK(st.n_wall_nodes == 0);
    CHECK(st.n_moved == 0);
    CHECK(st.n_iters == 0);
}

TEST_CASE("varyhedron cylinder wall residual does not worsen with OCC project (M10)") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const CadModel cad = CadModel::load_step(path);
    const auto topo = extract_topology(cad, 8);
    const auto surface = cad.tessellate();
    const double h = 0.04;
    const double R = 0.05;
    // Mid-height band excludes sharp end rims (z=0 and z=0.2).
    const double z_lo = 0.06;
    const double z_hi = 0.14;

    auto fill_off =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0, {}, 0.0,
                                15.0, &topo, &cad, /*wall_smooth_iters=*/0);
    auto fill_on =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0, {}, 0.0,
                                15.0, &topo, &cad, /*wall_smooth_iters=*/4);

    REQUIRE(fill_off.n_tets > 0);
    REQUIRE(fill_on.n_tets > 0);
    REQUIRE(fill_on.n_wall_nodes > 0);
    REQUIRE(fill_on.n_wall_iters > 0);

    const double mean_off =
        mean_lateral_radial_residual(fill_off.mesh.nodes, fill_off.mesh.tets, R, z_lo, z_hi);
    const double mean_on =
        mean_lateral_radial_residual(fill_on.mesh.nodes, fill_on.mesh.tets, R, z_lo, z_hi);

    // Primary claim: wall pass must not make mean radial residual worse
    // (allow tiny FP noise). Also absolute residual stays a fraction of h.
    CHECK(mean_on <= mean_off + 1e-9);
    CHECK(mean_on < 0.35 * h);
    // Surface residual from the wall pass itself should be small.
    CHECK(fill_on.wall_mean_surface_residual < 0.25 * h);
}

TEST_CASE("varyhedron sphere wall OCC project runs (M10)") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/sphere.step";
    if (!std::filesystem::exists(path)) {
        SKIP("sphere.step missing");
    }
    const CadModel cad = CadModel::load_step(path);
    const auto topo = extract_topology(cad, 6);
    const auto surface = cad.tessellate();
    const double h = 0.03;
    auto fill =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0, {}, 0.0,
                                15.0, &topo, &cad, /*wall_smooth_iters=*/3);
    REQUIRE(fill.n_tets > 0);
    // Sphere STEP may still have seams; wall pass should find free-face nodes.
    CHECK(fill.n_wall_nodes > 0);
    CHECK(fill.n_wall_iters > 0);
    // Mean BRep residual on wall nodes is finite and bounded by h.
    CHECK(fill.wall_mean_surface_residual < 0.5 * h);
}

TEST_CASE("varyhedron unit cube still meshes; sharp corners protected (M10)") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const CadModel cad = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = extract_topology(cad, 6);
    const auto surface = cad.tessellate();
    const double h = 0.25;
    auto fill =
        varyhedron_fill_surface(surface, cad.bbox_min(), cad.bbox_max(), h, 1, {}, 0.0, {}, 0.0,
                                15.0, &topo, &cad, /*wall_smooth_iters=*/4);
    REQUIRE(fill.n_tets > 0);
    REQUIRE(fill.n_edge_seeds > 0);
    // Cube: all edges sharp → wall band is face-interior only; corners stay near CAD verts.
    const double corner_tol = 0.55 * h; // soft; protect band is 0.4 h
    int corners_near = 0;
    const std::vector<Eigen::Vector3d> corners = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
    };
    // Use CAD bbox corners (unit cube may be [0,1]^3).
    std::vector<Eigen::Vector3d> cad_corners;
    const Eigen::Vector3d lo = cad.bbox_min();
    const Eigen::Vector3d hi = cad.bbox_max();
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                cad_corners.push_back({i ? hi.x() : lo.x(), j ? hi.y() : lo.y(),
                                       k ? hi.z() : lo.z()});
            }
        }
    }
    (void)corners;
    for (const auto& c : cad_corners) {
        double best = 1e300;
        for (const auto& p : fill.mesh.nodes) {
            best = std::min(best, (p - c).norm());
        }
        if (best <= corner_tol) {
            ++corners_near;
        }
    }
    // Most cube corners should still have a mesh node nearby after wall pass.
    CHECK(corners_near >= 6);
}

TEST_CASE("varyhedron STL-only path has no CadModel wall project (no crash)") {
    // Surface-only fill: no topo, no cad — existing snap only.
    const std::filesystem::path path = "bench/geometries/public/unit_box.stl";
    if (!std::filesystem::exists(path)) {
        SKIP("unit_box.stl missing");
    }
    const auto surface = polymesh::geom::load_stl(path);
    Eigen::Vector3d lo = surface.vertices.front();
    Eigen::Vector3d hi = lo;
    for (const auto& v : surface.vertices) {
        lo = lo.cwiseMin(v);
        hi = hi.cwiseMax(v);
    }
    const double h = 0.3 * (hi - lo).norm();
    auto fill =
        varyhedron_fill_surface(surface, lo, hi, h, 1, {}, 0.0, {}, 0.0, 0.0, nullptr, nullptr, 4);
    REQUIRE(fill.n_tets > 0);
    CHECK(fill.n_wall_nodes == 0);
    CHECK(fill.n_wall_moved == 0);
    CHECK(fill.n_wall_iters == 0);
}

TEST_CASE("volume_mesh varyhedron reports wall OCC project in mesher_note") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const Model model = Model::load(path.string());
    REQUIRE(model.cad.has_value());
    const auto out = volume_mesh(model, 0.05, VolumeMesher::kVaryhedron, 1, true);
    REQUIRE(out.mesh.elements.size() > 5);
    REQUIRE(out.mesher_note.find("wall OCC project") != std::string::npos);
    REQUIRE(out.mesher_note.find("wall_nodes=") != std::string::npos);
}
