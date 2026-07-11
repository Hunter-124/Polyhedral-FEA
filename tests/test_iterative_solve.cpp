// SPDX-License-Identifier: BSD-3-Clause

// F2: iterative CG path — agreement with direct on small cases, auto-selection
// above the free-DOF threshold, and a moderately large free system (~10k+ DOFs).

#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

using namespace polymesh::fea;
using namespace polymesh::test_support;

namespace {

const Material kSteel{.youngs_modulus = 200e9, .poissons_ratio = 0.3};

/// Cantilever along x, fixed at x=0, gravity body load in -z.
struct CantileverSetup {
    NodalMesh mesh;
    Dirichlet bc;
    Eigen::VectorXd loads;
    double length = 1.0;
    Eigen::Index nfree = 0;
};

CantileverSetup make_cantilever_hex(int nx, int ny, int nz) {
    CantileverSetup s;
    s.length = 1.0;
    const double width = 0.1;
    s.mesh = box_hex_mesh(nx, ny, nz, {s.length, width, width});
    s.mesh.check_validity();

    for (std::size_t i = 0; i < s.mesh.nodes.size(); ++i) {
        if (s.mesh.nodes[i][0] < 1e-12) {
            s.bc.fix_node(static_cast<std::uint32_t>(i));
        }
    }
    const double bz = -1e6;
    s.loads = assemble_body_load(
        s.mesh, [&](const Eigen::Vector3d&) { return Eigen::Vector3d(0.0, 0.0, bz); });

    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(s.mesh.nodes.size());
    s.nfree = ndof - static_cast<Eigen::Index>(s.bc.dof_values.size());
    return s;
}

double mean_tip_uz(const NodalMesh& mesh, const Eigen::VectorXd& u, double length) {
    double tip = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        if (mesh.nodes[i][0] > length - 1e-12) {
            tip += u[3 * static_cast<Eigen::Index>(i) + 2];
            ++count;
        }
    }
    REQUIRE(count > 0);
    return tip / count;
}

} // namespace

TEST_CASE("select_solve_method respects auto threshold and overrides") {
    SolveOptions opt;
    opt.method = SolveMethod::kAuto;
    opt.cg_threshold = 8000;
    CHECK(select_solve_method(8000, opt) == SolveMethod::kDirect);
    CHECK(select_solve_method(8001, opt) == SolveMethod::kCG);

    opt.method = SolveMethod::kDirect;
    CHECK(select_solve_method(100000, opt) == SolveMethod::kDirect);

    opt.method = SolveMethod::kCG;
    CHECK(select_solve_method(1, opt) == SolveMethod::kCG);
}

TEST_CASE("forced CG matches direct LDLT on small cantilever") {
    auto setup = make_cantilever_hex(12, 2, 2);
    REQUIRE(setup.nfree < 8000);

    SolveOptions opt_direct;
    opt_direct.method = SolveMethod::kDirect;
    SolveOptions opt_cg;
    opt_cg.method = SolveMethod::kCG;
    opt_cg.cg_tol = 1e-12;
    const auto u_direct =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_direct);
    const auto u_cg =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_cg);

    const double tip_d = mean_tip_uz(setup.mesh, u_direct, setup.length);
    const double tip_cg = mean_tip_uz(setup.mesh, u_cg, setup.length);
    INFO("tip direct " << tip_d << " CG " << tip_cg);
    REQUIRE(std::abs(tip_d) > 0.0);
    CHECK(std::abs(tip_cg - tip_d) / std::abs(tip_d) < 1e-8);

    const double rel_l2 = (u_cg - u_direct).norm() / u_direct.norm();
    CHECK(rel_l2 < 1e-8);
}

TEST_CASE("forced CG reproduces constant-strain patch within solver tol") {
    // Distorted hex8 unit box; boundary u = G x. Direct is exact; CG within tol.
    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, //
        3e-4, -8e-4, 5e-4,  //
        -6e-4, 2e-4, 7e-4;

    const Eigen::Vector3d size(1.0, 0.8, 1.2);
    NodalMesh mesh = box_hex_mesh(3, 3, 3, size);
    distort_interior(mesh, 0.15, size[0] / 3.0, /*seed=*/42);
    mesh.check_validity();

    Eigen::Vector3d lo = mesh.nodes.front();
    Eigen::Vector3d hi = mesh.nodes.front();
    for (const auto& p : mesh.nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const double btol = 1e-9;
    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& p = mesh.nodes[i];
        const bool boundary =
            (p - lo).cwiseAbs().minCoeff() < btol || (hi - p).cwiseAbs().minCoeff() < btol;
        if (boundary) {
            bc.fix_node(static_cast<std::uint32_t>(i), g * p);
        }
    }
    const Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));

    SolveOptions opt_direct;
    opt_direct.method = SolveMethod::kDirect;
    SolveOptions opt_cg;
    opt_cg.method = SolveMethod::kCG;
    opt_cg.cg_tol = 1e-12;
    const auto u_direct = solve_elastostatics(mesh, kSteel, bc, loads, opt_direct);
    const auto u_cg = solve_elastostatics(mesh, kSteel, bc, loads, opt_cg);

    double max_err_direct = 0.0;
    double max_err_cg = 0.0;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const Eigen::Vector3d exact = g * mesh.nodes[i];
        max_err_direct =
            std::max(max_err_direct,
                     (u_direct.segment<3>(3 * static_cast<Eigen::Index>(i)) - exact).norm());
        max_err_cg = std::max(
            max_err_cg, (u_cg.segment<3>(3 * static_cast<Eigen::Index>(i)) - exact).norm());
    }
    CHECK(max_err_direct < 1e-12); // sacred direct path
    // CG vs exact: looser, driven by relative residual tol.
    CHECK(max_err_cg < 1e-8);
    CHECK((u_cg - u_direct).norm() / std::max(u_direct.norm(), 1e-30) < 1e-8);
}

TEST_CASE("auto path selects CG above threshold and solves large free system") {
    // ~50×10×8 hex8 cantilever: nodes = 51×11×9 = 5049, free DOFs ≈ 3*(5049−99) ≈ 14850.
    auto setup = make_cantilever_hex(50, 10, 8);
    INFO("nfree=" << setup.nfree << " nodes=" << setup.mesh.nodes.size());
    REQUIRE(setup.nfree > 10000);

    SolveOptions auto_opt; // kAuto, default threshold 8000
    CHECK(select_solve_method(setup.nfree, auto_opt) == SolveMethod::kCG);

    const auto u = solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, auto_opt);
    const double tip = mean_tip_uz(setup.mesh, u, setup.length);
    INFO("large-mesh tip uz " << tip << " m");
    REQUIRE(std::isfinite(tip));
    // Gravity load bz < 0 ⇒ tip deflects in −z; magnitude should be small but nonzero.
    CHECK(tip < 0.0);
    CHECK(std::abs(tip) > 1e-12);
    CHECK(std::abs(tip) < 1.0); // steel, short beam — not metres of tip drop
}
