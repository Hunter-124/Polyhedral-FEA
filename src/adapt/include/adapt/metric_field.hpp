// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Symmetric-positive-definite metric fields for anisotropic adaptation.
// For an edge e (metres), sqrt(e^T M e) is its dimensionless metric length;
// an eigenvalue lambda_i = 1/h_i^2 requests edge length h_i (metres).

#include "mesh/cvt_lloyd.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <functional>
#include <vector>

namespace polymesh::adapt {

/// A three-dimensional Riemannian metric. `M` is symmetric positive-definite
/// with units m^-2; the identity metric therefore requests one-metre edges.
struct Metric3d {
    Eigen::Matrix3d M = Eigen::Matrix3d::Identity();

    Metric3d() = default;
    explicit Metric3d(const Eigen::Matrix3d& matrix);

    /// Uniform desired edge length h (metres), h > 0.
    static Metric3d isotropic(double h);

    /// Desired lengths h (metres) along the columns of `axes`. The supplied
    /// frame is re-orthonormalised defensively and must have full rank.
    static Metric3d from_axes(const Eigen::Vector3d& h, const Eigen::Matrix3d& axes);

    /// Principal desired lengths (metres), ordered by ascending metric
    /// eigenvalue. `axes()` returns the corresponding directions.
    [[nodiscard]] Eigen::Vector3d sizes() const;
    [[nodiscard]] Eigen::Matrix3d axes() const;

    /// Dimensionless metric length sqrt(edge^T M edge), for edge in metres.
    [[nodiscard]] double length(const Eigen::Vector3d& edge) const;

    /// Element density sqrt(det M), m^-3, up to an element-shape constant.
    [[nodiscard]] double volume_density() const;

    /// Clamp principal sizes to [h_min,h_max] (metres), then raise the smaller
    /// sizes until max(h)/min(h) <= max_aspect.
    [[nodiscard]] Metric3d clamped(double h_min, double h_max, double max_aspect) const;

    /// Simultaneous-reduction intersection: the more restrictive metric in
    /// every common principal direction.
    [[nodiscard]] Metric3d intersect(const Metric3d& other) const;

    /// Log-Euclidean interpolation; t is clamped to [0,1].
    [[nodiscard]] static Metric3d log_interp(const Metric3d& a, const Metric3d& b, double t);

    /// Conservative scalar projection (metres): the smallest principal size.
    /// Scalar consumers therefore never under-resolve an anisotropic request.
    [[nodiscard]] double isotropic_size() const;
};

/// Cartesian node samples of an SPD metric over an AABB (metres). Requested
/// node counts are auto-coarsened proportionally when their product exceeds
/// `kMaxNodes`; each axis always retains at least two nodes.
class MetricGrid {
  public:
    static constexpr std::size_t kMaxNodes = 2'097'152;

    MetricGrid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
               const Eigen::Vector3i& resolution);

    void set(std::size_t i, std::size_t j, std::size_t k, const Metric3d& metric);
    [[nodiscard]] Metric3d at_node(std::size_t i, std::size_t j, std::size_t k) const;

    /// Log-metric trilinear interpolation. Queries outside the AABB clamp to
    /// its boundary.
    [[nodiscard]] Metric3d sample(const Eigen::Vector3d& x) const;

    /// Fill nodes in deterministic k-major, then j-major, then i-major order.
    void fill(const std::function<Metric3d(const Eigen::Vector3d&)>& f);

    /// Conservative scalar view. The grid must outlive the returned function.
    [[nodiscard]] mesh::SizeFieldFn as_size_field() const;

    [[nodiscard]] const Eigen::Vector3d& bbox_min() const { return bbox_min_; }
    [[nodiscard]] const Eigen::Vector3d& bbox_max() const { return bbox_max_; }
    [[nodiscard]] const Eigen::Vector3i& resolution() const { return resolution_; }
    [[nodiscard]] Eigen::Vector3d node_position(std::size_t i, std::size_t j,
                                                std::size_t k) const;

  private:
    [[nodiscard]] std::size_t index(std::size_t i, std::size_t j, std::size_t k) const;

    Eigen::Vector3d bbox_min_ = Eigen::Vector3d::Zero();
    Eigen::Vector3d bbox_max_ = Eigen::Vector3d::Ones();
    Eigen::Vector3d spacing_ = Eigen::Vector3d::Ones();
    Eigen::Vector3i resolution_{2, 2, 2};
    std::vector<Metric3d> metrics_;

    friend void limit_gradation(MetricGrid& grid, double beta, int max_sweeps);
    friend double complexity(const MetricGrid& grid);
    friend void normalize_complexity(MetricGrid& grid, double target_complexity, double h_min,
                                     double h_max, double max_aspect);
};

/// Linear-interpolation Hessian metric. The constant c=1 is absorbed into
/// eps_target: lambda(M) = |lambda(H)| / eps_target. Flat directions request
/// h_max; all principal sizes are clamped in metres.
Metric3d metric_from_hessian(const Eigen::Matrix3d& H, double eps_target, double h_min,
                             double h_max, double max_aspect);

/// Alauzet-style adjacent-node gradation. Forward and backward deterministic
/// sweeps stop when no metric changes by more than 1e-12 in Frobenius norm.
void limit_gradation(MetricGrid& grid, double beta, int max_sweeps = 20);

/// Midpoint-rule integral C = integral sqrt(det M) dV over grid cells.
/// The result is an element count up to an element-shape constant.
[[nodiscard]] double complexity(const MetricGrid& grid);

/// Apply the global 3-D metric scale (target/current)^(2/3), then clamp each
/// node. Re-clamping can move the achieved complexity away from the target;
/// callers requiring exact normalisation should iterate and re-measure.
void normalize_complexity(MetricGrid& grid, double target_complexity, double h_min,
                          double h_max, double max_aspect);

} // namespace polymesh::adapt
