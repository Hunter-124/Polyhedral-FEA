// SPDX-License-Identifier: BSD-3-Clause
#include "fea/quadrature.hpp"

#include "fea/shape.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Eigen/Dense>

#include <array>
#include <cmath>
#include <vector>

using namespace polymesh::fea;

namespace {

double integrate(const std::vector<QuadraturePoint>& rule, int a, int b, int c) {
    double sum = 0.0;
    for (const auto& qp : rule) {
        sum +=
            qp.weight * std::pow(qp.xi[0], a) * std::pow(qp.xi[1], b) * std::pow(qp.xi[2], c);
    }
    return sum;
}

/// Exact integral of xi^a eta^b zeta^c over the reference tet: a!b!c!/(a+b+c+3)!.
double tet_monomial_exact(int a, int b, int c) {
    const auto factorial = [](int n) {
        double f = 1.0;
        for (int i = 2; i <= n; ++i) {
            f *= i;
        }
        return f;
    };
    return factorial(a) * factorial(b) * factorial(c) / factorial(a + b + c + 3);
}

/// Exact integral of x^a y^b z^c over [-1,1]^3.
double hex_monomial_exact(int a, int b, int c) {
    const auto axis = [](int n) { return n % 2 == 1 ? 0.0 : 2.0 / (n + 1); };
    return axis(a) * axis(b) * axis(c);
}

} // namespace

TEST_CASE("tet rules integrate monomials exactly up to their degree") {
    const int degree = GENERATE(1, 2, 3, 4, 5);
    const auto rule = tet_rule(degree);
    for (int a = 0; a <= degree; ++a) {
        for (int b = 0; a + b <= degree; ++b) {
            for (int c = 0; a + b + c <= degree; ++c) {
                const double exact = tet_monomial_exact(a, b, c);
                INFO("degree " << degree << " monomial " << a << " " << b << " " << c);
                CHECK(std::abs(integrate(rule, a, b, c) - exact) < 1e-14);
            }
        }
    }
}

TEST_CASE("hex rules integrate per-axis monomials exactly up to 2n-1") {
    const int n = GENERATE(1, 2, 3, 4, 5, 6);
    const auto rule = hex_rule(n);
    const int degree = 2 * n - 1;
    for (int a = 0; a <= degree; ++a) {
        for (int b = 0; b <= degree; ++b) {
            for (int c = 0; c <= degree; ++c) {
                const double exact = hex_monomial_exact(a, b, c);
                INFO("n " << n << " monomial " << a << " " << b << " " << c);
                CHECK(std::abs(integrate(rule, a, b, c) - exact) < 1e-13);
            }
        }
    }
}

TEST_CASE("default rules exist for every element type") {
    for (const auto type :
         {ElementType::kTet4, ElementType::kTet10, ElementType::kHex8, ElementType::kHex20}) {
        CHECK(!default_rule(type).empty());
    }
}

namespace {

/// Volume the solver actually sees: sum over the element's own default rule of
/// w·|det J| with J built from the element's own shape derivatives. This is the
/// only quantity that couples `default_rule` to `eval_shape`; a rule written for
/// a different parametric domain than its shape functions fails here and
/// nowhere else.
double isoparametric_volume(ElementType type,
                            const std::vector<Eigen::Vector3d>& nodes) {
    Eigen::MatrixXd x(static_cast<Eigen::Index>(nodes.size()), 3);
    for (std::size_t a = 0; a < nodes.size(); ++a) {
        x.row(static_cast<Eigen::Index>(a)) = nodes[a].transpose();
    }
    double total = 0.0;
    for (const auto& qp : default_rule(type)) {
        const Eigen::Matrix3d jac = eval_shape(type, qp.xi).dn.transpose() * x;
        total += qp.weight * std::abs(jac.determinant());
    }
    return total;
}

std::vector<Eigen::Vector3d> affine_image(const Eigen::Matrix3d& a,
                                          const Eigen::Vector3d& b,
                                          const std::vector<Eigen::Vector3d>& reference) {
    std::vector<Eigen::Vector3d> out;
    out.reserve(reference.size());
    for (const auto& p : reference) {
        out.push_back(a * p + b);
    }
    return out;
}

} // namespace

TEST_CASE("the pyramid rule measures the volume its shape functions describe") {
    // Six pyramids fanned from the centre of the unit cube tile it exactly. This
    // is the hybrid mesher's transition fan; a domain-mismatched rule reported
    // 0.6 m³ for a 1 m³ box and the fill guard called it a 40% hole.
    const Eigen::Vector3d apex{0.5, 0.5, 0.5};
    const std::array<std::array<Eigen::Vector3d, 4>, 6> faces{{
        {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}},
        {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}},
        {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}},
        {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}},
        {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}},
        {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}},
    }};
    double fan = 0.0;
    for (const auto& face : faces) {
        const std::vector<Eigen::Vector3d> nodes{face[0], face[1], face[2], face[3], apex};
        // Each pyramid is base area 1 × height 1/2 ÷ 3.
        CHECK(std::abs(isoparametric_volume(ElementType::kPyramid5, nodes) - 1.0 / 6.0) < 1e-14);
        fan += isoparametric_volume(ElementType::kPyramid5, nodes);
    }
    CHECK(std::abs(fan - 1.0) < 1e-14);
}

TEST_CASE("pyramid and prism rules are exact on affine images of their reference cells") {
    // A skewed, rotated, scaled cell: exact volume is |det A| × reference volume,
    // so this catches a wrong domain, a wrong weight sum and a wrong Jacobian
    // convention at once.
    Eigen::Matrix3d a;
    a << 1.3, 0.4, -0.2, 0.0, 0.9, 0.35, 0.25, -0.15, 1.1;
    const Eigen::Vector3d b{-0.7, 2.1, 0.3};
    const double scale = std::abs(a.determinant());

    const std::vector<Eigen::Vector3d> pyramid_ref{
        {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {0, 0, 1}};
    // Base 2×2 at zeta=-1, apex at zeta=+1: (1/3)·4·2 = 8/3.
    CHECK(std::abs(isoparametric_volume(ElementType::kPyramid5,
                                        affine_image(a, b, pyramid_ref)) -
                   scale * 8.0 / 3.0) < 1e-13);

    const std::vector<Eigen::Vector3d> prism_ref{{0, 0, -1}, {1, 0, -1}, {0, 1, -1},
                                                 {0, 0, 1},  {1, 0, 1},  {0, 1, 1}};
    // Unit triangle (area 1/2) extruded over zeta ∈ [-1,1]: 1.
    CHECK(std::abs(isoparametric_volume(ElementType::kPrism6, affine_image(a, b, prism_ref)) -
                   scale) < 1e-13);
}

TEST_CASE("every quadrature rule's weights sum to its reference cell volume") {
    const auto weight_sum = [](ElementType type) {
        double sum = 0.0;
        for (const auto& qp : default_rule(type)) {
            sum += qp.weight;
        }
        return sum;
    };
    CHECK(std::abs(weight_sum(ElementType::kTet4) - 1.0 / 6.0) < 1e-14);
    CHECK(std::abs(weight_sum(ElementType::kTet10) - 1.0 / 6.0) < 1e-14);
    CHECK(std::abs(weight_sum(ElementType::kHex8) - 8.0) < 1e-13);
    CHECK(std::abs(weight_sum(ElementType::kHex20) - 8.0) < 1e-13);
    CHECK(std::abs(weight_sum(ElementType::kPrism6) - 1.0) < 1e-14);
    // Collapsed-brick domain: the cube, not the reference pyramid's 8/3.
    CHECK(std::abs(weight_sum(ElementType::kPyramid5) - 8.0) < 1e-13);
}
