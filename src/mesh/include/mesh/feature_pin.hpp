// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Hard pinning of boundary nodes onto exact BRep vertices and sharp edge
// curves (ADR-0035).
//
// The snap pass (mesh/surface_project.hpp) projects a boundary node to the
// nearest point of the *surface*. On a 90° crease between a curved wall and a
// planar cap that nearest point is on one of the two faces, never on the edge
// between them, so the crease is reconstructed as a chamfer whose width is set
// by the lattice, not the CAD. Measured before this pass, icecream_cone at
// h = 8 mm: mesh feature segments sat p99 = 17.9·h from the nearest sharp BRep
// edge, and the shipped mesh carried 246 spurious crease segments.
//
// This pass fixes the two feature classes the surface projector structurally
// cannot:
//
//   1. CAD vertices — one boundary node each, moved onto the exact point
//      (`geom::project_point_on_vertex`) and frozen for every later pass.
//   2. Sharp CAD edges — the nodes nearest the curve are collected into an
//      ordered chain and pinned onto the exact curve
//      (`geom::project_point_on_edge`).
//
// Chain spacing uses the Fourier machinery from geom/signal_fft.hpp: a chain's
// pinned arclength parameters come out of the lattice unevenly (the lattice
// samples the curve where its cells happen to cross it), and uneven spacing on
// a curved crease is exactly the sawtooth the user sees. Closed chains are
// re-spaced through a periodic low-pass of the curve's coordinate signals, so
// a circular rim keeps its two true modes and loses the lattice noise; open
// chains use plain cumulative-chord arclength. Pinning itself is always the
// exact OCC projection — Fourier only chooses *where along the curve* a node
// sits, never where the curve is.
//
// Every move is validity-gated with the caller's own predicate, so this pass
// can never ship an inverted or below-floor cell.

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <vector>

namespace polymesh::mesh {

/// Everything a Cartesian fill needs to reach the exact BRep instead of the
/// tessellation: the model and its topology for pinning plus the persistent
/// per-node owner oracle used by snapping and smoothing.
///
/// A fill with a null `cad`/`topo` degrades to the tessellated path unchanged,
/// which is what OCC-disabled builds and STL inputs get.
struct BoundaryFit {
    const geom::CadModel* cad = nullptr;
    const geom::CadTopology* topo = nullptr;
    BoundaryProjectionContext* projection = nullptr;

    [[nodiscard]] bool can_pin() const {
        return cad != nullptr && topo != nullptr && !cad->empty() && !topo->empty();
    }
};

struct FeaturePinReport {
    std::size_t vertex_pinned = 0;  // nodes moved onto an exact CAD vertex
    std::size_t edge_pinned = 0;    // nodes moved onto an exact sharp edge curve
    std::size_t chains = 0;         // sharp-edge chains that received nodes
    std::size_t rejected = 0;       // pins reverted by the validity gate
    double max_edge_residual = 0.0; // metres, pinned node → its edge curve
    /// Worst boundary node against the whole BRep after pinning, and the owner
    /// class the projection oracle assigned it. A large residual on a
    /// kCadFace-owned node means the snap could not reach the face; on a
    /// kCadVertex/kCadEdge node it means a wrong owner was latched.
    double worst_node_distance = 0.0;
    std::uint32_t worst_node = 0;
    BoundarySupportKind worst_node_owner = BoundarySupportKind::kUnknown;
};

/// Pin boundary nodes onto exact CAD vertices and sharp edge curves.
///
/// @param nodes         Mesh node array, mutated in place.
/// @param boundary_nodes Candidate node ids (free-surface nodes).
/// @param h             Characteristic size; capture radii scale with it.
/// @param node_offends  True when any cell incident to the node is invalid.
///                      A pin that offends is retreated along its own segment
///                      (0.75/0.5/0.25) and abandoned if no fraction is legal.
/// @param provenance    Optional per-node owner slots, grown as needed, so the
///                      later smoothing/wall passes see the pins as owned.
FeaturePinReport pin_feature_nodes(const geom::CadModel& cad, const geom::CadTopology& topo,
                                   std::vector<Eigen::Vector3d>& nodes,
                                   const std::vector<std::uint32_t>& boundary_nodes, double h,
                                   const NodeOffendsFn& node_offends,
                                   std::vector<BoundarySupport>* provenance = nullptr);

} // namespace polymesh::mesh
