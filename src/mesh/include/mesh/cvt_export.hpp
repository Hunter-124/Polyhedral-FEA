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
[[nodiscard]] ClippedVoronoiExport
export_clipped_voronoi(const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
                       const DomainClipParams& domain_clip = {});

/// Convenience: export from CvtSite list (uses positions only).
[[nodiscard]] ClippedVoronoiExport
export_clipped_voronoi(const ClipBox& domain, std::span<const CvtSite> sites,
                       const DomainClipParams& domain_clip = {});

/// Build inward-oriented clip planes from a closed triangle surface, each
/// plane oriented so `interior_hint` is on the keep side (a·x+d ≥ 0).
/// Used by tests and by export when precomputing global planes for convex Ω.
[[nodiscard]] std::vector<ClipPlane>
domain_planes_from_surface(const geom::TriSurface& surface,
                           const Eigen::Vector3d& interior_hint, double min_area = 0.0);

/// One tetrahedron used as a solid domain atom for true RVD ∩ Ω (M5).
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
/// faces remain on the tet-mesh skin. This supports non-convex solids such as
/// plate_hole where one global halfspace intersection is invalid.
/// Requires POLYMESH_WITH_GEOGRAM.
[[nodiscard]] ClippedVoronoiExport
export_rvd_tet_clipped(const ClipBox& domain, std::span<const Eigen::Vector3d> sites,
                       std::span<const DomainTet> tets, double tet_search_radius = 0.0);

[[nodiscard]] ClippedVoronoiExport export_rvd_tet_clipped(const ClipBox& domain,
                                                          std::span<const CvtSite> sites,
                                                          std::span<const DomainTet> tets,
                                                          double tet_search_radius = 0.0);

} // namespace polymesh::mesh
