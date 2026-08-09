// SPDX-License-Identifier: BSD-3-Clause
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Eigen/Core>

#include <cmath>
#include <filesystem>

using polymesh::geom::CadModel;
using polymesh::geom::GeomError;
using polymesh::geom::load_cad;
using polymesh::geom::load_step;
using polymesh::geom::occ_enabled;

TEST_CASE("CadModel empty default") {
    CadModel m;
    CHECK(m.empty());
    CHECK_FALSE(m.has_brep());
    CHECK(m.shape_handle() == nullptr);
}

TEST_CASE("CadModel::load_step when OCC disabled throws") {
    if (occ_enabled()) {
        SKIP("OpenCASCADE enabled; stub path not exercised");
    }
    REQUIRE_THROWS_MATCHES(CadModel::load_step("/definitely/missing/polymesh_no_such.step"),
                           GeomError,
                           Catch::Matchers::MessageMatches(
                               Catch::Matchers::ContainsSubstring("OpenCASCADE not enabled")));
}

TEST_CASE("CadModel unit cube retains BRep and tessellates when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const std::filesystem::path path = "tests/fixtures/unit_cube.step";
    REQUIRE(std::filesystem::exists(path));

    const CadModel model = CadModel::load_step(path);
    REQUIRE_FALSE(model.empty());
    REQUIRE(model.has_brep());
    REQUIRE(model.shape_handle() != nullptr);
    CHECK(model.bbox_diagonal() > 0.5); // unit cube diagonal ~√3

    const auto surface = model.tessellate();
    REQUIRE_FALSE(surface.triangles.empty());
    REQUIRE_FALSE(surface.vertices.empty());
    REQUIRE_NOTHROW(surface.validate());
    CHECK(surface.triangles.size() >= 12);

    // Historical load_step stays consistent with CadModel tessellation path.
    const auto legacy = load_step(path);
    CHECK(legacy.triangles.size() == surface.triangles.size());
}

TEST_CASE("load_cad dispatches by extension when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const auto model = load_cad("tests/fixtures/unit_cube.step");
    REQUIRE_FALSE(model.empty());
    REQUIRE_THROWS_AS(load_cad("tests/fixtures/parts/plate_hole.stl"), GeomError);
}

TEST_CASE("project_point_on_surface empty model returns nullopt") {
    CadModel m;
    const auto r = polymesh::geom::project_point_on_surface(m, Eigen::Vector3d(0.5, 0.5, 0.5));
    CHECK_FALSE(r.has_value());
}

TEST_CASE("project_point_on_surface unit cube face + normal when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const CadModel model = CadModel::load_step("tests/fixtures/unit_cube.step");
    REQUIRE_FALSE(model.empty());

    // Point above the top face of the unit cube → projects to z≈1 (or bbox max z).
    const Eigen::Vector3d query(0.5, 0.5, 2.0);
    const auto r = polymesh::geom::project_point_on_surface(model, query);
    REQUIRE(r.has_value());
    CHECK(r->distance > 0.5);
    CHECK(r->point.z() == Catch::Approx(model.bbox_max().z()).margin(1e-6));
    CHECK(r->point.x() == Catch::Approx(0.5).margin(1e-4));
    CHECK(r->point.y() == Catch::Approx(0.5).margin(1e-4));
    // Outward-ish normal should point roughly +z.
    REQUIRE(r->normal.norm() > 0.5);
    CHECK(r->normal.z() == Catch::Approx(1.0).margin(0.15));
}

TEST_CASE("project_point_on_surface cylinder lateral wall when OCC enabled") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const std::filesystem::path path = "tests/fixtures/parts/cylinder.step";
    if (!std::filesystem::exists(path)) {
        SKIP("cylinder.step missing");
    }
    const CadModel model = CadModel::load_step(path);
    // Cylinder R=0.05, axis +z. Query outside the wall.
    const Eigen::Vector3d query(0.08, 0.0, 0.1);
    const auto r = polymesh::geom::project_point_on_surface(model, query);
    REQUIRE(r.has_value());
    const double radial = std::hypot(r->point.x(), r->point.y());
    CHECK(radial == Catch::Approx(0.05).margin(1e-4));
    CHECK(r->distance == Catch::Approx(0.03).margin(1e-3));
    // Outward normal ≈ radial direction in xy.
    REQUIRE(r->normal.norm() > 0.5);
    CHECK(r->normal.x() == Catch::Approx(1.0).margin(0.2));
}

TEST_CASE("exact constrained projections retain stable cube topology owners") {
    if (!occ_enabled()) {
        SKIP("OpenCASCADE not enabled (POLYMESH_WITH_OCC=OFF)");
    }
    const CadModel model = CadModel::load_step("tests/fixtures/unit_cube.step");
    const auto topo = polymesh::geom::extract_topology(model, 4);
    REQUIRE_FALSE(topo.faces.empty());
    REQUIRE_FALSE(topo.edges.empty());
    REQUIRE_FALSE(topo.vertices.empty());

    const auto top =
        polymesh::geom::project_point_on_surface(model, Eigen::Vector3d(0.5, 0.5, 2.0));
    REQUIRE(top.has_value());
    REQUIRE(top->face_id != polymesh::geom::kInvalidCadSupportId);

    // A face-constrained query outside the face bounds must stop on the
    // trimmed wire, not continue onto the underlying infinite plane.
    const auto face = polymesh::geom::project_point_on_face(model, top->face_id,
                                                            Eigen::Vector3d(2.0, 2.0, 2.0));
    REQUIRE(face.has_value());
    CHECK(face->support_kind == polymesh::geom::CadSupportKind::kFace);
    CHECK(face->support_id == top->face_id);
    CHECK(face->face_id == top->face_id);
    CHECK((face->point.array() >= model.bbox_min().array() - 1e-9).all());
    CHECK((face->point.array() <= model.bbox_max().array() + 1e-9).all());

    const auto& edge = topo.edges.front();
    REQUIRE(edge.samples.size() >= 2);
    const Eigen::Vector3d edge_mid = 0.5 * (edge.samples.front() + edge.samples.back());
    const auto edge_projection = polymesh::geom::project_point_on_edge(
        model, edge.id, edge_mid + Eigen::Vector3d(0.07, 0.05, 0.03));
    REQUIRE(edge_projection.has_value());
    CHECK(edge_projection->support_kind == polymesh::geom::CadSupportKind::kEdge);
    CHECK(edge_projection->support_id == edge.id);
    CHECK((edge_projection->point.array() >= model.bbox_min().array() - 1e-9).all());
    CHECK((edge_projection->point.array() <= model.bbox_max().array() + 1e-9).all());

    const auto& vertex = topo.vertices.front();
    const auto vertex_projection = polymesh::geom::project_point_on_vertex(
        model, vertex.id, vertex.position + Eigen::Vector3d(0.1, -0.2, 0.3));
    REQUIRE(vertex_projection.has_value());
    CHECK(vertex_projection->support_kind == polymesh::geom::CadSupportKind::kVertex);
    CHECK(vertex_projection->support_id == vertex.id);
    CHECK((vertex_projection->point - vertex.position).norm() < 1e-12);

    CHECK_FALSE(
        polymesh::geom::project_point_on_face(
            model, static_cast<std::uint32_t>(topo.faces.size()), Eigen::Vector3d::Zero())
            .has_value());
    CHECK_FALSE(
        polymesh::geom::project_point_on_edge(
            model, static_cast<std::uint32_t>(topo.edges.size()), Eigen::Vector3d::Zero())
            .has_value());
    CHECK_FALSE(
        polymesh::geom::project_point_on_vertex(
            model, static_cast<std::uint32_t>(topo.vertices.size()), Eigen::Vector3d::Zero())
            .has_value());
}
