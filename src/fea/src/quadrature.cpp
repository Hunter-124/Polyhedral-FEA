// SPDX-License-Identifier: BSD-3-Clause
#include "fea/quadrature.hpp"

#include <array>
#include <cmath>
#include <format>
#include <span>

namespace polymesh::fea {
namespace {

struct Gauss1d {
    std::span<const double> nodes;   // on [-1, 1]
    std::span<const double> weights; // sum to 2
};

Gauss1d gauss_1d(int n) {
    // Standard Gauss-Legendre nodes/weights; symmetric pairs listed explicitly.
    static constexpr std::array<double, 1> x1{0.0};
    static constexpr std::array<double, 1> w1{2.0};
    static constexpr std::array<double, 2> x2{-0.5773502691896257, 0.5773502691896257};
    static constexpr std::array<double, 2> w2{1.0, 1.0};
    static constexpr std::array<double, 3> x3{-0.7745966692414834, 0.0, 0.7745966692414834};
    static constexpr std::array<double, 3> w3{5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
    static constexpr std::array<double, 4> x4{-0.8611363115940526, -0.3399810435848563,
                                              0.3399810435848563, 0.8611363115940526};
    static constexpr std::array<double, 4> w4{0.3478548451374538, 0.6521451548625461,
                                              0.6521451548625461, 0.3478548451374538};
    static constexpr std::array<double, 5> x5{-0.9061798459386640, -0.5384693101056831, 0.0,
                                              0.5384693101056831, 0.9061798459386640};
    static constexpr std::array<double, 5> w5{0.2369268850561891, 0.4786286704993665,
                                              0.5688888888888889, 0.4786286704993665,
                                              0.2369268850561891};
    switch (n) {
    case 1:
        return {x1, w1};
    case 2:
        return {x2, w2};
    case 3:
        return {x3, w3};
    case 4:
        return {x4, w4};
    case 5:
        return {x5, w5};
    default:
        throw FeaError(std::format("gauss_1d: unsupported point count {}", n));
    }
}

/// Tet rule via the Duffy transform: map the unit cube (u,v,w) onto the
/// reference tet by xi = u, eta = v(1-u), zeta = w(1-u)(1-v), with Jacobian
/// (1-u)^2 (1-v). A degree-d integrand on the tet becomes a polynomial of
/// per-axis degree <= d+2 on the cube, so n >= ceil((d+3)/2) Gauss points per
/// axis are exact by construction — no memorized point tables to get wrong.
std::vector<QuadraturePoint> tet_rule_duffy(int degree) {
    const int n = (degree + 3 + 1) / 2; // ceil((degree + 3) / 2)
    const auto g = gauss_1d(n);
    std::vector<QuadraturePoint> rule;
    rule.reserve(static_cast<std::size_t>(n) * static_cast<std::size_t>(n) *
                 static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double u = 0.5 * (g.nodes[static_cast<std::size_t>(i)] + 1.0);
        for (int j = 0; j < n; ++j) {
            const double v = 0.5 * (g.nodes[static_cast<std::size_t>(j)] + 1.0);
            for (int k = 0; k < n; ++k) {
                const double w = 0.5 * (g.nodes[static_cast<std::size_t>(k)] + 1.0);
                const double jac = (1.0 - u) * (1.0 - u) * (1.0 - v);
                const double weight = 0.125 * g.weights[static_cast<std::size_t>(i)] *
                                      g.weights[static_cast<std::size_t>(j)] *
                                      g.weights[static_cast<std::size_t>(k)] * jac;
                rule.push_back({{u, v * (1.0 - u), w * (1.0 - u) * (1.0 - v)}, weight});
            }
        }
    }
    return rule;
}

} // namespace

std::vector<QuadraturePoint> tet_rule(int degree) {
    if (degree <= 1) {
        // Centroid rule.
        return {{{0.25, 0.25, 0.25}, 1.0 / 6.0}};
    }
    if (degree == 2) {
        // Classic symmetric 4-point rule.
        constexpr double a = 0.5854101966249685;
        constexpr double b = 0.1381966011250105;
        constexpr double w = 1.0 / 24.0;
        return {{{a, b, b}, w}, {{b, a, b}, w}, {{b, b, a}, w}, {{b, b, b}, w}};
    }
    return tet_rule_duffy(degree);
}

std::vector<QuadraturePoint> hex_rule(int points_per_axis) {
    const auto g = gauss_1d(points_per_axis);
    std::vector<QuadraturePoint> rule;
    const auto n = static_cast<std::size_t>(points_per_axis);
    rule.reserve(n * n * n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            for (std::size_t k = 0; k < n; ++k) {
                rule.push_back({{g.nodes[i], g.nodes[j], g.nodes[k]},
                                g.weights[i] * g.weights[j] * g.weights[k]});
            }
        }
    }
    return rule;
}

std::vector<QuadraturePoint> default_rule(ElementType type) {
    switch (type) {
    case ElementType::kTet4:
        return tet_rule(1);
    case ElementType::kTet10:
        return tet_rule(2);
    case ElementType::kHex8:
        return hex_rule(2);
    case ElementType::kHex20:
        return hex_rule(3);
    }
    throw FeaError("default_rule: unknown element type");
}

} // namespace polymesh::fea
