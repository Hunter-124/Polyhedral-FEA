// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Closest-point projection onto a triangle surface, boundary conformity
// metrics, and Jacobian-safe limited surface snap for Cartesian fills.

#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mirror.hpp"

#include <Eigen/Core>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace polymesh::mesh {

struct ClosestPoint {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    double distance = 0.0;
    std::size_t triangle = 0;
};

/// Closest point on the surface to `p` (brute-force; fine for product STLs).
ClosestPoint closest_on_surface(const geom::TriSurface& surface, const Eigen::Vector3d& p);

/// Max / mean distance of `points` to the surface (metres).
struct ConformityStats {
    double max_distance = 0.0;
    double mean_distance = 0.0;
    std::size_t count = 0;
};

ConformityStats surface_conformity(const geom::TriSurface& surface,
                                   const std::vector<Eigen::Vector3d>& points,
                                   const std::vector<std::uint32_t>& point_indices);

/// Compact persistent owner for one boundary node. IDs are exact BRep topology
/// ids supplied by the callback; unknown is the STL/legacy heuristic state.
enum class BoundarySupportKind : std::uint8_t {
    kUnknown = 0,
    kCadVertex,
    kCadEdge,
    kCadFace,
};

struct BoundarySupport {
    BoundarySupportKind kind = BoundarySupportKind::kUnknown;
    std::uint32_t id = 0;
};

struct BoundaryTarget {
    Eigen::Vector3d point = Eigen::Vector3d::Zero();
    double distance = 0.0;
};

using BoundaryTargetFn = std::function<std::optional<BoundaryTarget>(
    const Eigen::Vector3d& query, BoundarySupport& support)>;

/// Optional exact projection oracle and persistent global-node-indexed owners.
/// The vector is grown, never reset, when new/refined nodes first reach snap.
struct BoundaryProjectionContext {
    std::vector<BoundarySupport>* provenance = nullptr;
    BoundaryTargetFn target;
    /// Sharp-edge table the oracle classified against, when it is CAD-backed.
    /// Lets a mid-edge node inherit an edge owner its endpoints share even
    /// when their provenance kinds differ (a rim node is on the edge AND on a
    /// face). Null on the legacy tessellation path.
    std::shared_ptr<const geom::CadTopology> topology;
};

/// Resolve only through the exact owner-aware oracle. Returns nullopt when no
/// oracle is installed or the immutable owner cannot be projected. This is the
/// no-triangle-fallback entry point for later wall/CVT passes.
///
/// `mirror` folds the query into the canonical octant of the geometry's verified
/// reflection symmetry and reflects the answer back (mesh/mirror.hpp), so a node
/// and its mirror image are projected to mirrored points and classify to the same
/// owner. Without it the two see different tessellation, different nearest
/// features and different owners, and the mesh loses the symmetry the lattice had.
[[nodiscard]] std::optional<BoundaryTarget>
owned_boundary_projection_target(const Eigen::Vector3d& p, std::uint32_t node,
                                 BoundaryProjectionContext* context,
                                 const MirrorFrame* mirror = nullptr);

/// Resolve a node target through the exact owner-aware oracle when present,
/// otherwise through the legacy TriSurface closest-point path. `mirror` as above.
[[nodiscard]] std::optional<BoundaryTarget>
boundary_projection_target(const geom::TriSurface& surface, const Eigen::Vector3d& p,
                           std::uint32_t node, BoundaryProjectionContext* context = nullptr,
                           const MirrorFrame* mirror = nullptr);

/// Result of a Jacobian-safe boundary snap.
struct SnapStats {
    std::size_t n_candidates = 0;
    std::size_t n_moved = 0;
    std::size_t n_unsnapped = 0;
    std::size_t n_relax_rescued = 0; // projections saved by interior relaxation
    double max_residual = 0.0;       // metres, after snap/unsnap
};

/// Collect every node that participates in an invalid element. Used for the
/// initial/final global validity sweeps.
using CollectOffendersFn = std::function<void(std::set<std::uint32_t>& offenders)>;
using RepairInteriorFn = std::function<void()>;
/// Fast local validity query used while line-searching one moved node. The
/// caller should inspect only elements incident to `node`; omitting it keeps the
/// compatibility global-scan path.
using NodeOffendsFn = std::function<bool(std::uint32_t node)>;
/// Open room for a boundary node that cannot keep any fraction of its
/// projection: relax the INTERIOR nodes of its incident star toward their own
/// neighbour centroids, accepting only moves that keep every incident cell
/// valid. Returns true when something actually moved, i.e. the caller should
/// retry the projection. Interior nodes carry no geometry constraint, so this
/// cannot cost boundary fidelity — without it a stair-fold cell forces the
/// node all the way back to its raw lattice site, which is the O(h)
/// off-surface outlier the fidelity metric reports (ADR-0035).
using RelaxNeighborhoodFn = std::function<bool(std::uint32_t node)>;

/// Reflection-equivariant ordering key for a point: the quantised distance from
/// a mirror-invariant centre, per axis.
///
/// Every pass that moves, merges or deletes a cell after testing its incident
/// star is order-dependent by construction — an earlier accepted decision
/// decides whether a later one is legal. ADR-0032 made that order
/// platform-independent by driving it from ascending node id or from a tet's
/// index. Neither mirrors, so a cell and its mirror image saw different
/// predecessor states and their accept/reject outcomes diverged. Measured on
/// `cylinder.step` at h = 8 mm, whose tessellation is exactly mirror-symmetric:
/// the sliver-collapse round dropped the mirrored-tet fraction from
/// 99.83/100/100% to 98.35/96.91/98.97%, and the tangential smoothing pass then
/// amplified that seed to 91.4/86.8/93.4%.
///
/// Distance from the centre is mirror-invariant, and quantising it to 1e-9 of the
/// bbox diagonal makes a mirror pair key bit-identically — so comparing keys is
/// equivariant, and a mirror pair is adjacent in any order derived from them
/// (ADR-0036).
struct MirrorKeyFrame {
    Eigen::Vector3d center = Eigen::Vector3d::Zero();
    double inv_quantum = 0.0;

    [[nodiscard]] std::array<long long, 3> key(const Eigen::Vector3d& p) const {
        const Eigen::Vector3d d = (p - center).cwiseAbs() * inv_quantum;
        return {static_cast<long long>(d.x()), static_cast<long long>(d.y()),
                static_cast<long long>(d.z())};
    }
};

/// Frame spanning every node in `nodes` (an empty range yields a degenerate
/// frame whose keys are all zero, which orders nothing and breaks nothing).
MirrorKeyFrame mirror_key_frame(const std::vector<Eigen::Vector3d>& nodes);

/// Sort `ids` by `MirrorKeyFrame::key`, node id as the final tie-break, using a
/// frame spanning only the referenced nodes.
void sort_mirror_canonical(const std::vector<Eigen::Vector3d>& nodes,
                           std::vector<std::uint32_t>& ids);

/// Pull boundary lattice nodes toward the STL in multi-pass steps, then unsnap
/// any node that participates in an inverted element (B3 / ADR-0015).
///
/// @param h Characteristic lattice size (metres) — caps travel.
/// @param max_move_frac Max |Δ| / h per node across all passes (default 0.75;
///        product paths often pass 1.0–1.15 so LEB mid-edges leave the stair).
/// @param passes Number of incremental projection passes (default 4).
/// @param feature_edges Optional sharp CAD edges: true crease nodes (as close
///        to a feature as to the surface) project to the edge first; free-face
///        / hole-wall nodes still project to the surface.
/// @param defer_coupled Keep locally irreparable fan nodes for a coupled
///        restore; pure hex/poly meshes leave this false for a linear scan.
/// @param relax_neighborhood Optional interior-room opener; see
///        RelaxNeighborhoodFn. Called before a node is allowed to retreat.
/// @param mirror Optional verified reflection symmetry: every projection,
///        closest-point and feature-capture query is answered in the canonical
///        octant and reflected back, so mirrored nodes snap to mirrored targets.
SnapStats snap_boundary_nodes(
    const geom::TriSurface& surface, std::vector<Eigen::Vector3d>& nodes,
    const std::vector<std::uint32_t>& boundary_nodes, double h,
    const CollectOffendersFn& collect_offenders, double max_move_frac = 0.75, int passes = 4,
    std::span<const geom::SharpEdge> feature_edges = {},
    const RepairInteriorFn& repair_interior = {}, const NodeOffendsFn& node_offends = {},
    bool defer_coupled = false, BoundaryProjectionContext* projection = nullptr,
    const RelaxNeighborhoodFn& relax_neighborhood = {}, const MirrorFrame* mirror = nullptr);

/// Result of a tangential boundary smoothing pass.
struct SmoothStats {
    std::size_t n_moved = 0;    // nodes moved in the final accepted state
    std::size_t n_reverted = 0; // nodes reverted by the inversion guard
    double max_residual = 0.0;  // metres, after smoothing
};

/// Constrained Laplacian smoothing of free-surface nodes: each boundary node
/// relaxes toward the centroid of its boundary neighbors, then re-projects to
/// the STL, so travel is tangential (residual stays ~0) while stair/sawtooth
/// spacing evens out. Crease nodes (within ~0.1 h of a sharp feature edge and
/// forming a 2-neighbor crease chain) relax along the crease and re-project to
/// it; corners/junctions and near-crease wall nodes are left untouched so
/// sharp edges stay sharp. Nodes whose move inverts an element are reverted
/// via `collect_offenders` (B3-safe like the snap).
SmoothStats smooth_boundary_nodes(const geom::TriSurface& surface,
                                  std::vector<Eigen::Vector3d>& nodes,
                                  std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                                  double h, const CollectOffendersFn& collect_offenders,
                                  int passes = 2, double relax = 0.5,
                                  std::span<const geom::SharpEdge> feature_edges = {},
                                  BoundaryProjectionContext* projection = nullptr,
                                  const MirrorFrame* mirror = nullptr);

} // namespace polymesh::mesh
