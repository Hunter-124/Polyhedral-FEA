// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Clipped restricted-Voronoi cells exported as product PolyMesh polyhedra —
// these cells are the polyhedral path; dual-of-tet stays blocked
// (ADR-0024 Q8 / ADR-0025). Cells can be clipped to the solid (surface
// halfspaces or a tet scaffold) so domain faces land on the tessellated
// boundary instead of the AABB.

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
    std::size_t n_cells = 0; // non-empty exported cells
    std::size_t n_empty_cells = 0;
    /// Extra connected components created when one site's restricted region is
    /// disconnected. Each component is an independent admissible VEM cell.
    std::size_t n_split_site_components = 0;
    std::size_t n_faces = 0;
    std::size_t n_interior_faces = 0;
    std::size_t n_boundary_faces = 0;
    /// Voronoi-bisector fragments without the opposite owning cell.
    /// A conforming RVD export has zero; domain/tet boundary faces are excluded.
    std::size_t n_unpaired_bisector_faces = 0;
    /// Internal tetra-scaffold fragments without exactly two opposite claims
    /// from the same site. Any nonzero value makes the RVD inadmissible.
    std::size_t n_unpaired_scaffold_faces = 0;
    /// Exact face keys with a third claim, wrong site pair, or same winding.
    /// Any nonzero value makes the RVD inadmissible.
    std::size_t n_invalid_face_claims = 0;
    /// Exact coplanar RVD fragments removed by edge-connected polygon union.
    std::size_t n_coalesced_face_fragments = 0;
    /// Number of resulting polygon faces assembled from multiple fragments.
    std::size_t n_coalesced_faces = 0;
    std::size_t n_vertices = 0;
    std::size_t n_domain_plane_clips = 0; // total halfspace clips from surface
    /// Sum of raw clipped-piece volumes before topology/geometry admission.
    double sum_cell_volume = 0.0;
    bool geogram_ok = false;
    bool domain_clip_used = false;
};

struct ClippedVoronoiExport {
    PolyMesh mesh;
    ClippedVoronoiExportStats stats;
    /// Site index → first connected cell id in mesh (or npos if empty/skipped).
    /// A disconnected restricted region may yield additional cells counted by
    /// `stats.n_split_site_components`.
    std::vector<std::size_t> site_to_cell;
};

/// Optional solid-domain clip for export_clipped_voronoi. When `surface` is
/// non-null and non-empty, every cell is intersected with one global set of
/// supporting halfspaces from the surface triangles (≤ 8000, strided), i.e.
/// the convex envelope of the solid; use export_rvd_tet_clipped for
/// non-convex solids.
struct DomainClipParams {
    const geom::TriSurface* surface = nullptr;
    /// Unused: global planes need no local clip radius.
    double clip_radius = 0.0;
    /// Unused: planes are filtered by an absolute degeneracy floor instead.
    double min_area_frac = 1e-8;
};

/// Build a face-based PolyMesh of restricted Voronoi cells for `sites` inside
/// `domain` (AABB), optionally clipped to `domain_clip.surface`.
/// Each non-empty cell is CellKind::kPolyhedron. Interior bisector faces are
/// shared (owner + neighbour); domain / AABB faces are boundary.
/// Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] ClippedVoronoiExport
export_clipped_voronoi(const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
                       const DomainClipParams& domain_clip = {});

/// Convenience: export from CvtSite list (uses positions only).
[[nodiscard]] ClippedVoronoiExport
export_clipped_voronoi(const ClipBox& domain, std::span<const CvtSite> sites,
                       const DomainClipParams& domain_clip = {});

/// One tetrahedron used as a solid domain atom for true RVD ∩ Ω.
/// Vertices must have positive orientation (same as tet_fill).
struct DomainTet {
    Eigen::Vector3d v0, v1, v2, v3;
    Eigen::Vector3d centroid{0, 0, 0};
};

/// Restricted Voronoi: intersect every site cell with nearby domain tets, then
/// merge face-connected pieces; disconnected regions of one site become
/// independent cells. Shared fragments are paired by canonical identity after
/// the global tolerance weld, and edge-connected coplanar fragments are
/// coalesced into true polygon faces before VEM conversion. Domain-boundary
/// faces remain on the tet-mesh skin. This supports non-convex solids, where
/// one global halfspace intersection is invalid. `tet_search_radius` is
/// unused: a per-site security radius bounds the tet search.
/// Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] ClippedVoronoiExport
export_rvd_tet_clipped(const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
                       std::span<const DomainTet> tets, double tet_search_radius = 0.0);

[[nodiscard]] ClippedVoronoiExport export_rvd_tet_clipped(const ClipBox& domain,
                                                          std::span<const CvtSite> sites,
                                                          std::span<const DomainTet> tets,
                                                          double tet_search_radius = 0.0);

} // namespace polymesh::mesh
