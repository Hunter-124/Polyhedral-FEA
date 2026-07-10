// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// A posteriori error utilities: Dörfler marking and feature-aware sizing.

#include "adapt/sizing_field.hpp"
#include "geom/features.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <vector>

namespace polymesh::adapt {

/// Dörfler marking: mark smallest set of elements whose η² sum ≥ θ * total η².
/// Returns sorted element indices (descending η). θ in (0,1], default 0.3.
std::vector<std::size_t> dorfler_mark(const std::vector<double>& element_eta,
                                      double theta = 0.3);

/// Smooth (low-error) complement of Dörfler: candidates for p-elevation.
/// Returns element indices not in the high-η Dörfler set (ascending order).
/// When total η² is zero, returns all indices (entire mesh is smooth).
std::vector<std::size_t> mark_smooth(const std::vector<double>& element_eta,
                                     double theta = 0.3);

/// Sizing that refines toward sharp features:
/// \( h(x) = \mathrm{clamp}(h_{\min} + \alpha\, d_{\mathrm{feat}},\, h_{\min},\, h_{\max}) \).
/// Lengths \(h_{\min}\), \(h_{\max}\), \(d_{\mathrm{feat}}\) in metres; \(\alpha\)
/// dimensionless.
class FeatureGradedSizing final : public SizingField {
  public:
    /// @param h_min,h_max Edge lengths, metres. @param alpha Dimensionless scale on
    /// \(d_{\mathrm{feat}}\).
    FeatureGradedSizing(const geom::TriSurface& surface, std::vector<geom::SharpEdge> edges,
                        double h_min, double h_max, double alpha = 0.35)
        : surface_(&surface), edges_(std::move(edges)), h_min_(h_min), h_max_(h_max),
          alpha_(alpha) {}

    double size_at(const Eigen::Vector3d& point) const override;

  private:
    const geom::TriSurface* surface_;
    std::vector<geom::SharpEdge> edges_;
    double h_min_; // m
    double h_max_; // m
    double alpha_;
};

} // namespace polymesh::adapt
