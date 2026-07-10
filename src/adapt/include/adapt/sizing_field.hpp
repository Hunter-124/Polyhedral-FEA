// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Adaptivity: sizing fields, a posteriori error estimation
// (Zienkiewicz–Zhu patch recovery), Dörfler marking, and per-element
// h-vs-p refinement decisions.
//
// Fills in during Phase P5. Until then this module carries only the
// interfaces the mesher (P2/P3) codes against.

#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <functional>
#include <memory>
#include <vector>

namespace polymesh::adapt {

/// Target element size as a field over space, metres.
///
/// Produced by geometric feature analysis (a priori, Phase P3) and updated by
/// error estimation (a posteriori, Phase P5).
class SizingField {
  public:
    virtual ~SizingField() = default;

    /// Desired local edge length at `point`, metres. Must be strictly positive.
    virtual double size_at(const Eigen::Vector3d& point) const = 0;
};

/// Constant size everywhere — the trivial field used for uniform baselines.
class UniformSizing final : public SizingField {
  public:
    /// `h`: edge length, metres.
    explicit UniformSizing(double h) : h_(h) {}

    double size_at(const Eigen::Vector3d& /*point*/) const override { return h_; }

  private:
    double h_;
};

/// Size blends from `h_min` at features (distance 0) to `h_max` at and beyond
/// `blend_distance`. Linear ramp in between. All lengths in metres.
class FeatureSizing final : public SizingField {
  public:
    using DistanceFn = std::function<double(const Eigen::Vector3d&)>;

    /// @param h_min Size at features, metres (must be > 0).
    /// @param h_max Far-field size, metres (must be ≥ h_min).
    /// @param blend_distance Distance to full h_max, metres.
    /// @param distance_fn Distance to nearest feature, metres.
    FeatureSizing(double h_min, double h_max, double blend_distance, DistanceFn distance_fn);

    double size_at(const Eigen::Vector3d& point) const override;

  private:
    double h_min_ = 0.0; // m
    double h_max_ = 0.0; // m
    double blend_ = 0.0; // m
    DistanceFn dist_;
};

/// Feature sizing using `geom::distance_to_features` on the given sharp edges.
/// @param h_min,h_max,blend_distance Lengths in metres (see FeatureSizing).
std::unique_ptr<SizingField> make_feature_sizing(double h_min, double h_max,
                                                 double blend_distance,
                                                 const geom::TriSurface& surface,
                                                 const std::vector<geom::SharpEdge>& edges);

/// Geometry-aware sizing: min of sharp-edge feature blend, curvature target
/// \(h \approx c_\kappa / \kappa\), and thin-wall target \(h \approx f_t\, t\).
///
/// Heuristic limits: same as `geom::estimate_vertex_curvature` /
/// `estimate_local_thickness`. Attributes are taken from the nearest surface
/// vertex and relaxed toward `h_max` over `blend_distance` from the surface
/// so deep interior bulk is not forced to wall thickness alone.
class GeometrySizing final : public SizingField {
  public:
    /// @param h_min,h_max Bounds on returned size, metres (0 < h_min ≤ h_max).
    /// @param blend_distance Surface / feature distance to full h_max, metres.
    /// @param surface Closed or open triangle mesh (copied).
    /// @param edges Sharp edges for crease grading (may be empty).
    /// @param curvature_fraction Target h ≈ fraction / κ (κ in 1/m); default 0.25
    ///        ≈ quarter radius-of-curvature per element.
    /// @param thickness_fraction Target h ≈ fraction × local thickness; default 0.35.
    GeometrySizing(double h_min, double h_max, double blend_distance, geom::TriSurface surface,
                   std::vector<geom::SharpEdge> edges, double curvature_fraction = 0.25,
                   double thickness_fraction = 0.35);

    double size_at(const Eigen::Vector3d& point) const override;

  private:
    double h_min_ = 0.0;
    double h_max_ = 0.0;
    double blend_ = 0.0;
    double curv_frac_ = 0.25;
    double thick_frac_ = 0.35;
    geom::TriSurface surface_;
    std::vector<geom::SharpEdge> edges_;
    std::vector<double> kappa_;     // 1/m per vertex
    std::vector<double> thickness_; // m per vertex
};

/// Factory: builds GeometrySizing from surface (+ optional precomputed edges).
std::unique_ptr<SizingField> make_geometry_sizing(double h_min, double h_max,
                                                  double blend_distance,
                                                  const geom::TriSurface& surface,
                                                  const std::vector<geom::SharpEdge>& edges,
                                                  double curvature_fraction = 0.25,
                                                  double thickness_fraction = 0.35);

} // namespace polymesh::adapt
