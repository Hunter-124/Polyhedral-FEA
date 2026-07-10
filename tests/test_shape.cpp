// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace polymesh::fea;

namespace {
const std::vector<ElementType> kAllTypes{ElementType::kTet4, ElementType::kTet10,
                                         ElementType::kHex8, ElementType::kHex20};

/// Sample points strictly inside the reference element.
std::vector<Eigen::Vector3d> interior_points(ElementType type) {
    if (type == ElementType::kTet4 || type == ElementType::kTet10) {
        return {{0.25, 0.25, 0.25}, {0.1, 0.2, 0.3}, {0.6, 0.1, 0.05}};
    }
    return {{0.0, 0.0, 0.0}, {0.3, -0.7, 0.2}, {-0.9, 0.4, 0.8}};
}
} // namespace

TEST_CASE("partition of unity and zero derivative sum") {
    for (const auto type : kAllTypes) {
        for (const auto& xi : interior_points(type)) {
            const auto s = eval_shape(type, xi);
            INFO("type " << static_cast<int>(type) << " xi " << xi.transpose());
            CHECK(std::abs(s.n.sum() - 1.0) < 1e-14);
            CHECK(s.dn.colwise().sum().norm() < 1e-13);
        }
    }
}

TEST_CASE("nodal delta property") {
    for (const auto type : kAllTypes) {
        const auto ref = reference_nodes(type);
        REQUIRE(ref.size() == static_cast<std::size_t>(element_num_nodes(type)));
        for (std::size_t j = 0; j < ref.size(); ++j) {
            const auto s = eval_shape(type, ref[j]);
            for (Eigen::Index i = 0; i < s.n.size(); ++i) {
                const double expected = static_cast<std::size_t>(i) == j ? 1.0 : 0.0;
                INFO("type " << static_cast<int>(type) << " N_" << i << " at node " << j);
                CHECK(std::abs(s.n[i] - expected) < 1e-14);
            }
        }
    }
}

TEST_CASE("shape functions reproduce linear fields exactly") {
    // sum_i N_i(xi) * f(x_i) == f(x(xi)) for linear f on the reference element.
    for (const auto type : kAllTypes) {
        const auto ref = reference_nodes(type);
        const auto f = [](const Eigen::Vector3d& p) {
            return 1.5 + 2.0 * p[0] - 0.7 * p[1] + 0.3 * p[2];
        };
        for (const auto& xi : interior_points(type)) {
            const auto s = eval_shape(type, xi);
            double interpolated = 0.0;
            Eigen::Vector3d mapped = Eigen::Vector3d::Zero();
            for (std::size_t i = 0; i < ref.size(); ++i) {
                interpolated += s.n[static_cast<Eigen::Index>(i)] * f(ref[i]);
                mapped += s.n[static_cast<Eigen::Index>(i)] * ref[i];
            }
            INFO("type " << static_cast<int>(type));
            CHECK(std::abs(interpolated - f(mapped)) < 1e-13);
        }
    }
}
