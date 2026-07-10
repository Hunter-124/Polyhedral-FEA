// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Method of Manufactured Solutions support (BENCHMARKS.md Tier 2).
//
// A manufactured displacement field u(x) is a random-coefficient trivariate
// polynomial (seeded, so coefficients cannot be memorized or hardcoded). The
// matching body force b = -div(sigma(u)) and boundary values are derived
// *analytically* from the polynomial — no finite differencing — so observed
// convergence orders are limited only by the element formulation.

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <map>

namespace polymesh::test_support {

/// Sparse trivariate polynomial: sum of c * x^a y^b z^c terms.
class Poly3 {
  public:
    void add_term(int px, int py, int pz, double coeff);
    double eval(const Eigen::Vector3d& p) const;
    Poly3 derivative(int axis) const;
    Poly3& operator+=(const Poly3& other);
    Poly3 operator*(double s) const;

  private:
    std::map<std::array<int, 3>, double> coeffs_;
};

/// A manufactured elasticity solution: displacement, gradient, and the body
/// force -div(sigma) that makes it the exact solution.
struct ManufacturedSolution {
    fea::Material material{};
    std::array<Poly3, 3> u;
    std::array<std::array<Poly3, 3>, 3> grad; // grad[i][j] = du_i/dx_j
    std::array<Poly3, 3> body;                // N/m^3

    /// Random polynomial field of the given total degree; deterministic in
    /// `seed` (engineering rule 5). Displacement scale ~1e-3 m on the unit box.
    static ManufacturedSolution random(int degree, std::uint64_t seed,
                                       const fea::Material& material);

    Eigen::Vector3d displacement(const Eigen::Vector3d& p) const;
    /// Exact strain in Voigt order (xx, yy, zz, yz, xz, xy), engineering shear.
    Eigen::Matrix<double, 6, 1> strain(const Eigen::Vector3d& p) const;
    Eigen::Vector3d body_force(const Eigen::Vector3d& p) const;
};

/// Energy-norm error ||u_fem - u_exact||_E = sqrt(integral of
/// (eps_h - eps)^T D (eps_h - eps) dV), integrated with an elevated
/// quadrature rule.
double energy_norm_error(const fea::NodalMesh& mesh, const fea::Material& material,
                         const Eigen::VectorXd& u_fem, const ManufacturedSolution& exact);

} // namespace polymesh::test_support
