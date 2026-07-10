// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Adaptivity: sizing fields, a posteriori error estimation
// (Zienkiewicz–Zhu patch recovery), Dörfler marking, and per-element
// h-vs-p refinement decisions.
//
// Fills in during Phase P5. Until then this module carries only the
// interfaces the mesher (P2/P3) codes against.

#include <Eigen/Core>

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

} // namespace polymesh::adapt
