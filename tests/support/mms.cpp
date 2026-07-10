// SPDX-License-Identifier: AGPL-3.0-or-later
#include "support/mms.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"

// Eigen/Dense (not just Core) is REQUIRED in any TU calling .inverse():
// without the LU module, Inverse<> falls back to a generic assignment that
// recurses infinitely at runtime, and the linker's COMDAT folding then
// poisons every other TU with that instantiation.
#include <Eigen/Dense>

#include <cmath>

namespace polymesh::test_support {

using fea::ElementType;

void Poly3::add_term(int px, int py, int pz, double coeff) {
    if (coeff != 0.0) {
        coeffs_[{px, py, pz}] += coeff;
    }
}

double Poly3::eval(const Eigen::Vector3d& p) const {
    double sum = 0.0;
    for (const auto& [powers, c] : coeffs_) {
        sum += c * std::pow(p[0], powers[0]) * std::pow(p[1], powers[1]) *
               std::pow(p[2], powers[2]);
    }
    return sum;
}

Poly3 Poly3::derivative(int axis) const {
    Poly3 d;
    for (const auto& [powers, c] : coeffs_) {
        if (powers[static_cast<std::size_t>(axis)] > 0) {
            auto p = powers;
            const int n = p[static_cast<std::size_t>(axis)]--;
            d.add_term(p[0], p[1], p[2], c * n);
        }
    }
    return d;
}

Poly3& Poly3::operator+=(const Poly3& other) {
    for (const auto& [powers, c] : other.coeffs_) {
        add_term(powers[0], powers[1], powers[2], c);
    }
    return *this;
}

Poly3 Poly3::operator*(double s) const {
    Poly3 r;
    for (const auto& [powers, c] : coeffs_) {
        r.add_term(powers[0], powers[1], powers[2], c * s);
    }
    return r;
}

ManufacturedSolution ManufacturedSolution::random(int degree, std::uint64_t seed,
                                                  const fea::Material& material) {
    std::uint64_t state = seed;
    const auto next = [&] {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const auto bits = state * 0x2545F4914F6CDD1Dull;
        return 2.0 * (static_cast<double>(bits >> 11) * 0x1.0p-53) - 1.0;
    };

    ManufacturedSolution mms;
    mms.material = material;
    for (auto& component : mms.u) {
        for (int a = 0; a <= degree; ++a) {
            for (int b = 0; a + b <= degree; ++b) {
                for (int c = 0; a + b + c <= degree; ++c) {
                    component.add_term(a, b, c, 1e-3 * next());
                }
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            mms.grad[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                mms.u[static_cast<std::size_t>(i)].derivative(j);
        }
    }
    // b = -div(sigma) = -(lambda + mu) grad(div u) - mu laplacian(u).
    const double l = material.lambda();
    const double m = material.mu();
    Poly3 div_u;
    for (int k = 0; k < 3; ++k) {
        div_u += mms.grad[static_cast<std::size_t>(k)][static_cast<std::size_t>(k)];
    }
    for (int i = 0; i < 3; ++i) {
        Poly3 laplacian;
        for (int j = 0; j < 3; ++j) {
            laplacian +=
                mms.grad[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)].derivative(
                    j);
        }
        Poly3 b = div_u.derivative(i) * (-(l + m));
        b += laplacian * (-m);
        mms.body[static_cast<std::size_t>(i)] = b;
    }
    return mms;
}

Eigen::Vector3d ManufacturedSolution::displacement(const Eigen::Vector3d& p) const {
    return {u[0].eval(p), u[1].eval(p), u[2].eval(p)};
}

Eigen::Matrix<double, 6, 1> ManufacturedSolution::strain(const Eigen::Vector3d& p) const {
    Eigen::Matrix3d g;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            g(i, j) = grad[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)].eval(p);
        }
    }
    Eigen::Matrix<double, 6, 1> eps;
    eps << g(0, 0), g(1, 1), g(2, 2), g(1, 2) + g(2, 1), g(0, 2) + g(2, 0), g(0, 1) + g(1, 0);
    return eps;
}

Eigen::Vector3d ManufacturedSolution::body_force(const Eigen::Vector3d& p) const {
    return {body[0].eval(p), body[1].eval(p), body[2].eval(p)};
}

double energy_norm_error(const fea::NodalMesh& mesh, const fea::Material& material,
                         const Eigen::VectorXd& u_fem, const ManufacturedSolution& exact) {
    const auto d = material.d_matrix();
    double error_sq = 0.0;
    for (const auto& element : mesh.elements) {
        const bool is_tet =
            element.type == ElementType::kTet4 || element.type == ElementType::kTet10;
        const auto rule = is_tet ? fea::tet_rule(4) : fea::hex_rule(4);

        Eigen::Matrix<double, Eigen::Dynamic, 3> x(element.nodes.size(), 3);
        Eigen::VectorXd u_e(3 * element.nodes.size());
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[element.nodes[a]].transpose();
            u_e.segment<3>(3 * static_cast<Eigen::Index>(a)) =
                u_fem.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[a]));
        }

        for (const auto& qp : rule) {
            const auto shape = fea::eval_shape(element.type, qp.xi);
            const Eigen::Matrix3d jac = shape.dn.transpose() * x;
            const double det = jac.determinant();
            // Materialized inverse — see the matching note in fea assembly.cpp.
            const Eigen::Matrix3d jac_inv = jac.inverse();
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx =
                shape.dn * jac_inv.transpose();

            Eigen::Matrix<double, 6, 1> eps_h = Eigen::Matrix<double, 6, 1>::Zero();
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                const auto ai = static_cast<Eigen::Index>(a);
                const Eigen::Vector3d ua = u_e.segment<3>(3 * ai);
                eps_h[0] += dndx(ai, 0) * ua[0];
                eps_h[1] += dndx(ai, 1) * ua[1];
                eps_h[2] += dndx(ai, 2) * ua[2];
                eps_h[3] += dndx(ai, 2) * ua[1] + dndx(ai, 1) * ua[2];
                eps_h[4] += dndx(ai, 2) * ua[0] + dndx(ai, 0) * ua[2];
                eps_h[5] += dndx(ai, 1) * ua[0] + dndx(ai, 0) * ua[1];
            }
            const Eigen::Vector3d point = x.transpose() * shape.n;
            const Eigen::Matrix<double, 6, 1> diff = eps_h - exact.strain(point);
            error_sq += diff.dot(d * diff) * det * qp.weight;
        }
    }
    return std::sqrt(error_sq);
}

} // namespace polymesh::test_support
