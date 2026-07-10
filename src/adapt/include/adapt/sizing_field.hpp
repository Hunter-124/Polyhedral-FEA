// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Adaptivity: sizing fields, a posteriori error estimation
// (Zienkiewicz–Zhu patch recovery), Dörfler marking, and per-element
// h-vs-p refinement decisions.
//
// Fills in during Phase P5. Until then this module carries only the
// interfaces the mesher (P2/P3) codes against.

#include "geom/features.hpp"
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
/// `blend_distance`. Linear ramp in between.
class FeatureSizing final : public SizingField {
  public:
    using DistanceFn = std::function<double(const Eigen::Vector3d&)>;

    FeatureSizing(double h_min, double h_max, double blend_distance, DistanceFn distance_fn);

    double size_at(const Eigen::Vector3d& point) const override;

  private:
    double h_min_ = 0.0;
    double h_max_ = 0.0;
    double blend_ = 0.0;
    DistanceFn dist_;
};

/// Feature sizing using `geom::distance_to_features` on the given sharp edges.
std::unique_ptr<SizingField> make_feature_sizing(double h_min, double h_max,
                                                 double blend_distance,
                                                 const geom::TriSurface& surface,
                                                 const std::vector<geom::SharpEdge>& edges);

} // namespace polymesh::adapt
