// SPDX-License-Identifier: BSD-3-Clause
// Curved-CAD import fidelity + robust volume-mesh regressions.
//
// 1. A cylinder wall must tessellate with sub-percent chord deviation from the
//    true radius — imported pipes should not show coarse facets.
// 2. The ice-cream STEP must remain one closed round-cone/scoop solid with the
//    contract bbox and deterministic coarse BC/load selection.
// 3. Longest-edge bisection on a large-coordinate curved mesh must not abort on
//    a degenerate child ("local_refine_tets: non-positive child volume"); the
//    sliver region is skipped and the mesh stays valid.

#include "fea/boundary_faces.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>
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
#include <numeric>
#include <set>
#include <vector>

namespace {
constexpr double kPipeR = 30.0; // matches tests/fixtures/parts/pipe.step
constexpr double kPipeH = 400.0;
constexpr char kPipe[] = "tests/fixtures/parts/pipe.step";
constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";
constexpr char kIcecreamCone[] = "tests/fixtures/parts/icecream_cone.step";
constexpr char kPlateHole[] = "tests/fixtures/parts/plate_hole.step";
} // namespace

TEST_CASE("curved CAD import: cylinder wall tessellates with sub-percent deviation") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPipe)) {
        SKIP("pipe.step missing");
    }
    const auto cad = polymesh::geom::CadModel::load_step(kPipe);
    const auto surf = cad.tessellate();

    // Lateral-wall facets: normal ~perpendicular to the z axis. Their centroids
    // sit on the tessellation chord; distance to axis vs R = the sag we resolve.
    double max_dev = 0.0;
    std::size_t wall_facets = 0;
    for (const auto& t : surf.triangles) {
        const Eigen::Vector3d a = surf.vertices[t[0]];
        const Eigen::Vector3d b = surf.vertices[t[1]];
        const Eigen::Vector3d c = surf.vertices[t[2]];
        const Eigen::Vector3d n = Eigen::Vector3d(b - a).cross(Eigen::Vector3d(c - a));
        if (n.norm() < 1e-12) {
            continue;
        }
        const Eigen::Vector3d nn = n.normalized();
        if (std::abs(nn.z()) > 0.5) {
            continue; // end cap, not the curved wall
        }
        const Eigen::Vector3d ctr = (a + b + c) / 3.0;
        const double r = std::hypot(ctr.x(), ctr.y());
        max_dev = std::max(max_dev, std::abs(r - kPipeR));
        ++wall_facets;
    }
    // ~2π/0.2rad ≈ 31 facets around → many wall facets, chord sag < 1% R.
    REQUIRE(wall_facets >= 40);
    CHECK(max_dev / kPipeR < 0.01);
}

TEST_CASE("BRep fidelity sample summaries filter and normalize deterministically") {
    const std::vector<double> samples{
        0.0,
        1.0,
        2.0,
        3.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };
    const auto summary = polymesh::mesh::summarize_samples(samples);
    REQUIRE(summary.count == 4);
    CHECK(summary.rms == Catch::Approx(std::sqrt(3.5)));
    CHECK(summary.p95 == Catch::Approx(2.85));
    CHECK(summary.p99 == Catch::Approx(2.97));
    CHECK(summary.max == Catch::Approx(3.0));

    const std::vector<double> distances{
        0.0,
        1.0,
        2.0,
        3.0,
        -1.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
    };
    const auto normalized = polymesh::mesh::summarize_distances(distances, 2.0, 4.0);
    REQUIRE(normalized.metres.count == 4);
    CHECK(normalized.metres.p95 == Catch::Approx(2.85));
    REQUIRE(normalized.over_h.count == 4);
    CHECK(normalized.over_h.rms == Catch::Approx(std::sqrt(3.5) / 2.0));
    CHECK(normalized.over_h.p95 == Catch::Approx(1.425));
    CHECK(normalized.over_h.p99 == Catch::Approx(1.485));
    CHECK(normalized.over_h.max == Catch::Approx(1.5));
    REQUIRE(normalized.over_bbox_diagonal.count == 4);
    CHECK(normalized.over_bbox_diagonal.p95 == Catch::Approx(0.7125));
    CHECK(normalized.over_bbox_diagonal.p99 == Catch::Approx(0.7425));
    CHECK(normalized.over_bbox_diagonal.max == Catch::Approx(0.75));

    const auto invalid_scales = polymesh::mesh::summarize_distances(
        distances, 0.0, std::numeric_limits<double>::infinity());
    CHECK(invalid_scales.metres.count == 4);
    CHECK(invalid_scales.over_h.count == 0);
    CHECK(invalid_scales.over_bbox_diagonal.count == 0);

    const polymesh::geom::CadModel empty;
    CHECK_FALSE(polymesh::geom::inspect_brep(empty).available);
    const auto unavailable =
        polymesh::mesh::evaluate_brep_geometry_fidelity(empty, {}, {}, {}, 1.0, 0.0);
    CHECK_FALSE(unavailable.available);

    CHECK(unavailable.mesh_boundary_samples_to_brep_surface.metres.count == 0);
}

TEST_CASE("exact trimmed BRep surface sampling obeys a hard budget") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    const auto cad = polymesh::geom::CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto inspection = polymesh::geom::inspect_brep(cad);
    REQUIRE(inspection.face_count > 1);
    CHECK_THROWS_AS(polymesh::geom::sample_brep_surface(cad, inspection.face_count - 1),
                    polymesh::geom::GeomError);

    const auto coverage = polymesh::geom::sample_brep_surface(cad, inspection.face_count);
    REQUIRE(coverage.face_count == inspection.face_count);
    REQUIRE(coverage.points.size() == inspection.face_count);
    REQUIRE(coverage.uv_attempt_count <= 9 * inspection.face_count);
    REQUIRE(coverage.fallback_vertex_count <= inspection.face_count);
    std::set<std::uint32_t> owning_faces;
    for (const Eigen::Vector3d& point : coverage.points) {
        const auto projected = polymesh::geom::project_point_on_surface(cad, point);
        REQUIRE(projected);
        CHECK(projected->distance < 1e-10);
        owning_faces.insert(projected->face_id);
    }
    CHECK(owning_faces.size() == inspection.face_count);

    constexpr std::size_t kBudget = 128;
    const auto first = polymesh::geom::sample_brep_surface(cad, kBudget);
    const auto second = polymesh::geom::sample_brep_surface(cad, kBudget);
    REQUIRE(first.points.size() >= inspection.face_count);
    REQUIRE(first.points.size() <= kBudget);
    REQUIRE(first.uv_attempt_count <= 9 * kBudget);
    REQUIRE(first.fallback_vertex_count <= inspection.face_count);
    REQUIRE(second.points.size() == first.points.size());
    bool deterministic = first.uv_attempt_count == second.uv_attempt_count &&
                         first.fallback_vertex_count == second.fallback_vertex_count;
    bool all_on_exact_brep = true;
    for (std::size_t i = 0; i < first.points.size(); ++i) {
        deterministic = deterministic && first.points[i].isApprox(second.points[i], 0.0);
        const auto projected = polymesh::geom::project_point_on_surface(cad, first.points[i]);
        all_on_exact_brep =
            all_on_exact_brep && projected.has_value() && projected->distance < 1e-10;
    }
    CHECK(deterministic);
    CHECK(all_on_exact_brep);
}

TEST_CASE("graded plate-hole mesh resolves exact BRep surfaces and protected rims") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPlateHole)) {
        SKIP("plate_hole.step missing");
    }

    constexpr double kH = 0.006;
    const auto cad = polymesh::geom::CadModel::load_step(kPlateHole);

    const auto model = polymesh::pipeline::Model::load(kPlateHole);
    const auto graded = polymesh::pipeline::volume_mesh(
        model, kH, polymesh::pipeline::VolumeMesher::kGradedTet,
        /*skin_layers=*/2, /*feature_refine=*/true);
    REQUIRE_NOTHROW(graded.mesh.check_validity());
    REQUIRE(graded.mesh.elements.size() > 30'000);

    const auto free_faces = polymesh::fea::extract_boundary_faces(graded.mesh);
    REQUIRE_FALSE(free_faces.empty());
    using Edge = std::array<std::uint32_t, 2>;
    std::map<Edge, std::vector<Eigen::Vector3d>> edge_normals;
    bool all_boundary_faces_non_degenerate = true;
    for (const auto& face : free_faces) {
        const std::size_t count = face[3] == face[2] ? 3 : 4;
        Eigen::Vector3d normal =
            (graded.mesh.nodes[face[1]] - graded.mesh.nodes[face[0]])
                .cross(graded.mesh.nodes[face[2]] - graded.mesh.nodes[face[0]]);
        const double norm = normal.norm();
        if (!(norm > 1e-15)) {
            all_boundary_faces_non_degenerate = false;
            continue;
        }
        normal /= norm;
        for (std::size_t i = 0; i < count; ++i) {
            const auto [a, b] = std::minmax(face[i], face[(i + 1) % count]);
            edge_normals[{a, b}].push_back(normal);
        }
    }
    REQUIRE(all_boundary_faces_non_degenerate);

    std::vector<polymesh::geom::MeshEdgeSegment> feature_segments;
    for (const auto& [edge, normals] : edge_normals) {
        if (normals.size() != 2) {
            continue;
        }
        const double cosine = std::clamp(normals[0].dot(normals[1]), -1.0, 1.0);
        if (std::acos(cosine) < 30.0 * 3.14159265358979323846 / 180.0) {
            continue;
        }
        feature_segments.push_back({graded.mesh.nodes[edge[0]], graded.mesh.nodes[edge[1]]});
    }
    REQUIRE(feature_segments.size() > 400);

    double mesh_volume = 0.0;
    bool all_tets = true;
    for (const auto& element : graded.mesh.elements) {
        if (element.type != polymesh::fea::ElementType::kTet4 || element.nodes.size() != 4) {
            all_tets = false;
            continue;
        }
        mesh_volume += std::abs(polymesh::mesh::tet_signed_volume(
            graded.mesh.nodes[element.nodes[0]], graded.mesh.nodes[element.nodes[1]],
            graded.mesh.nodes[element.nodes[2]], graded.mesh.nodes[element.nodes[3]]));
    }
    REQUIRE(all_tets);
    const auto fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
        cad, graded.mesh.nodes, free_faces, feature_segments, kH, mesh_volume);
    REQUIRE(fidelity.available);
    REQUIRE(fidelity.mesh_boundary_samples_to_brep_surface.over_h.count > 0);
    REQUIRE(fidelity.brep_surface_samples_to_mesh_boundary.over_h.count > 0);
    REQUIRE(fidelity.sharp_brep_edge_samples_to_mesh_feature_segments.over_h.count > 0);
    REQUIRE(fidelity.brep_vertices_to_mesh_boundary_nodes.over_h.count > 0);

    CHECK(fidelity.mesh_boundary_samples_to_brep_surface.over_h.p99 < 0.025);
    CHECK(fidelity.mesh_boundary_samples_to_brep_surface.over_h.max < 0.22);
    CHECK(fidelity.brep_surface_samples_to_mesh_boundary.over_h.p99 < 0.30);
    CHECK(fidelity.mesh_feature_segment_samples_to_sharp_brep_edges.over_h.p99 < 1.0);
    CHECK(fidelity.sharp_brep_edge_samples_to_mesh_feature_segments.over_h.p99 < 0.35);
    CHECK(fidelity.brep_vertices_to_mesh_boundary_nodes.over_h.p99 < 0.45);
    CHECK(fidelity.mesh_boundary_normal_angle_to_brep_normal.p99 * 180.0 /
              3.14159265358979323846 <
          25.0);
    REQUIRE(fidelity.has_relative_volume_error);
    CHECK(fidelity.mesh_vs_brep_relative_volume_error < 0.002);
}

// The S5 void carve peels the tets of "juts" -- boundary nodes the snap could
// not place on the surface. Those are meant to be stair chords hanging inside a
// CAD hole, and peeling them opens the hole. The test used to be distance
// alone, which also condemned the mirror case: a node that stayed put because
// projecting it OUTWARD would invert a skin tet, i.e. a node sitting inside the
// solid. Peeling that one digs a pit and calls the pit floor the boundary.
//
// A sphere has no holes, so every jut on it is the mirror case, and this is the
// cheapest geometry that isolates the bug. Measured before the fix at h = 8 mm:
// 12 boundary nodes as deep as 9.46 mm (1.16 h) inside the sphere. Every crater
// was watertight, so no shell census could see it -- only radius can.
TEST_CASE("a graded sphere has no boundary node carved into its interior") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kSphere)) {
        SKIP("sphere.step missing");
    }

    constexpr double kH = 0.008;
    const auto model = polymesh::pipeline::Model::load(kSphere);
    const auto graded = polymesh::pipeline::volume_mesh(
        model, kH, polymesh::pipeline::VolumeMesher::kGradedTet,
        /*skin_layers=*/2, /*feature_refine=*/true);
    REQUIRE_NOTHROW(graded.mesh.check_validity());

    const auto free_faces = polymesh::fea::extract_boundary_faces(graded.mesh);
    REQUIRE_FALSE(free_faces.empty());

    std::set<std::uint32_t> boundary_nodes;
    for (const auto& face : free_faces) {
        const std::size_t count = face[3] == face[2] ? 3 : 4;
        for (std::size_t i = 0; i < count; ++i) {
            boundary_nodes.insert(face[i]);
        }
    }
    REQUIRE(boundary_nodes.size() > 500);

    // Centre and radius from the boundary bounding box. A crater is an INWARD
    // defect and cannot move the box, so the reference stays honest even on the
    // mesh being accused -- which a least-squares radius fit would not.
    Eigen::Vector3d lo = Eigen::Vector3d::Constant(std::numeric_limits<double>::max());
    Eigen::Vector3d hi = Eigen::Vector3d::Constant(std::numeric_limits<double>::lowest());
    for (const auto ni : boundary_nodes) {
        lo = lo.cwiseMin(graded.mesh.nodes[ni]);
        hi = hi.cwiseMax(graded.mesh.nodes[ni]);
    }
    const Eigen::Vector3d centre = 0.5 * (lo + hi);
    const double radius = (hi - lo).sum() / 6.0;
    REQUIRE(radius > 0.04);
    REQUIRE(radius < 0.06);

    double deepest = 0.0;
    double highest = 0.0;
    std::size_t carved = 0;
    for (const auto ni : boundary_nodes) {
        const double dr = (graded.mesh.nodes[ni] - centre).norm() - radius;
        deepest = std::min(deepest, dr);
        highest = std::max(highest, dr);
        if (dr < -0.25 * kH) {
            ++carved;
        }
    }
    INFO("deepest " << deepest << " m, highest " << highest << " m, carved " << carved);
    CHECK(carved == 0);
    CHECK(deepest > -0.25 * kH);
    CHECK(highest < 0.10 * kH);
}

// The S7 overlapped-sheet carve. Snap gives every boundary node its exact CAD
// owner, so at a concave crease two sheets project onto their own face patches
// and can interpenetrate with every tet positive and every edge manifold. The
// only census that sees it is point-in-foreign-tet. Pre-fix ship counts: 9
// buried free faces on icecream_cone at h = 10 mm (rendered as black holes in
// the showcase), ~500 on sphere_box_s0 at h = 3.6 mm at the near-tangent
// pocket/box junction.
TEST_CASE("graded fills ship no free face buried inside another cell") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* path;
        double h;
    };
    const Case cases[] = {
        {"tests/fixtures/parts/icecream_cone.step", 0.010},
        {"bench/geometries/corpus/primitives/sphere_box_s0.step", 0.0036},
    };
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            continue;
        }
        DYNAMIC_SECTION(c.path << " h=" << c.h) {
            const auto model = polymesh::pipeline::Model::load(c.path);
            REQUIRE(model.cad);
            std::vector<polymesh::mesh::BoundarySupport> provenance;
            polymesh::mesh::BoundaryProjectionContext projection;
            std::shared_ptr<const polymesh::geom::CadTopology> topology;
            REQUIRE(polymesh::pipeline::make_boundary_projection(*model.cad, c.h, &projection,
                                                                 &provenance, &topology));
            const polymesh::mesh::BoundaryFit fit{&*model.cad, topology.get(), &projection};
            const auto fill = polymesh::mesh::graded_tet_fill_surface(
                model.surface, model.bbox_min, model.bbox_max, c.h, 2, {}, 0.0, {}, 0.0, 0.0,
                &fit);
            const auto stats = polymesh::mesh::count_buried_free_tet_faces(
                fill.mesh.nodes, fill.mesh.tets, fill.h_coarse > 0.0 ? fill.h_coarse : c.h);
            INFO("free faces " << stats.n_free_faces << ", buried " << stats.n_buried);
            REQUIRE(stats.n_free_faces > 0);
            CHECK(stats.n_buried == 0);
        }
    }
}

TEST_CASE("ice-cream cone STEP is one closed fused solid with stable coarse selections") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kIcecreamCone)) {
        SKIP("icecream_cone.step missing");
    }

    const auto cad = polymesh::geom::CadModel::load_step(kIcecreamCone);
    const Eigen::Vector3d lo = cad.bbox_min();
    const Eigen::Vector3d hi = cad.bbox_max();
    CHECK(lo.x() == Catch::Approx(-0.035).margin(1e-6));
    CHECK(lo.y() == Catch::Approx(-0.035).margin(1e-6));
    CHECK(lo.z() == Catch::Approx(0.0).margin(1e-7));
    CHECK(hi.x() == Catch::Approx(0.035).margin(1e-6));
    CHECK(hi.y() == Catch::Approx(0.035).margin(1e-6));
    CHECK(hi.z() == Catch::Approx(0.147).margin(1e-6));

    const auto inspection = polymesh::geom::inspect_brep(cad);
    REQUIRE(inspection.available);
    CHECK(inspection.valid);
    CHECK(inspection.closed);
    CHECK(inspection.solid_count == 1);
    CHECK(inspection.shell_count == 1);
    CHECK(inspection.closed_shell_count == 1);
    CHECK(inspection.volume == Catch::Approx(2.65748e-4).margin(1e-7));

    const auto model = polymesh::pipeline::Model::load(kIcecreamCone);
    constexpr double kH = 0.010;
    const auto coarse = polymesh::pipeline::volume_mesh(
        model, kH, polymesh::pipeline::VolumeMesher::kGradedTet);
    const auto repeated = polymesh::pipeline::volume_mesh(
        model, kH, polymesh::pipeline::VolumeMesher::kGradedTet);
    REQUIRE_FALSE(coarse.mesh.nodes.empty());
    REQUIRE_FALSE(coarse.mesh.elements.empty());
    REQUIRE_NOTHROW(coarse.mesh.check_validity());
    CHECK(repeated.mesh.nodes.size() == coarse.mesh.nodes.size());
    CHECK(repeated.mesh.elements.size() == coarse.mesh.elements.size());

    const auto free_faces = polymesh::fea::extract_boundary_faces(coarse.mesh);
    const auto repeated_free_faces = polymesh::fea::extract_boundary_faces(repeated.mesh);
    REQUIRE_FALSE(free_faces.empty());
    CHECK(repeated_free_faces == free_faces);

    std::set<std::array<std::uint32_t, 2>> boundary_edge_ids;
    for (const auto& face : free_faces) {
        const std::size_t count = face[3] == face[2] ? 3 : 4;
        for (std::size_t i = 0; i < count; ++i) {
            const auto [a, b] = std::minmax(face[i], face[(i + 1) % count]);
            boundary_edge_ids.insert({a, b});
        }
    }
    std::vector<polymesh::geom::MeshEdgeSegment> feature_candidates;
    feature_candidates.reserve(boundary_edge_ids.size());
    for (const auto& edge : boundary_edge_ids) {
        feature_candidates.push_back({coarse.mesh.nodes[edge[0]], coarse.mesh.nodes[edge[1]]});
    }
    REQUIRE_FALSE(feature_candidates.empty());

    double mesh_volume = 0.0;
    std::size_t volume_tet_count = 0;
    for (const auto& element : coarse.mesh.elements) {
        if (element.type != polymesh::fea::ElementType::kTet4 || element.nodes.size() != 4) {
            continue;
        }
        mesh_volume += std::abs(polymesh::mesh::tet_signed_volume(
            coarse.mesh.nodes[element.nodes[0]], coarse.mesh.nodes[element.nodes[1]],
            coarse.mesh.nodes[element.nodes[2]], coarse.mesh.nodes[element.nodes[3]]));
        ++volume_tet_count;
    }
    REQUIRE(volume_tet_count > 0);
    REQUIRE(mesh_volume > 0.0);

    const auto fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
        cad, coarse.mesh.nodes, free_faces, feature_candidates, kH, mesh_volume);
    REQUIRE(fidelity.available);
    REQUIRE(fidelity.brep.available);

    const auto check_distance_distribution =
        [](const polymesh::mesh::DistanceDistribution& distribution) {
            REQUIRE(distribution.metres.count > 0);
            CHECK(distribution.over_h.count == distribution.metres.count);
            CHECK(distribution.over_bbox_diagonal.count == distribution.metres.count);
            CHECK(std::isfinite(distribution.metres.rms));
            CHECK(std::isfinite(distribution.metres.p95));
            CHECK(std::isfinite(distribution.metres.p99));
            CHECK(std::isfinite(distribution.metres.max));
            CHECK(distribution.metres.p95 <= distribution.metres.p99);
            CHECK(distribution.metres.p99 <= distribution.metres.max);
        };
    check_distance_distribution(fidelity.mesh_boundary_samples_to_brep_surface);
    check_distance_distribution(fidelity.brep_surface_samples_to_mesh_boundary);
    check_distance_distribution(fidelity.mesh_feature_segment_samples_to_sharp_brep_edges);
    check_distance_distribution(fidelity.sharp_brep_edge_samples_to_mesh_feature_segments);
    check_distance_distribution(fidelity.brep_vertices_to_mesh_boundary_nodes);

    REQUIRE(fidelity.mesh_boundary_normal_angle_to_brep_normal.count > 0);
    CHECK(std::isfinite(fidelity.mesh_boundary_normal_angle_to_brep_normal.rms));
    CHECK(std::isfinite(fidelity.mesh_boundary_normal_angle_to_brep_normal.p95));
    CHECK(std::isfinite(fidelity.mesh_boundary_normal_angle_to_brep_normal.p99));
    CHECK(std::isfinite(fidelity.mesh_boundary_normal_angle_to_brep_normal.max));
    REQUIRE(fidelity.mesh_feature_segment_count > 0);
    CHECK(std::isfinite(fidelity.max_mesh_feature_segment_midpoint_to_sharp_brep_edge));
    CHECK(std::isfinite(fidelity.max_sharp_edge_chordal_efficiency));
    REQUIRE(fidelity.has_relative_volume_error);
    CHECK(std::isfinite(fidelity.mesh_vs_brep_relative_volume_error));

    std::vector<std::uint32_t> fixed_nodes;
    std::vector<std::uint32_t> loaded_nodes;
    for (std::size_t i = 0; i < coarse.mesh.nodes.size(); ++i) {
        const double z = coarse.mesh.nodes[i].z();
        if (z <= 0.012 + 1e-10) {
            fixed_nodes.push_back(static_cast<std::uint32_t>(i));
        }
        if (z >= 0.120 - 1e-10) {
            loaded_nodes.push_back(static_cast<std::uint32_t>(i));
        }
    }
    REQUIRE_FALSE(fixed_nodes.empty());
    REQUIRE_FALSE(loaded_nodes.empty());

    const auto surface_faces = polymesh::fea::boundary_surface_faces(coarse.mesh);
    const auto fixed_faces = polymesh::fea::faces_within(surface_faces, fixed_nodes);
    const auto loaded_faces = polymesh::fea::faces_within(surface_faces, loaded_nodes);
    REQUIRE_FALSE(fixed_faces.empty());
    REQUIRE_FALSE(loaded_faces.empty());
}

TEST_CASE("LEB on a large-coordinate curved mesh does not abort on slivers") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kPipe)) {
        SKIP("pipe.step missing");
    }
    const auto model = polymesh::pipeline::Model::load(kPipe);
    const double h = 0.06 * kPipeH; // first coarse ladder rung with <5% delivered volume loss
    const auto vol = polymesh::pipeline::volume_mesh(
        model, h, polymesh::pipeline::VolumeMesher::kGradedTet);
    REQUIRE_FALSE(vol.mesh.elements.empty());

    std::vector<std::array<std::uint32_t, 4>> tets;
    for (const auto& el : vol.mesh.elements) {
        if (el.type == polymesh::fea::ElementType::kTet4 && el.nodes.size() == 4) {
            tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
        }
    }
    REQUIRE_FALSE(tets.empty());

    // Mark every tet and cascade a few LEB waves against the CAD surface —
    // exactly the pipeline adapt path that used to throw on a degenerate child.
    auto nodes = vol.mesh.nodes;
    for (int wave = 0; wave < 3; ++wave) {
        std::vector<std::size_t> marks(tets.size());
        std::iota(marks.begin(), marks.end(), 0);
        polymesh::mesh::LocalRefineStats st;
        polymesh::mesh::TetFillOutput refined;
        REQUIRE_NOTHROW(refined = polymesh::mesh::local_refine_tets(nodes, tets, marks, &st,
                                                                    &model.surface));
        // Every kept tet is strictly positive (validated inside); mesh grew.
        REQUIRE(refined.tets.size() >= tets.size());
        nodes = std::move(refined.nodes);
        tets = std::move(refined.tets);
    }
}

TEST_CASE("diagnostic: independently sum coarse graded volumes") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    struct Case {
        const char* name;
        const char* path;
        double h;
    };
    const Case cases[] = {
        {"icecream", kIcecreamCone, 0.010},
        {"pipe-0.10", kPipe, 0.10 * kPipeH},
        {"pipe-0.08", kPipe, 0.08 * kPipeH},
        {"pipe-0.06", kPipe, 0.06 * kPipeH},
        {"pipe-0.05", kPipe, 0.05 * kPipeH},
        {"sphere-0.20", "tests/fixtures/parts/sphere.step", 0.20 * 0.17320508075688773},
        {"sphere-0.10", "tests/fixtures/parts/sphere.step", 0.10 * 0.17320508075688773},
        {"sphere-0.08", "tests/fixtures/parts/sphere.step", 0.08 * 0.17320508075688773},
        {"sphere-0.06", "tests/fixtures/parts/sphere.step", 0.06 * 0.17320508075688773},
        {"sphere-0.05", "tests/fixtures/parts/sphere.step", 0.05 * 0.17320508075688773},
    };
    for (const auto& c : cases) {
        if (!std::filesystem::exists(c.path)) {
            continue;
        }
        const auto model = polymesh::pipeline::Model::load(c.path);
        REQUIRE(model.cad);
        std::vector<polymesh::mesh::BoundarySupport> provenance;
        polymesh::mesh::BoundaryProjectionContext projection;
        std::shared_ptr<const polymesh::geom::CadTopology> topology;
        REQUIRE(polymesh::pipeline::make_boundary_projection(*model.cad, c.h, &projection,
                                                             &provenance, &topology));
        const polymesh::mesh::BoundaryFit fit{&*model.cad, topology.get(), &projection};
        const auto fill = polymesh::mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, c.h, 2, {}, 0.0, {}, 0.0, 0.0,
            &fit);
        double mesh_volume = 0.0;
        for (const auto& tet : fill.mesh.tets) {
            mesh_volume += std::abs(polymesh::mesh::tet_signed_volume(
                fill.mesh.nodes[tet[0]], fill.mesh.nodes[tet[1]], fill.mesh.nodes[tet[2]],
                fill.mesh.nodes[tet[3]]));
        }
        const auto exact = polymesh::geom::inspect_brep(*model.cad);
        REQUIRE(exact.available);
        const double relative_error = std::abs(mesh_volume - exact.volume) / exact.volume;
        WARN(c.name << ": element_sum=" << mesh_volume << " cad=" << exact.volume
                    << " rel_err=" << relative_error << " tets=" << fill.mesh.tets.size());
    }
}

namespace {

/// One hex20 sector of a hollow cylinder: outer face on radius `R`, inner on
/// `r`, spanning `sweep` radians. Mid-edge nodes on the outer wall are snapped
/// onto the true cylinder, exactly as `project_quadratic_boundary_mids` does on
/// a real part; every other mid-edge node is the straight midpoint.
polymesh::fea::NodalMesh curved_hex20_sector(double r, double R, double sweep, double height) {
    const auto at = [](double radius, double angle, double z) {
        return Eigen::Vector3d{radius * std::cos(angle), radius * std::sin(angle), z};
    };
    const double a0 = -0.5 * sweep, a1 = 0.5 * sweep;
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {at(r, a0, 0.0),    at(r, a1, 0.0),    at(R, a1, 0.0),    at(R, a0, 0.0),
                  at(r, a0, height), at(r, a1, height), at(R, a1, height), at(R, a0, height)};
    static constexpr std::array<std::array<std::size_t, 2>, 12> kEdges{{{0, 1},
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
    for (const auto& e : kEdges) {
        const Eigen::Vector3d chord = 0.5 * (mesh.nodes[e[0]] + mesh.nodes[e[1]]);
        const bool on_outer_wall = std::abs(mesh.nodes[e[0]].head<2>().norm() - R) < 1e-12 &&
                                   std::abs(mesh.nodes[e[1]].head<2>().norm() - R) < 1e-12;
        if (on_outer_wall && chord.head<2>().norm() > 1e-12) {
            Eigen::Vector3d snapped = chord;
            snapped.head<2>() *= R / chord.head<2>().norm();
            mesh.nodes.push_back(snapped);
        } else {
            mesh.nodes.push_back(chord);
        }
    }
    polymesh::fea::NodalElement el;
    el.type = polymesh::fea::ElementType::kHex20;
    el.nodes.resize(20);
    for (std::uint32_t i = 0; i < 20; ++i) {
        el.nodes[i] = i;
    }
    mesh.elements.push_back(std::move(el));
    return mesh;
}

/// Worst gap between a rendered facet and the true cylinder: the largest
/// shortfall in radius at the midpoint of any drawn edge whose two endpoints
/// both sit on the outer wall. This is precisely the faceting a viewer sees.
double outer_wall_sag(const polymesh::fea::NodalMesh& mesh,
                      const std::vector<std::vector<std::uint32_t>>& facets, double R) {
    double worst = 0.0;
    for (const auto& facet : facets) {
        for (std::size_t i = 0; i < facet.size(); ++i) {
            const Eigen::Vector3d& p = mesh.nodes[facet[i]];
            const Eigen::Vector3d& q = mesh.nodes[facet[(i + 1) % facet.size()]];
            if (std::abs(p.head<2>().norm() - R) > 1e-9 ||
                std::abs(q.head<2>().norm() - R) > 1e-9) {
                continue;
            }
            worst = std::max(worst, R - (0.5 * (p + q)).head<2>().norm());
        }
    }
    return worst;
}

} // namespace

// The mesher projects quadratic mid-edge nodes onto the exact B-rep (ADR-0028),
// and then the display path threw that away: `collect_element_loops` faceted
// tet10/hex20 from CORNER nodes only, so a correctly curved rim still drew as
// the straight chord between corners. That is the "defects along edges and
// curved surfaces" the mesh itself did not have.
TEST_CASE("the display surface follows quadratic curvature, the physics surface does not") {
    constexpr double r = 20.0, R = 30.0, sweep = 1.0471975511965976 /* 60 deg */,
                     height = 40.0;
    const auto mesh = curved_hex20_sector(r, R, sweep, height);

    // Physics topology is corner-only by design: BC/load selection and traction
    // area are defined on it, and this test pins that it did NOT change.
    const auto physics = polymesh::fea::extract_boundary_faces(mesh);
    CHECK(physics.size() == 6);
    std::vector<std::vector<std::uint32_t>> physics_loops;
    for (const auto& face : physics) {
        physics_loops.push_back({face[0], face[1], face[2], face[3]});
        CHECK(*std::max_element(face.begin(), face.end()) < 8U);
    }

    const auto display = polymesh::fea::extract_boundary_polys(mesh);
    // Each of the 6 faces splits into 4 corner triangles plus a central quad.
    CHECK(display.size() == 30);

    // Every projected mid-edge node on the outer wall is actually drawn.
    std::set<std::uint32_t> drawn;
    for (const auto& facet : display) {
        drawn.insert(facet.begin(), facet.end());
    }
    for (std::uint32_t n = 8; n < 20; ++n) {
        INFO("mid-edge node " << n);
        CHECK(drawn.count(n) == 1);
    }

    const double corner_sag = outer_wall_sag(mesh, physics_loops, R);
    const double curved_sag = outer_wall_sag(mesh, display, R);
    INFO("corner-only sag " << corner_sag << " mm, curved sag " << curved_sag << " mm");
    // Chord over the full 60 deg sweep vs over each 30 deg half: R(1-cos30) =
    // 4.019 mm against R(1-cos15) = 1.022 mm, a 3.93x improvement.
    CHECK(corner_sag == Catch::Approx(R * (1.0 - std::cos(0.5 * sweep))).margin(1e-9));
    CHECK(curved_sag == Catch::Approx(R * (1.0 - std::cos(0.25 * sweep))).margin(1e-9));
    CHECK(curved_sag < corner_sag / 3.0);
}

// A linear mesh must be untouched: no mid-edge nodes exist, so the display path
// has to hand back exactly the loops it always did, or every hex/pyramid mesh
// silently changes shape.
TEST_CASE("the display surface is unchanged on linear meshes") {
    polymesh::fea::NodalMesh mesh;
    mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    polymesh::fea::NodalElement el;
    el.type = polymesh::fea::ElementType::kHex8;
    el.nodes = {0, 1, 2, 3, 4, 5, 6, 7};
    mesh.elements.push_back(std::move(el));

    const auto display = polymesh::fea::extract_boundary_polys(mesh);
    CHECK(display.size() == 6);
    for (const auto& facet : display) {
        CHECK(facet.size() == 4);
    }
}
