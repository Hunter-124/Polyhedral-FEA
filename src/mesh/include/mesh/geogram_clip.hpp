// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Thin facade over vendored Geogram ConvexCell (G1 / ADR-0025).
// Callers must not include Delaunay_psm.h outside geogram_clip.cpp.

#include <Eigen/Core>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// True when this build linked `polymesh_geogram` (POLYMESH_WITH_GEOGRAM).
[[nodiscard]] bool geogram_available() noexcept;

/// Initialize Geogram process state once (safe to call repeatedly).
/// No-op when geogram is not compiled in.
void geogram_ensure_initialized();

/// Axis-aligned box used as the initial ConvexCell domain.
struct ClipBox {
    Eigen::Vector3d min{0, 0, 0};
    Eigen::Vector3d max{1, 1, 1};
};

/// Plane ax + by + cz + d = 0. ConvexCell keeps the halfspace
/// a·x + d ≥ 0 (see G1 smoke / Geogram VBW::ConvexCell convention).
struct ClipPlane {
    double a = 0;
    double b = 0;
    double c = 0;
    double d = 0;

    static ClipPlane from_point_normal(const Eigen::Vector3d& p,
                                       const Eigen::Vector3d& n_unit) {
        // n·x - n·p = 0 → keep n·x ≥ n·p  ⇒  n·x + d ≥ 0 with d = -n·p
        ClipPlane pl;
        pl.a = n_unit.x();
        pl.b = n_unit.y();
        pl.c = n_unit.z();
        pl.d = -n_unit.dot(p);
        return pl;
    }
};

/// Geometry of a clipped convex cell after halfspace intersection.
struct ClippedCell {
    double volume = 0.0;
    Eigen::Vector3d barycenter{0, 0, 0};
    /// Number of triangle facets in the VBW triangulation of the cell boundary.
    std::size_t n_triangles = 0;
    /// Number of halfspaces / vertices in the dual arrangement (including box).
    std::size_t n_planes = 0;
    bool empty = true;
};

/// Build a ConvexCell from `box`, clip by each plane in order, compute volume
/// and barycenter. Returns nullopt if geogram is unavailable or the cell dies.
[[nodiscard]] std::optional<ClippedCell> clip_convex_cell(
    const ClipBox& box, std::span<const ClipPlane> planes);

/// Convenience: unit cube [0,1]³ with no extra planes.
[[nodiscard]] std::optional<ClippedCell> unit_cube_cell();

}  // namespace polymesh::mesh
