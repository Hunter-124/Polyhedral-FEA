// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/poly_mesh.hpp"

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
