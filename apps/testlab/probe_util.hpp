// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Pure probe helpers for polymesh_testlab (face-mean displacement vs global max).
// Kept header-only so unit tests can exercise the face-mean contract without
// linking the full campaign runner.

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace polymesh::testlab {

/// Mean of |u| over unique node indices. Returns 0 if `nodes` is empty.
inline double face_mean_displacement_mag(const Eigen::VectorXd& u,
                                         const std::vector<std::uint32_t>& nodes) {
    if (nodes.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    int n = 0;
    for (const auto ni : nodes) {
        const Eigen::Index base = 3 * static_cast<Eigen::Index>(ni);
        if (base + 2 >= u.size()) {
            continue;
        }
        sum += u.segment<3>(base).norm();
        ++n;
    }
    return (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
}

/// Mean of signed displacement component `axis` (0=x,1=y,2=z) over nodes.
inline double face_mean_displacement_component(const Eigen::VectorXd& u,
                                               const std::vector<std::uint32_t>& nodes,
                                               int axis) {
    if (nodes.empty() || axis < 0 || axis > 2) {
        return 0.0;
    }
    double sum = 0.0;
    int n = 0;
    for (const auto ni : nodes) {
        const Eigen::Index dof = 3 * static_cast<Eigen::Index>(ni) + axis;
        if (dof >= u.size()) {
            continue;
        }
        sum += u[dof];
        ++n;
    }
    return (n > 0) ? (sum / static_cast<double>(n)) : 0.0;
}

/// Global max |u| over the first `n_nodes` nodes (3*n_nodes DOFs).
inline double global_max_displacement_mag(const Eigen::VectorXd& u, std::size_t n_nodes) {
    double mx = 0.0;
    const auto n = static_cast<Eigen::Index>(n_nodes);
    for (Eigen::Index i = 0; i < n; ++i) {
        const Eigen::Index base = 3 * i;
        if (base + 2 >= u.size()) {
            break;
        }
        mx = std::max(mx, u.segment<3>(base).norm());
    }
    return mx;
}

/// Dominant Cartesian axis of a traction vector (largest |component|).
inline int dominant_axis(const Eigen::Vector3d& v) {
    Eigen::Index ax = 0;
    v.cwiseAbs().maxCoeff(&ax);
    return static_cast<int>(ax);
}

} // namespace polymesh::testlab
