// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// G4: export clipped restricted-Voronoi cells as product PolyMesh polyhedra.
// Dual-of-tet remains hard-blocked — these cells *are* the poly path
// (ADR-0024 Q8 / ADR-0025).
//
// M5: optional BRep/surface-domain clip so cells stop at the solid interior
// (not the AABB). Domain faces then land on the tessellated surface, which is
// required for honest load_area_ok on plate_hole / cylinder.

#include "geom/tri_surface.hpp"
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
    std::size_t n_domain_plane_clips = 0;  // total halfspace clips from surface
    double sum_cell_volume = 0.0;
    bool geogram_ok = false;
    bool domain_clip_used = false;
};

struct ClippedVoronoiExport {
    PolyMesh mesh;
    ClippedVoronoiExportStats stats;
    /// Site index → cell id in mesh (or npos if empty/skipped).
    std::vector<std::size_t> site_to_cell;
};

/// Optional solid-domain clip (M5). When `surface` is non-null and non-empty,
/// each Voronoi cell is further intersected with local triangle halfspaces
/// (oriented so the site stays inside). This approximates RVD ∩ Ω without a
/// full volume tet mesh. `clip_radius ≤ 0` → auto (~2× mean nearest-neighbour).
struct DomainClipParams {
    const geom::TriSurface* surface = nullptr;
    double clip_radius = 0.0;
    /// Skip triangles whose area is below this fraction of mean area (noise).
    double min_area_frac = 1e-8;
};

/// Build a face-based PolyMesh of restricted Voronoi cells for `sites` inside
/// `domain` (AABB), optionally clipped to `domain_clip.surface`.
/// Each non-empty cell is CellKind::kPolyhedron. Interior bisector faces are
/// shared (owner + neighbour); domain / AABB faces are boundary.
/// Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
    const DomainClipParams& domain_clip = {});

/// Convenience: export from CvtSite list (uses positions only).
[[nodiscard]] ClippedVoronoiExport export_clipped_voronoi(
    const ClipBox& domain, std::span<const CvtSite> sites,
    const DomainClipParams& domain_clip = {});

/// Build inward-oriented clip planes from a closed triangle surface, each
/// plane oriented so `interior_hint` is on the keep side (a·x+d ≥ 0).
/// Used by tests and by export when precomputing global planes for convex Ω.
[[nodiscard]] std::vector<ClipPlane> domain_planes_from_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& interior_hint,
    double min_area = 0.0);

}  // namespace polymesh::mesh
