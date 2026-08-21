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

#include <cstddef>
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
/// kPolyVem: constant-strain LSQ fit on nodal u (VEM k=1 projector proxy) so
/// face-mean SCF is measurable for M5 gate. `quality` is the *measured* cell
/// shape quality from `fea::cell_quality` (see cell_quality.hpp) for **every**
/// element type in [0,1] — 1 = regular cell, → 0 = sliver, and exactly 0 when
/// the cell could not be measured, so a quality floor never trusts an
/// unmeasured cell.
struct ElementCentroidStress {
    Stress stress = Stress::Zero();
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    double volume = 0.0;
    double quality = 0.0;
    std::uint32_t element_index = 0;
};

std::vector<ElementCentroidStress> recover_element_centroid_stress(const NodalMesh& mesh,
                                                                   const Material& material,
                                                                   const Eigen::VectorXd& u);

/// Von Mises equivalent stress, Pa.
double von_mises(const Stress& s);

/// Magnitude of the spatial gradient of a per-node scalar field, per node, in
/// [field units] per metre (node coordinates are metres — see nodal_mesh.hpp).
///
/// Definition. For node i with position xᵢ and value sᵢ, the patch is every
/// node j ≠ i that shares at least one element with i, counted once however
/// many elements it shares. With dⱼ = xⱼ - xᵢ the gradient g is the unweighted
/// linear least-squares fit of s(x) ≈ sᵢ + g·(x - xᵢ) over that patch, i.e. the
/// solution of the 3×3 normal equations (Σ dⱼ dⱼᵀ) g = Σ dⱼ (sⱼ - sᵢ). The
/// returned entry is |g|.
///
/// The fit reproduces a field that is linear in x exactly (this is the property
/// `tests/test_stress_gradient.cpp` pins), and a constant field gives exactly
/// 0.0. On a curved field it is a *recovery*, not a derivative, and it is
/// first-order accurate: a patch of diameter h cannot see the curvature it is
/// averaging over. Measured on exp(x)·sin(3y)·(1+z²) over a unit-cube hex
/// lattice, max interior error, h = 1/8 → 1/64: on the symmetric lattice the
/// odd patch moments cancel and it converges at rate 1.80 / 1.90 / 1.95, but
/// perturb the interior nodes by h/4 and the rate drops to 0.82 / 0.69 / 0.78.
/// O(h) is the rate to budget for on any real mesh.
///
/// A 0.0 entry means one of two things, and the two are distinguishable only by
/// `n_unresolved`: either the field really is flat there, or the node's normal
/// matrix is rank deficient (fewer than 3 patch nodes, or patch nodes collinear
/// / coplanar) so no gradient exists to report. Rank deficiency yields exactly
/// 0.0 — never an extrapolated slope out of a near-null direction — and
/// increments `*n_unresolved` when that pointer is non-null. `*n_unresolved` is
/// reset to 0 on entry, so it counts this call only.
///
/// Returns an empty vector when `nodal.size() != mesh.nodes.size()`. A
/// mismatched field is a caller bug; padding it would put a number on screen
/// that was never computed from the solution.
std::vector<double> nodal_scalar_gradient_magnitude(const NodalMesh& mesh,
                                                    const std::vector<double>& nodal,
                                                    std::size_t* n_unresolved = nullptr);

} // namespace polymesh::fea
