// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Deterministic tet4 fill of a closed triangle surface (P2 v1 mesher).
// Cartesian grid over the bbox; each inside voxel is split into 6 tets along
// the space diagonal so shared faces match. Boundary is stair-cased; optional
// limited multi-pass surface snap (≤0.75 h) with Jacobian safety (unsnap if a
// tet would invert). NOT constrained Delaunay / frontal — see ADR-0015. Fully
// deterministic for (surface, h, snap flag).
//
// Lives in mesh/ (not fea/) so library deps stay acyclic: mesh → geom only.
// pipeline converts TetFillOutput into fea::NodalMesh for the frozen solver.

#include "geom/tri_surface.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/poly_mesh.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::mesh {

struct TetFillOutput {
    std::vector<Eigen::Vector3d> nodes; // metres
    /// Each tet: four node indices, positive orientation.
    std::vector<std::array<std::uint32_t, 4>> tets;
    /// Outer quads of the voxel lattice (region mapping / rendering).
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    double h = 0.0; // grid spacing used, metres
    /// Boundary conformity accounting for the mesher note (ADR-0035).
    SnapStats snap;
    FeaturePinReport pin;
};

/// Fill the interior of `surface` (assumed closed, outward CCW) at spacing `h` (metres).
/// Bbox corners are metres. Throws ValidityError on empty volume or absurd grid size.
///
/// @param fit Optional exact-BRep boundary fitting (ADR-0035): the snap
///        projects to the CAD oracle instead of the tessellation, sharp edges
///        and CAD vertices are hard-pinned, and the free surface is smoothed
///        with owner-aware re-projection. Null keeps the tessellated path.
TetFillOutput tet_fill_surface(const geom::TriSurface& surface,
                               const Eigen::Vector3d& bbox_min,
                               const Eigen::Vector3d& bbox_max, double h,
                               bool snap_boundary = true, const BoundaryFit* fit = nullptr);

/// Signed tet volume, m³ (positive for right-handed a,b,c,d).
double tet_signed_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                         const Eigen::Vector3d& c, const Eigen::Vector3d& d);

/// Positive volumes and finite coordinates for every tet in `out`.
/// @param min_volume Lower bound on |V|, m³ (0 = any positive).
void check_tet_fill_geometry(const TetFillOutput& out, double min_volume = 0.0);

/// Census of boundary self-intersection in an all-tet mesh.
///
/// A free face is *buried* when its centroid lies strictly inside a tet it
/// does not belong to: two positive-volume sheets interpenetrate, typically at
/// a concave CAD crease where each sheet's nodes snap to their own face patch.
/// Watertight and orientation censuses cannot see this — every tet is
/// positive, every edge manifold — but renderers show the buried faces as
/// holes and any downstream contact/BC selection on them is wrong.
struct BuriedFaceStats {
    std::size_t n_free_faces = 0;
    std::size_t n_buried = 0;
};

/// Count buried free faces. `h` sizes the spatial hash (use the fill spacing).
BuriedFaceStats count_buried_free_tet_faces(std::span<const Eigen::Vector3d> nodes,
                                            std::span<const std::array<std::uint32_t, 4>> tets,
                                            double h);


/// Owner tets of buried free faces, deduplicated. The owners are overlapping
/// volume — the same material is inside another cell too — so the remedy is
/// deletion (shell-guarded, then re-snap), not node motion: pulling nodes at a
/// near-tangent crease piles both sheets onto the crease and multiplies the
/// crossings (measured: 299 → 706 buried on sphere_box_s0 at h = 3.6 mm).
std::vector<std::uint32_t>
buried_free_tet_face_owners(std::span<const Eigen::Vector3d> nodes,
                            std::span<const std::array<std::uint32_t, 4>> tets, double h);

/// Finisher for shallow residue after the overlap carve: pull each buried
/// face's nodes toward the centroid of their incident tet star (inward, off
/// the foreign sheet), bisecting so no incident tet inverts. Deep or
/// near-tangent overlap MUST be carved first — pulling cannot resolve it.
/// Returns buried faces remaining (0 = clean).
std::size_t pull_buried_free_faces(std::vector<Eigen::Vector3d>& nodes,
                                   std::span<const std::array<std::uint32_t, 4>> tets,
                                   double h, int max_iters = 8);

} // namespace polymesh::mesh
