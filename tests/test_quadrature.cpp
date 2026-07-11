// SPDX-License-Identifier: BSD-3-Clause
#include "fea/quadrature.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

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
