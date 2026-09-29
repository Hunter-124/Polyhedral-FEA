// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to the stress-recovery TUs (stress.cpp, zz.cpp).

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <cstddef>

namespace polymesh::fea::detail {

/// Voigt strain (xx, yy, zz, yz, xz, xy; engineering shear) of an isoparametric
/// `element` from its physical shape gradients `dndx` (row b = dN_b/dx) and the
/// global displacement vector `u` (3 DOFs per mesh node).
inline Eigen::Matrix<double, 6, 1>
strain_from_gradients(const NodalElement& element,
                      const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx,
                      const Eigen::VectorXd& u) {
    Eigen::Matrix<double, 6, 1> eps = Eigen::Matrix<double, 6, 1>::Zero();
    for (std::size_t b = 0; b < element.nodes.size(); ++b) {
        const auto bi = static_cast<Eigen::Index>(b);
        const Eigen::Vector3d ub =
            u.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[b]));
        eps[0] += dndx(bi, 0) * ub[0];
        eps[1] += dndx(bi, 1) * ub[1];
        eps[2] += dndx(bi, 2) * ub[2];
        eps[3] += dndx(bi, 2) * ub[1] + dndx(bi, 1) * ub[2];
        eps[4] += dndx(bi, 2) * ub[0] + dndx(bi, 0) * ub[2];
        eps[5] += dndx(bi, 1) * ub[0] + dndx(bi, 0) * ub[1];
    }
    return eps;
}

} // namespace polymesh::fea::detail
