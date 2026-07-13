// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// CAD edge / face / vertex map extracted from CadModel (ADR-0020 / V1b).
// Used for edge-profile packing, auto-h, and boundary alignment metrics.

#include "geom/cad_model.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <optional>
#include <vector>

namespace polymesh::geom {

struct CadVertex {
    std::uint32_t id = 0;
    Eigen::Vector3d position = Eigen::Vector3d::Zero();
};

/// Classification of a BRep edge for feature protection / free-slide walls.
/// Default threshold in extract_topology: 25° deviation from flat (π).
enum class CadEdgeFeature : std::uint8_t {
    kSharp = 0,  // hard feature — protect / snap
    kSmooth = 1, // G1-ish dihedral — free-sliding wall only
    kSeam = 2,   // OCC parameterization seam (same face both sides) — never protect
};

struct CadEdge {
    std::uint32_t id = 0;
    std::uint32_t v0 = 0;
    std::uint32_t v1 = 0;
    double length = 0.0;
    /// Sampled polyline along the curve (metres), open chain v0→v1.
    std::vector<Eigen::Vector3d> samples;
    /// Feature class (sharp / smooth / seam). Default sharp for safety.
    CadEdgeFeature feature = CadEdgeFeature::kSharp;
    /// Dihedral between adjacent faces (rad). Convention: π = flat / coplanar
    /// continuation; smaller = sharper crease. Seam / unknown may be 0.
    double dihedral_rad = 0.0;
};

enum class CadSurfaceKind : std::uint8_t {
    kPlane = 0,
    kCylinder,
    kSphere,
    kCone,
    kTorus,
    kOther
};

struct CadFace {
    std::uint32_t id = 0;
    CadSurfaceKind kind = CadSurfaceKind::kOther;
    double area = 0.0;
    std::vector<std::uint32_t> edge_ids;
};

/// Topology + sampled geometry for a CadModel BRep.
struct CadTopology {
    std::vector<CadVertex> vertices;
    std::vector<CadEdge> edges;
    std::vector<CadFace> faces;

    [[nodiscard]] bool empty() const noexcept {
        return vertices.empty() && edges.empty() && faces.empty();
    }
};

/// Extract vertices, edges (with polyline samples), and faces from `model`.
/// `samples_per_edge` is the target interior sample count (≥1); endpoints are
/// always included. Throws GeomError if model is empty or OCC is disabled.
///
/// Edge feature classification (OCC path):
/// - same face on both sides → kSeam
/// - ≥2 faces: dihedral from outward normals; if |dihedral − π| < 25° → kSmooth,
///   else kSharp (default sharp threshold: 25° from flat)
/// - single-face open-shell boundary → kSharp
[[nodiscard]] CadTopology extract_topology(const CadModel& model,
                                           int samples_per_edge = 8);

struct ClosestEdgeQuery {
    std::uint32_t edge_id = 0;
    double distance = 0.0;
    Eigen::Vector3d closest = Eigen::Vector3d::Zero();
    /// Arc-length parameter estimate in [0,1] along the sampled polyline.
    double t = 0.0;
};

/// Closest CAD edge (by sampled polyline distance) to point `p`.
[[nodiscard]] std::optional<ClosestEdgeQuery>
closest_edge(const CadTopology& topo, const Eigen::Vector3d& p);

/// Closest CAD edge matching the filter. When `sharp_only` is true, only
/// CadEdgeFeature::kSharp edges are considered (protected features).
[[nodiscard]] std::optional<ClosestEdgeQuery>
closest_edge(const CadTopology& topo, const Eigen::Vector3d& p, bool sharp_only);

/// Max Hausdorff-style residual of `polyline` vs nearest CAD edge samples
/// (max over polyline points of min distance to any edge sample). Metres.
[[nodiscard]] double edge_profile_hausdorff(const CadTopology& topo,
                                            const std::vector<Eigen::Vector3d>& polyline);

/// One-sided Hausdorff polyline → CAD curves. When `sharp_only` is true, only
/// sharp (protected) edges contribute — use for residual of protected features.
[[nodiscard]] double edge_profile_hausdorff_filtered(
    const CadTopology& topo, const std::vector<Eigen::Vector3d>& polyline,
    bool sharp_only = true);

/// Per-segment chordal residual vs CAD plus chordal-efficiency proxy.
/// `h` is the local mesh size used for efficiency e = d / max(eps, h²κ/8).
struct ChordalEdgeMetrics {
    double hausdorff = 0;        // max distance polyline→CAD (m)
    double hausdorff_over_h = 0; // / max(h, eps)
    double max_chordal = 0;      // max midpoint sagitta (m)
    double max_efficiency = 0;   // max d/(h²κ/8); κ from CAD samples or 0
    int n_segments = 0;
};

[[nodiscard]] ChordalEdgeMetrics
chordal_edge_metrics(const CadTopology& topo,
                     const std::vector<Eigen::Vector3d>& mesh_feature_polyline,
                     double h, bool sharp_edges_only = true);

/// Count edges by feature class (tests / logging).
struct CadEdgeClassCounts {
    int n_sharp = 0;
    int n_smooth = 0;
    int n_seam = 0;
};

[[nodiscard]] CadEdgeClassCounts count_edge_features(const CadTopology& topo);

} // namespace polymesh::geom
