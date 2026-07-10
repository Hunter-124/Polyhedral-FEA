// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Linear elastostatics solver.
//
// One Element interface covers the whole zoo (ADR-0003): standard
// isoparametric FEM for tets/hexes/prisms/pyramids at order p = 1..4, and
// VEM for general polyhedra. Assembly and solve layers are formulation-blind
// and backend-blind (CPU / CUDA, ADR-0008).
//
// All arithmetic is double. Units are SI: metres, pascals, newtons.

#include <Eigen/Core>

namespace polymesh::fea {

/// Isotropic linear-elastic material.
struct Material {
    /// Young's modulus, Pa.
    double youngs_modulus;
    /// Poisson's ratio, dimensionless, in (-1, 0.5).
    double poissons_ratio;

    /// Lamé's first parameter λ, Pa.
    double lambda() const {
        return youngs_modulus * poissons_ratio /
               ((1.0 + poissons_ratio) * (1.0 - 2.0 * poissons_ratio));
    }

    /// Shear modulus μ (Lamé's second parameter), Pa.
    double mu() const { return youngs_modulus / (2.0 * (1.0 + poissons_ratio)); }

    /// 6×6 constitutive matrix D in Voigt notation
    /// (order: xx, yy, zz, yz, xz, xy; engineering shear strains), Pa.
    Eigen::Matrix<double, 6, 6> d_matrix() const;
};

} // namespace polymesh::fea
