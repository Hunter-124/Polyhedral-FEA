// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Thin facade over vendored Geogram ConvexCell (G1 / ADR-0025).
// Callers must not include Delaunay_psm.h outside geogram_clip.cpp.

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
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

/// Uniform bucket grid over site positions for neighbour-restricted Voronoi
/// clipping. A Voronoi cell only touches its geometric neighbours, so callers
/// clip ring-by-ring (Chebyshev shells) around a query site and stop via a
/// security radius (Geogram `ConvexCell::squared_radius`) instead of testing
/// all N sites — turning per-cell clipping from O(N) into O(neighbours).
/// Rebuild is O(N) (counting sort); cheap to redo after each Lloyd iteration.
class SiteGrid {
  public:
    /// Build over `pts` with bucket edge `cell_edge` (≈ target site spacing).
    void build(std::span<const Eigen::Vector3d> pts, double cell_edge);
    [[nodiscard]] bool empty() const noexcept { return n_pts_ == 0; }
    [[nodiscard]] double cell_edge() const noexcept { return cell_; }
    /// Rings that exhaust the grid from any bucket (upper bound for the loop).
    [[nodiscard]] int max_ring() const noexcept { return max_ring_; }
    /// Append site indices whose bucket is at Chebyshev distance exactly `ring`
    /// from `p`'s bucket (ring 0 = home bucket). `out` is NOT cleared.
    void ring(const Eigen::Vector3d& p, int ring, std::vector<std::uint32_t>& out) const;

  private:
    [[nodiscard]] int clampi(int v, int hi) const noexcept {
        return v < 0 ? 0 : (v > hi ? hi : v);
    }
    Eigen::Vector3d origin_{0, 0, 0};
    double cell_ = 1.0;
    double inv_cell_ = 1.0;
    int nx_ = 0, ny_ = 0, nz_ = 0;
    int max_ring_ = 0;
    std::size_t n_pts_ = 0;
    std::vector<std::uint32_t> cell_start_;  // CSR offsets, size nx*ny*nz+1
    std::vector<std::uint32_t> items_;       // site indices bucketed
};

}  // namespace polymesh::mesh
