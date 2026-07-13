// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Stress recovery from a displacement solution.
//
// v0 recovery: element stress evaluated at each node's reference position,
// averaged over all elements sharing the node. Zienkiewicz-Zhu
// superconvergent patch recovery replaces this as the error-estimation
// workhorse in Phase P5 (adapt module); this simple average is fine for
// visualization and peak-stress benchmarks.

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <vector>

namespace polymesh::fea {

/// Stress in Voigt order (xx, yy, zz, yz, xz, xy), Pa.
using Stress = Eigen::Matrix<double, 6, 1>;

/// Nodal-averaged stress for every node, Pa.
/// Note: nodal extrapolation amplifies sliver/inverse-Jacobian spikes — do **not**
/// use raw nodal max as a campaign score (ADR-0023 / measure-first). Prefer
/// `recover_element_centroid_stress` for scoring.
std::vector<Stress> recover_nodal_stress(const NodalMesh& mesh, const Material& material,
                                         const Eigen::VectorXd& u);

/// Per-element stress at the reference centroid (interior / Gauss-like sample).
/// Skips kPolyVem (no FE shape path). `quality` is tet volume/edge³ aspect in
/// (0,1] when the element is tet4; 1.0 for other supported types; 0 if skipped.
struct ElementCentroidStress {
    Stress stress = Stress::Zero();
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    double volume = 0.0;
    double quality = 0.0;
    std::uint32_t element_index = 0;
};

std::vector<ElementCentroidStress>
recover_element_centroid_stress(const NodalMesh& mesh, const Material& material,
                                const Eigen::VectorXd& u);

/// Von Mises equivalent stress, Pa.
double von_mises(const Stress& s);

} // namespace polymesh::fea
