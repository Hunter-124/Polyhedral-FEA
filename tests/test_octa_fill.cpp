// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/octa_fill.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

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

TEST_CASE("octa_fill unit box produces tets and boundary") {
    const auto s = unit_box();
    auto fill = octa_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.25, false);
    REQUIRE_FALSE(fill.mesh.tets.empty());
    REQUIRE(fill.n_octahedra > 0);
    REQUIRE(fill.n_boundary_pyramids > 0);
    REQUIRE_FALSE(fill.mesh.boundary_quads.empty());
    REQUIRE_NOTHROW(check_tet_fill_geometry(fill.mesh));
}

TEST_CASE("pipeline octahedral experimental mesher smoke") {
    pipeline::Model m;
    m.surface = unit_box();
    m.bbox_min = {0, 0, 0};
    m.bbox_max = {1, 1, 1};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    auto vol = pipeline::volume_mesh(m, 0.25, pipeline::VolumeMesher::kOctahedral);
    REQUIRE_FALSE(vol.mesh.elements.empty());
    REQUIRE_NOTHROW(vol.mesh.check_validity());
    REQUIRE(vol.mesher_note.find("octahedral experimental") != std::string::npos);
    for (const auto& el : vol.mesh.elements) {
        REQUIRE(el.type == fea::ElementType::kTet4);
    }
}

TEST_CASE("octa fill patch-testable volume on unit box") {
    auto fill = octa_fill_surface(unit_box(), {0, 0, 0}, {1, 1, 1}, 0.5, false);
    double vol = 0.0;
    for (const auto& t : fill.mesh.tets) {
        vol += std::abs(tet_signed_volume(fill.mesh.nodes[t[0]], fill.mesh.nodes[t[1]],
                                          fill.mesh.nodes[t[2]], fill.mesh.nodes[t[3]]));
    }
    REQUIRE(std::abs(vol - 1.0) < 1e-9);
}
