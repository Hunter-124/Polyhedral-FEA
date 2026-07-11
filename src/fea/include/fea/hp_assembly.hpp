// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Conforming assembly of the hierarchical (modal) basis (ADR-0019) with
// per-entity DOF numbering and the minimum rule for mixed order. Each mesh
// entity — vertex, edge, face, cell interior — owns a block of global DOFs;
// a shared entity carries order = min over its adjacent elements, and any
// element mode above that order is suppressed. Because entity DOFs are shared
// by global id, neighbouring elements of different order stay conforming with
// no constraint equations.
//
// Scope: uniform or mixed order p in {1, 2} on tet and hex. At p <= 2 every
// orientation sign is +1 (the edge bubble phi_2 is even and the single hex
// face mode is symmetric), so no orientation bookkeeping is needed yet; the
// p >= 3 sign/transform machinery is the next increment (ADR-0019 lane B).
//
// This is the FE half of the eventual mixed FE+VEM system (node
// `fe-vem-assembly`); VEM polyhedra scatter into the same global matrix.

#include "fea/assembly.hpp" // BodyForce
#include "fea/hierarchical.hpp"
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>
#include <Eigen/SparseCore>

#include <cstdint>
#include <map>
#include <vector>

namespace polymesh::fea {

/// One hierarchical element: its straight-sided vertices (global node ids, in
/// the canonical nodal order for its type) and its polynomial order.
struct HpElementDef {
    ElementType type = ElementType::kHex8;
    std::vector<std::uint32_t> vertices;
    std::uint8_t order = 1;
};

/// A hierarchical FE model: vertex coordinates + hierarchical elements.
struct HpModel {
    std::vector<Eigen::Vector3d> nodes;
    std::vector<HpElementDef> elements;
};

/// Assembled hierarchical system plus the maps needed to apply boundary
/// conditions, add loads, and evaluate the field.
struct HpSystem {
    Eigen::SparseMatrix<double> k;
    /// Total scalar DOFs = 3 * n_modes.
    Eigen::Index ndof = 0;
    /// Number of global vector modes (each owns 3 DOFs at 3*mode + axis).
    Eigen::Index n_modes = 0;
    /// Per element, per local hierarchical mode (in hp_modes order): the global
    /// mode index, or -1 when the minimum rule suppresses it.
    std::vector<std::vector<Eigen::Index>> local_to_global;
    /// Per global mode, the node ids that define its owning entity (1 vertex,
    /// 2 edge, 4 hex face, 8 interior). Lets callers classify boundary modes.
    std::vector<std::vector<std::uint32_t>> mode_nodes;
};

/// Build the global stiffness and DOF maps for `model`.
HpSystem assemble_hp(const HpModel& model, const Material& material);

/// Consistent hierarchical load vector f = integral of N^T b dV (size ndof).
Eigen::VectorXd assemble_hp_body_load(const HpModel& model, const HpSystem& system,
                                      const BodyForce& body_force);

/// Solve K u = loads with prescribed DOF values (global DOF index -> metres),
/// by static partitioning. Returns the full displacement vector (size ndof).
Eigen::VectorXd solve_hp(const HpSystem& system, const Eigen::VectorXd& loads,
                         const std::map<Eigen::Index, double>& fixed);

/// Exact strain at a physical point, Voigt order (xx, yy, zz, yz, xz, xy) with
/// engineering shear strains — the convention of the B matrix and D matrix.
using StrainField = std::function<Eigen::Matrix<double, 6, 1>(const Eigen::Vector3d&)>;

/// Energy-norm error ||u_h - u_exact||_E = sqrt( integral of
/// (eps_h - eps_exact)^T D (eps_h - eps_exact) dV ), the natural norm for
/// convergence-rate checks.
double hp_energy_error(const HpModel& model, const HpSystem& system, const Eigen::VectorXd& u,
                       const StrainField& exact_strain, const Material& material);

} // namespace polymesh::fea
