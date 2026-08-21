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
    const double expected =
        2.0 * (size.x() * size.y() + size.y() * size.z() + size.z() * size.x());
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
    auto mesh = promote_to_quadratic(box_tet_mesh(1, 1, 1, Eigen::Vector3d(1.0, 1.0, 1.0)));
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
            reconstructed += sample.weights[i] * mesh.nodes[sample.source_nodes[i]];
        }
        CHECK(weight_sum == Approx(1.0).margin(1e-12));
        CHECK((sample.position - reconstructed).norm() < 1e-12);
        const bool is_mesh_node = std::any_of(
            mesh.nodes.begin(), mesh.nodes.end(), [&](const Eigen::Vector3d& node) {
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

TEST_CASE("traction: quadratic faces load the mid-side nodes, not the corners", "[traction]") {
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
            corner_load += std::abs(load.loads[3 * static_cast<Eigen::Index>(f.nodes[a]) + 2]);
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

// A box selection names a region of the boundary surface. The whole-face rule
// ("accept a face when every node is inside") stops the loaded patch on a
// staircase of element edges, so the applied traction depends on the tiling; the
// region integrators clip to the box instead. These tests pin that difference.
namespace {

// Nodes of `mesh` inside `region` — what --fix-box / --load-box select.
std::vector<std::uint32_t> nodes_in_region(const fea::NodalMesh& mesh,
                                           const fea::LoadRegion& region) {
    std::vector<std::uint32_t> out;
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d& p = mesh.nodes[i];
        if ((p.array() >= region.lo.array()).all() && (p.array() <= region.hi.array()).all()) {
            out.push_back(i);
        }
    }
    return out;
}

} // namespace

TEST_CASE("traction: a load region is integrated at the box plane, not at a face staircase",
          "[traction]") {
    // Top face z = 1 of the unit box, cut at x = 0.375. On a 4x4 tiling that
    // plane runs through the middle of the second column of quads, so the
    // whole-face rule can only reach x = 0.25.
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const fea::LoadRegion region{{-0.5, -0.5, 1.0}, {0.375, 1.5, 1.5}};
    const double exact = 0.375 * size.y();

    const auto mesh = box_hex_mesh(4, 4, 2, size);
    const auto faces = fea::boundary_surface_faces(mesh);

    const auto whole = fea::faces_within(faces, nodes_in_region(mesh, region));
    CHECK(fea::integrated_face_area(mesh, whole) == Approx(0.25 * size.y()).epsilon(1e-12));

    const auto touching = fea::faces_touching(mesh, faces, region);
    CHECK(touching.size() > whole.size());
    CHECK(fea::integrated_region_area(mesh, touching, region) == Approx(exact).epsilon(1e-12));

    // Same region, three tilings none of whose element edges land on the plane:
    // the clipped area is a property of the region, the staircase is not.
    for (const int n : {3, 5, 7}) {
        const auto other = box_hex_mesh(n, n, 2, size);
        const auto other_faces = fea::boundary_surface_faces(other);
        CHECK(fea::integrated_region_area(other,
                                          fea::faces_touching(other, other_faces, region),
                                          region) == Approx(exact).epsilon(1e-12));
        CHECK(fea::integrated_face_area(
                  other, fea::faces_within(other_faces, nodes_in_region(other, region))) !=
              Approx(exact).epsilon(1e-6));
    }
}

TEST_CASE("traction: a region load conserves the resultant and only loads the region",
          "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const fea::LoadRegion region{{-0.5, -0.5, 1.0}, {0.375, 1.5, 1.5}};
    const auto mesh = box_hex_mesh(4, 4, 2, size);
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto touching = fea::faces_touching(mesh, faces, region);

    const Eigen::Vector3d total(0.0, 0.0, -1234.5);
    const auto load = fea::consistent_region_load(mesh, touching, region, total);
    CHECK(load.area == Approx(0.375).epsilon(1e-12));
    CHECK(load.conservation_error < 1e-9);
    CHECK((nodal_sum(load.loads) - total).norm() < 1e-9);

    // The traction is uniform over the clipped patch, so a node's share is the
    // integral of its shape function over that patch: strictly positive for the
    // nodes bounding the cut column and exactly zero beyond it.
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d& p = mesh.nodes[i];
        const bool reachable = p.z() > 1.0 - 1e-12 && p.x() < 0.5 + 1e-12;
        if (!reachable) {
            CHECK(load.loads.segment<3>(3 * static_cast<Eigen::Index>(i)).norm() == 0.0);
        }
    }
}

TEST_CASE("traction: a region that contains a face integrates it exactly as before",
          "[traction]") {
    // The clip is a no-op on faces it does not cut, including the quadratic ones:
    // a region swallowing the whole top face must reproduce consistent_face_load
    // to round-off, so nothing that already worked moved.
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto mesh = promote_to_quadratic(box_tet_mesh(3, 3, 2, size));
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto top = fea::faces_within(faces, nodes_at_max_z(mesh, size.z()));
    REQUIRE(!top.empty());
    REQUIRE(top.front().type == fea::FaceType::kTri6);

    const fea::LoadRegion region{{-1.0, -1.0, 1.0}, {2.0, 2.0, 2.0}};
    const Eigen::Vector3d total(0.0, 0.0, 600.0);
    const auto plain = fea::consistent_face_load(mesh, top, total);
    const auto clipped = fea::consistent_region_load(mesh, top, region, total);
    CHECK(clipped.area == Approx(plain.area).epsilon(1e-14));
    CHECK((clipped.loads - plain.loads).norm() <= 1e-12 * plain.loads.norm());
}

// A fixture selection box names a region of the boundary surface too, not a
// volume of material to freeze. These pin the difference, because the volume
// rule is silently wrong rather than loudly wrong: it solves, and the answer is
// a rigid inclusion with a mesh-shaped boundary.
TEST_CASE("traction: a boundary selection never reaches an interior node", "[traction]") {
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const auto mesh = promote_to_quadratic(box_tet_mesh(4, 4, 4, size));
    const auto faces = fea::boundary_surface_faces(mesh);
    const auto boundary = fea::boundary_face_nodes(faces);
    REQUIRE(boundary.size() < mesh.nodes.size()); // there ARE interior nodes

    // A slab through the lower half of the box: the volume rule reaches material
    // the surface rule cannot.
    const fea::LoadRegion slab{{-1.0, -1.0, -1.0}, {2.0, 2.0, 0.5}};
    const auto surface_sel = fea::boundary_nodes_within(mesh, faces, slab);
    const auto volume_sel = nodes_in_region(mesh, slab);
    REQUIRE(!surface_sel.empty());
    CHECK(surface_sel.size() < volume_sel.size());
    CHECK(std::is_sorted(surface_sel.begin(), surface_sel.end()));
    for (const auto id : surface_sel) {
        CHECK(std::binary_search(boundary.begin(), boundary.end(), id));
        const Eigen::Vector3d& p = mesh.nodes[id];
        CHECK((p.array() >= slab.lo.array()).all());
        CHECK((p.array() <= slab.hi.array()).all());
    }
    // Every boundary node the volume rule would have taken is taken.
    for (const auto id : volume_sel) {
        if (std::binary_search(boundary.begin(), boundary.end(), id)) {
            CHECK(std::binary_search(surface_sel.begin(), surface_sel.end(), id));
        }
    }
}

TEST_CASE("traction: a boundary selection leaves no element fully constrained", "[traction]") {
    // The property that decides whether a solve has a rigid inclusion in it: an
    // element whose every node is prescribed has identically zero strain, so its
    // stress is identically zero however the rest of the part is loaded, and the
    // union of such elements ends on a one-element staircase set by the tiling.
    const Eigen::Vector3d size(1.0, 1.0, 1.0);
    const fea::LoadRegion slab{{-1.0, -1.0, -1.0}, {2.0, 2.0, 0.5}};
    for (const int n : {3, 4, 6}) {
        const auto mesh = promote_to_quadratic(box_tet_mesh(n, n, n, size));
        const auto faces = fea::boundary_surface_faces(mesh);
        const auto surface_sel = fea::boundary_nodes_within(mesh, faces, slab);
        const auto volume_sel = nodes_in_region(mesh, slab);

        const auto fully_constrained = [&](const std::vector<std::uint32_t>& sel) {
            std::size_t count = 0;
            for (const auto& el : mesh.elements) {
                const bool all_in =
                    std::all_of(el.nodes.begin(), el.nodes.end(), [&](std::uint32_t id) {
                        return std::binary_search(sel.begin(), sel.end(), id);
                    });
                count += all_in ? 1 : 0;
            }
            return count;
        };
        CHECK(fully_constrained(volume_sel) > 0);
        CHECK(fully_constrained(surface_sel) == 0);
    }
}
