// SPDX-License-Identifier: BSD-3-Clause

// Conforming selective p-elevation: algebra, patch, nullspace, energy, and determinism.

#include "fea/assembly.hpp"
#include "fea/constraints.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <Eigen/Eigenvalues>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <span>
#include <vector>

namespace fea = polymesh::fea;
using polymesh::test_support::box_hex_mesh;

namespace {

const fea::Material kMaterial{.youngs_modulus = 70e9, .poissons_ratio = 0.27};

Eigen::Vector3d affine_displacement(const Eigen::Vector3d& x) {
    Eigen::Matrix3d a;
    a << 1.1e-3, -2.7e-4, 3.2e-4, 4.3e-4, -7.0e-4, 1.9e-4, -3.6e-4, 2.4e-4, 8.5e-4;
    const Eigen::Vector3d b{2.0e-5, -3.0e-5, 1.0e-5};
    return a * x + b;
}

bool is_boundary(const Eigen::Vector3d& x, const Eigen::Vector3d& extents) {
    constexpr double kTol = 1e-12;
    return x.minCoeff() <= kTol || (extents - x).minCoeff() <= kTol;
}

fea::Dirichlet affine_boundary(const fea::NodalMesh& mesh, const Eigen::Vector3d& extents,
                               const fea::LinearConstraints* constraints = nullptr) {
    fea::Dirichlet bc;
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        if (!is_boundary(mesh.nodes[node], extents)) {
            continue;
        }
        const auto value = affine_displacement(mesh.nodes[node]);
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            const auto dof = 3u * static_cast<std::uint32_t>(node) + axis;
            if (constraints == nullptr || !constraints->is_slave(dof)) {
                bc.dof_values[static_cast<Eigen::Index>(dof)] = value[axis];
            }
        }
    }
    return bc;
}

double affine_max_error(const fea::NodalMesh& mesh, const Eigen::VectorXd& u) {
    double error = 0.0;
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        const auto actual = u.segment<3>(3 * static_cast<Eigen::Index>(node));
        error = std::max(error, (actual - affine_displacement(mesh.nodes[node])).norm());
    }
    return error;
}

std::vector<std::size_t> elements_left_of(const fea::NodalMesh& mesh, double x_cut) {
    std::vector<std::size_t> selected;
    for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
        double x = 0.0;
        for (const auto node : mesh.elements[e].nodes) {
            x += mesh.nodes[node].x();
        }
        x /= static_cast<double>(mesh.elements[e].nodes.size());
        if (x < x_cut) {
            selected.push_back(e);
        }
    }
    return selected;
}

bool same_mesh(const fea::NodalMesh& a, const fea::NodalMesh& b) {
    if (a.nodes.size() != b.nodes.size() || a.elements.size() != b.elements.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        if ((a.nodes[i].array() != b.nodes[i].array()).any()) {
            return false;
        }
    }
    for (std::size_t e = 0; e < a.elements.size(); ++e) {
        if (a.elements[e].type != b.elements[e].type ||
            a.elements[e].nodes != b.elements[e].nodes ||
            a.elements[e].faces != b.elements[e].faces) {
            return false;
        }
    }
    return true;
}

fea::Dirichlet fixed_min_x(const fea::NodalMesh& mesh) {
    fea::Dirichlet bc;
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        if (mesh.nodes[node].x() <= 1e-12) {
            bc.fix_node(static_cast<std::uint32_t>(node));
        }
    }
    return bc;
}

struct CantileverSolution {
    Eigen::VectorXd u;
    double energy = 0.0;
};

CantileverSolution solve_body_loaded(const fea::NodalMesh& mesh,
                                     const fea::LinearConstraints* constraints = nullptr) {
    const auto loads = fea::assemble_body_load(
        mesh, [](const Eigen::Vector3d&) { return Eigen::Vector3d{0.0, 0.0, -2.5e5}; });
    fea::SolveOptions options;
    options.method = fea::SolveMethod::kDirect;
    const auto u = fea::solve_elastostatics(mesh, kMaterial, fixed_min_x(mesh), loads, options,
                                            constraints)
                       .u;
    return {.u = u, .energy = fea::strain_energy(mesh, kMaterial, u)};
}

} // namespace

TEST_CASE("linear-constraint transform and recovery are exact", "[pconform][hp]") {
    fea::LinearConstraints constraints;
    constraints.add({.slave_dof = 1, .masters = {{0, 0.25}, {2, 0.75}}});
    constraints.add({.slave_dof = 4, .masters = {{0, -1.0}, {3, 2.0}}});

    const auto t = constraints.transform(5);
    REQUIRE(t.rows() == 5);
    REQUIRE(t.cols() == 3);
    CHECK(t.coeff(0, 0) == 1.0);
    CHECK(t.coeff(1, 0) == 0.25);
    CHECK(t.coeff(1, 1) == 0.75);
    CHECK(t.coeff(2, 1) == 1.0);
    CHECK(t.coeff(3, 2) == 1.0);
    CHECK(t.coeff(4, 0) == -1.0);
    CHECK(t.coeff(4, 2) == 2.0);

    Eigen::Vector3d reduced{2.0, 4.0, 5.0};
    const auto full = constraints.recover(reduced, 5);
    CHECK(full[0] == 2.0);
    CHECK(full[1] == 3.5);
    CHECK(full[2] == 4.0);
    CHECK(full[3] == 5.0);
    CHECK(full[4] == 8.0);

    fea::LinearConstraints chained;
    chained.add({.slave_dof = 2, .masters = {{1, 1.0}}});
    chained.add({.slave_dof = 1, .masters = {{0, 1.0}}});
    REQUIRE_THROWS_AS(chained.transform(3), fea::FeaError);
}

TEST_CASE("selective p interface passes affine patch only when constrained",
          "[pconform][hp][patch]") {
    const Eigen::Vector3d extents{1.0, 1.0, 1.0};
    const auto linear = box_hex_mesh(2, 2, 2, extents);
    const auto selected = elements_left_of(linear, 0.5);
    REQUIRE_FALSE(selected.empty());
    REQUIRE(selected.size() < linear.elements.size());
    const auto elevated = fea::p_elevate_with_constraints(linear, selected);
    REQUIRE(elevated.n_constrained_midside > 0);

    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(elevated.mesh.nodes.size()));
    fea::SolveOptions options;
    options.method = fea::SolveMethod::kDirect;
    const auto constrained_bc = affine_boundary(elevated.mesh, extents, &elevated.constraints);
    const auto constrained_solve = fea::solve_elastostatics(
        elevated.mesh, kMaterial, constrained_bc, loads, options, &elevated.constraints);
    CHECK_FALSE(constrained_solve.reactions_complete);
    const Eigen::VectorXd& u_constrained = constrained_solve.u;
    const double constrained_error = affine_max_error(elevated.mesh, u_constrained);

    const auto discontinuous_bc = affine_boundary(elevated.mesh, extents);
    const auto u_discontinuous =
        fea::solve_elastostatics(elevated.mesh, kMaterial, discontinuous_bc, loads, options).u;
    const double unconstrained_error = affine_max_error(elevated.mesh, u_discontinuous);

    std::cout << "pconform patch max error: constrained=" << constrained_error
              << " unconstrained=" << unconstrained_error << '\n';
    CHECK(constrained_error <= 1e-10);
    CHECK(unconstrained_error > 1e-10);
}

TEST_CASE("selective constrained stiffness has exactly six rigid-body modes",
          "[pconform][hp]") {
    const auto linear = box_hex_mesh(2, 1, 1, {2.0, 1.0, 1.0});
    const std::vector<std::size_t> selected{0};
    const auto elevated = fea::p_elevate_with_constraints(linear, selected);
    REQUIRE(elevated.n_constrained_midside == 4);

    const auto t = elevated.constraints.transform(
        3 * static_cast<Eigen::Index>(elevated.mesh.nodes.size()));
    const auto k = fea::assemble_stiffness(elevated.mesh, kMaterial);
    const Eigen::MatrixXd constrained = Eigen::MatrixXd(t.transpose() * k * t);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(constrained);
    REQUIRE(eig.info() == Eigen::Success);
    const double cutoff = 1e-9 * eig.eigenvalues().cwiseAbs().maxCoeff();
    const Eigen::Index zero_modes = (eig.eigenvalues().cwiseAbs().array() <= cutoff).count();
    CHECK(zero_modes == 6);
}

TEST_CASE("all and no promotion are exact no-op endpoints", "[pconform][hp]") {
    const auto linear = box_hex_mesh(2, 1, 1, {1.0, 0.4, 0.3});
    std::vector<std::size_t> all(linear.elements.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    const auto elevated_all = fea::p_elevate_with_constraints(linear, all);
    const auto legacy_all = fea::promote_to_quadratic(linear);
    CHECK(elevated_all.constraints.empty());
    CHECK(elevated_all.n_constrained_midside == 0);
    REQUIRE(same_mesh(elevated_all.mesh, legacy_all));

    const std::vector<std::size_t> none;
    const auto elevated_none = fea::p_elevate_with_constraints(linear, none);
    CHECK(elevated_none.constraints.empty());
    CHECK(elevated_none.n_promoted == 0);
    REQUIRE(same_mesh(elevated_none.mesh, linear));

    const auto all_a = solve_body_loaded(elevated_all.mesh);
    const auto all_b = solve_body_loaded(legacy_all);
    CHECK((all_a.u.array() == all_b.u.array()).all());
    const auto none_a = solve_body_loaded(elevated_none.mesh);
    const auto none_b = solve_body_loaded(linear);
    CHECK((none_a.u.array() == none_b.u.array()).all());
}

TEST_CASE("selective conforming enrichment has intermediate load energy", "[pconform][hp]") {
    const auto linear = box_hex_mesh(3, 2, 2, {1.0, 0.3, 0.3});
    const auto selected = elements_left_of(linear, 0.34);
    const auto selective = fea::p_elevate_with_constraints(linear, selected);
    REQUIRE(selective.n_constrained_midside > 0);
    const auto quadratic = fea::promote_to_quadratic(linear);

    const double p1_energy = solve_body_loaded(linear).energy;
    const double selective_energy =
        solve_body_loaded(selective.mesh, &selective.constraints).energy;
    const double p2_energy = solve_body_loaded(quadratic).energy;
    INFO("For fixed force loading, strain energy/compliance increases as the "
         "Rayleigh-Ritz displacement space is enriched; its error decreases.");
    CAPTURE(p1_energy, selective_energy, p2_energy);
    CHECK(selective_energy >= p1_energy * (1.0 - 1e-11));
    CHECK(selective_energy <= p2_energy * (1.0 + 1e-11));
}

TEST_CASE("selective constrained solves are bit deterministic", "[pconform][hp]") {
    const auto linear = box_hex_mesh(3, 2, 2, {1.0, 0.3, 0.3});
    const auto selected = elements_left_of(linear, 0.34);
    const auto first_mesh = fea::p_elevate_with_constraints(linear, selected);
    const auto second_mesh = fea::p_elevate_with_constraints(linear, selected);
    REQUIRE(same_mesh(first_mesh.mesh, second_mesh.mesh));

    const auto first = solve_body_loaded(first_mesh.mesh, &first_mesh.constraints).u;
    const auto second = solve_body_loaded(second_mesh.mesh, &second_mesh.constraints).u;
    REQUIRE(first.size() == second.size());
    CHECK((first.array() == second.array()).all());
}
