// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Varyhedron packing fill (ADR-0021 / ADR-0023) — packing-seed engine:
// protecting balls on **sharp** CAD edges only (seams/smooth skipped) +
// interior bubble seeds + graded tet scaffold + soft snap to sharp features.
// Poly export target: constrained restricted CVT / clipped Voronoi (M5 path);
// dual-of-tet deferred. Export remains a tet scaffold for FE today.

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
    /// CAD edge protecting-ball samples used as fixed packing anchors (sharp only).
    std::size_t n_edge_seeds = 0;
    /// Min/max protecting-ball radii used for edge seeds (m); 0 if none.
    /// Sized by r = min(α h, β lfs) with α=0.45, β=1/3, plus corner shrink.
    double min_protect_radius = 0.0;
    double max_protect_radius = 0.0;
    /// CAD edge class counts from topology (diagnostics).
    std::size_t n_sharp_edges = 0;
    std::size_t n_smooth_edges = 0;
    std::size_t n_seam_edges = 0;
    /// Interior packing seeds (volume bubbles) after edge-protect repulsion.
    std::size_t n_volume_seeds = 0;
    /// Bubble-relax iterations applied to volume seeds.
    int n_pack_relax_iters = 0;
    /// Packed-ball volume proxy / bbox volume (clamped [0,1]); diagnostic only.
    double pack_fill_frac = 0.0;
    /// Max Hausdorff residual of free-boundary mesh vs **sharp** CAD edges (m).
    double edge_profile_hausdorff_max = 0.0;
    double edge_profile_rel = 0.0; // max / characteristic sharp CAD edge length
    /// Chordal efficiency max e = d_actual / (ℓ²κ/8) on sharp-feature mesh
    /// segments (ADR-0023). ℓ = mesh edge length; κ from CAD curve curvature.
    double edge_chordal_efficiency_max = 0.0;
    double edge_hausdorff_over_h = 0.0;
};

/// Pack volume mesh with varyhedron packing-seed algorithm (ADR-0023).
/// When `topo` is non-null, **sharp** CAD edge samples become protecting seeds;
/// seams/smooth edges are never fixed sites. Interior volume seeds pack away
/// from them; free nodes near sharp edges soft-snap after the tet scaffold.
VaryhedronFillOutput varyhedron_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double curvature_turn_deg = 15.0, const geom::CadTopology* topo = nullptr);

} // namespace polymesh::mesh
