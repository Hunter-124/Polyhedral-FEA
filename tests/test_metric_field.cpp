// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/metric_field.hpp"

#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using polymesh::adapt::Metric3d;
using polymesh::adapt::MetricGrid;

namespace {

void check_matrix_near(const Eigen::Matrix3d& actual, const Eigen::Matrix3d& expected,
                       double tolerance) {
    CHECK_THAT((actual - expected).norm(),
               WithinAbs(0.0, tolerance * std::max(1.0, expected.norm())));
}

std::vector<double> make_limited_field() {
    MetricGrid grid({-0.2, 0.1, -0.3}, {0.8, 0.9, 0.5}, {5, 4, 3});
    grid.fill([](const Eigen::Vector3d& x) {
        const double h =
            0.018 + 0.013 * (x.x() + 0.2) + 0.007 * (x.y() - 0.1) + 0.003 * (x.z() + 0.3);
        const Eigen::Matrix3d axes =
            Eigen::AngleAxisd(0.31 * x.x() - 0.17 * x.y(), Eigen::Vector3d::UnitZ())
                .toRotationMatrix();
        return Metric3d::from_axes({h, 0.72 * h, 1.35 * h}, axes);
    });
    grid.set(1, 2, 1, Metric3d::isotropic(0.004));
    polymesh::adapt::limit_gradation(grid, 0.35);

    std::vector<double> serialized;
    const Eigen::Vector3i n = grid.resolution();
    serialized.reserve(static_cast<std::size_t>(n.prod()) * 9);
    for (int k = 0; k < n.z(); ++k) {
        for (int j = 0; j < n.y(); ++j) {
            for (int i = 0; i < n.x(); ++i) {
                const Eigen::Matrix3d metric =
                    grid.at_node(static_cast<std::size_t>(i), static_cast<std::size_t>(j),
                                 static_cast<std::size_t>(k))
                        .M;
                serialized.insert(serialized.end(), metric.data(), metric.data() + 9);
            }
        }
    }
    return serialized;
}

} // namespace

TEST_CASE("metric field isotropic length and principal sizes", "[metric]") {
    const double h = 0.037;
    const Metric3d metric = Metric3d::isotropic(h);
    const std::array<Eigen::Vector3d, 5> edges = {
        Eigen::Vector3d{0.0, 0.0, 0.0}, Eigen::Vector3d{0.13, -0.27, 0.51},
        Eigen::Vector3d{-0.91, 0.04, 0.33}, Eigen::Vector3d{1.7, -2.1, 0.8},
        Eigen::Vector3d{-0.002, 0.005, -0.011}};
    for (const Eigen::Vector3d& edge : edges) {
        CHECK_THAT(metric.length(edge), WithinRel(edge.norm() / h, 1e-14));
    }
    for (double size : metric.sizes()) {
        CHECK_THAT(size, WithinRel(h, 1e-14));
    }
}

TEST_CASE("metric field axes round-trip reconstructs the tensor", "[metric]") {
    const Eigen::Vector3d h{0.1, 0.01, 0.05};
    const Eigen::Matrix3d frame =
        (Eigen::AngleAxisd(0.61, Eigen::Vector3d{1.0, 2.0, -0.5}.normalized()) *
         Eigen::AngleAxisd(-0.23, Eigen::Vector3d::UnitY()))
            .toRotationMatrix();
    const Metric3d original = Metric3d::from_axes(h, frame);

    const Eigen::Vector3d recovered_h = original.sizes();
    Eigen::Vector3d sorted_original = h;
    std::sort(sorted_original.data(), sorted_original.data() + 3, std::greater<double>());
    for (int i = 0; i < 3; ++i) {
        CHECK_THAT(recovered_h[i], WithinRel(sorted_original[i], 1e-13));
    }
    const Eigen::Vector3d recovered_lambda =
        recovered_h.cwiseInverse().array().square().matrix();
    const Eigen::Matrix3d reconstructed =
        original.axes() * recovered_lambda.asDiagonal() * original.axes().transpose();
    check_matrix_near(reconstructed, original.M, 1e-12);
}

TEST_CASE("metric field eigenvector order and signs are bit deterministic", "[metric]") {
    const Eigen::Matrix3d frame =
        Eigen::AngleAxisd(0.47, Eigen::Vector3d{0.3, -0.8, 0.5}.normalized())
            .toRotationMatrix();
    const Metric3d first = Metric3d::from_axes({0.023, 0.081, 0.047}, frame);
    const Metric3d second = Metric3d::from_axes({0.023, 0.081, 0.047}, frame);
    const Eigen::Matrix3d first_axes = first.axes();
    const Eigen::Matrix3d second_axes = second.axes();
    CHECK(std::memcmp(first_axes.data(), second_axes.data(), 9 * sizeof(double)) == 0);
    for (int col = 0; col < 3; ++col) {
        int pivot = 0;
        for (int row = 1; row < 3; ++row) {
            if (std::abs(first_axes(row, col)) > std::abs(first_axes(pivot, col))) {
                pivot = row;
            }
        }
        CHECK(first_axes(pivot, col) > 0.0);
    }
}

TEST_CASE("metric field clamping enforces size and aspect bounds", "[metric]") {
    const Metric3d extreme =
        Metric3d::from_axes({1e-4, 0.1, 0.03}, Eigen::Matrix3d::Identity());
    const Eigen::Vector3d sizes = extreme.clamped(1e-5, 0.2, 10.0).sizes();
    CHECK(sizes.minCoeff() >= 1e-5);
    CHECK(sizes.maxCoeff() <= 0.2);
    CHECK(sizes.maxCoeff() / sizes.minCoeff() <= 10.0 + 1e-13);
    CHECK_THAT(sizes.maxCoeff() / sizes.minCoeff(), WithinRel(10.0, 1e-13));
}

TEST_CASE("metric field intersection selects restrictive principal sizes", "[metric]") {
    const Metric3d coarse = Metric3d::isotropic(0.1);
    const Metric3d fine = Metric3d::isotropic(0.025);
    check_matrix_near(coarse.intersect(fine).M, fine.M, 0.0);
    check_matrix_near(fine.intersect(coarse).M, fine.M, 0.0);

    const Metric3d a = Metric3d::from_axes({0.1, 0.02, 0.05}, Eigen::Matrix3d::Identity());
    const Metric3d b = Metric3d::from_axes({0.04, 0.03, 0.01}, Eigen::Matrix3d::Identity());
    const Metric3d ab = a.intersect(b);
    const Metric3d ba = b.intersect(a);
    const Metric3d expected =
        Metric3d::from_axes({0.04, 0.02, 0.01}, Eigen::Matrix3d::Identity());
    check_matrix_near(ab.M, expected.M, 1e-12);
    check_matrix_near(ab.M, ba.M, 1e-12);
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(ab.M);
    REQUIRE(eig.info() == Eigen::Success);
    CHECK(eig.eigenvalues().minCoeff() > 0.0);
}

TEST_CASE("metric field log interpolation has exact endpoints and geometric midpoint",
          "[metric]") {
    const Metric3d a = Metric3d::isotropic(0.02);
    const Metric3d b = Metric3d::isotropic(0.18);
    check_matrix_near(Metric3d::log_interp(a, b, 0.0).M, a.M, 1e-12);
    check_matrix_near(Metric3d::log_interp(a, b, 1.0).M, b.M, 1e-12);
    const double expected_h = std::sqrt(0.02 * 0.18);
    const Metric3d midpoint = Metric3d::log_interp(a, b, 0.5);
    for (double size : midpoint.sizes()) {
        CHECK_THAT(size, WithinRel(expected_h, 1e-13));
    }
}

TEST_CASE("metric field grid interpolates nodes and preserves uniform metrics", "[metric]") {
    MetricGrid varied({-1.0, 2.0, 0.5}, {2.0, 5.0, 2.5}, {4, 3, 5});
    varied.fill([](const Eigen::Vector3d& x) {
        return Metric3d::from_axes({0.03 + 0.002 * (x.x() + 1.0), 0.05 + 0.001 * (x.y() - 2.0),
                                    0.08 + 0.003 * (x.z() - 0.5)},
                                   Eigen::Matrix3d::Identity());
    });
    const Eigen::Vector3i n = varied.resolution();
    for (int k = 0; k < n.z(); ++k) {
        for (int j = 0; j < n.y(); ++j) {
            for (int i = 0; i < n.x(); ++i) {
                const std::size_t ui = static_cast<std::size_t>(i);
                const std::size_t uj = static_cast<std::size_t>(j);
                const std::size_t uk = static_cast<std::size_t>(k);
                check_matrix_near(varied.sample(varied.node_position(ui, uj, uk)).M,
                                  varied.at_node(ui, uj, uk).M, 1e-10);
            }
        }
    }

    MetricGrid uniform({0.0, 0.0, 0.0}, {1.0, 2.0, 3.0}, {3, 4, 5});
    const Metric3d constant = Metric3d::from_axes(
        {0.02, 0.07, 0.11},
        Eigen::AngleAxisd(0.4, Eigen::Vector3d::UnitX()).toRotationMatrix());
    uniform.fill([&](const Eigen::Vector3d&) { return constant; });
    for (const Eigen::Vector3d point :
         {Eigen::Vector3d{0.2, 0.8, 1.7}, Eigen::Vector3d{-4.0, 1.0, 9.0},
          Eigen::Vector3d{1.0, 2.0, 3.0}}) {
        check_matrix_near(uniform.sample(point).M, constant.M, 1e-10);
    }
    CHECK_THAT(uniform.as_size_field()(Eigen::Vector3d{0.3, 0.4, 0.5}),
               WithinRel(0.02, 1e-12));
}

TEST_CASE("metric field gradation limits every adjacent node without coarsening seed",
          "[metric]") {
    MetricGrid grid({0.0, 0.0, 0.0}, {0.4, 0.4, 0.4}, {5, 5, 5});
    grid.fill([](const Eigen::Vector3d&) { return Metric3d::isotropic(0.1); });
    grid.set(0, 0, 0, Metric3d::isotropic(1e-3));
    const Eigen::Matrix3d seed_before = grid.at_node(0, 0, 0).M;
    const double beta = 0.3;
    polymesh::adapt::limit_gradation(grid, beta);
    check_matrix_near(grid.at_node(0, 0, 0).M, seed_before, 0.0);

    const Eigen::Vector3i n = grid.resolution();
    auto check_direction = [&](int i0, int j0, int k0, int i1, int j1, int k1) {
        const Metric3d m0 =
            grid.at_node(static_cast<std::size_t>(i0), static_cast<std::size_t>(j0),
                         static_cast<std::size_t>(k0));
        const Metric3d m1 =
            grid.at_node(static_cast<std::size_t>(i1), static_cast<std::size_t>(j1),
                         static_cast<std::size_t>(k1));
        const Eigen::Vector3d edge =
            grid.node_position(static_cast<std::size_t>(i1), static_cast<std::size_t>(j1),
                               static_cast<std::size_t>(k1)) -
            grid.node_position(static_cast<std::size_t>(i0), static_cast<std::size_t>(j0),
                               static_cast<std::size_t>(k0));
        CHECK(m1.isotropic_size() / m0.isotropic_size() <=
              1.0 + beta * m0.length(edge) + 1e-11);
    };
    for (int k = 0; k < n.z(); ++k) {
        for (int j = 0; j < n.y(); ++j) {
            for (int i = 0; i < n.x(); ++i) {
                if (i + 1 < n.x()) {
                    check_direction(i, j, k, i + 1, j, k);
                    check_direction(i + 1, j, k, i, j, k);
                }
                if (j + 1 < n.y()) {
                    check_direction(i, j, k, i, j + 1, k);
                    check_direction(i, j + 1, k, i, j, k);
                }
                if (k + 1 < n.z()) {
                    check_direction(i, j, k, i, j, k + 1);
                    check_direction(i, j, k + 1, i, j, k);
                }
            }
        }
    }
}

TEST_CASE("metric field uniform complexity equals volume over h cubed", "[metric]") {
    MetricGrid grid({-1.0, 0.0, 2.0}, {1.0, 3.0, 6.0}, {4, 5, 6});
    const double h = 0.2;
    grid.fill([&](const Eigen::Vector3d&) { return Metric3d::isotropic(h); });
    const double expected = (2.0 * 3.0 * 4.0) / (h * h * h);
    CHECK_THAT(polymesh::adapt::complexity(grid), WithinRel(expected, 1e-9));
}

TEST_CASE("metric field complexity normalization reaches an unclamped target", "[metric]") {
    MetricGrid grid({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, {4, 4, 4});
    grid.fill([](const Eigen::Vector3d&) { return Metric3d::isotropic(0.2); });
    const double target = 1000.0;
    polymesh::adapt::normalize_complexity(grid, target, 1e-5, 1e3, 1e6);
    CHECK_THAT(polymesh::adapt::complexity(grid), WithinRel(target, 1e-6));
}

TEST_CASE("metric field gradation is bit deterministic", "[metric]") {
    const std::vector<double> first = make_limited_field();
    const std::vector<double> second = make_limited_field();
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(double)) == 0);
}

TEST_CASE("metric field Hessian conversion maps flat directions to coarse size", "[metric]") {
    const Eigen::Matrix3d H = (Eigen::Vector3d{0.0, -4.0, 100.0}).asDiagonal();
    const Metric3d metric = polymesh::adapt::metric_from_hessian(H, 0.01, 0.005, 0.2, 20.0);
    const Eigen::Vector3d sizes = metric.sizes();
    CHECK(sizes.maxCoeff() <= 0.2 + 1e-14);
    CHECK(sizes.minCoeff() >= 0.005 - 1e-14);
    CHECK(sizes.maxCoeff() / sizes.minCoeff() <= 20.0 + 1e-12);
}

TEST_CASE("metric field invalid inputs throw", "[metric]") {
    CHECK_THROWS_AS(Metric3d::isotropic(0.0), std::invalid_argument);
    CHECK_THROWS_AS(Metric3d::isotropic(-0.1), std::invalid_argument);
    CHECK_THROWS_AS(Metric3d::isotropic(std::numeric_limits<double>::quiet_NaN()),
                    std::invalid_argument);
    CHECK_THROWS_AS(Metric3d::isotropic(0.1).clamped(0.01, 1.0, 0.99), std::invalid_argument);

    MetricGrid grid({0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}, {2, 2, 2});
    CHECK_THROWS_AS(polymesh::adapt::normalize_complexity(grid, 0.0, 0.01, 1.0, 10.0),
                    std::invalid_argument);
    CHECK_THROWS_AS(polymesh::adapt::normalize_complexity(grid, -1.0, 0.01, 1.0, 10.0),
                    std::invalid_argument);
}
