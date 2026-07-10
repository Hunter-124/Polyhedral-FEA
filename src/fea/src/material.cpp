// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/material.hpp"

namespace polymesh::fea {

Eigen::Matrix<double, 6, 6> Material::d_matrix() const {
    const double l = lambda();
    const double m = mu();
    Eigen::Matrix<double, 6, 6> d = Eigen::Matrix<double, 6, 6>::Zero();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            d(i, j) = l;
        }
        d(i, i) = l + 2.0 * m;
        d(i + 3, i + 3) = m;
    }
    return d;
}

} // namespace polymesh::fea
