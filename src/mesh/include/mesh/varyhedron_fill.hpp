// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Varyhedron packing fill (ADR-0021) — v1 packing-seed engine (V6c):
// CAD edge protecting balls (denser on short features) + interior bubble /
// packing seeds that repel from edge seeds and each other, then multi-level
// graded tet scaffold + soft boundary edge-profile snap. Dual-of-tet poly
// clustering is deferred to V11; export remains a tet scaffold for FE today.

#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <optional>
#include <span>

namespace polymesh::mesh {

struct VaryhedronFillOutput {
    TetFillOutput mesh;
    double h_coarse = 0.0;
    double h_fine = 0.0;
    std::size_t n_tets = 0;
    /// CAD edge protecting-ball samples used as fixed packing anchors.
    std::size_t n_edge_seeds = 0;
    /// Interior packing seeds (volume bubbles) after edge-protect repulsion.
    std::size_t n_volume_seeds = 0;
    /// Bubble-relax iterations applied to volume seeds.
    int n_pack_relax_iters = 0;
    /// Packed-ball volume proxy / bbox volume (clamped [0,1]); diagnostic only.
    double pack_fill_frac = 0.0;
    /// Max Hausdorff residual of free-boundary mesh edges vs CAD edge samples
    /// (metres). 0 if no topology provided.
    double edge_profile_hausdorff_max = 0.0;
    double edge_profile_rel = 0.0; // max / characteristic CAD edge length
};

/// Pack volume mesh with varyhedron v1 packing-seed algorithm.
/// When `topo` is non-null, CAD edge samples become protecting seeds, interior
/// volume seeds are packed away from them, and free nodes near edges are
/// attracted to the CAD edge profile after the tet scaffold is built.
VaryhedronFillOutput varyhedron_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double curvature_turn_deg = 15.0, const geom::CadTopology* topo = nullptr);

} // namespace polymesh::mesh
