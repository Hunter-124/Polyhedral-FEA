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

struct CadEdge {
    std::uint32_t id = 0;
    std::uint32_t v0 = 0;
    std::uint32_t v1 = 0;
    double length = 0.0;
    /// Sampled polyline along the curve (metres), open chain v0→v1.
    std::vector<Eigen::Vector3d> samples;
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

/// Max Hausdorff-style residual of `polyline` vs nearest CAD edge samples
/// (max over polyline points of min distance to any edge sample). Metres.
[[nodiscard]] double edge_profile_hausdorff(const CadTopology& topo,
                                            const std::vector<Eigen::Vector3d>& polyline);

} // namespace polymesh::geom
