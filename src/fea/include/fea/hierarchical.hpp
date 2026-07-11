// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Hierarchical (modal) shape functions for arbitrary polynomial order p,
// per ADR-0019. Unlike the nodal zoo in shape.hpp, DOFs here are attached to
// mesh *entities* — vertices, edges, faces, cell interior — using a
// Szabo-Babuska integrated-Legendre (Lobatto) basis. The order-p space nests
// inside order-(p+1): raising p only *adds* modes, never moves existing ones.
// That nesting is what makes mixed-order meshes conforming by the minimum
// rule (a shared entity carries order = min over its adjacent elements) with
// no constraint equations.
//
// This module provides the reference-element basis + single-element
// stiffness. Global per-entity DOF numbering and the orientation-sign
// bookkeeping that assembles a conforming mixed-order system live in the
// assembler (ADR-0019 lane B, node `fe-vem-assembly`); the HpMode descriptors
// returned here carry the entity/order/orientation data that assembler needs.
//
// Geometry is subparametric: the isoparametric map uses the p=1 vertex
// functions (trilinear hex / linear tet), which is exact for the straight-
// edged elements the Cartesian meshers produce. Curved-boundary blending is
// deferred with the CAD-fitted boundary (ADR-0015).
//
// Support in this landing:
//   - Hex: full tensor-product hierarchical basis, order 1..4 (the cap is the
//     available Gauss rule, hex_rule n<=5 => exact to degree 9 >= 2p).
//   - Tet: vertex + quadratic edge bubbles, order 1..2 (complete quadratic
//     space; the k>=3 edge/face/interior kernels are the next increment).

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <vector>

namespace polymesh::fea {

/// 1-D integrated-Legendre (Lobatto) basis on the reference interval [-1, 1].
/// Mode 0 = (1-xi)/2, mode 1 = (1+xi)/2 (the two vertex functions); mode
/// k >= 2 = the order-k bubble phi_k, which vanishes at both endpoints. The
/// bubble reverses as phi_k(-xi) = (-1)^k phi_k(xi) — the sign the assembler
/// applies when an element traverses a shared edge against its global
/// orientation.
namespace lobatto {
/// phi value of the given mode at xi in [-1, 1].
double value(int mode, double xi);
/// d/dxi of the given mode at xi.
double deriv(int mode, double xi);
} // namespace lobatto

/// Which mesh entity a hierarchical mode belongs to, and its role in the
/// minimum rule.
struct HpMode {
    enum class Entity : std::uint8_t { kVertex, kEdge, kFace, kInterior };
    Entity entity = Entity::kVertex;
    /// Index of the entity within the element: vertex 0..n_v-1, edge in the
    /// canonical edge list (matching nodal_mesh.hpp tet10/hex20 ordering),
    /// face in the canonical face list, interior always 0.
    std::uint8_t entity_index = 0;
    /// Polynomial order of this mode (1 for vertex modes; >=2 for the rest).
    /// The highest order among an entity's modes is that entity's order.
    std::uint8_t order = 1;
    /// True if this mode flips sign when the owning edge is traversed against
    /// global orientation (odd-order edge bubbles). Faces >= order 3 need the
    /// richer transform the assembler applies; flagged false here.
    bool edge_odd = false;
};

/// Reference-space values and gradients of every hierarchical mode of an
/// element, in the canonical order returned by `hp_modes`.
struct HpShape {
    /// Mode values, one per mode.
    Eigen::VectorXd n;
    /// d(mode)/d(xi, eta, zeta), row i = gradient of mode i.
    Eigen::Matrix<double, Eigen::Dynamic, 3> dn;
};

/// Canonical mode list for `type` at uniform order `order`. Order is:
/// all vertex modes, then edge modes (edge-major, ascending order within an
/// edge), then face modes, then interior modes — so the first n_vertices
/// entries are exactly the p=1 nodal functions. Throws FeaError for
/// unsupported (type, order) combinations.
std::vector<HpMode> hp_modes(ElementType type, std::uint8_t order);

/// Total mode count for `type` at uniform `order` (== hp_modes(...).size()).
std::size_t hp_num_modes(ElementType type, std::uint8_t order);

/// Evaluate all hierarchical modes at reference point `xi`, aligned with
/// `hp_modes(type, order)`.
HpShape hp_eval(ElementType type, std::uint8_t order, const Eigen::Vector3d& xi);

/// Single-element hierarchical stiffness, size 3*hp_num_modes square, in N/m.
/// `vertex_coords` are the element's straight-sided vertex positions in the
/// canonical nodal order (4 rows tet, 8 rows hex). DOF layout matches the
/// nodal convention: mode i owns local DOFs (3i, 3i+1, 3i+2). Throws FeaError
/// on non-positive geometry Jacobian.
Eigen::MatrixXd hp_element_stiffness(const Eigen::Matrix<double, Eigen::Dynamic, 3>& vertex_coords,
                                     ElementType type, std::uint8_t order,
                                     const Material& material);

} // namespace polymesh::fea
