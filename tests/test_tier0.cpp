// SPDX-License-Identifier: AGPL-3.0-or-later

// Tier-0 correctness gates (BENCHMARKS.md): patch test on distorted meshes,
// rigid-body modes, single-element eigenvalue check. These must pass exactly
// (to solver tolerance) for every element type — a change that breaks them
// cannot merge (engineering rule #2).

#include "fea/assembly.hpp"
#include "fea/shape.hpp"
#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Eigen/Dense>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

const Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

/// Distorted mesh of the given element type over a unit-ish box.
NodalMesh make_distorted_mesh(ElementType type) {
    const Eigen::Vector3d size(1.0, 0.8, 1.2);
    const int n = 3;
    NodalMesh mesh = (type == ElementType::kTet4 || type == ElementType::kTet10)
                         ? box_tet_mesh(n, n, n, size)
                         : box_hex_mesh(n, n, n, size);
    distort_interior(mesh, 0.15, size[0] / n, /*seed=*/42);
    if (type == ElementType::kTet10 || type == ElementType::kHex20) {
        mesh = promote_to_quadratic(mesh);
    }
    mesh.check_validity();
    return mesh;
}

/// A single mildly distorted element of the given type.
NodalElement single_element(ElementType type, NodalMesh& mesh) {
    const auto base = (type == ElementType::kTet4 || type == ElementType::kTet10)
                          ? ElementType::kTet4
                          : ElementType::kHex8;
    const auto ref = reference_nodes(base);
    for (std::size_t i = 0; i < ref.size(); ++i) {
        // Deterministic per-node distortion, small enough to keep Jacobians positive.
        const double wiggle = 0.06 * static_cast<double>((i * 2654435761u) % 100) / 100.0;
        mesh.nodes.push_back(ref[i] + Eigen::Vector3d::Constant(wiggle));
    }
    NodalElement element{base, {}};
    for (std::size_t i = 0; i < ref.size(); ++i) {
        element.nodes.push_back(static_cast<std::uint32_t>(i));
    }
    if (type == ElementType::kTet10 || type == ElementType::kHex20) {
        NodalMesh tmp;
        tmp.nodes = mesh.nodes;
        tmp.elements = {element};
        tmp = promote_to_quadratic(tmp);
        mesh.nodes = tmp.nodes;
        element = tmp.elements[0];
    }
    return element;
}

const std::vector<ElementType> kAllTypes{ElementType::kTet4, ElementType::kTet10,
                                         ElementType::kHex8, ElementType::kHex20};

} // namespace

TEST_CASE("patch test: constant strain reproduced exactly on distorted meshes") {
    // Impose u = G x on the whole boundary; the interior solution must be
    // exactly u = G x (constant strain) for a consistent element.
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;

    for (const auto type : kAllTypes) {
        const auto mesh = make_distorted_mesh(type);

        Eigen::Vector3d lo = mesh.nodes.front();
        Eigen::Vector3d hi = mesh.nodes.front();
        for (const auto& p : mesh.nodes) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
        const double tol = 1e-9;

        Dirichlet bc;
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            const auto& p = mesh.nodes[i];
            const bool boundary =
                (p - lo).cwiseAbs().minCoeff() < tol || (hi - p).cwiseAbs().minCoeff() < tol;
            if (boundary) {
                bc.fix_node(static_cast<std::uint32_t>(i), g * p);
            }
        }

        const Eigen::VectorXd loads =
            Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
        const auto u = solve_elastostatics(mesh, kSteel, bc, loads);

        double max_error = 0.0;
        for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
            const Eigen::Vector3d exact = g * mesh.nodes[i];
            const Eigen::Vector3d fem = u.segment<3>(3 * static_cast<Eigen::Index>(i));
            max_error = std::max(max_error, (fem - exact).norm());
        }
        INFO("element type " << static_cast<int>(type));
        CHECK(max_error < 1e-12); // metres, against ~1e-3 m displacements
    }
}

TEST_CASE("rigid-body modes produce zero strain energy") {
    for (const auto type : kAllTypes) {
        NodalMesh mesh;
        const auto element = single_element(type, mesh);
        const auto k = element_stiffness(mesh, element, kSteel);
        const double scale = k.norm();

        // Three translations and three linearized rotations u = omega x r.
        std::vector<Eigen::Vector3d> translations{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        std::vector<Eigen::Vector3d> rotations{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (const auto& t : translations) {
            Eigen::VectorXd u(3 * element.nodes.size());
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                u.segment<3>(3 * static_cast<Eigen::Index>(a)) = t;
            }
            INFO("type " << static_cast<int>(type) << " translation " << t.transpose());
            CHECK((k * u).norm() < 1e-12 * scale);
        }
        for (const auto& omega : rotations) {
            Eigen::VectorXd u(3 * element.nodes.size());
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                u.segment<3>(3 * static_cast<Eigen::Index>(a)) =
                    omega.cross(mesh.nodes[element.nodes[a]]);
            }
            INFO("type " << static_cast<int>(type) << " rotation " << omega.transpose());
            CHECK((k * u).norm() < 1e-12 * scale);
        }
    }
}

TEST_CASE("single-element eigenvalue check: exactly six zero-energy modes") {
    for (const auto type : kAllTypes) {
        NodalMesh mesh;
        const auto element = single_element(type, mesh);
        const auto k = element_stiffness(mesh, element, kSteel);

        REQUIRE(k.isApprox(k.transpose(), 1e-12));
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(k);
        const auto& lambda = eig.eigenvalues();
        const double cutoff = 1e-10 * lambda[lambda.size() - 1];
        int zero_modes = 0;
        for (Eigen::Index i = 0; i < lambda.size(); ++i) {
            if (std::abs(lambda[i]) < cutoff) {
                ++zero_modes;
            } else {
                INFO("type " << static_cast<int>(type) << " eigenvalue " << lambda[i]);
                CHECK(lambda[i] > 0.0); // no negative or extra spurious modes
            }
        }
        INFO("element type " << static_cast<int>(type));
        CHECK(zero_modes == 6);
    }
}
