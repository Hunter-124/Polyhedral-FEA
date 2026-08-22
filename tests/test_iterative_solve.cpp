// SPDX-License-Identifier: BSD-3-Clause

// F2: iterative CG path — agreement with direct on small cases, auto-selection
// above the free-DOF threshold, and a moderately large free system (~10k+ DOFs).

#include "fea/solve.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Eigen/Eigenvalues>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

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

TEST_CASE("default auto threshold keeps mid-size systems on the direct path") {
    // Regression guard for the 8000-free-DOF cliff: kAuto used to hand 8k-50k
    // systems to CG, which was 100-200x slower than LDLT on exactly these
    // sparsities (11040-DOF plate-with-hole hex: 158 s CG vs 0.9 s LDLT).
    const SolveOptions defaults;
    CHECK(defaults.method == SolveMethod::kAuto);
    CHECK(select_solve_method(8001, defaults) == SolveMethod::kDirect);
    CHECK(select_solve_method(50000, defaults) == SolveMethod::kDirect);
    CHECK(select_solve_method(50001, defaults) == SolveMethod::kCG);
}

TEST_CASE("CG honours its iteration cap instead of grinding") {
    // A cap the system cannot possibly meet must fail fast, not run to 2*nfree.
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions opt;
    opt.method = SolveMethod::kCG;
    opt.cg_tol = 1e-14;
    opt.cg_accept_tol = 1e-14;
    opt.cg_max_iters = 2;
    try {
        static_cast<void>(solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt).u);
        FAIL("two CG iterations unexpectedly converged");
    } catch (const polymesh::fea::FeaError& e) {
        const std::string message = e.what();
        CHECK(message.find("preconditioner attempts=[") != std::string::npos);
        CHECK(message.find("incomplete Cholesky") != std::string::npos);
        CHECK(message.find("Jacobi") != std::string::npos);
    }
}

TEST_CASE("unattainable CG tolerance does not cycle reliable restarts") {
    // This is the same case that exposed the old false success. With tol=1e-12,
    // recursive residuals used to claim convergence while b-K*x remained
    // 2e-11 to 6e-11. The first reliable-residual implementation corrected the
    // result but restarted 29 times under IC and 8 under Jacobi. A reduction-
    // bound policy must report the unattainable tolerance without that cycle.
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions options;
    options.method = SolveMethod::kCG;
    options.cg_tol = 1e-12;
    options.cg_accept_tol = 1e-13;
    options.cg_max_iters = 1000;

    try {
        static_cast<void>(
            solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u);
        FAIL("an independently unattainable tolerance unexpectedly converged");
    } catch (const polymesh::fea::FeaError& e) {
        const std::string message = e.what();
        const std::string marker = "reliable restarts=";
        std::size_t cursor = 0;
        int attempts = 0;
        while ((cursor = message.find(marker, cursor)) != std::string::npos) {
            cursor += marker.size();
            const int count = std::stoi(message.substr(cursor));
            CHECK(count >= 1);
            CHECK(count <= 4);
            ++attempts;
        }
        CHECK(attempts == 2);
        CHECK(message.find("attainable-accuracy limit") != std::string::npos);
    }
}

TEST_CASE("CG may accept a measured residual without claiming target convergence") {
    // The target remains 1e-14, but this system's independently recomputed
    // residual floors above it. A result below the explicit acceptance
    // threshold is returned with loud provenance rather than being mislabeled
    // as target convergence.
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions options;
    options.method = SolveMethod::kCG;
    options.cg_tol = 1e-14;
    options.cg_accept_tol = 1e-8;
    options.cg_max_iters = 1000;

    std::string notes;
    options.on_note = [&](std::string_view note) {
        if (!notes.empty()) {
            notes += '\n';
        }
        notes += note;
    };
    const Eigen::VectorXd u =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u;
    const Eigen::SparseMatrix<double> k = assemble_stiffness(setup.mesh, kSteel);
    const Eigen::VectorXd full_residual = setup.loads - k * u;

    double free_residual2 = 0.0;
    double free_load2 = 0.0;
    for (Eigen::Index dof = 0; dof < full_residual.size(); ++dof) {
        if (!setup.bc.dof_values.contains(dof)) {
            free_residual2 += full_residual[dof] * full_residual[dof];
            free_load2 += setup.loads[dof] * setup.loads[dof];
        }
    }
    const double independently_measured =
        std::sqrt(free_residual2 / std::max(free_load2, 1e-30));

    CHECK(independently_measured > options.cg_tol);
    CHECK(independently_measured <= options.cg_accept_tol);
    CHECK(notes.find("CG TARGET NOT MET: accepted ") != std::string::npos);
    CHECK(notes.find("achieved true relative residual=") != std::string::npos);
    CHECK(notes.find("attempts=[") != std::string::npos);
}

TEST_CASE("solve rejects invalid CG tolerances and non-finite inputs") {
    auto setup = make_cantilever_hex(4, 1, 1);
    SolveOptions options;
    options.method = SolveMethod::kCG;

    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        options.cg_tol = invalid;
        CHECK_THROWS_AS(
            solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u,
            polymesh::fea::FeaError);
    }

    options.cg_tol = 1e-8;
    for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        options.cg_accept_tol = invalid;
        CHECK_THROWS_AS(
            solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u,
            polymesh::fea::FeaError);
    }

    options.cg_accept_tol = 1e-6;
    CHECK_NOTHROW(solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u);

    Eigen::VectorXd invalid_loads = setup.loads;
    invalid_loads[0] = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(solve_elastostatics(setup.mesh, kSteel, setup.bc, invalid_loads, options).u,
                    polymesh::fea::FeaError);

    Dirichlet invalid_bc = setup.bc;
    REQUIRE_FALSE(invalid_bc.dof_values.empty());
    invalid_bc.dof_values.begin()->second = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(solve_elastostatics(setup.mesh, kSteel, invalid_bc, setup.loads, options).u,
                    polymesh::fea::FeaError);
}

TEST_CASE("forced CG matches direct LDLT on small cantilever") {
    auto setup = make_cantilever_hex(12, 2, 2);
    REQUIRE(setup.nfree < 8000);

    SolveOptions opt_direct;
    opt_direct.method = SolveMethod::kDirect;
    SolveOptions opt_cg;
    opt_cg.method = SolveMethod::kCG;
    // 1e-12 was a false-success contract: the old recursive residual crossed it
    // while independently measured b-K*x remained 2e-11 to 6e-11 at this
    // 1000-iteration budget. 1e-10 is attainable and still two orders tighter
    // than the displacement-agreement contract below.
    opt_cg.cg_tol = 1e-10;
    std::string cg_notes;
    opt_cg.on_note = [&](std::string_view note) {
        if (!cg_notes.empty()) {
            cg_notes += '\n';
        }
        cg_notes += note;
    };
    const auto u_direct =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_direct).u;
    const auto u_cg = solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_cg).u;

    // This forced-CG solve runs the equilibrated cascade; the same tolerance
    // contract must still hold.
    CHECK(cg_notes.find("equilibrated") != std::string::npos);

    const double tip_d = mean_tip_uz(setup.mesh, u_direct, setup.length);
    const double tip_cg = mean_tip_uz(setup.mesh, u_cg, setup.length);
    INFO("tip direct " << tip_d << " CG " << tip_cg);
    REQUIRE(std::abs(tip_d) > 0.0);
    CHECK(std::abs(tip_cg - tip_d) / std::abs(tip_d) < 1e-8);

    const double rel_l2 = (u_cg - u_direct).norm() / u_direct.norm();
    CHECK(rel_l2 < 1e-8);
}

TEST_CASE("CG progress reporting does not introduce recurrence restarts") {
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions plain;
    plain.method = SolveMethod::kCG;
    plain.cg_tol = 1e-10;
    const auto u_plain = solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, plain).u;

    SolveOptions reported = plain;
    reported.cg_progress_chunk = 1; // stress the old pathological case
    int callback_count = 0;
    int last_iter = 0;
    reported.on_progress = [&](int iter, int max_iters, double residual) {
        ++callback_count;
        CHECK(iter >= last_iter);
        CHECK(iter <= max_iters);
        CHECK(std::isfinite(residual));
        last_iter = iter;
    };
    const auto u_reported =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, reported).u;

    CHECK(callback_count > 1);
    CHECK(last_iter > 0);
    // Reporting is only an observer. It must not alter arithmetic, reliable
    // residual decisions, or the Krylov space as the old chunked path did.
    CHECK(u_reported == u_plain);
}

TEST_CASE("CG success uses a true residual and reports preconditioner provenance") {
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions options;
    options.method = SolveMethod::kCG;
    options.cg_tol = 1e-10;

    double reported_final_residual = std::numeric_limits<double>::infinity();
    std::string notes;
    options.on_progress = [&](int, int, double residual) {
        reported_final_residual = residual;
    };
    options.on_note = [&](std::string_view note) {
        if (!notes.empty()) {
            notes += '\n';
        }
        notes += note;
    };

    const Eigen::VectorXd u =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, options).u;
    const Eigen::SparseMatrix<double> k = assemble_stiffness(setup.mesh, kSteel);
    const Eigen::VectorXd full_residual = setup.loads - k * u;

    double free_residual2 = 0.0;
    double free_load2 = 0.0;
    for (Eigen::Index dof = 0; dof < full_residual.size(); ++dof) {
        if (!setup.bc.dof_values.contains(dof)) {
            free_residual2 += full_residual[dof] * full_residual[dof];
            free_load2 += setup.loads[dof] * setup.loads[dof];
        }
    }
    const double independently_measured =
        std::sqrt(free_residual2 / std::max(free_load2, 1e-30));

    CHECK(reported_final_residual <= options.cg_tol);
    CHECK(independently_measured <= options.cg_tol);
    CHECK(notes.find("CG converged with ") != std::string::npos);
    CHECK(notes.find("true relative residual=") != std::string::npos);
    INFO(notes);
    CHECK(notes.find("reliable restarts=0") != std::string::npos);
    CHECK(notes.find("attempts=[") != std::string::npos);
}

TEST_CASE("CG acceptance threshold does not alter honest convergence") {
    auto setup = make_cantilever_hex(12, 2, 2);
    SolveOptions defaults;
    defaults.method = SolveMethod::kCG;
    defaults.cg_tol = 1e-10;

    SolveOptions tightened = defaults;
    tightened.cg_accept_tol = defaults.cg_tol;

    const Eigen::VectorXd u_default =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, defaults).u;
    const Eigen::VectorXd u_tightened =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, tightened).u;

    // Both attempts meet the target. The fallback acceptance policy is
    // unreachable and therefore cannot perturb the returned arithmetic.
    CHECK(u_tightened == u_default);
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
    const auto u_direct = solve_elastostatics(mesh, kSteel, bc, loads, opt_direct).u;
    const auto u_cg = solve_elastostatics(mesh, kSteel, bc, loads, opt_cg).u;

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

    // This size is below the default threshold now, so drive the CG branch of
    // kAuto through the documented override instead of the old default.
    SolveOptions auto_opt;
    auto_opt.cg_threshold = setup.nfree - 1;
    CHECK(select_solve_method(setup.nfree, auto_opt) == SolveMethod::kCG);

    const auto u = solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, auto_opt).u;
    const double tip = mean_tip_uz(setup.mesh, u, setup.length);
    INFO("large-mesh tip uz " << tip << " m");
    REQUIRE(std::isfinite(tip));
    // Gravity load bz < 0 ⇒ tip deflects in −z; magnitude should be small but nonzero.
    CHECK(tip < 0.0);
    CHECK(std::abs(tip) > 1e-12);
    CHECK(std::abs(tip) < 1.0); // steel, short beam — not metres of tip drop

    // Same system with plain defaults must take the direct path and agree.
    const SolveOptions defaults;
    REQUIRE(select_solve_method(setup.nfree, defaults) == SolveMethod::kDirect);
    const auto u_direct =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, defaults).u;
    CHECK((u - u_direct).norm() / u_direct.norm() < 1e-6);
}

TEST_CASE("symmetric_diagonal_scaling equilibrates an SPD matrix to unit diagonal") {
    // Diagonally dominant SPD with diagonal spread 25x (4, 9, 100).
    const std::vector<Eigen::Triplet<double>> triplets = {
        {0, 0, 4.0}, {0, 1, 1.0},  {0, 2, 0.5},  //
        {1, 0, 1.0}, {1, 1, 9.0},  {1, 2, -1.0}, //
        {2, 0, 0.5}, {2, 1, -1.0}, {2, 2, 100.0},
    };
    Eigen::SparseMatrix<double> a(3, 3);
    a.setFromTriplets(triplets.begin(), triplets.end());

    const Eigen::VectorXd s = symmetric_diagonal_scaling(a);
    REQUIRE(s.size() == 3);
    CHECK(std::abs(s[0] - 0.5) < 1e-15);
    CHECK(std::abs(s[1] - 1.0 / 3.0) < 1e-15);
    CHECK(std::abs(s[2] - 0.1) < 1e-15);

    // The exact congruence S·A·S has unit diagonal and stays SPD.
    const Eigen::Matrix3d scaled = s.asDiagonal() * Eigen::Matrix3d(a) * s.asDiagonal();
    for (int i = 0; i < 3; ++i) {
        CHECK(std::abs(scaled(i, i) - 1.0) < 1e-15);
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(scaled);
    REQUIRE(eig.info() == Eigen::Success);
    CHECK(eig.eigenvalues().minCoeff() > 0.0);
}

TEST_CASE("symmetric_diagonal_scaling rejects a non-positive diagonal") {
    // Missing (zero) diagonal entry: not SPD.
    const std::vector<Eigen::Triplet<double>> zero_diag = {
        {0, 1, 1.0},
        {1, 0, 1.0},
        {1, 1, 4.0},
    };
    Eigen::SparseMatrix<double> a(2, 2);
    a.setFromTriplets(zero_diag.begin(), zero_diag.end());
    CHECK_THROWS_AS(symmetric_diagonal_scaling(a), polymesh::fea::FeaError);

    // Explicitly negative diagonal entry: not SPD.
    const std::vector<Eigen::Triplet<double>> neg_diag = {
        {0, 0, -2.0},
        {0, 1, 1.0},
        {1, 0, 1.0},
        {1, 1, 4.0},
    };
    Eigen::SparseMatrix<double> b(2, 2);
    b.setFromTriplets(neg_diag.begin(), neg_diag.end());
    CHECK_THROWS_AS(symmetric_diagonal_scaling(b), polymesh::fea::FeaError);
}

TEST_CASE("equilibrated CG converges on a diagonally heterogeneous MPC system") {
    // An MPC master column picks up weight²-scaled stiffness, so a weight of
    // 1e3 spreads the K_ff diagonal by ~1e6 — the MPC/graded-mesh case
    // symmetric diagonal equilibration exists for.
    auto setup = make_cantilever_hex(12, 2, 2);

    std::vector<std::uint32_t> free_nodes;
    for (std::size_t i = 0; i < setup.mesh.nodes.size(); ++i) {
        if (setup.mesh.nodes[i][0] > 1e-12) {
            free_nodes.push_back(static_cast<std::uint32_t>(i));
        }
    }
    REQUIRE(free_nodes.size() >= 2);
    LinearConstraints constraints;
    constraints.add(LinearConstraint{
        .slave_dof = 3 * free_nodes.back() + 2,
        .masters = {{3 * free_nodes.front() + 2, 1e3}},
    });

    SolveOptions opt_cg;
    opt_cg.method = SolveMethod::kCG;
    double final_residual = std::numeric_limits<double>::infinity();
    std::string notes;
    opt_cg.on_progress = [&](int, int, double residual) { final_residual = residual; };
    opt_cg.on_note = [&](std::string_view note) {
        if (!notes.empty()) {
            notes += '\n';
        }
        notes += note;
    };
    const Eigen::VectorXd u_cg =
        solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_cg, &constraints).u;

    INFO(notes);
    CHECK(notes.find("equilibrated") != std::string::npos);
    // The completion residual is the true relative residual of the original
    // (physical) system and must meet the acceptance contract.
    CHECK(final_residual <= opt_cg.cg_accept_tol);

    // The direct path solves the identical reduced system; CG must agree.
    SolveOptions opt_direct;
    opt_direct.method = SolveMethod::kDirect;
    const Eigen::VectorXd u_direct = solve_elastostatics(setup.mesh, kSteel, setup.bc, setup.loads, opt_direct, &constraints).u;
    CHECK((u_cg - u_direct).norm() / u_direct.norm() < 1e-5);
}
