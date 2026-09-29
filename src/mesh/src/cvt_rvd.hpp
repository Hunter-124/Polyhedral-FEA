// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to polymesh_mesh: raw ConvexCell face extraction shared by the
// clipped-Voronoi exports, and the phase seam of export_rvd_tet_clipped:
//   build_rvd_tet_cells (cvt_rvd_cells.cpp) → RvdTetCells →
//   claim_rvd_faces     (cvt_rvd_faces.cpp) → PolyMesh with face tombstones →
//   admission (compaction) in cvt_export.cpp.

#include "cvt_geometry.hpp"
#include "mesh/cvt_export.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

namespace polymesh::mesh::detail {

struct QuantKey {
    long long x = 0, y = 0, z = 0;
    bool operator==(const QuantKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct QuantHash {
    std::size_t operator()(const QuantKey& k) const noexcept {
        // splitmix-ish combine
        std::size_t h = static_cast<std::size_t>(k.x) * 0x9e3779b97f4a7c15ULL;
        h ^= static_cast<std::size_t>(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

enum class RawFaceProvenance {
    kDomainBoundary,
    kTetScaffold,
    kBisector,
};

/// Face extracted from one cell before welding / pairing.
struct RawFace {
    std::vector<Eigen::Vector3d> loop; // ordered polygon
    RawFaceProvenance provenance = RawFaceProvenance::kDomainBoundary;
    /// Set only for a bisector face; preserves the opposite site identity.
    std::size_t neighbour_site = static_cast<std::size_t>(-1);
    /// Set only for a tagged tetra scaffold face.
    std::size_t tet_local_face = static_cast<std::size_t>(-1);
};

struct RawCell {
    std::vector<RawFace> faces;
    double volume = 0.0;
    bool empty = true;
};

/// Facet loops of a clipped cell with provenance from the vglobal tags:
/// -1 box, -2 domain surface, tet_face_tag(f) scaffold face (only when
/// `tet_scaffold_tags`), otherwise the neighbour site id of a bisector.
/// `empty` when the volume is not positive or fewer than 4 faces survive.
RawCell extract_raw_cell(VBW::ConvexCell& cell, bool tet_scaffold_tags = false);

/// Scaffold face class from the exact-coordinate multiplicity of its tet face:
/// 1 = domain skin, 2 = internal cut, >2 = non-manifold (rejected).
enum class ScaffoldFaceProvenance {
    kDomainBoundary,
    kInternal,
    kNonManifold,
};

/// One non-empty clipped piece V_site ∩ tet.
struct RvdPiece {
    std::size_t site = 0;
    std::size_t tet = 0;
    RawCell cell;
};

/// Seam between RVD-tet cell construction and face claiming.
struct RvdTetCells {
    /// Pieces in deterministic (site, tet) order, independent of thread count.
    std::vector<RvdPiece> pieces;
    /// Indexed by input tet; one entry per local face (fixed local face order).
    std::vector<std::array<ScaffoldFaceProvenance, 4>> tet_face_provenance;
};

/// Clip every site's Voronoi cell against every nearby tet of the scaffold.
/// Inputs are in the domain-local frame; `diag` is the local domain diagonal.
/// Accumulates scaffold, empty-cell, clip-count and volume stats.
[[nodiscard]] RvdTetCells build_rvd_tet_cells(const ClipBox& local_domain,
                                              std::span<const Eigen::Vector3d> sites,
                                              std::span<const DomainTet> tets, double diag,
                                              ClippedVoronoiExportStats& stats);

/// Weld piece vertices around `weld_origin`, merge same-site pieces joined by
/// an internal scaffold face into cells, validate exact face claims and
/// coalesce coplanar interior fragments. Appends to `out.mesh` (cancelled
/// faces are left as empty-loop tombstones), `out.site_to_cell` and `out.stats`.
void claim_rvd_faces(const RvdTetCells& cells, const Eigen::Vector3d& weld_origin, double diag,
                     ClippedVoronoiExport& out);

} // namespace polymesh::mesh::detail

#endif // POLYMESH_WITH_GEOGRAM
