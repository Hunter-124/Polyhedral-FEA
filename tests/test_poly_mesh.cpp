// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/poly_mesh.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace polymesh::mesh;

namespace {

/// Single tetrahedron with four boundary faces.
PolyMesh single_tet() {
    PolyMesh m;
    m.vertices = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    for (const auto& vs : {std::vector<VertexId>{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}) {
        m.faces.push_back(Face{.vertices = vs, .owner = 0, .neighbour = {}});
    }
    m.cells.push_back(Cell{.kind = CellKind::kTet, .faces = {0, 1, 2, 3}});
    return m;
}

PolyMesh single_poly_cube() {
    PolyMesh m;
    m.vertices = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}, {1.0, 1.0, 1.0}, {0.0, 1.0, 1.0},
    };
    const std::array<std::array<VertexId, 4>, 6> loops{{
        {0, 3, 2, 1},
        {4, 5, 6, 7},
        {0, 1, 5, 4},
        {1, 2, 6, 5},
        {2, 3, 7, 6},
        {3, 0, 4, 7},
    }};
    for (const auto& loop : loops) {
        m.faces.push_back(Face{.vertices = {loop[0], loop[1], loop[2], loop[3]},
                               .owner = 0,
                               .neighbour = std::nullopt});
    }
    m.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {0, 1, 2, 3, 4, 5}});
    return m;
}

void append_poly_cube(PolyMesh& mesh, const Eigen::Vector3d& origin, double size) {
    PolyMesh cube = single_poly_cube();
    const VertexId vertex_offset = static_cast<VertexId>(mesh.vertices.size());
    const FaceId face_offset = static_cast<FaceId>(mesh.faces.size());
    const CellId cell_id = static_cast<CellId>(mesh.cells.size());
    for (Eigen::Vector3d vertex : cube.vertices) {
        mesh.vertices.push_back(origin + size * vertex);
    }
    for (Face face : cube.faces) {
        for (VertexId& vertex : face.vertices) {
            vertex += vertex_offset;
        }
        face.owner = cell_id;
        mesh.faces.push_back(std::move(face));
    }
    Cell cell = cube.cells.front();
    for (FaceId& face : cell.faces) {
        face += face_offset;
    }
    mesh.cells.push_back(std::move(cell));
}

PolyMesh two_adjacent_poly_cubes() {
    PolyMesh mesh;
    mesh.vertices = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {1.0, 1.0, 0.0}, {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0}, {1.0, 0.0, 1.0}, {1.0, 1.0, 1.0}, {0.0, 1.0, 1.0},
        {2.0, 0.0, 0.0}, {2.0, 1.0, 0.0}, {2.0, 0.0, 1.0}, {2.0, 1.0, 1.0},
    };
    const auto add_face = [&](std::initializer_list<VertexId> vertices, CellId owner,
                              std::optional<CellId> neighbour = std::nullopt) {
        mesh.faces.push_back(
            Face{.vertices = vertices, .owner = owner, .neighbour = neighbour});
    };
    add_face({0, 3, 2, 1}, 0);
    add_face({4, 5, 6, 7}, 0);
    add_face({0, 1, 5, 4}, 0);
    add_face({1, 2, 6, 5}, 0, 1);
    add_face({2, 3, 7, 6}, 0);
    add_face({3, 0, 4, 7}, 0);
    add_face({1, 2, 9, 8}, 1);
    add_face({5, 10, 11, 6}, 1);
    add_face({1, 8, 10, 5}, 1);
    add_face({8, 9, 11, 10}, 1);
    add_face({9, 2, 6, 11}, 1);
    mesh.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {0, 1, 2, 3, 4, 5}});
    mesh.cells.push_back(Cell{.kind = CellKind::kPolyhedron, .faces = {3, 6, 7, 8, 9, 10}});
    return mesh;
}

} // namespace

TEST_CASE("single tet is valid") { REQUIRE_NOTHROW(single_tet().check_validity()); }

TEST_CASE("dangling vertex ref is caught") {
    auto m = single_tet();
    m.faces[0].vertices[0] = 99;
    REQUIRE_THROWS_MATCHES(m.check_validity(), ValidityError,
                           Catch::Matchers::MessageMatches(
                               Catch::Matchers::ContainsSubstring("out-of-range vertex 99")));
}

TEST_CASE("ownership mismatch is caught") {
    auto m = single_tet();
    // Add a second cell claiming face 0, which doesn't reference it back.
    m.cells.push_back(Cell{.kind = CellKind::kTet, .faces = {0, 1, 2, 3}});
    REQUIRE_THROWS_MATCHES(m.check_validity(), ValidityError,
                           Catch::Matchers::MessageMatches(Catch::Matchers::ContainsSubstring(
                               "does not reference it back")));
}

TEST_CASE("poly geometry admission accepts an oriented cube") {
    REQUIRE_NOTHROW(single_poly_cube().check_geometry());
}

TEST_CASE("poly geometry admission accepts adjacent cells sharing a face") {
    REQUIRE_NOTHROW(two_adjacent_poly_cubes().check_geometry());
}

TEST_CASE("poly geometry admission rejects crossing and contained cells") {
    PolyMesh crossing;
    append_poly_cube(crossing, {0.0, 0.0, 0.0}, 1.0);
    append_poly_cube(crossing, {0.5, 0.25, 0.25}, 1.0);
    REQUIRE_THROWS(crossing.check_geometry());

    PolyMesh contained;
    append_poly_cube(contained, {0.0, 0.0, 0.0}, 2.0);
    append_poly_cube(contained, {0.5, 0.5, 0.5}, 0.5);
    REQUIRE_THROWS(contained.check_geometry());
}

TEST_CASE("poly geometry admission is rigid-translation robust at large coordinates") {
    const Eigen::Vector3d offset(1.0e9, -1.0e9, 1.0e9);

    auto valid = single_poly_cube();
    for (Eigen::Vector3d& vertex : valid.vertices) {
        vertex += offset;
    }
    REQUIRE_NOTHROW(valid.check_geometry());

    PolyMesh crossing;
    append_poly_cube(crossing, offset, 1.0);
    append_poly_cube(crossing, offset + Eigen::Vector3d(0.5, 0.25, 0.25), 1.0);
    REQUIRE_THROWS(crossing.check_geometry());

    PolyMesh contained;
    append_poly_cube(contained, offset, 2.0);
    append_poly_cube(contained, offset + Eigen::Vector3d(0.5, 0.5, 0.5), 0.5);
    REQUIRE_THROWS(contained.check_geometry());
}

TEST_CASE("poly geometry admission rejects nonplanar and self-crossing faces") {
    auto nonplanar = single_poly_cube();
    nonplanar.vertices[6].z() = 1.2;
    REQUIRE_THROWS(nonplanar.check_geometry());

    auto repeated = single_poly_cube();
    repeated.faces[1].vertices.insert(repeated.faces[1].vertices.begin() + 2, 5);
    REQUIRE_THROWS(repeated.check_geometry());

    auto bow_tie = single_poly_cube();
    bow_tie.faces[1].vertices = {4, 6, 5, 7};
    REQUIRE_THROWS(bow_tie.check_geometry());
}

TEST_CASE("poly geometry admission rejects inconsistent and negative shells") {
    auto inconsistent = single_poly_cube();
    std::reverse(inconsistent.faces[1].vertices.begin(), inconsistent.faces[1].vertices.end());
    REQUIRE_THROWS(inconsistent.check_geometry());

    auto negative = single_poly_cube();
    for (auto& face : negative.faces) {
        std::reverse(face.vertices.begin(), face.vertices.end());
    }
    REQUIRE_THROWS(negative.check_geometry());
}

TEST_CASE("boundary-incident triangulation preserves a valid polyhedron") {
    auto cube = single_poly_cube();
    cube.triangulate_boundary_incident_faces();
    REQUIRE(cube.faces.size() == 12);
    REQUIRE_NOTHROW(cube.check_geometry());
}
