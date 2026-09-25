// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <Eigen/Core>

namespace polymesh::fea::detail {

/// Strain-displacement matrix B (6 x 3n) in Voigt order
/// (xx, yy, zz, yz, xz, xy) with engineering shear strains, from physical
/// shape-function gradients (n x 3).
inline Eigen::MatrixXd strain_displacement(const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx) {
    const Eigen::Index n = dndx.rows();
    Eigen::MatrixXd b = Eigen::MatrixXd::Zero(6, 3 * n);
    for (Eigen::Index a = 0; a < n; ++a) {
        const double dx = dndx(a, 0);
        const double dy = dndx(a, 1);
        const double dz = dndx(a, 2);
        b(0, 3 * a + 0) = dx;
        b(1, 3 * a + 1) = dy;
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 1) = dz; // gamma_yz = dv/dz + dw/dy
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 0) = dz; // gamma_xz = du/dz + dw/dx
        b(4, 3 * a + 2) = dx;
        b(5, 3 * a + 0) = dy; // gamma_xy = du/dy + dv/dx
        b(5, 3 * a + 1) = dx;
    }
    return b;
}

} // namespace polymesh::fea::detail
