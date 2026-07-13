// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// G4: export clipped restricted-Voronoi cells as product PolyMesh polyhedra.
// Dual-of-tet remains hard-blocked — these cells *are* the poly path
// (ADR-0024 Q8 / ADR-0025).

#include "mesh/cvt_lloyd.hpp"
#include "mesh/geogram_clip.hpp"
#include "mesh/poly_mesh.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::mesh {

struct ClippedVoronoiExportStats {
    std::size_t n_sites = 0;
    std::size_t n_cells = 0;       // non-empty exported cells
    std::size_t n_empty_cells = 0;
    std::size_t n_faces = 0;
    std::size_t n_interior_faces = 0;
    std::size_t n_boundary_faces = 0;
    std::size_t n_vertices = 0;
    double sum_cell_volume = 0.0;
    bool geogram_ok = false;
};

struct ClippedVoronoiExport {
    PolyMesh mesh;
    ClippedVoronoiExportStats stats;
    /// Site index → cell id in mesh (or npos if empty/skipped).
    std::vector<std::size_t> site_to_cell;
};

/// Build a face-based PolyMesh of restricted Voronoi cells for `sites` inside
/// `domain`. Each non-empty cell is CellKind::kPolyhedron. Interior bisector
/// faces are shared (owner + neighbour); domain faces are boundary.
/// Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const Eigen::Vector3d> sites);

/// Convenience: export from CvtSite list (uses positions only).
[[nodiscard]] ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const CvtSite> sites);

}  // namespace polymesh::mesh
