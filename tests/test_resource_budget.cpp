// SPDX-License-Identifier: BSD-3-Clause

#include "fea/assembly.hpp"
#include "fea/resource_budget.hpp"
#include "fea/solve.hpp"
#include "fea/solve_cost.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace polymesh::fea;
using polymesh::test_support::box_hex_mesh;

namespace {

const Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

Dirichlet fix_min_x(const NodalMesh& mesh) {
    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i].x() < 1e-12) {
            bc.fix_node(static_cast<std::uint32_t>(i));
        }
    }
    return bc;
}

} // namespace

TEST_CASE("solve resource estimate grows with mesh DOF") {
    const auto small = box_hex_mesh(1, 1, 1, {1.0, 1.0, 1.0});
    const auto large = box_hex_mesh(4, 3, 2, {1.0, 1.0, 1.0});
    const auto small_dof = 3 * static_cast<Eigen::Index>(small.nodes.size());
    const auto large_dof = 3 * static_cast<Eigen::Index>(large.nodes.size());

    const auto a = estimate_solve_resources(small, small_dof);
    const auto b = estimate_solve_resources(large, large_dof);
    REQUIRE(large_dof > small_dof);
    CHECK(b.csr_nnz_upper > a.csr_nnz_upper);
    CHECK(b.direct_peak_bytes > a.direct_peak_bytes);
    CHECK(b.cg_peak_bytes > a.cg_peak_bytes);
}

TEST_CASE("solve resource estimate accepts exact symbolic factor count") {
    const auto mesh = box_hex_mesh(3, 2, 2, {1.0, 1.0, 1.0});
    const auto bc = fix_min_x(mesh);
    const auto cost = analyze_solve_cost(mesh, bc);
    const auto estimate = estimate_solve_resources(mesh, cost.nfree, cost.factor_nnz);

    CHECK(estimate.ldlt_factor_nnz == cost.factor_nnz);
    CHECK(estimate.ldlt_factor_bytes > 0);
}

TEST_CASE("CSR connectivity bound tracks an assembled small system") {
    const auto mesh = box_hex_mesh(4, 4, 4, {1.0, 1.0, 1.0});
    const auto ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    const auto estimate = estimate_solve_resources(mesh, ndof);
    const auto stiffness = assemble_stiffness(mesh, kSteel);
    const auto actual_nnz = static_cast<std::uint64_t>(stiffness.nonZeros());

    REQUIRE(actual_nnz > 0);
    CHECK(estimate.csr_nnz_upper >= actual_nnz);
    // Dense local 24x24 hex blocks count shared couplings more than once. On a
    // structured 3-D mesh this allocation-free upper bound stays within 4x of
    // the unique assembled CSR pattern.
    CHECK(estimate.csr_nnz_upper <= 4 * actual_nnz);
}

TEST_CASE("tiny explicit memory cap refuses before solve allocation") {
    const auto mesh = box_hex_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    const auto bc = fix_min_x(mesh);
    const auto loads = Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    SolveOptions options;
    options.method = SolveMethod::kDirect;
    options.max_mem_gb = 0.000001; // 1000 bytes

    try {
        (void)solve_elastostatics(mesh, kSteel, bc, loads, options).u;
        FAIL("tiny memory cap should reject the solve");
    } catch (const FeaError& e) {
        const std::string message = e.what();
        CHECK(message.find("estimated LDLT solve footprint") != std::string::npos);
        CHECK(message.find("effective memory cap 1000 B") != std::string::npos);
        CHECK(message.find("limiting term:") != std::string::npos);
        CHECK(message.find("raise --max-mem <GB>") != std::string::npos);
    }
}

TEST_CASE("auto solve records LDLT to CG downgrade when only CG fits") {
    const auto mesh = box_hex_mesh(6, 6, 6, {1.0, 1.0, 1.0});
    const auto bc = fix_min_x(mesh);
    const auto ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    const auto nfree = ndof - static_cast<Eigen::Index>(bc.dof_values.size());
    const auto symbolic = analyze_solve_cost(mesh, bc);
    const auto estimate =
        estimate_solve_resources(mesh, nfree, symbolic.factor_nnz);
    REQUIRE(estimate.direct_peak_bytes > estimate.cg_peak_bytes);
    const auto cap =
        estimate.cg_peak_bytes + (estimate.direct_peak_bytes - estimate.cg_peak_bytes) / 2;

    SolveOptions options;
    options.method = SolveMethod::kAuto;
    options.cg_threshold = nfree + 1; // threshold policy would choose LDLT
    const auto decision = decide_solve_method(nfree, options, estimate, cap);
    REQUIRE(decision.method == SolveMethod::kCG);
    CHECK(decision.note.find("LDLT estimate") != std::string::npos);
    CHECK(decision.note.find("using CG estimate") != std::string::npos);

    std::vector<std::string> recorded_notes;
    options.max_mem_gb = static_cast<double>(cap) / 1'000'000'000.0;
    options.on_note = [&](std::string_view note) { recorded_notes.emplace_back(note); };
    const auto loads = Eigen::VectorXd::Zero(ndof);
    const auto u = solve_elastostatics(mesh, kSteel, bc, loads, options).u;
    CHECK(u.isZero());
    REQUIRE(recorded_notes.size() >= 2);
    CHECK(recorded_notes.front() == decision.note);
    CHECK(recorded_notes.back().find("CG converged with ") != std::string::npos);
}
