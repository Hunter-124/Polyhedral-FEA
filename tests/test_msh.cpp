// SPDX-License-Identifier: AGPL-3.0-or-later

// Gmsh .msh v2.2 ASCII import: volume connectivity + physical surface groups.

#include "fea/msh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace polymesh::fea;

namespace {

// Single linear tet in the unit tetrahedron, plus one triangular face tagged
// as physical group 7 ("pressure"). Node ids deliberately non-contiguous to
// exercise the id remapping.
constexpr const char* kTinyTetMsh = R"($MeshFormat
2.2 0 8
$EndMeshFormat
$PhysicalNames
1
2 7 "pressure"
$EndPhysicalNames
$Nodes
4
10 0.0 0.0 0.0
20 1.0 0.0 0.0
30 0.0 1.0 0.0
40 0.0 0.0 1.0
$EndNodes
$Elements
2
1 2 2 7 1 10 20 30
2 4 2 1 1 10 20 30 40
$EndElements
)";

// One hex8 unit cube with a quad4 face on z=0 tagged physical 3.
constexpr const char* kTinyHexMsh = R"($MeshFormat
2.2 0 8
$EndMeshFormat
$Nodes
8
1 0 0 0
2 1 0 0
3 1 1 0
4 0 1 0
5 0 0 1
6 1 0 1
7 1 1 1
8 0 1 1
$EndNodes
$Elements
2
1 3 2 3 1 1 2 3 4
2 5 2 1 1 1 2 3 4 5 6 7 8
$EndElements
)";

} // namespace

TEST_CASE("msh v2.2 parses tet4 volume and tagged tri3 face") {
    const auto model = parse_msh(kTinyTetMsh);
    REQUIRE(model.mesh.nodes.size() == 4);
    REQUIRE(model.mesh.elements.size() == 1);
    CHECK(model.mesh.elements[0].type == ElementType::kTet4);
    CHECK(model.mesh.elements[0].nodes == std::vector<std::uint32_t>({0, 1, 2, 3}));
    REQUIRE(model.physical_faces.count(7) == 1);
    REQUIRE(model.physical_faces.at(7).size() == 1);
    CHECK(model.physical_faces.at(7)[0].type == FaceType::kTri3);
    CHECK(model.physical_names.at(7) == "pressure");
    // Geometry of the unit tet is intact after id remap.
    CHECK(model.mesh.nodes[0].isApprox(Eigen::Vector3d(0, 0, 0)));
    CHECK(model.mesh.nodes[3].isApprox(Eigen::Vector3d(0, 0, 1)));
}

TEST_CASE("msh v2.2 parses hex8 volume and tagged quad4 face") {
    const auto model = parse_msh(kTinyHexMsh);
    REQUIRE(model.mesh.elements.size() == 1);
    CHECK(model.mesh.elements[0].type == ElementType::kHex8);
    REQUIRE(model.physical_faces.at(3).size() == 1);
    CHECK(model.physical_faces.at(3)[0].type == FaceType::kQuad4);
    CHECK(model.physical_faces.at(3)[0].nodes.size() == 4);
}

TEST_CASE("msh rejects binary and v4 formats") {
    REQUIRE_THROWS_AS(parse_msh("$MeshFormat\n4.1 0 8\n$EndMeshFormat\n"), FeaError);
    REQUIRE_THROWS_AS(parse_msh("$MeshFormat\n2.2 1 8\n$EndMeshFormat\n"), FeaError);
}

TEST_CASE("msh rejects meshes with no volume elements") {
    constexpr const char* kSurfaceOnly = R"($MeshFormat
2.2 0 8
$EndMeshFormat
$Nodes
3
1 0 0 0
2 1 0 0
3 0 1 0
$EndNodes
$Elements
1
1 2 2 1 1 1 2 3
$EndElements
)";
    REQUIRE_THROWS_AS(parse_msh(kSurfaceOnly), FeaError);
}
