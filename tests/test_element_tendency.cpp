// SPDX-License-Identifier: BSD-3-Clause
// DAG node mesher-tendency: continuous element_tendency dial → mesher choice
// and cell-kind mix (deterministic Cartesian fills).

#include "fea/nodal_mesh.hpp"
#include "geom/tri_surface.hpp"
#include "pipeline/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using polymesh::fea::ElementType;
using polymesh::pipeline::ElementTendencyPlan;
using polymesh::pipeline::Model;
using polymesh::pipeline::resolve_element_tendency;
using polymesh::pipeline::volume_mesh;
using polymesh::pipeline::VolumeMesher;

namespace {

polymesh::geom::TriSurface unit_box_surface() {
    polymesh::geom::TriSurface s;
    s.vertices = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1},
    };
    s.triangles = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
        {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
    };
    return s;
}

Model unit_box_model() {
    Model m;
    m.surface = unit_box_surface();
    m.bbox_min = {-0.05, -0.05, -0.05};
    m.bbox_max = {1.05, 1.05, 1.05};
    m.triangle_region.assign(12, 0);
    m.region_count = 1;
    m.name = "unit_box";
    return m;
}

struct KindCounts {
    std::size_t hex = 0;
    std::size_t tet = 0;
    std::size_t pyr = 0;
    std::size_t vem = 0;
    std::size_t other = 0;
};

KindCounts count_kinds(const polymesh::fea::NodalMesh& mesh) {
    KindCounts c;
    for (const auto& el : mesh.elements) {
        switch (el.type) {
        case ElementType::kHex8:
        case ElementType::kHex20:
            ++c.hex;
            break;
        case ElementType::kTet4:
        case ElementType::kTet10:
            ++c.tet;
            break;
        case ElementType::kPyramid5:
            ++c.pyr;
            break;
        case ElementType::kPolyVem:
            ++c.vem;
            break;
        default:
            ++c.other;
            break;
        }
    }
    return c;
}

} // namespace

TEST_CASE("resolve_element_tendency: zero preserves base mesher", "[tendency]") {
    const auto hybrid = resolve_element_tendency(VolumeMesher::kHybrid, 0.0, 2);
    REQUIRE(hybrid.mesher == VolumeMesher::kHybrid);
    REQUIRE_FALSE(hybrid.native_poly_transitions);
    REQUIRE_FALSE(hybrid.remapped);
    REQUIRE(hybrid.skin_layers == 2);
    REQUIRE(std::string(hybrid.label) == "hybrid-fan");

    const auto vem = resolve_element_tendency(VolumeMesher::kHybridVem, 0.0, 2);
    REQUIRE(vem.mesher == VolumeMesher::kHybridVem);
    REQUIRE(vem.native_poly_transitions);
    REQUIRE_FALSE(vem.remapped);
    REQUIRE(std::string(vem.label) == "hybrid-vem");

    const auto hex = resolve_element_tendency(VolumeMesher::kHexFill, 0.0, 2);
    REQUIRE(hex.mesher == VolumeMesher::kHexFill);
    REQUIRE_FALSE(hex.remapped);
}

TEST_CASE("resolve_element_tendency: hybrid-family thresholds", "[tendency]") {
    // Strong hex
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybrid, -1.0, 2);
        REQUIRE(p.mesher == VolumeMesher::kHexFill);
        REQUIRE(p.remapped);
        REQUIRE(std::string(p.label) == "hex");
        REQUIRE_FALSE(p.native_poly_transitions);
    }
    // Mild hex → still fan hybrid, thinner skin
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybrid, -0.3, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybrid);
        REQUIRE_FALSE(p.native_poly_transitions);
        REQUIRE(p.skin_layers == 1);
        REQUIRE(p.remapped); // skin thinned
    }
    // Boundary of fan band
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybrid, 0.25, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybrid);
        REQUIRE_FALSE(p.native_poly_transitions);
    }
    // Native poly VEM band
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybrid, 0.5, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybridVem);
        REQUIRE(p.native_poly_transitions);
        REQUIRE(p.remapped);
        REQUIRE(std::string(p.label) == "hybrid-vem");
    }
    // Strong tet
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybrid, 1.0, 2);
        REQUIRE(p.mesher == VolumeMesher::kGradedTet);
        REQUIRE(p.skin_layers == 3); // +1 skin hop
        REQUIRE(p.remapped);
        REQUIRE(std::string(p.label) == "graded-tet");
    }
    // kHybridVem base + mild negative drops to fan
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHybridVem, -0.2, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybrid);
        REQUIRE_FALSE(p.native_poly_transitions);
        REQUIRE(p.remapped);
    }
}

TEST_CASE("resolve_element_tendency: clamps and preserves specialty meshers", "[tendency]") {
    const auto lo = resolve_element_tendency(VolumeMesher::kHybrid, -5.0, 2);
    REQUIRE(lo.tendency == -1.0);
    REQUIRE(lo.mesher == VolumeMesher::kHexFill);

    const auto hi = resolve_element_tendency(VolumeMesher::kHybrid, 5.0, 2);
    REQUIRE(hi.tendency == 1.0);
    REQUIRE(hi.mesher == VolumeMesher::kGradedTet);

    // Prism / octa / hexpyr: tendency ignored
    for (auto base : {VolumeMesher::kPrismSweep, VolumeMesher::kOctahedral,
                      VolumeMesher::kHexPyramid}) {
        const auto p = resolve_element_tendency(base, 1.0, 2);
        REQUIRE(p.mesher == base);
        REQUIRE_FALSE(p.remapped);
    }
}

TEST_CASE("resolve_element_tendency: hex/tet family soft remap", "[tendency]") {
    {
        const auto p = resolve_element_tendency(VolumeMesher::kHexFill, 0.5, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybridVem);
        REQUIRE(p.native_poly_transitions);
    }
    {
        const auto p = resolve_element_tendency(VolumeMesher::kGradedTet, -1.0, 2);
        REQUIRE(p.mesher == VolumeMesher::kHexFill);
    }
    {
        const auto p = resolve_element_tendency(VolumeMesher::kTetFill, 0.1, 2);
        REQUIRE(p.mesher == VolumeMesher::kHybrid);
    }
}

TEST_CASE("volume_mesh: element_tendency changes cell-kind mix", "[tendency]") {
    const auto model = unit_box_model();
    // Match fe-vem-assembly gate params so 2:1 transitions are guaranteed.
    constexpr double h = 0.2;
    const std::vector<Eigen::Vector3d> seeds{{0.5, 0.5, 0.5}};
    constexpr double seed_band = 0.4;

    auto vol_hex = volume_mesh(model, h, VolumeMesher::kHybrid, 1, false, seeds, seed_band,
                               /*element_tendency=*/-1.0);
    auto vol_fan = volume_mesh(model, h, VolumeMesher::kHybrid, 1, false, seeds, seed_band,
                               /*element_tendency=*/0.0);
    auto vol_vem = volume_mesh(model, h, VolumeMesher::kHybrid, 1, false, seeds, seed_band,
                               /*element_tendency=*/0.5);
    auto vol_tet = volume_mesh(model, h, VolumeMesher::kHybrid, 1, false, seeds, seed_band,
                               /*element_tendency=*/1.0);

    REQUIRE_FALSE(vol_hex.mesh.elements.empty());
    REQUIRE_FALSE(vol_fan.mesh.elements.empty());
    REQUIRE_FALSE(vol_vem.mesh.elements.empty());
    REQUIRE_FALSE(vol_tet.mesh.elements.empty());
    REQUIRE_NOTHROW(vol_hex.mesh.check_validity());
    REQUIRE_NOTHROW(vol_fan.mesh.check_validity());
    REQUIRE_NOTHROW(vol_vem.mesh.check_validity());
    REQUIRE_NOTHROW(vol_tet.mesh.check_validity());

    const auto c_hex = count_kinds(vol_hex.mesh);
    const auto c_fan = count_kinds(vol_fan.mesh);
    const auto c_vem = count_kinds(vol_vem.mesh);
    const auto c_tet = count_kinds(vol_tet.mesh);

    INFO("hex note: " << vol_hex.mesher_note);
    INFO("fan note: " << vol_fan.mesher_note);
    INFO("vem note: " << vol_vem.mesher_note);
    INFO("tet note: " << vol_tet.mesher_note);
    INFO("counts hex path: hex=" << c_hex.hex << " pyr=" << c_hex.pyr << " tet=" << c_hex.tet
                                 << " vem=" << c_hex.vem);
    INFO("counts fan path: hex=" << c_fan.hex << " pyr=" << c_fan.pyr << " tet=" << c_fan.tet
                                 << " vem=" << c_fan.vem);
    INFO("counts vem path: hex=" << c_vem.hex << " pyr=" << c_vem.pyr << " tet=" << c_vem.tet
                                 << " vem=" << c_vem.vem);
    INFO("counts tet path: hex=" << c_tet.hex << " pyr=" << c_tet.pyr << " tet=" << c_tet.tet
                                 << " vem=" << c_tet.vem);

    // Strong hex: Cartesian hex fill (all hex8, no pyramid product expand).
    REQUIRE(c_hex.hex > 0);
    REQUIRE(c_hex.pyr == 0);
    REQUIRE(c_hex.vem == 0);
    REQUIRE(vol_hex.mesher_note.find("element_tendency") != std::string::npos);
    REQUIRE(vol_hex.mesher_note.find("→hex") != std::string::npos);

    // Default hybrid fan: product-FE all-pyramid expand (no leftover hex FE).
    REQUIRE(c_fan.pyr > 0);
    REQUIRE(c_fan.hex == 0);
    REQUIRE(c_fan.vem == 0);

    // Poly tendency: hybrid-VEM keeps hex FE + native poly VEM transitions.
    REQUIRE(c_vem.hex > 0);
    REQUIRE(c_vem.vem > 0);
    REQUIRE(c_vem.pyr == 0);
    REQUIRE(vol_vem.mesher_note.find("hybrid-VEM") != std::string::npos);
    REQUIRE(vol_vem.mesher_note.find("element_tendency") != std::string::npos);

    // Strong tet: graded tet path (all tets).
    REQUIRE(c_tet.tet > 0);
    REQUIRE(c_tet.hex == 0);
    REQUIRE(c_tet.vem == 0);
    REQUIRE(c_tet.pyr == 0);
    REQUIRE(vol_tet.mesher_note.find("element_tendency") != std::string::npos);

    // Cell mixes differ across the dial (not just notes).
    REQUIRE(c_vem.vem > 0);
    REQUIRE(c_fan.vem == 0);
    REQUIRE(c_tet.tet != c_hex.hex);
}

TEST_CASE("SimSetup.element_tendency defaults to 0", "[tendency]") {
    polymesh::pipeline::SimSetup s;
    REQUIRE(s.element_tendency == 0.0);
    REQUIRE(s.mesher == VolumeMesher::kHybrid);
}
