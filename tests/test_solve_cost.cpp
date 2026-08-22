// SPDX-License-Identifier: BSD-3-Clause

#include "advisor/calibration.hpp"
#include "fea/assembly.hpp"
#include "fea/solve.hpp"
#include "fea/solve_cost.hpp"
#include "geom/step.hpp"
#include "pipeline/scene.hpp"
#include "support/structured_mesh.hpp"

#include <Eigen/OrderingMethods>
#include <Eigen/SparseCholesky>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

using namespace polymesh::fea;
using polymesh::test_support::box_hex_mesh;

namespace {

Eigen::SparseMatrix<double> hand_pattern() {
    constexpr int kSize = 7;
    const std::vector<std::pair<int, int>> edges{
        {0, 1}, {0, 3}, {1, 2}, {1, 4}, {2, 4}, {2, 5}, {3, 4}, {3, 6}, {4, 5}, {4, 6}, {5, 6},
    };
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(static_cast<std::size_t>(kSize) + 2 * edges.size());
    for (int i = 0; i < kSize; ++i) {
        triplets.emplace_back(i, i, 16.0);
    }
    for (const auto& [a, b] : edges) {
        triplets.emplace_back(a, b, -1.0);
        triplets.emplace_back(b, a, -1.0);
    }
    Eigen::SparseMatrix<double> pattern(kSize, kSize);
    pattern.setFromTriplets(triplets.begin(), triplets.end());
    pattern.makeCompressed();
    return pattern;
}

std::uint64_t brute_force_ldlt_nnz(const Eigen::SparseMatrix<double>& pattern) {
    Eigen::AMDOrdering<int> amd;
    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> inverse_permutation;
    amd(pattern, inverse_permutation);
    const Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> permutation =
        inverse_permutation.inverse();
    const Eigen::SparseMatrix<double> ordered =
        permutation * pattern * permutation.transpose();

    const int size = static_cast<int>(ordered.rows());
    const std::size_t size_u = static_cast<std::size_t>(size);
    std::vector<unsigned char> adjacency(size_u * size_u, 0);
    const auto at = [size_u, &adjacency](int row, int col) -> unsigned char& {
        return adjacency[static_cast<std::size_t>(row) * size_u +
                         static_cast<std::size_t>(col)];
    };
    for (int col = 0; col < size; ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(ordered, col); it; ++it) {
            if (it.row() != col) {
                at(static_cast<int>(it.row()), col) = 1;
                at(col, static_cast<int>(it.row())) = 1;
            }
        }
    }

    std::uint64_t count = 0;
    std::vector<int> higher;
    higher.reserve(static_cast<std::size_t>(size));
    for (int eliminated = 0; eliminated < size; ++eliminated) {
        higher.clear();
        for (int row = eliminated + 1; row < size; ++row) {
            if (at(row, eliminated) != 0) {
                higher.push_back(row);
                ++count;
            }
        }
        for (const int row : higher) {
            for (const int col : higher) {
                if (row != col) {
                    at(row, col) = 1;
                }
            }
        }
    }
    return count;
}

Dirichlet fixed_min_x(const NodalMesh& mesh) {
    Dirichlet bc;
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        if (mesh.nodes[node].x() < 1e-12) {
            bc.fix_node(static_cast<std::uint32_t>(node));
        }
    }
    return bc;
}

Eigen::VectorXd tip_load(const NodalMesh& mesh) {
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        if (mesh.nodes[node].x() > 1.0 - 1e-12) {
            loads[3 * static_cast<Eigen::Index>(node) + 2] = -100.0;
        }
    }
    return loads;
}

struct ReducedAssembly {
    Eigen::SparseMatrix<double> stiffness;
    Eigen::VectorXd rhs;
    std::vector<Eigen::Index> full_to_free;
};

ReducedAssembly assemble_reduced_without_cost(const NodalMesh& mesh, const Material& material,
                                              const Dirichlet& bc,
                                              const Eigen::VectorXd& loads) {
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    ReducedAssembly reduced;
    reduced.full_to_free.assign(static_cast<std::size_t>(ndof), -1);
    Eigen::Index nfree = 0;
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        if (!bc.dof_values.contains(dof)) {
            reduced.full_to_free[static_cast<std::size_t>(dof)] = nfree++;
        }
    }
    reduced.rhs.resize(nfree);
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        const Eigen::Index free = reduced.full_to_free[static_cast<std::size_t>(dof)];
        if (free >= 0) {
            reduced.rhs[free] = loads[dof];
        }
    }

    const Eigen::SparseMatrix<double> full = assemble_stiffness(mesh, material);
    std::vector<Eigen::Triplet<double>> triplets;
    triplets.reserve(static_cast<std::size_t>(full.nonZeros()));
    for (int outer = 0; outer < full.outerSize(); ++outer) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(full, outer); it; ++it) {
            const Eigen::Index row = reduced.full_to_free[static_cast<std::size_t>(it.row())];
            const Eigen::Index col = reduced.full_to_free[static_cast<std::size_t>(it.col())];
            if (row >= 0 && col >= 0) {
                triplets.emplace_back(row, col, it.value());
            } else if (row >= 0 && col < 0) {
                reduced.rhs[row] -= it.value() * bc.dof_values.at(it.col());
            }
        }
    }
    reduced.stiffness.resize(nfree, nfree);
    reduced.stiffness.setFromTriplets(triplets.begin(), triplets.end());
    return reduced;
}

Eigen::VectorXd direct_solve_without_cost(const NodalMesh& mesh, const Material& material,
                                          const Dirichlet& bc, const Eigen::VectorXd& loads) {
    ReducedAssembly reduced = assemble_reduced_without_cost(mesh, material, bc, loads);
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(reduced.stiffness);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("test baseline factorization failed");
    }
    const Eigen::VectorXd free = ldlt.solve(reduced.rhs);
    if (ldlt.info() != Eigen::Success) {
        throw FeaError("test baseline back-substitution failed");
    }
    Eigen::VectorXd displacement(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (Eigen::Index dof = 0; dof < displacement.size(); ++dof) {
        const Eigen::Index mapped = reduced.full_to_free[static_cast<std::size_t>(dof)];
        displacement[dof] = mapped >= 0 ? free[mapped] : bc.dof_values.at(dof);
    }
    return displacement;
}

Dirichlet fixed_mesh_min_x(const NodalMesh& mesh) {
    double min_x = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    for (const Eigen::Vector3d& node : mesh.nodes) {
        min_x = std::min(min_x, node.x());
        max_x = std::max(max_x, node.x());
    }
    const double tolerance = std::max((max_x - min_x) * 1e-9, 1e-12);
    Dirichlet bc;
    for (std::size_t node = 0; node < mesh.nodes.size(); ++node) {
        if (mesh.nodes[node].x() <= min_x + tolerance) {
            bc.fix_node(static_cast<std::uint32_t>(node));
        }
    }
    return bc;
}

} // namespace

TEST_CASE("symbolic LDLT column count matches brute-force elimination") {
    const Eigen::SparseMatrix<double> pattern = hand_pattern();
    const SolveCostEstimate cost = analyze_solve_cost(pattern);

    CHECK(cost.nfree == pattern.rows());
    CHECK(cost.pattern_nnz == static_cast<std::uint64_t>(pattern.nonZeros()));
    CHECK(cost.factor_nnz == brute_force_ldlt_nnz(pattern));
    CHECK(cost.factor_flops > 0.0);
    CHECK(cost.cg_flops_per_iter > 0.0);
    CHECK(cost.cg_bytes_per_iter > 0.0);
}

TEST_CASE("symbolic LDLT count matches Eigen's stored factor") {
    const Eigen::SparseMatrix<double> pattern = hand_pattern();
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(pattern);
    REQUIRE(ldlt.info() == Eigen::Success);

    const SolveCostEstimate cost = analyze_solve_cost(pattern);
    CHECK(cost.factor_nnz ==
          static_cast<std::uint64_t>(ldlt.matrixL().nestedExpression().nonZeros()));
}

TEST_CASE("structured solve result reports direct portable work") {
    const NodalMesh mesh = box_hex_mesh(2, 1, 1, {1.0, 0.2, 0.2});
    const Dirichlet bc = fixed_min_x(mesh);
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    SolveOptions options;
    options.method = SolveMethod::kDirect;

    const SolveCostEstimate symbolic = analyze_solve_cost(mesh, bc);
    const LinearSolveResult solved =
        solve_elastostatics(mesh, steel, bc, tip_load(mesh), options);

    CHECK(solved.u.allFinite());
    CHECK(solved.cost.method == "direct");
    CHECK(solved.cost.cg_iterations == 0);
    CHECK(solved.cost.factor_nnz == symbolic.factor_nnz);
    CHECK(solved.cost.flops == Catch::Approx(symbolic.factor_flops +
                                             4.0 * static_cast<double>(symbolic.factor_nnz)));
    CHECK(solved.cost.bytes > 0.0);
}

TEST_CASE("structured CG work uses the reported iteration count") {
    const NodalMesh mesh = box_hex_mesh(2, 1, 1, {1.0, 0.2, 0.2});
    const Dirichlet bc = fixed_min_x(mesh);
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    SolveOptions options;
    options.method = SolveMethod::kCG;
    options.cg_tol = 1e-10;
    options.cg_progress_chunk = 1;
    int last_reported_iteration = -1;
    options.on_progress = [&](int iteration, int, double) {
        last_reported_iteration = iteration;
    };

    const SolveCostEstimate symbolic = analyze_solve_cost(mesh, bc);
    const LinearSolveResult solved =
        solve_elastostatics(mesh, steel, bc, tip_load(mesh), options);

    CHECK(solved.u.allFinite());
    CHECK((solved.cost.method == "cg-ichol" || solved.cost.method == "cg-jacobi"));
    CHECK(solved.cost.cg_iterations > 0);
    CHECK(solved.cost.cg_iterations == last_reported_iteration);
    CHECK(solved.cost.flops == Catch::Approx(static_cast<double>(solved.cost.cg_iterations) *
                                             symbolic.cg_flops_per_iter));
    CHECK(solved.cost.bytes == Catch::Approx(static_cast<double>(solved.cost.cg_iterations) *
                                             symbolic.cg_bytes_per_iter));
}

TEST_CASE("roofline conversion uses the slower portable resource") {
    const polymesh::advisor::HostCalibration calibration{
        .host = "test",
        .flops_per_s = 1.0e9,
        .bytes_per_s = 1.0e8,
        .ref_mesh_ms = 10.0,
        .generated_utc = "test",
    };
    CHECK(polymesh::advisor::predicted_seconds(2.0e9, 1.0e8, calibration) ==
          Catch::Approx(2.0));
    CHECK(polymesh::advisor::predicted_seconds(1.0e9, 3.0e8, calibration) ==
          Catch::Approx(3.0));

    auto invalid = calibration;
    invalid.flops_per_s = 0.0;
    CHECK(std::isnan(polymesh::advisor::predicted_seconds(1.0, 1.0, invalid)));
}

TEST_CASE("solve cost observation leaves direct displacement bit-identical") {
    const NodalMesh mesh = box_hex_mesh(2, 1, 1, {1.0, 0.2, 0.2});
    const Dirichlet bc = fixed_min_x(mesh);
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd loads = tip_load(mesh);
    SolveOptions options;
    options.method = SolveMethod::kDirect;

    const Eigen::VectorXd observed = solve_elastostatics(mesh, steel, bc, loads, options).u;
    const Eigen::VectorXd baseline = direct_solve_without_cost(mesh, steel, bc, loads);
    REQUIRE(observed.size() == baseline.size());
    CHECK(std::memcmp(observed.data(), baseline.data(),
                      static_cast<std::size_t>(observed.size()) * sizeof(double)) == 0);
}

TEST_CASE("plate-hole direct-regime symbolic factor count matches Eigen LDLT") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    constexpr std::string_view kPart = "tests/fixtures/parts/plate_hole.step";
    if (!std::filesystem::exists(kPart)) {
        SKIP("plate_hole.step missing");
    }
    // h=4 mm produces ~221k DOF on this mesher and is intentionally in the CG
    // regime; allocating its direct factor would defeat the count-only API and
    // make the test itself exceed ten minutes. Use the same CAD part at a
    // direct-regime h for the exact numeric-factor cross-check.
    constexpr double kH = 0.02;
    const auto model = polymesh::pipeline::Model::load(std::string(kPart));
    const auto plan =
        polymesh::pipeline::build_refinement_plan(model, kH, {}, false, false, 0);
    auto volume = polymesh::pipeline::volume_mesh(
        model, kH, polymesh::pipeline::VolumeMesher::kGradedTet, 2, false, plan.refine_seeds,
        plan.seed_band, 0.0, 0, 0, 0, {}, plan.size_field);
    volume.mesh.check_validity();
    const Dirichlet bc = fixed_mesh_min_x(volume.mesh);
    REQUIRE_FALSE(bc.dof_values.empty());

    const SolveCostEstimate symbolic = analyze_solve_cost(volume.mesh, bc);
    const Material steel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
    const Eigen::VectorXd zero_loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(volume.mesh.nodes.size()));
    ReducedAssembly reduced =
        assemble_reduced_without_cost(volume.mesh, steel, bc, zero_loads);
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt(reduced.stiffness);
    REQUIRE(ldlt.info() == Eigen::Success);
    INFO("nodes=" << volume.mesh.nodes.size() << " elements=" << volume.mesh.elements.size()
                  << " nfree=" << symbolic.nfree);
    CHECK(symbolic.factor_nnz ==
          static_cast<std::uint64_t>(ldlt.matrixL().nestedExpression().nonZeros()));
}
