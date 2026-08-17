// SPDX-License-Identifier: BSD-3-Clause

// Consistent-traction load application from a nodal selection: boundary face
// extraction (incl. quadratic upgrade), integrated area, face filtering by node
// set, and total-force-conserving load assembly. These back the CLI/GUI load
// path, which previously split the requested force evenly over the selected
// nodes — a mesh-density-dependent distribution.

#include "fea/traction.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fea = polymesh::fea;
using Catch::Approx;
using polymesh::test_support::box_hex_mesh;
using polymesh::test_support::box_tet_mesh;
using polymesh::test_support::promote_to_quadratic;

namespace {

std::vector<std::uint32_t> nodes_at_max_z(const fea::NodalMesh& mesh, double z) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        if (std::abs(mesh.nodes[i].z() - z) < 1e-12) {
            out.push_back(i);
        }
    }
    return out;
}

Eigen::Vector3d nodal_sum(const Eigen::VectorXd& f) {
    Eigen::Vector3d s = Eigen::Vector3d::Zero();
    for (Eigen::Index i = 0; i + 2 < f.size(); i += 3) {
        s += f.segment<3>(i);
    }
    return s;
}

} // namespace

TEST_CASE("traction: boundary_surface_faces covers the exact box surface", "[traction]") {
    const Eigen::Vector3d size(2.0, 3.0, 5.0);
    const auto mesh = box_hex_mesh(2, 3, 4, size);
    const auto faces = fea::boundary_surface_faces(mesh);

    REQUIRE(!faces.empty());
    for (const auto& f : faces) {
        REQUIRE(f.type == fea::FaceType::kQuad4);
        REQUIRE(f.nodes.size() == 4);
    }
    const double expected = 2.0 * (size.x() * size.y() + size.y() * size.z() +
                                   size.z() * size.x());
    CHECK(fea::integrated_face_area(mesh, faces) == Approx(expected).epsilon(1e-12));
}

TEST_CASE("traction: boundary_surface_faces upgrades quadratic meshes", "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto linear = box_tet_mesh(1, 1, 1, size);
    const auto quadratic = promote_to_quadratic(linear);

    const auto lin_faces = fea::boundary_surface_faces(linear);
    const auto quad_faces = fea::boundary_surface_faces(quadratic);
    REQUIRE(lin_faces.size() == quad_faces.size());
    for (const auto& f : quad_faces) {
        CHECK(f.type == fea::FaceType::kTri6);
        CHECK(f.nodes.size() == 6);
    }
    // Straight-sided promotion: same surface, so the same integrated area.
    CHECK(fea::integrated_face_area(quadratic, quad_faces) ==
          Approx(fea::integrated_face_area(linear, lin_faces)).epsilon(1e-12));
}

TEST_CASE("traction: tessellation evaluates authoritative quadratic faces",
          "[traction][curved]") {
    auto mesh = promote_to_quadratic(
        box_tet_mesh(1, 1, 1, Eigen::Vector3d(1.0, 1.0, 1.0)));
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto curved =
        std::find_if(faces.begin(), faces.end(), [](const fea::SurfaceFace& face) {
            return face.type == fea::FaceType::kTri6;
        });
    REQUIRE(curved != faces.end());
    REQUIRE(curved->nodes.size() == 6);
    mesh.nodes[curved->nodes[3]].z() += 0.2;

    const auto surface = fea::tessellate_boundary_surface(mesh, 4);
    REQUIRE_FALSE(surface.samples.empty());
    REQUIRE(surface.triangles.size() == faces.size() * 16);
    bool saw_interpolated_position = false;
    for (const auto& sample : surface.samples) {
        double weight_sum = 0.0;
        Eigen::Vector3d reconstructed = Eigen::Vector3d::Zero();
        for (std::size_t i = 0; i < sample.count; ++i) {
            weight_sum += sample.weights[i];
            reconstructed +=
                sample.weights[i] * mesh.nodes[sample.source_nodes[i]];
        }
        CHECK(weight_sum == Approx(1.0).margin(1e-12));
        CHECK((sample.position - reconstructed).norm() < 1e-12);
        const bool is_mesh_node =
            std::any_of(mesh.nodes.begin(), mesh.nodes.end(),
                        [&](const Eigen::Vector3d& node) {
                            return (node - sample.position).norm() < 1e-12;
                        });
        saw_interpolated_position = saw_interpolated_position || !is_mesh_node;
    }
    CHECK(saw_interpolated_position);
}

TEST_CASE("traction: faces_within keeps only fully contained faces", "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto mesh = box_hex_mesh(4, 4, 1, size);
    const auto faces = fea::boundary_surface_faces(mesh);

    auto top = nodes_at_max_z(mesh, size.z());
    const auto top_faces = fea::faces_within(faces, top);
    CHECK(top_faces.size() == 16);
    CHECK(fea::integrated_face_area(mesh, top_faces) ==
          Approx(size.x() * size.y()).epsilon(1e-12));

    // Dropping one node must drop every face touching it (here a corner: 1).
    const auto dropped = top.front();
    top.erase(top.begin());
    const auto reduced = fea::faces_within(faces, top);
    CHECK(reduced.size() < top_faces.size());
    for (const auto& f : reduced) {
        CHECK(std::find(f.nodes.begin(), f.nodes.end(), dropped) == f.nodes.end());
    }
}

TEST_CASE("traction: surface_face_normal is a unit vector normal to the face", "[traction]") {
    const Eigen::Vector3d size(1.0, 2.0, 3.0);
    const auto mesh = box_hex_mesh(1, 1, 1, size);
    const auto faces = fea::boundary_surface_faces(mesh);

    for (const auto& f : faces) {
        const Eigen::Vector3d n = fea::surface_face_normal(mesh, f);
        CHECK(n.norm() == Approx(1.0).epsilon(1e-12));
        for (std::size_t a = 1; a < f.nodes.size(); ++a) {
            const Eigen::Vector3d edge = mesh.nodes[f.nodes[a]] - mesh.nodes[f.nodes[0]];
            CHECK(std::abs(n.dot(edge)) < 1e-12);
        }
        // Box faces are axis-aligned, so the normal picks out exactly one axis.
        CHECK(n.cwiseAbs().maxCoeff() == Approx(1.0).epsilon(1e-12));
    }
}

TEST_CASE("traction: consistent_face_load conserves the requested resultant", "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto mesh = box_hex_mesh(4, 4, 1, size);
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto top_faces = fea::faces_within(faces, nodes_at_max_z(mesh, size.z()));

    const Eigen::Vector3d total(0.0, 0.0, 7854.0);
    const auto load = fea::consistent_face_load(mesh, top_faces, total);

    CHECK(load.area == Approx(size.x() * size.y()).epsilon(1e-12));
    CHECK(load.conservation_error < 1e-9);
    CHECK((nodal_sum(load.loads) - total).norm() < 1e-9);
    // Direction is the traction direction, not the face orientation.
    CHECK(load.resultant.x() == Approx(0.0).margin(1e-9));
    CHECK(load.resultant.y() == Approx(0.0).margin(1e-9));
}

TEST_CASE("traction: consistent load is tributary-area weighted, not evenly split",
          "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto mesh = box_hex_mesh(4, 4, 1, size);
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto top = nodes_at_max_z(mesh, size.z());
    const auto top_faces = fea::faces_within(faces, top);

    const Eigen::Vector3d total(0.0, 0.0, 1000.0);
    const auto load = fea::consistent_face_load(mesh, top_faces, total);

    // On a uniform bilinear quad grid, one quad gives area/4 to each of its
    // nodes: a corner node (1 quad) carries a quarter of an interior node
    // (4 quads), and an edge node (2 quads) a half. Even splitting over the
    // 25 selected nodes would give all of them 40 N.
    auto load_at = [&](double x, double y) {
        for (const auto n : top) {
            if (std::abs(mesh.nodes[n].x() - x) < 1e-12 &&
                std::abs(mesh.nodes[n].y() - y) < 1e-12) {
                return load.loads[3 * static_cast<Eigen::Index>(n) + 2];
            }
        }
        FAIL("no top node at the requested position");
        return 0.0;
    };
    const double interior = load_at(0.5, 0.5);
    const double edge = load_at(0.5, 0.0);
    const double corner = load_at(0.0, 0.0);
    const double quad_area = (size.x() / 4.0) * (size.y() / 4.0);
    const double pressure = total.z() / (size.x() * size.y());
    CHECK(interior == Approx(pressure * quad_area).epsilon(1e-10));
    CHECK(edge == Approx(0.5 * pressure * quad_area).epsilon(1e-10));
    CHECK(corner == Approx(0.25 * pressure * quad_area).epsilon(1e-10));
    CHECK(corner != Approx(total.z() / 25.0).epsilon(1e-3));
}

TEST_CASE("traction: quadratic faces load the mid-side nodes, not the corners",
          "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto linear = box_tet_mesh(2, 2, 1, size);
    const auto n_corner_nodes = linear.nodes.size();
    const auto mesh = promote_to_quadratic(linear);

    const auto faces = fea::boundary_surface_faces(mesh);
    const auto top_faces = fea::faces_within(faces, nodes_at_max_z(mesh, size.z()));
    REQUIRE(!top_faces.empty());
    REQUIRE(top_faces.front().type == fea::FaceType::kTri6);

    const Eigen::Vector3d total(0.0, 0.0, 600.0);
    const auto load = fea::consistent_face_load(mesh, top_faces, total);
    CHECK(load.conservation_error < 1e-9);
    CHECK((nodal_sum(load.loads) - total).norm() < 1e-9);

    // For a straight-sided tri6, integral(N_corner) is exactly 0 and each
    // mid-side node takes area/3 — corner lumping would be plainly wrong.
    double corner_load = 0.0, mid_load = 0.0;
    for (const auto& f : top_faces) {
        for (std::size_t a = 0; a < 3; ++a) {
            corner_load +=
                std::abs(load.loads[3 * static_cast<Eigen::Index>(f.nodes[a]) + 2]);
        }
        for (std::size_t a = 3; a < 6; ++a) {
            REQUIRE(f.nodes[a] >= n_corner_nodes);
            mid_load += load.loads[3 * static_cast<Eigen::Index>(f.nodes[a]) + 2];
        }
    }
    CHECK(corner_load < 1e-9 * total.z());
    CHECK(mid_load > 0.0);
}

TEST_CASE("traction: an empty face set is reported as zero area, not silently loaded",
          "[traction]") {
    const auto mesh = box_hex_mesh(1, 1, 1, Eigen::Vector3d(1.0, 1.0, 1.0));
    const auto load = fea::consistent_face_load(mesh, {}, Eigen::Vector3d(0.0, 0.0, 100.0));
    CHECK(load.area == 0.0);
    CHECK(load.loads.norm() == 0.0);
    CHECK(load.conservation_error == Approx(100.0));
}
