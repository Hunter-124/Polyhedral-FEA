// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Varyhedron packing fill (ADR-0021) — v1 scaffold:
// CAD edge seeds + multi-level graded tet scaffold + boundary edge-profile
// snap toward CadTopology polylines. Interior dual-poly clustering is the
// follow-on packing track (V6c / V11); v1 ships a working volume path that
// measures edge_profile residual and solves as tet/VEM-ready nodal mesh.

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
    std::size_t n_edge_seeds = 0;
    /// Max Hausdorff residual of free-boundary mesh edges vs CAD edge samples
    /// (metres). 0 if no topology provided.
    double edge_profile_hausdorff_max = 0.0;
    double edge_profile_rel = 0.0; // max / characteristic CAD edge length
};

/// Pack volume mesh with varyhedron v1 algorithm.
/// When `topo` is non-null, CAD edge samples become refine seeds and free
/// nodes near edges are attracted to the CAD edge profile.
VaryhedronFillOutput varyhedron_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double curvature_turn_deg = 15.0, const geom::CadTopology* topo = nullptr);

} // namespace polymesh::mesh
