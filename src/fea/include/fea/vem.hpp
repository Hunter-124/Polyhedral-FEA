// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Virtual Element Method for convex polyhedra (ADR-0003, ADR-0011, ADR-0017).
//
// k=1: vertex DOFs only. Consistency = constant-strain B-bar via face integrals;
//      stabilization vanishes on the 12-dimensional linear-displacement space
//      (6 rigid-body + 6 constant-strain) so the patch test is exact.
//
// k=2: serendipity-style edge midpoints after the vertices (shared with
//      p-elevate mid-edge nodes). On hex cells the formulation coincides with
//      isoparametric hex20 (MMS order 2). General polyhedra use a [P₂]³ nodal
//      projector + stabilization (ADR-0017).

#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace polymesh::fea {

/// A polyhedral cell: global node indices + oriented faces (each face is a
/// loop of *local* indices into `nodes`, outward from the cell).
/// For k=2, `nodes` = vertices first (indices used by `faces`), then one
/// mid-edge node per unique undirected edge of the face graph, in the order
/// returned by `poly_edges(faces)`.
struct PolyCell {
    std::vector<std::uint32_t> nodes;
    std::vector<std::vector<std::uint32_t>> faces; // local vertex indices
};

/// Unique undirected edges from face loops, deterministic order (first-seen
/// while walking faces in order, each face edge in loop order). Endpoints are
/// local vertex indices.
std::vector<std::array<std::uint32_t, 2>>
poly_edges(const std::vector<std::vector<std::uint32_t>>& faces);

/// Number of distinct local vertex indices referenced by faces (expects 0..nv-1).
std::size_t poly_vertex_count(const std::vector<std::vector<std::uint32_t>>& faces);

/// Infer VEM order from connectivity: 1 if nodes == vertices, 2 if
/// nodes == vertices + edges. Throws FeaError otherwise.
int vem_infer_order(std::size_t num_nodes,
                    const std::vector<std::vector<std::uint32_t>>& faces);

/// VEM stiffness, size 3n×3n, N/m. `order` is 1 or 2 (see ADR-0011 / ADR-0017).
/// Throws FeaError on non-positive volume or inconsistent connectivity.
Eigen::MatrixXd vem_poly_stiffness(const std::vector<Eigen::Vector3d>& coords,
                                   const std::vector<std::vector<std::uint32_t>>& faces,
                                   const Material& material, int order = 1);

/// Same, from mesh coordinates via PolyCell; order inferred from connectivity.
Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material);

/// Explicit order overload.
Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material, int order);

/// Convert a hex8 element to a PolyCell (6 quad faces, outward normals).
PolyCell hex8_as_poly(const NodalElement& hex);

/// Convert a hex20 (vertices + mid-edges) to a k=2 PolyCell. Mid-edge nodes are
/// reordered to match `poly_edges` of the hex faces (may differ from hex20
/// serendipity edge order).
PolyCell hex20_as_poly(const NodalElement& hex20);

/// Convert a tet4 element to a PolyCell (4 tri faces).
PolyCell tet4_as_poly(const NodalElement& tet);

/// Volume of a closed polyhedron (divergence theorem), m³. Face indices must
/// reference the vertex block of `coords` only.
double poly_volume(const std::vector<Eigen::Vector3d>& coords,
                   const std::vector<std::vector<std::uint32_t>>& faces);

/// Volume centroid of a closed polyhedron (vertex-referenced faces), m.
Eigen::Vector3d poly_centroid(const std::vector<Eigen::Vector3d>& coords,
                              const std::vector<std::vector<std::uint32_t>>& faces);

/// Consistent VEM body-load contribution for one cell (3n vector, newtons).
/// For k=1: equal share of V * b(centroid). For k=2: Πᵀ ∫ φ b dV over [P₂]³.
Eigen::VectorXd
vem_body_load(const std::vector<Eigen::Vector3d>& coords,
              const std::vector<std::vector<std::uint32_t>>& faces,
              const std::function<Eigen::Vector3d(const Eigen::Vector3d&)>& body, int order);

/// Energy-norm error squared for one VEM cell:
/// ∫ (ε(Π u_h) − ε_exact)ᵀ D (·) dV using the same projector as the stiffness.
double vem_energy_error_sq(
    const std::vector<Eigen::Vector3d>& coords,
    const std::vector<std::vector<std::uint32_t>>& faces, const Eigen::VectorXd& u_elem,
    const Material& material,
    const std::function<Eigen::Matrix<double, 6, 1>(const Eigen::Vector3d&)>& exact_strain,
    int order);

} // namespace polymesh::fea
