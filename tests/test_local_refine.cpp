// SPDX-License-Identifier: BSD-3-Clause
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/solve.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/tet_fill.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using polymesh::fea::ElementType;
using polymesh::fea::NodalElement;
using polymesh::fea::NodalMesh;
using polymesh::mesh::local_refine_tets;
using polymesh::mesh::LocalRefineStats;
using polymesh::mesh::tet_signed_volume;

namespace {

NodalMesh tets_to_nodal(const std::vector<Eigen::Vector3d>& nodes,
                        const std::vector<std::array<std::uint32_t, 4>>& tets) {
    NodalMesh m;
    m.nodes = nodes;
    m.elements.reserve(tets.size());
    for (const auto& t : tets) {
        m.elements.push_back(NodalElement{ElementType::kTet4, {t[0], t[1], t[2], t[3]}});
    }
    return m;
}

void extract_tet4(const NodalMesh& mesh, std::vector<Eigen::Vector3d>& nodes,
                  std::vector<std::array<std::uint32_t, 4>>& tets) {
    nodes = mesh.nodes;
    tets.clear();
    for (const auto& el : mesh.elements) {
        REQUIRE(el.type == ElementType::kTet4);
        REQUIRE(el.nodes.size() == 4);
        tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
    }
}

double total_volume(const std::vector<Eigen::Vector3d>& nodes,
                    const std::vector<std::array<std::uint32_t, 4>>& tets) {
    double v = 0.0;
    for (const auto& t : tets) {
        v += tet_signed_volume(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]);
    }
    return v;
}

/// Index of tet whose centroid is closest to `p`.
std::size_t nearest_tet(const NodalMesh& mesh, const Eigen::Vector3d& p) {
    std::size_t best = 0;
    double best_d2 = 1e300;
    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        for (auto n : mesh.elements[e].nodes) {
            c += mesh.nodes[n];
        }
        c /= static_cast<double>(mesh.elements[e].nodes.size());
        const double d2 = (c - p).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best = e;
        }
    }
    return best;
}

} // namespace

TEST_CASE("local_refine_tets: single tet splits into two positive-volume children") {
    std::vector<Eigen::Vector3d> nodes{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    std::vector<std::array<std::uint32_t, 4>> tets{{{0, 1, 2, 3}}};
    const double v0 = total_volume(nodes, tets);
    REQUIRE(v0 > 0.0);

    LocalRefineStats stats;
    const std::size_t mark = 0;
    auto out = local_refine_tets(nodes, tets, std::span{&mark, 1}, &stats);

    REQUIRE(out.tets.size() == 2);
    REQUIRE(out.nodes.size() == 5); // +1 midpoint
    REQUIRE(stats.n_bisections == 1);
    REQUIRE(stats.n_new_nodes == 1);
    REQUIRE_NOTHROW(polymesh::mesh::check_tet_fill_geometry(out));
    for (const auto& t : out.tets) {
        const double v = tet_signed_volume(out.nodes[t[0]], out.nodes[t[1]], out.nodes[t[2]],
                                           out.nodes[t[3]]);
        REQUIRE(v > 0.0);
    }
    REQUIRE(std::abs(total_volume(out.nodes, out.tets) - v0) < 1e-12);
}

TEST_CASE("local_refine_tets: center tet of multi-tet mesh increases count, valid, +vol") {
    // 2×2×2 Kuhn tets on unit box → 8 cells * 6 = 48 tets.
    auto mesh = polymesh::test_support::box_tet_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    REQUIRE(mesh.elements.size() == 48);
    REQUIRE_NOTHROW(mesh.check_validity());

    std::vector<Eigen::Vector3d> nodes;
    std::vector<std::array<std::uint32_t, 4>> tets;
    extract_tet4(mesh, nodes, tets);
    const double v0 = total_volume(nodes, tets);
    const std::size_t n0 = tets.size();

    const std::size_t center = nearest_tet(mesh, {0.5, 0.5, 0.5});
    LocalRefineStats stats;
    auto out =
        local_refine_tets(std::move(nodes), std::move(tets), std::span{&center, 1}, &stats);

    REQUIRE(out.tets.size() > n0);
    REQUIRE(stats.n_bisections >= 1);
    REQUIRE(stats.n_new_nodes >= 1);
    REQUIRE_NOTHROW(polymesh::mesh::check_tet_fill_geometry(out));

    auto refined = tets_to_nodal(out.nodes, out.tets);
    REQUIRE_NOTHROW(refined.check_validity());
    for (const auto& el : refined.elements) {
        REQUIRE(el.type == ElementType::kTet4);
        const double v =
            tet_signed_volume(refined.nodes[el.nodes[0]], refined.nodes[el.nodes[1]],
                              refined.nodes[el.nodes[2]], refined.nodes[el.nodes[3]]);
        REQUIRE(v > 0.0);
        REQUIRE(std::isfinite(v));
    }
    // Volume conserved (no domain change).
    REQUIRE(std::abs(total_volume(out.nodes, out.tets) - v0) < 1e-10 * std::max(1.0, v0));
}

TEST_CASE("local_refine_tets: empty marks is identity") {
    auto mesh = polymesh::test_support::box_tet_mesh(1, 1, 1, {1.0, 1.0, 1.0});
    std::vector<Eigen::Vector3d> nodes;
    std::vector<std::array<std::uint32_t, 4>> tets;
    extract_tet4(mesh, nodes, tets);
    const auto n0 = tets.size();
    const auto nn0 = nodes.size();
    std::vector<std::size_t> none;
    auto out = local_refine_tets(nodes, tets, none);
    REQUIRE(out.tets.size() == n0);
    REQUIRE(out.nodes.size() == nn0);
}

TEST_CASE("local_refine_tets: refined tet mesh still solves elastostatics") {
    auto mesh = polymesh::test_support::box_tet_mesh(2, 1, 1, {1.0, 0.25, 0.25});
    std::vector<Eigen::Vector3d> nodes;
    std::vector<std::array<std::uint32_t, 4>> tets;
    extract_tet4(mesh, nodes, tets);
    const std::size_t mark = nearest_tet(mesh, {0.5, 0.125, 0.125});
    auto out = local_refine_tets(std::move(nodes), std::move(tets), std::span{&mark, 1});
    auto refined = tets_to_nodal(out.nodes, out.tets);
    REQUIRE_NOTHROW(refined.check_validity());

    polymesh::fea::Material mat{.youngs_modulus = 1e9, .poissons_ratio = 0.3};
    polymesh::fea::Dirichlet bc;
    for (std::uint32_t i = 0; i < refined.nodes.size(); ++i) {
        if (refined.nodes[i][0] < 1e-12) {
            bc.fix_node(i);
        }
    }
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(refined.nodes.size()));
    for (std::uint32_t i = 0; i < refined.nodes.size(); ++i) {
        if (refined.nodes[i][0] > 1.0 - 1e-12) {
            loads(3 * static_cast<Eigen::Index>(i) + 1) = 1.0;
        }
    }
    REQUIRE_FALSE(bc.dof_values.empty());
    const auto u = polymesh::fea::solve_elastostatics(refined, mat, bc, loads);
    REQUIRE(u.size() == 3 * static_cast<Eigen::Index>(refined.nodes.size()));
    REQUIRE(u.allFinite());
    REQUIRE(u.norm() > 0.0);
}
