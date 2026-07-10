// SPDX-License-Identifier: BSD-3-Clause
#include "fea/backend.hpp"
#include "fea/spmv.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/SparseCore>

#include <cmath>
#include <random>
#include <vector>

using polymesh::fea::active_backend;
using polymesh::fea::Backend;
using polymesh::fea::csr_from_eigen;
using polymesh::fea::CsrMatrix;
using polymesh::fea::spmv;
using polymesh::fea::spmv_cpu;
using polymesh::fea::try_spmv_cuda;

namespace {

Eigen::SparseMatrix<double> make_spd_test_matrix(int n, unsigned seed) {
    // Symmetric positive definite-ish: A = B^T B + n I on free DOFs.
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<Eigen::Triplet<double>> trips;
    trips.reserve(static_cast<std::size_t>(n) * 6);
    for (int i = 0; i < n; ++i) {
        trips.emplace_back(i, i, static_cast<double>(n) + 1.0);
        if (i + 1 < n) {
            const double off = dist(rng);
            trips.emplace_back(i, i + 1, off);
            trips.emplace_back(i + 1, i, off);
        }
        if (i + 3 < n) {
            const double off = 0.25 * dist(rng);
            trips.emplace_back(i, i + 3, off);
            trips.emplace_back(i + 3, i, off);
        }
    }
    Eigen::SparseMatrix<double> a(n, n);
    a.setFromTriplets(trips.begin(), trips.end());
    a.makeCompressed();
    return a;
}

std::vector<double> random_vector(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(-2.0, 2.0);
    std::vector<double> x(static_cast<std::size_t>(n));
    for (double& v : x) {
        v = dist(rng);
    }
    return x;
}

double max_abs_diff(const std::vector<double>& a, const Eigen::VectorXd& b) {
    REQUIRE(static_cast<Eigen::Index>(a.size()) == b.size());
    double m = 0.0;
    for (Eigen::Index i = 0; i < b.size(); ++i) {
        m = std::max(m, std::abs(a[static_cast<std::size_t>(i)] - b[i]));
    }
    return m;
}

double max_abs_diff(const std::vector<double>& a, const std::vector<double>& b) {
    REQUIRE(a.size() == b.size());
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        m = std::max(m, std::abs(a[i] - b[i]));
    }
    return m;
}

} // namespace

TEST_CASE("CPU CSR SpMV matches Eigen sparse * vector") {
    constexpr int n = 64;
    const auto a = make_spd_test_matrix(n, 42);
    const auto x_host = random_vector(n, 7);
    Eigen::VectorXd x = Eigen::Map<const Eigen::VectorXd>(x_host.data(), n);
    const Eigen::VectorXd y_ref = a * x;

    const CsrMatrix csr = csr_from_eigen(a);
    REQUIRE(csr.rows == static_cast<std::size_t>(n));
    REQUIRE(csr.cols == static_cast<std::size_t>(n));
    REQUIRE(csr.nnz() == static_cast<std::size_t>(a.nonZeros()));

    std::vector<double> y(static_cast<std::size_t>(n), 0.0);
    spmv_cpu(csr, x_host, y);

    // Exact arithmetic path for this size; allow tiny FP noise from order.
    CHECK(max_abs_diff(y, y_ref) < 1e-12);
}

TEST_CASE("dispatch spmv matches CPU reference on host backend") {
    constexpr int n = 32;
    const auto a = make_spd_test_matrix(n, 99);
    const auto x_host = random_vector(n, 3);
    const CsrMatrix csr = csr_from_eigen(a);

    std::vector<double> y_cpu(static_cast<std::size_t>(n), 0.0);
    std::vector<double> y_dispatch(static_cast<std::size_t>(n), 0.0);
    spmv_cpu(csr, x_host, y_cpu);
    spmv(csr, x_host, y_dispatch);

    // When CUDA is active, dispatch may use the device path; still must match CPU.
    CHECK(max_abs_diff(y_cpu, y_dispatch) < 1e-12);
}

TEST_CASE("CUDA CSR SpMV parity vs CPU (skip without toolkit/device)") {
#ifndef POLYMESH_WITH_CUDA
    SKIP("POLYMESH_WITH_CUDA=OFF — CUDA SpMV not compiled");
#else
    constexpr int n = 128;
    const auto a = make_spd_test_matrix(n, 11);
    const auto x_host = random_vector(n, 13);
    const CsrMatrix csr = csr_from_eigen(a);

    std::vector<double> y_cpu(static_cast<std::size_t>(n), 0.0);
    spmv_cpu(csr, x_host, y_cpu);

    std::vector<double> y_cuda(static_cast<std::size_t>(n), -1.0);
    if (!try_spmv_cuda(csr, x_host, y_cuda)) {
        SKIP("CUDA compiled but no usable device (try_spmv_cuda returned false)");
    }
    CHECK(active_backend() == Backend::kCuda);
    CHECK(max_abs_diff(y_cpu, y_cuda) < 1e-12);
#endif
}

TEST_CASE("empty and rectangular CSR SpMV") {
    // 0x0
    {
        CsrMatrix empty;
        empty.row_ptr = {0};
        std::vector<double> x, y;
        REQUIRE_NOTHROW(spmv_cpu(empty, x, y));
        CHECK(y.empty());
#ifndef POLYMESH_WITH_CUDA
        CHECK_FALSE(try_spmv_cuda(empty, x, y));
#endif
    }

    // 3x2 rectangular
    {
        std::vector<Eigen::Triplet<double>> trips = {
            {0, 0, 1.0},
            {0, 1, 2.0},
            {1, 0, 3.0},
            {2, 1, 4.0},
        };
        Eigen::SparseMatrix<double> a(3, 2);
        a.setFromTriplets(trips.begin(), trips.end());
        const CsrMatrix csr = csr_from_eigen(a);
        const std::vector<double> x = {1.0, -1.0};
        std::vector<double> y(3, 0.0);
        spmv_cpu(csr, x, y);
        CHECK_THAT(y[0], Catch::Matchers::WithinAbs(-1.0, 1e-15));
        CHECK_THAT(y[1], Catch::Matchers::WithinAbs(3.0, 1e-15));
        CHECK_THAT(y[2], Catch::Matchers::WithinAbs(-4.0, 1e-15));
    }
}
