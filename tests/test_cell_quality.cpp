// SPDX-License-Identifier: BSD-3-Clause
// Contract tests for measured per-cell quality (fea::cell_quality) and for the
// dimensionless relative ZZ error indicator. Both defended behaviours used to be
// fabricated constants: quality 1.0 for every non-tet cell, and a Pa-valued
// root-sum-square global η that grew as sqrt(N).

#include "adapt/error.hpp"
#include "fea/cell_quality.hpp"
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/zz.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using Catch::Approx;
using polymesh::fea::cell_quality;
using polymesh::fea::ElementType;
using polymesh::fea::NodalElement;
using polymesh::fea::NodalMesh;

namespace {

/// Single-cell mesh from explicit node coordinates.
NodalMesh one_cell(ElementType type, const std::vector<Eigen::Vector3d>& nodes,
                   std::vector<std::vector<std::uint32_t>> faces = {}) {
    NodalMesh m;
    m.nodes = nodes;
    NodalElement el;
    el.type = type;
    el.nodes.resize(nodes.size());
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        el.nodes[i] = i;
    }
    el.faces = std::move(faces);
    m.elements.push_back(el);
    return m;
}

std::vector<Eigen::Vector3d> box_nodes(double sx, double sy, double sz) {
    return {{0, 0, 0},  {sx, 0, 0},  {sx, sy, 0},  {0, sy, 0},
            {0, 0, sz}, {sx, 0, sz}, {sx, sy, sz}, {0, sy, sz}};
}

/// Outward quad faces of the canonical hex node ordering.
std::vector<std::vector<std::uint32_t>> box_faces() {
    return {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
            {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
}

/// Cantilever on a structured hex grid; returns (mesh, u) for ZZ tests.
struct Cantilever {
    NodalMesh mesh;
    Eigen::VectorXd u;
};

Cantilever cantilever(int nx, double load, double youngs) {
    Cantilever c;
    c.mesh = polymesh::test_support::box_hex_mesh(nx, 2, 2, {1.0, 0.2, 0.2});
    const polymesh::fea::Material mat{.youngs_modulus = youngs, .poissons_ratio = 0.3};
    polymesh::fea::Dirichlet bc;
    for (std::uint32_t i = 0; i < c.mesh.nodes.size(); ++i) {
        if (c.mesh.nodes[i][0] < 1e-9) {
            bc.fix_node(i);
        }
    }
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(c.mesh.nodes.size()));
    for (std::uint32_t i = 0; i < c.mesh.nodes.size(); ++i) {
        if (c.mesh.nodes[i][0] > 1.0 - 1e-9) {
            loads(3 * static_cast<Eigen::Index>(i) + 1) = load;
        }
    }
    c.u = polymesh::fea::solve_elastostatics(c.mesh, mat, bc, loads);
    return c;
}

} // namespace

TEST_CASE("cell_quality: regular cells score 1, degenerate cells do not") {
    // hex8: unit cube is the ideal cell.
    const auto cube = one_cell(ElementType::kHex8, box_nodes(1.0, 1.0, 1.0));
    REQUIRE(cell_quality(cube, cube.elements[0]) == Approx(1.0).margin(1e-12));

    // hex8 sheared until the top face crosses the bottom: inverted ⇒ negative.
    auto inverted = one_cell(ElementType::kHex8, box_nodes(1.0, 1.0, 1.0));
    for (std::size_t i = 4; i < 8; ++i) {
        inverted.nodes[i][2] = -1.0;
    }
    REQUIRE(cell_quality(inverted, inverted.elements[0]) < 0.0);

    // hex8 with a collapsed edge: measured as fully degenerate, never 1.0.
    auto collapsed = one_cell(ElementType::kHex8, box_nodes(1.0, 1.0, 1.0));
    collapsed.nodes[1] = collapsed.nodes[0];
    REQUIRE(cell_quality(collapsed, collapsed.elements[0]) == Approx(0.0).margin(1e-12));

    // hex8 skewed by 45° at every corner: strictly between 0 and 1.
    auto skewed = one_cell(ElementType::kHex8, box_nodes(1.0, 1.0, 1.0));
    for (std::size_t i = 4; i < 8; ++i) {
        skewed.nodes[i][0] += 1.0;
    }
    const double q_skew = cell_quality(skewed, skewed.elements[0]);
    REQUIRE(q_skew > 0.0);
    REQUIRE(q_skew < 0.75);

    // tet4: regular tet scores 1, sliver collapses toward 0.
    const auto reg_tet =
        one_cell(ElementType::kTet4, {{0, 0, 0},
                                      {1, 0, 0},
                                      {0.5, 0.8660254037844386, 0},
                                      {0.5, 0.28867513459481287, 0.816496580927726}});
    REQUIRE(cell_quality(reg_tet, reg_tet.elements[0]) == Approx(1.0).margin(1e-9));
    const auto sliver =
        one_cell(ElementType::kTet4, {{0, 0, 0}, {1, 0, 0}, {0.5, 1, 0}, {0.5, 0.5, 1e-4}});
    REQUIRE(cell_quality(sliver, sliver.elements[0]) < 0.01);

    // prism6: equilateral-base prism is the ideal; the right-triangle prism the
    // hybrid fills emit is sin45/sin60 = 0.8165.
    const double s3 = 0.8660254037844386;
    const auto equi_prism =
        one_cell(ElementType::kPrism6,
                 {{0, 0, 0}, {1, 0, 0}, {0.5, s3, 0}, {0, 0, 1}, {1, 0, 1}, {0.5, s3, 1}});
    REQUIRE(cell_quality(equi_prism, equi_prism.elements[0]) == Approx(1.0).margin(1e-12));
    const auto right_prism =
        one_cell(ElementType::kPrism6,
                 {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}});
    REQUIRE(cell_quality(right_prism, right_prism.elements[0]) ==
            Approx(0.816496580927726).margin(1e-9));

    // pyramid5: all-edges-equal pyramid is the ideal (apex at a/√2).
    const auto reg_pyr =
        one_cell(ElementType::kPyramid5,
                 {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5, 0.5, 0.7071067811865476}});
    REQUIRE(cell_quality(reg_pyr, reg_pyr.elements[0]) == Approx(1.0).margin(1e-12));
    // Squashed pyramid: apex almost in the base plane.
    auto flat_pyr = reg_pyr;
    flat_pyr.nodes[4][2] = 1e-4;
    REQUIRE(cell_quality(flat_pyr, flat_pyr.elements[0]) < 0.01);

    // kPolyVem: cube-shaped cell ideal, stretched box penalized, inverted zeroed.
    const auto vem_cube =
        one_cell(ElementType::kPolyVem, box_nodes(1.0, 1.0, 1.0), box_faces());
    REQUIRE(cell_quality(vem_cube, vem_cube.elements[0]) == Approx(1.0).margin(1e-12));
    auto split_edge_nodes = box_nodes(1.0, 1.0, 1.0);
    split_edge_nodes.push_back({0.5, 0.0, 0.0});
    auto split_edge_faces = box_faces();
    split_edge_faces[0] = {0, 3, 2, 1, 8};
    split_edge_faces[2] = {0, 8, 1, 5, 4};
    const auto vem_split_edge =
        one_cell(ElementType::kPolyVem, split_edge_nodes, split_edge_faces);
    REQUIRE(cell_quality(vem_split_edge, vem_split_edge.elements[0]) ==
            Approx(1.0).margin(1e-12));
    auto zero_edge_nodes = box_nodes(1.0, 1.0, 1.0);
    zero_edge_nodes.push_back(zero_edge_nodes[0]);
    auto zero_edge_faces = box_faces();
    zero_edge_faces[0] = {0, 8, 3, 2, 1};
    const auto vem_zero_edge =
        one_cell(ElementType::kPolyVem, zero_edge_nodes, zero_edge_faces);
    REQUIRE(cell_quality(vem_zero_edge, vem_zero_edge.elements[0]) ==
            Approx(0.0).margin(1e-12));
    auto collapsed_face_nodes = box_nodes(1.0, 1.0, 1.0);
    collapsed_face_nodes.push_back({1.0 / 3.0, 0.0, 0.0});
    collapsed_face_nodes.push_back({2.0 / 3.0, 0.0, 0.0});
    collapsed_face_nodes.push_back({1.0 / 3.0, 0.0, 0.0});
    auto collapsed_face_faces = box_faces();
    collapsed_face_faces.push_back({0, 8, 9, 10});
    const auto vem_collapsed_face =
        one_cell(ElementType::kPolyVem, collapsed_face_nodes, collapsed_face_faces);
    REQUIRE(cell_quality(vem_collapsed_face, vem_collapsed_face.elements[0]) ==
            Approx(0.0).margin(1e-12));
    const auto vem_slab =
        one_cell(ElementType::kPolyVem, box_nodes(1.0, 1.0, 0.1), box_faces());
    const double q_slab = cell_quality(vem_slab, vem_slab.elements[0]);
    REQUIRE(q_slab > 0.0);
    REQUIRE(q_slab < 0.25);
    auto vem_flipped = one_cell(ElementType::kPolyVem, box_nodes(1.0, 1.0, 1.0), box_faces());
    for (auto& face : vem_flipped.elements[0].faces) {
        std::reverse(face.begin(), face.end()); // inward normals ⇒ negative volume
    }
    REQUIRE(cell_quality(vem_flipped, vem_flipped.elements[0]) == Approx(0.0).margin(1e-12));
}

TEST_CASE("cell_quality: unmeasurable cells report NaN, never a perfect score") {
    // Polyhedral cell without face loops: nothing to measure.
    const auto no_faces = one_cell(ElementType::kPolyVem, box_nodes(1.0, 1.0, 1.0));
    REQUIRE(std::isnan(cell_quality(no_faces, no_faces.elements[0])));

    // Hex element with too few nodes: no Jacobian, so no number.
    NodalMesh broken;
    broken.nodes = box_nodes(1.0, 1.0, 1.0);
    NodalElement el;
    el.type = ElementType::kHex8;
    el.nodes = {0, 1, 2, 3};
    broken.elements.push_back(el);
    REQUIRE(std::isnan(cell_quality(broken, broken.elements[0])));

    // Summary counts the unmeasured cell instead of averaging it in as 1.0.
    NodalMesh mixed;
    mixed.nodes = box_nodes(1.0, 1.0, 1.0);
    NodalElement good;
    good.type = ElementType::kHex8;
    good.nodes = {0, 1, 2, 3, 4, 5, 6, 7};
    mixed.elements = {good, el};
    const auto stats = polymesh::fea::summarize_cell_quality(mixed);
    REQUIRE(stats.n_measured == 1);
    REQUIRE(stats.n_unmeasured == 1);
    REQUIRE(stats.min == Approx(1.0).margin(1e-12));
    REQUIRE(stats.mean == Approx(1.0).margin(1e-12));

    // Nothing measurable at all ⇒ NaN summary, not 1.0/0.0.
    NodalMesh only_broken;
    only_broken.nodes = box_nodes(1.0, 1.0, 1.0);
    only_broken.elements = {el};
    const auto none = polymesh::fea::summarize_cell_quality(only_broken);
    REQUIRE(none.n_measured == 0);
    REQUIRE(std::isnan(none.min));
    REQUIRE(std::isnan(none.mean));
}

TEST_CASE("stress samples carry measured quality for non-tet cells") {
    // Two hex cells: one cube, one squashed sliver sharing nothing but the mesh.
    NodalMesh mesh;
    const auto a = box_nodes(1.0, 1.0, 1.0);
    mesh.nodes.insert(mesh.nodes.end(), a.begin(), a.end());
    // Second cell: near-degenerate (apex nodes almost coincident with base).
    for (const auto& p : box_nodes(1.0, 1.0, 1.0)) {
        Eigen::Vector3d q = p;
        q[0] += 2.0;
        mesh.nodes.push_back(q);
    }
    // Collapse the top face onto the base: a 1×1×1e-4 pancake, the sliver shape
    // the stress-quality floor exists to reject. This line used to read
    // `mesh.nodes[i][0] += 0.9999` — a 45° *shear*, which is volume preserving
    // and honestly measures 0.7071 (measured 2026-08-08); that is the very shape
    // the first TEST_CASE above pins to (0, 0.75), so no continuous metric can
    // call one of them 0.707 and the other < 0.02. The pancake is the degeneracy
    // this fixture's comments always described, and it is the one the corner
    // scaled Jacobian is blind to — eight perfect right angles ⇒ exactly 1.0
    // before the volume-collapse term, 3.374e-4 after (measured 2026-08-08).
    for (std::size_t i = 12; i < 16; ++i) {
        mesh.nodes[i][2] = 1e-4;
    }
    NodalElement e0;
    e0.type = ElementType::kHex8;
    e0.nodes = {0, 1, 2, 3, 4, 5, 6, 7};
    NodalElement e1;
    e1.type = ElementType::kHex8;
    e1.nodes = {8, 9, 10, 11, 12, 13, 14, 15};
    mesh.elements = {e0, e1};

    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd u =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    const auto samples = polymesh::fea::recover_element_centroid_stress(mesh, mat, u);
    REQUIRE(samples.size() == 2);
    // Cube keeps 1.0 because it *measures* 1.0; the sheared cell must not.
    REQUIRE(samples[0].quality == Approx(1.0).margin(1e-12));
    REQUIRE(samples[1].quality < 1.0);
    // The testlab floor (0.02) has something to bite on now.
    REQUIRE(samples[1].quality < 0.02);
}

TEST_CASE("ZZ global eta is dimensionless and relative") {
    const auto c1 = cantilever(6, 1.0, 1e9);
    const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    const auto zz = polymesh::fea::recover_zz(c1.mesh, mat, c1.u);

    // Sane relative-error range: the old raw-Pa RSS was ~1e7..1e8 here.
    REQUIRE(zz.global_eta > 0.0);
    REQUIRE(zz.global_eta < 1.0);

    // Contract: global η² = Σ η_e².
    double sum_sq = 0.0;
    for (double e : zz.element_eta) {
        REQUIRE(e >= 0.0);
        sum_sq += e * e;
    }
    REQUIRE(std::sqrt(sum_sq) == Approx(zz.global_eta).epsilon(1e-10));

    // Load scale: η is a *ratio*, so 1000x the load leaves it unchanged. The old
    // indicator scaled linearly with load, which is why no fixed --eta-target
    // could ever be meaningful.
    const auto c1000 = cantilever(6, 1000.0, 1e9);
    const auto zz1000 = polymesh::fea::recover_zz(c1000.mesh, mat, c1000.u);
    REQUIRE(zz1000.global_eta == Approx(zz.global_eta).epsilon(1e-8));

    // Stiffness scale: same displacement field shape, same relative error.
    const auto soft = cantilever(6, 1.0, 1e6);
    const polymesh::fea::Material soft_mat{.youngs_modulus = 1e6, .poissons_ratio = 0.3};
    const auto zz_soft = polymesh::fea::recover_zz(soft.mesh, soft_mat, soft.u);
    REQUIRE(zz_soft.global_eta == Approx(zz.global_eta).epsilon(1e-8));

    // Refinement reduces the relative error (and does not grow like sqrt(N)).
    const auto fine = cantilever(12, 1.0, 1e9);
    const auto zz_fine = polymesh::fea::recover_zz(fine.mesh, mat, fine.u);
    REQUIRE(zz_fine.global_eta < zz.global_eta);

    // Dörfler marking still selects a strict, non-empty subset.
    const auto marked = polymesh::adapt::dorfler_mark(zz.element_eta, 0.3);
    REQUIRE(!marked.empty());
    REQUIRE(marked.size() < zz.element_eta.size());
}

// `ElementCentroidStress.volume` was `|det J|` at a single reference point,
// with the reference domain's own measure dropped: 0.125x true for a hex,
// ~0.09x for a pyramid, correct for tet4 only because a special case
// overrode it. Nothing read the field yet, so nothing was corrupted, but any
// volume-weighted average over these samples would have been wrong per element
// type. There is now one definition, `fea::element_volume`, and this is the
// contract that ties the samples to it.
TEST_CASE("element volumes are the real thing, per element type") {
    using polymesh::fea::ElementType;
    using polymesh::fea::NodalElement;
    using polymesh::fea::NodalMesh;

    SECTION("a unit hex measures one, not one eighth") {
        NodalMesh mesh;
        mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                      {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        mesh.elements.push_back(NodalElement{ElementType::kHex8, {0, 1, 2, 3, 4, 5, 6, 7}});
        CHECK(polymesh::fea::element_volume(mesh, mesh.elements[0]) ==
              Catch::Approx(1.0).margin(1e-14));
    }

    SECTION("six pyramids fanned from a hex centre sum to the hex") {
        NodalMesh mesh;
        mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1},
                      {1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0.5, 0.5, 0.5}};
        const std::vector<std::vector<std::uint32_t>> faces{
            {0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1},
            {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
        for (const auto& face : faces) {
            mesh.elements.push_back(NodalElement{
                ElementType::kPyramid5, {face[0], face[1], face[2], face[3], 8}});
        }
        for (const auto& element : mesh.elements) {
            CHECK(polymesh::fea::element_volume(mesh, element) ==
                  Catch::Approx(1.0 / 6.0).margin(1e-14));
        }
        CHECK(polymesh::fea::mesh_volume(mesh) == Catch::Approx(1.0).margin(1e-14));
    }

    SECTION("stress sample volumes partition the mesh") {
        const auto beam = cantilever(4, 1.0, 1e9);
        const polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
        const auto samples =
            polymesh::fea::recover_element_centroid_stress(beam.mesh, mat, beam.u);
        REQUIRE(!samples.empty());
        double sum = 0.0;
        for (const auto& sample : samples) {
            CHECK(sample.volume > 0.0);
            sum += sample.volume;
        }
        CHECK(sum == Catch::Approx(polymesh::fea::mesh_volume(beam.mesh)).epsilon(1e-12));
    }
}
