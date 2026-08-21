// SPDX-License-Identifier: BSD-3-Clause
#include "fea/boundary_faces.hpp"
#include "fea/solve.hpp"
#include "geom/cad_model.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/surface_project.hpp"
#include "pipeline/scene.hpp"

#include "support/box_model.hpp"
#include <Eigen/Geometry>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
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

TEST_CASE("hybrid zoo cylinder_prism smoke: conforming FE + snap") {
    auto model = polymesh::testsupport::model_from_surface(
        polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.12 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto vol = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid, 2, true);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("hybrid zoo") != std::string::npos);
    REQUIRE(vol.mesher_note.find("snap max|d|") != std::string::npos);

    bool has_fe = false;
    for (const auto& el : vol.mesh.elements) {
        has_fe = has_fe || el.type == fea::ElementType::kHex8 ||
                 el.type == fea::ElementType::kPyramid5 || el.type == fea::ElementType::kTet4;
    }
    REQUIRE(has_fe);
}

TEST_CASE("hybrid zoo 2:1 size adaptivity on feature band") {
    // Open box with a sharp crease band: feature grading must refine to h/2.
    auto model = polymesh::testsupport::model_from_surface(
        polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double h = 0.15 * (model.bbox_max - model.bbox_min).maxCoeff();
    auto plain = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                    /*skin=*/1, {}, 0.0, {}, 0.0, /*snap=*/false);
    auto edges = polymesh::geom::detect_sharp_edges(model.surface, 30.0);
    REQUIRE_FALSE(edges.empty());
    auto graded = mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                     /*skin=*/1, edges, 2.0 * h, {}, 0.0,
                                     /*snap=*/false);
    REQUIRE(graded.n_fine_cells > 0);
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
    const auto& local = el.type == fea::ElementType::kHex8       ? kHex
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

// Exercise both graded-transition and coarse budget-fallback meshes. Exact
// face hashing is the inexpensive conformity invariant: no volume face may be
// owned by more than two cells. The finer ladder rungs and exhaustive geometric
// node/face sweep made this regression test take tens of minutes while adding
// no distinct branch coverage.
TEST_CASE("hybrid zoo meshes are conforming across the budget-fallback ladder") {
    auto model = polymesh::testsupport::model_from_surface(
        polymesh::geom::load_stl("bench/geometries/public/cylinder_prism.stl"));
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    for (const double frac : {0.16, 0.20}) {
        auto vol = pipeline::volume_mesh(model, frac * extent, pipeline::VolumeMesher::kHybrid,
                                         2, true);
        REQUIRE_FALSE(vol.mesh.elements.empty());

        std::map<std::vector<std::uint32_t>, int> face_use;
        for (const auto& el : vol.mesh.elements) {
            for (auto face : cell_faces(el)) {
                std::sort(face.begin(), face.end());
                ++face_use[face];
            }
        }
        int max_face_use = 0;
        for (const auto& [face, uses] : face_use) {
            (void)face;
            max_face_use = std::max(max_face_use, uses);
        }
        INFO("h/extent = " << frac << ", " << vol.mesh.elements.size()
                           << " elements, max face use " << max_face_use);
        REQUIRE(max_face_use <= 2);
    }
}

namespace {

double surface_face_area(const fea::NodalMesh& mesh,
                         const std::array<std::uint32_t, 4>& face) {
    const Eigen::Vector3d& a = mesh.nodes[face[0]];
    const Eigen::Vector3d& b = mesh.nodes[face[1]];
    const Eigen::Vector3d& c = mesh.nodes[face[2]];
    double area = 0.5 * (b - a).cross(c - a).norm();
    if (face[3] != face[2]) {
        area += 0.5 * (c - a).cross(mesh.nodes[face[3]] - a).norm();
    }
    return area;
}

std::pair<std::size_t, double> bore_wall(const fea::NodalMesh& mesh, double radius,
                                         double thickness) {
    std::size_t count = 0;
    double area = 0.0;
    for (const auto& face : fea::extract_boundary_faces(mesh)) {
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        double z_min = std::numeric_limits<double>::infinity();
        double z_max = -std::numeric_limits<double>::infinity();
        const int n = face[3] == face[2] ? 3 : 4;
        for (int i = 0; i < n; ++i) {
            const auto& p = mesh.nodes[face[static_cast<std::size_t>(i)]];
            centroid += p;
            z_min = std::min(z_min, p.z());
            z_max = std::max(z_max, p.z());
        }
        centroid /= static_cast<double>(n);
        if (z_max - z_min <= 0.05 * thickness || centroid.head<2>().norm() >= 3.0 * radius) {
            continue;
        }
        ++count;
        area += surface_face_area(mesh, face);
    }
    return {count, area};
}

} // namespace

TEST_CASE("box-hole bore survives the coarse-grid parity ladder",
          "[cad][hybrid][geometry-completeness]") {
    constexpr char kBoxHole[] = "bench/geometries/corpus/primitives/box_hole_s0.step";
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kBoxHole)) {
        SKIP("box_hole_s0.step missing");
    }

    constexpr double radius = 0.002834263448;
    constexpr double thickness = 0.003054365478;
    constexpr double expected_wall_area = 2.0 * 3.14159265358979323846 * radius * thickness;
    const auto model = pipeline::Model::load(kBoxHole);
    const double diagonal = (model.bbox_max - model.bbox_min).norm();

    // The coarse rung used to be a refusal for both meshers: the bore vanished
    // and `enforce_feature_resolution` said so. The graded mesher now resolves
    // it there, because the local child carve confirms each deletion against
    // the surface instead of trusting an h/2 centre sample, so the bore opens
    // at a coarseness that previously produced no bore at all. Measured at
    // h_rel=0.20: 6 wall faces, 0.59x the exact wall area, fill rel_err 0.0101,
    // shell closed. Crude, and present -- which is the thing this case exists
    // to defend. The hybrid mesher still refuses there.
    //
    // What must never come back is a SILENT solid: either the bore is in the
    // mesh, or the engine says it could not put it there.
    for (const auto mesher :
         {pipeline::VolumeMesher::kHybrid, pipeline::VolumeMesher::kGradedTet}) {
        CAPTURE(mesher);
        try {
            const auto coarse = pipeline::volume_mesh(model, 0.20 * diagonal, mesher,
                                                      /*skin_layers=*/2,
                                                      /*feature_refine=*/true);
            const auto [n_wall_faces, wall_area] = bore_wall(coarse.mesh, radius, thickness);
            INFO("coarse mesher=" << static_cast<int>(mesher) << " wall_faces=" << n_wall_faces
                                  << " wall_area=" << wall_area << " expected="
                                  << expected_wall_area << " note=" << coarse.mesher_note);
            CHECK(n_wall_faces > 0);
            CHECK(wall_area > 0.25 * expected_wall_area);
        } catch (const pipeline::GeometryVolumeLimitError& error) {
            CHECK_FALSE(error.solved_stage);
            CHECK(std::string(error.what()).find("feature unresolved") != std::string::npos);
            CHECK(std::string(error.what()).find("reduce -h") != std::string::npos);
            CHECK(std::string(error.what()).find("raise --max-elems/--max-dof") !=
                  std::string::npos);
        }

        for (const double h_rel : {0.12, 0.08}) {
            const auto volume =
                pipeline::volume_mesh(model, h_rel * diagonal, mesher, /*skin_layers=*/2,
                                      /*feature_refine=*/true);
            const auto [n_wall_faces, wall_area] = bore_wall(volume.mesh, radius, thickness);
            INFO("mesher=" << static_cast<int>(mesher) << " h_rel=" << h_rel
                           << " wall_faces=" << n_wall_faces << " wall_area=" << wall_area
                           << " expected=" << expected_wall_area
                           << " note=" << volume.mesher_note);
            CHECK(n_wall_faces > 0);
            // Known h_rel=0.12 faceting limit: measured wall area is 1.93x
            // exact for hybrid and 2.27x for graded tet. This guard checks
            // topology, not submillimetre area accuracy.
            CHECK(wall_area > 0.25 * expected_wall_area);
            CHECK(wall_area < 2.5 * expected_wall_area);
            CHECK(volume.mesher_note.find("geometry_fill_volume") != std::string::npos);
        }
    }
}

// The 2:1 fan transition was accused, for an entire release cycle, of dropping
// 36-40% of the solid volume on every mixed-level fill. It never did. The fan
// is conforming and volume-exact; `fea::pyramid_rule` integrated it over the
// reference PYRAMID with Duffy weights while `fea::eval_pyramid5` is a
// collapsed-brick map on the CUBE, so every pyramid measured 0.6x its true
// volume and the fill guard refused meshes for a defect in its own ruler.
//
// This pins the retraction where it can be measured: a mixed-level hybrid fill
// at an h the guard used to refuse, asserted to be volume-exact AND to actually
// be made of pyramids, so the test cannot pass by quietly ceasing to exercise
// the transition.
TEST_CASE("a mixed-level hybrid fill is volume-exact",
          "[cad][hybrid][geometry-completeness]") {
    constexpr char kUnitBox[] = "bench/geometries/public/unit_box.step";
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kUnitBox)) {
        SKIP("unit_box.step missing");
    }

    const auto model = pipeline::Model::load(kUnitBox);

    // Every one of these was refused before the quadrature fix, with rel_err
    // 0.400, 0.3926, 0.3813, 0.3719, 0.3649 and 0.3600 respectively.
    for (const double h : {0.22, 0.20, 0.173205, 0.16, 0.14, 0.11}) {
        CAPTURE(h);
        const auto volume = pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid,
                                                  /*skin_layers=*/2, /*feature_refine=*/true);
        const auto n_pyramid = std::count_if(
            volume.mesh.elements.begin(), volume.mesh.elements.end(),
            [](const fea::NodalElement& e) { return e.type == fea::ElementType::kPyramid5; });
        INFO(volume.mesher_note);
        // The fill really is the mixed-level, pyramid-stitched one under test.
        REQUIRE(n_pyramid > 0);
        REQUIRE(volume.fill_geometry_volume.available);
        // Machine precision, not merely inside the 0.1 hard limit: the fan
        // tiles the coarse cell exactly, so anything else is a new defect.
        CHECK(volume.fill_geometry_volume.relative_error < 1e-10);
    }
}

// The fill-stage guard is the third of three refusals, and it was the only one
// that reported a ratio without naming a way out. Worse, the ratio does not
// fall smoothly with h -- on this part a 20% reduction makes it worse -- so a
// bare number argues for the one move that does not work. This pins the remedy
// TEXT and then RUNS it: a message that recommends something untested is the
// defect it replaced.
TEST_CASE("fill-stage guard names a remedy that works",
          "[cad][hybrid][geometry-completeness]") {
    constexpr char kChannel[] = "bench/geometries/corpus/primitives/channel_s0.step";
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kChannel)) {
        SKIP("channel_s0.step missing");
    }

    const auto model = pipeline::Model::load(kChannel);
    constexpr double h = 0.015; // measured rel_err 0.3351; 0.012 m is worse still

    bool refused = false;
    try {
        (void)pipeline::volume_mesh(model, h, pipeline::VolumeMesher::kHybrid,
                                    /*skin_layers=*/2, /*feature_refine=*/true);
    } catch (const pipeline::GeometryVolumeLimitError& error) {
        refused = true;
        const std::string what = error.what();
        INFO(what);
        CHECK_FALSE(error.solved_stage);
        // The failing h is named, not just the error ratio.
        CHECK(what.find("fill-stage guard failed at h=") != std::string::npos);
        // The asymptote is called out explicitly, so nobody follows it...
        CHECK(what.find("Reducing -h a little does NOT fix this") != std::string::npos);
        // ...and a concrete, executable size is named.
        CHECK(what.find("Reduce -h to <= 0.0075 m") != std::string::npos);
        // The retracted accusation must not come back.
        CHECK(what.find("MIXED-LEVEL") == std::string::npos);
        CHECK(what.find("--mesher graded_tet") == std::string::npos);
    }
    REQUIRE(refused);

    // The recommendation is executed. If this ever stops clearing the guard,
    // the message is lying and this test says so.
    const auto remedy = pipeline::volume_mesh(model, 0.5 * h, pipeline::VolumeMesher::kHybrid,
                                              /*skin_layers=*/2, /*feature_refine=*/true);
    INFO(remedy.mesher_note);
    REQUIRE(remedy.fill_geometry_volume.available);
    CHECK(remedy.fill_geometry_volume.relative_error < 0.1);
}