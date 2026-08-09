// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Gradient-limited sizing driven by geometry AND boundary conditions.
//
// A single primitive powers graded meshing: a size field assembled from a set
// of "size sources" {(x_i, h_i)} and read back as
//
//     size_at(p) = clamp( min_i ( h_i + beta * ||p - x_i|| ), h_min, h_max ).
//
// This lower-envelope (min-plus) form is Lipschitz-continuous with constant
// `beta`, so the requested size grows no faster than `beta` per unit distance —
// the classic gradient-limited mesh size function (Persson 2006; Alauzet,
// Borouchaki). Grading is therefore smooth by construction, with no separate
// post-limiter and no slivers at fine/coarse interfaces.
//
// Sources come from two places, fused into one field:
//   - geometry (a priori): surface vertices where curvature / thin-wall demand
//     a finer size than h_max (see geometry_size_sources);
//   - boundary conditions (a priori): centroids of the boundary faces that
//     carry a load or a fixture, finest under applied loads where stress
//     concentrates (see point_size_sources).
//
// The same field feeds every variable-density mesher through `SizeFieldFn`;
// `seed_plan` remains available for the legacy ball-grading path and for
// a-posteriori refinement marks.

#include "adapt/sizing_field.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::adapt {

/// A local size request: target edge length `h` (metres) at world point `x`.
struct SizeSource {
    Eigen::Vector3d x{0.0, 0.0, 0.0};
    double h = 0.0;
};

/// Gradient-limited sizing field built from size sources (see file header).
/// Empty source set ⇒ uniform h_max everywhere.
class GradedSizing final : public SizingField {
  public:
    /// @param sources Size requests; each h is clamped into [h_min, h_max].
    /// @param h_min,h_max Edge-length bounds, metres (0 < h_min ≤ h_max).
    /// @param beta Max gradation (dimensionless size growth per unit length), > 0.
    GradedSizing(std::vector<SizeSource> sources, double h_min, double h_max, double beta = 1.0);

    double size_at(const Eigen::Vector3d& point) const override;
    /// Non-owning field view. The GradedSizing object must outlive the callable.
    mesh::SizeFieldFn as_size_field() const;

    const std::vector<SizeSource>& sources() const { return sources_; }
    double beta() const { return beta_; }
    double h_min() const { return h_min_; }
    double h_max() const { return h_max_; }

  private:
    // Uniform hash grid over sources so size_at is O(nearby), not O(all): a
    // source only lowers the envelope within (h_max - h_min)/beta of the query,
    // so only that neighbourhood is scanned. Built once in the constructor.
    void build_grid();
    std::vector<SizeSource> sources_;
    double h_min_;
    double h_max_;
    double beta_;
    Eigen::Vector3d grid_origin_{0, 0, 0};
    double grid_cell_ = 1.0;
    std::int32_t grid_nx_ = 0, grid_ny_ = 0, grid_nz_ = 0;
    std::vector<std::uint32_t> cell_start_;  // CSR offsets
    std::vector<std::uint32_t> items_;       // source indices bucketed
};

/// Geometry-driven size sources: one per surface vertex whose curvature or
/// thin-wall target is finer than `h_max`. h = min(curvature_fraction / kappa,
/// thickness_fraction * thickness), clamped to [h_min, h_max]. Flat, thick
/// vertices emit no source, keeping the source set sparse (only the
/// geometrically interesting regions cost anything downstream).
std::vector<SizeSource> geometry_size_sources(const geom::TriSurface& surface, double h_min,
                                              double h_max, double curvature_fraction = 0.25,
                                              double thickness_fraction = 0.35);

/// Boundary-condition size sources: a source of target size `h` at each point
/// (typically the centroids of boundary faces carrying a load or fixture).
/// Loads should pass a finer `h` than fixtures — error concentrates where the
/// load is applied.
std::vector<SizeSource> point_size_sources(std::span<const Eigen::Vector3d> points, double h);
/// Owning gradient-limited size field. Source h and bounds are edge lengths
/// in metres; the returned callable is safe after `sources` goes out of scope.
mesh::SizeFieldFn size_field_from_sources(std::span<const SizeSource> sources, double h_min,
                                          double h_max, double beta = 1.0);


/// Refine-seed plan for the ball-grading meshers (pipeline::volume_mesh):
/// seeds = source points that ask for a size finer than `h_coarse`;
/// band = band_frac * h_coarse (single influence radius, ball semantics).
struct SeedPlan {
    std::vector<Eigen::Vector3d> refine_seeds;
    double seed_band = 0.0;
    double h_fine = 0.0; // finest requested target among the fine sources (m)
};
SeedPlan seed_plan(std::span<const SizeSource> sources, double h_coarse, double band_frac = 2.0);

} // namespace polymesh::adapt
