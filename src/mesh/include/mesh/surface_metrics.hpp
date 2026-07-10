// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Geometry-only curved-surface mesh quality metrics (M1–M6) for Cartesian
// product fills (ADR-0015). Used by Catch2 curved scorecard and mesher
// diagnostics — not a FE accuracy claim.

#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace polymesh::mesh {

/// Known circular feature (hole wall or outer cylinder) for M4/M5.
struct CircularFeature {
    Eigen::Vector3d axis_point = Eigen::Vector3d::Zero();
    Eigen::Vector3d axis_dir = Eigen::Vector3d::UnitZ(); // unit preferred
    double radius = 0.0;                                // metres
    /// Nodes within this radial band of the feature are scored (metres).
    double select_band = 0.0;
};

/// Free-surface face: triangle when verts[3] == verts[2], else quad.
using FreeFace = std::array<std::uint32_t, 4>;

struct CurvedMeshMetrics {
    // M1 — free-surface node residual (m)
    double m1_max = 0.0;
    double m1_mean = 0.0;
    std::size_t n_boundary_nodes = 0;

    // M2 — free-surface edge midpoints + face centroids residual (m)
    double m2_max = 0.0;
    double m2_mean = 0.0;
    std::size_t n_face_samples = 0;

    // M3 — |V_mesh - V_ref| / V_ref (dimensionless); 0 if ref unused
    double m3_rel_volume_err = 0.0;
    bool has_volume = false;

    // M4 — max |r - R| / R on selected circular-feature nodes
    double m4_radial_rel = 0.0;
    std::size_t n_circular_nodes = 0;
    bool has_circular = false;

    // M5 — max azimuthal gap among circular-feature nodes (radians in [0, π])
    double m5_max_azimuth_gap = 0.0;

    // M6 — min tet aspect on free-surface-touching tets in (0,1]; 1 if N/A
    double m6_min_boundary_aspect = 1.0;
    std::size_t n_boundary_tets = 0;
    bool has_tet_aspect = false;

    /// Composite in [0,1], higher better. Weights favor M2/M4 (curved silhouette).
    double composite_score = 0.0;
};

/// Evaluate curved-surface metrics for a volume mesh free surface.
///
/// @param surface CAD/STL surface the lattice was filled against.
/// @param nodes Mesh nodes (metres).
/// @param free_faces Exterior faces (tris as degenerate quads).
/// @param h Characteristic lattice size for residual normalization (metres).
/// @param mesh_volume Summed solid volume (m³); ignored if ≤ 0.
/// @param ref_volume Analytical / known solid volume (m³); ignored if ≤ 0.
/// @param circular Optional hole/cylinder for M4/M5.
/// @param tets Optional tet connectivity for M6 (boundary-touching tets only).
CurvedMeshMetrics evaluate_curved_mesh_quality(
    const geom::TriSurface& surface, const std::vector<Eigen::Vector3d>& nodes,
    const std::vector<FreeFace>& free_faces, double h, double mesh_volume = -1.0,
    double ref_volume = -1.0, const CircularFeature* circular = nullptr,
    const std::vector<std::array<std::uint32_t, 4>>* tets = nullptr);

/// Collect unique free-surface node indices from free faces.
std::vector<std::uint32_t> free_face_nodes(const std::vector<FreeFace>& free_faces);

} // namespace polymesh::mesh
