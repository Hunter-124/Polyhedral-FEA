// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/metric_field.hpp"

#include <Eigen/Eigenvalues>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>
#include <utility>

namespace polymesh::adapt {
namespace {

struct PrincipalSystem {
    Eigen::Vector3d eigenvalues;
    Eigen::Matrix3d eigenvectors;
};

Eigen::Matrix3d checked_spd(const Eigen::Matrix3d& matrix, const char* where) {
    if (!matrix.allFinite()) {
        throw std::invalid_argument(std::string(where) + ": metric must be finite");
    }
    const double scale = std::max(1.0, matrix.norm());
    if ((matrix - matrix.transpose()).norm() > 1e-12 * scale) {
        throw std::invalid_argument(std::string(where) + ": metric must be symmetric");
    }
    const Eigen::Matrix3d symmetric = 0.5 * (matrix + matrix.transpose());
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(symmetric);
    if (eig.info() != Eigen::Success || !eig.eigenvalues().allFinite() ||
        !(eig.eigenvalues().minCoeff() > 0.0)) {
        throw std::invalid_argument(std::string(where) + ": metric must be positive-definite");
    }
    return symmetric;
}

PrincipalSystem principal_system(const Eigen::Matrix3d& matrix, const char* where) {
    const Eigen::Matrix3d symmetric = checked_spd(matrix, where);
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(symmetric);

    std::array<int, 3> order{0, 1, 2};
    std::stable_sort(order.begin(), order.end(), [&](int lhs, int rhs) {
        return eig.eigenvalues()[lhs] < eig.eigenvalues()[rhs];
    });

    PrincipalSystem result;
    for (int out = 0; out < 3; ++out) {
        const int in = order[static_cast<std::size_t>(out)];
        result.eigenvalues[out] = eig.eigenvalues()[in];
        Eigen::Vector3d axis = eig.eigenvectors().col(in);
        int pivot = 0;
        for (int c = 1; c < 3; ++c) {
            if (std::abs(axis[c]) > std::abs(axis[pivot])) {
                pivot = c;
            }
        }
        if (axis[pivot] < 0.0) {
            axis = -axis;
        }
        result.eigenvectors.col(out) = axis;
    }
    return result;
}

Eigen::Matrix3d matrix_log(const Eigen::Matrix3d& matrix) {
    const PrincipalSystem p = principal_system(matrix, "matrix_log");
    return p.eigenvectors * p.eigenvalues.array().log().matrix().asDiagonal() *
           p.eigenvectors.transpose();
}

Eigen::Matrix3d matrix_exp(const Eigen::Matrix3d& matrix) {
    if (!matrix.allFinite()) {
        throw std::invalid_argument("matrix_exp: matrix must be finite");
    }
    const Eigen::Matrix3d symmetric = 0.5 * (matrix + matrix.transpose());
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(symmetric);
    if (eig.info() != Eigen::Success || !eig.eigenvalues().allFinite()) {
        throw std::runtime_error("matrix_exp: eigensolve failed");
    }
    const Eigen::Vector3d values = eig.eigenvalues().array().exp();
    if (!values.allFinite() || !(values.minCoeff() > 0.0)) {
        throw std::overflow_error("matrix_exp: result is not finite SPD");
    }
    return eig.eigenvectors() * values.asDiagonal() * eig.eigenvectors().transpose();
}

bool dominates(const Eigen::Matrix3d& finer, const Eigen::Matrix3d& coarser) {
    const Eigen::Matrix3d delta = 0.5 * ((finer - coarser) + (finer - coarser).transpose());
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(delta);
    const double scale = std::max({1.0, finer.norm(), coarser.norm()});
    return eig.info() == Eigen::Success && eig.eigenvalues().minCoeff() >=
                                               -64.0 * std::numeric_limits<double>::epsilon() *
                                                   scale;
}

Eigen::Matrix3d repaired_spd(Eigen::Matrix3d matrix) {
    matrix = 0.5 * (matrix + matrix.transpose());
    if (!matrix.allFinite()) {
        throw std::runtime_error("Metric3d::intersect: non-finite simultaneous reduction");
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(matrix);
    if (eig.info() != Eigen::Success || !eig.eigenvalues().allFinite()) {
        throw std::runtime_error("Metric3d::intersect: eigensolve failed");
    }
    Eigen::Vector3d values = eig.eigenvalues();
    if (!(values.minCoeff() > 0.0)) {
        const double floor = std::max(std::numeric_limits<double>::min(),
                                      values.cwiseAbs().maxCoeff() * 1e-14);
        values = values.cwiseMax(floor);
        matrix = eig.eigenvectors() * values.asDiagonal() * eig.eigenvectors().transpose();
        matrix = 0.5 * (matrix + matrix.transpose());
        eig.compute(matrix);
    }
    assert(eig.info() == Eigen::Success && eig.eigenvalues().minCoeff() > 0.0);
    return matrix;
}

std::size_t node_count(const Eigen::Vector3i& resolution) {
    return static_cast<std::size_t>(resolution.x()) * static_cast<std::size_t>(resolution.y()) *
           static_cast<std::size_t>(resolution.z());
}

Eigen::Vector3i capped_resolution(const Eigen::Vector3i& requested) {
    const long double requested_count = static_cast<long double>(requested.x()) *
                                        static_cast<long double>(requested.y()) *
                                        static_cast<long double>(requested.z());
    if (requested_count <= static_cast<long double>(MetricGrid::kMaxNodes)) {
        return requested;
    }

    auto scaled = [&](long double factor) {
        Eigen::Vector3i result;
        for (int axis = 0; axis < 3; ++axis) {
            const long double intervals = static_cast<long double>(requested[axis] - 1);
            const long double count = std::floor(intervals * factor) + 1.0L;
            result[axis] = std::max(2, static_cast<int>(count));
        }
        return result;
    };

    long double low = 0.0L;
    long double high = 1.0L;
    for (int iter = 0; iter < 80; ++iter) {
        const long double mid = 0.5L * (low + high);
        const Eigen::Vector3i candidate = scaled(mid);
        if (node_count(candidate) <= MetricGrid::kMaxNodes) {
            low = mid;
        } else {
            high = mid;
        }
    }
    Eigen::Vector3i result = scaled(low);
    while (node_count(result) > MetricGrid::kMaxNodes) {
        int axis_to_reduce = -1;
        double largest_fraction = -1.0;
        for (int axis = 0; axis < 3; ++axis) {
            if (result[axis] <= 2) {
                continue;
            }
            const double fraction = static_cast<double>(result[axis] - 1) /
                                    static_cast<double>(requested[axis] - 1);
            if (fraction > largest_fraction) {
                largest_fraction = fraction;
                axis_to_reduce = axis;
            }
        }
        if (axis_to_reduce < 0) {
            throw std::runtime_error("MetricGrid: unable to satisfy node cap");
        }
        --result[axis_to_reduce];
    }
    return result;
}

} // namespace

Metric3d::Metric3d(const Eigen::Matrix3d& matrix) : M(checked_spd(matrix, "Metric3d")) {}

Metric3d Metric3d::isotropic(double h) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw std::invalid_argument("Metric3d::isotropic: h must be finite and positive");
    }
    const double inv_h = 1.0 / h;
    const double lambda = inv_h * inv_h;
    if (!(lambda > 0.0) || !std::isfinite(lambda)) {
        throw std::invalid_argument("Metric3d::isotropic: h is outside the representable range");
    }
    return Metric3d(lambda * Eigen::Matrix3d::Identity());
}

Metric3d Metric3d::from_axes(const Eigen::Vector3d& h, const Eigen::Matrix3d& axes) {
    if (!h.allFinite() || !(h.minCoeff() > 0.0) || !axes.allFinite()) {
        throw std::invalid_argument("Metric3d::from_axes: finite positive sizes and axes required");
    }

    const double frame_scale = axes.colwise().norm().maxCoeff();
    if (!(frame_scale > 0.0) || !std::isfinite(frame_scale)) {
        throw std::invalid_argument("Metric3d::from_axes: axes must have full rank");
    }
    const double rank_tol = 64.0 * std::numeric_limits<double>::epsilon() * frame_scale;
    Eigen::Matrix3d q;
    for (int col = 0; col < 3; ++col) {
        Eigen::Vector3d v = axes.col(col);
        // Modified Gram-Schmidt with one re-orthogonalisation pass is stable for
        // the nearly orthonormal frames produced by geometry estimators.
        for (int pass = 0; pass < 2; ++pass) {
            for (int prev = 0; prev < col; ++prev) {
                v -= q.col(prev).dot(v) * q.col(prev);
            }
        }
        const double norm = v.norm();
        if (!(norm > rank_tol) || !std::isfinite(norm)) {
            throw std::invalid_argument("Metric3d::from_axes: axes must have full rank");
        }
        q.col(col) = v / norm;
    }

    const Eigen::Vector3d inv_h = h.cwiseInverse();
    const Eigen::Vector3d lambda = inv_h.array().square().matrix();
    if (!lambda.allFinite() || !(lambda.minCoeff() > 0.0)) {
        throw std::invalid_argument("Metric3d::from_axes: sizes are outside representable range");
    }
    const Eigen::Matrix3d metric = q * lambda.asDiagonal() * q.transpose();
    return Metric3d(metric);
}

Eigen::Vector3d Metric3d::sizes() const {
    const PrincipalSystem p = principal_system(M, "Metric3d::sizes");
    return p.eigenvalues.array().sqrt().inverse().matrix();
}

Eigen::Matrix3d Metric3d::axes() const {
    return principal_system(M, "Metric3d::axes").eigenvectors;
}

double Metric3d::length(const Eigen::Vector3d& edge) const {
    if (!edge.allFinite()) {
        throw std::invalid_argument("Metric3d::length: edge must be finite");
    }
    const Eigen::Matrix3d metric = checked_spd(M, "Metric3d::length");
    return std::sqrt(std::max(0.0, edge.dot(metric * edge)));
}

double Metric3d::volume_density() const {
    const Eigen::Matrix3d metric = checked_spd(M, "Metric3d::volume_density");
    return std::sqrt(metric.determinant());
}

Metric3d Metric3d::clamped(double h_min, double h_max, double max_aspect) const {
    if (!(h_min > 0.0) || !(h_max >= h_min) || !std::isfinite(h_min) ||
        !std::isfinite(h_max) || !(max_aspect >= 1.0) || !std::isfinite(max_aspect)) {
        throw std::invalid_argument(
            "Metric3d::clamped: need finite 0 < h_min <= h_max and max_aspect >= 1");
    }
    const PrincipalSystem p = principal_system(M, "Metric3d::clamped");
    Eigen::Vector3d h = p.eigenvalues.array().sqrt().inverse().matrix();
    h = h.cwiseMax(h_min).cwiseMin(h_max);
    const double aspect_floor = h.maxCoeff() / max_aspect;
    h = h.cwiseMax(aspect_floor);
    return from_axes(h, p.eigenvectors);
}

Metric3d Metric3d::intersect(const Metric3d& other) const {
    const Eigen::Matrix3d a = checked_spd(M, "Metric3d::intersect");
    const Eigen::Matrix3d b = checked_spd(other.M, "Metric3d::intersect");
    if (dominates(a, b)) {
        return Metric3d(a);
    }
    if (dominates(b, a)) {
        return Metric3d(b);
    }

    // Primary branch: solve M2 v = d M1 v. The eigenvectors V satisfy
    // V^T M1 V = I and V^T M2 V = diag(d), so lifting diag(max(1,d)) gives
    // the standard simultaneous-reduction intersection.
    Eigen::GeneralizedSelfAdjointEigenSolver<Eigen::Matrix3d> generalized(b, a);
    Eigen::Matrix3d candidate = Eigen::Matrix3d::Zero();
    bool primary_ok = generalized.info() == Eigen::Success &&
                      generalized.eigenvalues().allFinite() &&
                      generalized.eigenvectors().allFinite() &&
                      generalized.eigenvalues().minCoeff() > 0.0;
    if (primary_ok) {
        const Eigen::FullPivLU<Eigen::Matrix3d> lu(generalized.eigenvectors());
        primary_ok = lu.isInvertible() && lu.rcond() > 1e-12;
        if (primary_ok) {
            const Eigen::Matrix3d inverse = lu.inverse();
            const Eigen::Vector3d restricted =
                generalized.eigenvalues().cwiseMax(Eigen::Vector3d::Ones());
            candidate = inverse.transpose() * restricted.asDiagonal() * inverse;
        }
    }

    if (!primary_ok) {
        // Ill-conditioned fallback: diagonalise M1^-1 M2 = P diag(d) P^-1,
        // take max(1,d) in that common basis, then lift with M1. The exact
        // result is symmetric; explicit symmetrisation removes round-off skew.
        const Eigen::Matrix3d relative = a.fullPivLu().solve(b);
        const Eigen::EigenSolver<Eigen::Matrix3d> eig(relative, true);
        if (eig.info() != Eigen::Success || !eig.eigenvalues().allFinite() ||
            !eig.eigenvectors().allFinite()) {
            throw std::runtime_error("Metric3d::intersect: fallback eigensolve failed");
        }
        const double imag_tol = 1e-10 * std::max(1.0, eig.eigenvalues().cwiseAbs().maxCoeff());
        if (eig.eigenvalues().imag().cwiseAbs().maxCoeff() > imag_tol ||
            eig.eigenvectors().imag().cwiseAbs().maxCoeff() > imag_tol) {
            throw std::runtime_error("Metric3d::intersect: fallback produced complex basis");
        }
        const Eigen::Vector3d ratios = eig.eigenvalues().real();
        const Eigen::Matrix3d basis = eig.eigenvectors().real();
        const Eigen::FullPivLU<Eigen::Matrix3d> lu(basis);
        if (!ratios.allFinite() || !(ratios.minCoeff() > 0.0) || !lu.isInvertible()) {
            throw std::runtime_error("Metric3d::intersect: fallback basis is singular");
        }
        candidate = a * basis * ratios.cwiseMax(Eigen::Vector3d::Ones()).asDiagonal() *
                    lu.inverse();
    }

    return Metric3d(repaired_spd(candidate));
}

Metric3d Metric3d::log_interp(const Metric3d& a, const Metric3d& b, double t) {
    if (!std::isfinite(t)) {
        throw std::invalid_argument("Metric3d::log_interp: t must be finite");
    }
    const double weight = std::clamp(t, 0.0, 1.0);
    if (weight == 0.0) {
        return Metric3d(a.M);
    }
    if (weight == 1.0) {
        return Metric3d(b.M);
    }
    const Eigen::Matrix3d log_metric =
        (1.0 - weight) * matrix_log(a.M) + weight * matrix_log(b.M);
    return Metric3d(matrix_exp(log_metric));
}

double Metric3d::isotropic_size() const {
    return sizes().minCoeff();
}

MetricGrid::MetricGrid(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                       const Eigen::Vector3i& resolution)
    : bbox_min_(bbox_min), bbox_max_(bbox_max) {
    if (!bbox_min_.allFinite() || !bbox_max_.allFinite() ||
        !((bbox_max_ - bbox_min_).array() > 0.0).all()) {
        throw std::invalid_argument("MetricGrid: bbox must be finite with positive extent");
    }
    if ((resolution.array() < 2).any()) {
        throw std::invalid_argument("MetricGrid: resolution must have at least two nodes per axis");
    }
    resolution_ = capped_resolution(resolution);
    spacing_ = (bbox_max_ - bbox_min_).cwiseQuotient(
        (resolution_.cast<double>() - Eigen::Vector3d::Ones()));
    metrics_.assign(node_count(resolution_), Metric3d{});
}

std::size_t MetricGrid::index(std::size_t i, std::size_t j, std::size_t k) const {
    const std::size_t nx = static_cast<std::size_t>(resolution_.x());
    const std::size_t ny = static_cast<std::size_t>(resolution_.y());
    const std::size_t nz = static_cast<std::size_t>(resolution_.z());
    if (i >= nx || j >= ny || k >= nz) {
        throw std::out_of_range("MetricGrid: node index out of range");
    }
    return (k * ny + j) * nx + i;
}

void MetricGrid::set(std::size_t i, std::size_t j, std::size_t k, const Metric3d& metric) {
    metrics_[index(i, j, k)] = Metric3d(metric.M);
}

Metric3d MetricGrid::at_node(std::size_t i, std::size_t j, std::size_t k) const {
    return metrics_[index(i, j, k)];
}

Eigen::Vector3d MetricGrid::node_position(std::size_t i, std::size_t j, std::size_t k) const {
    static_cast<void>(index(i, j, k));
    return bbox_min_ +
           spacing_.cwiseProduct(Eigen::Vector3d{static_cast<double>(i), static_cast<double>(j),
                                                static_cast<double>(k)});
}

Metric3d MetricGrid::sample(const Eigen::Vector3d& x) const {
    if (!x.allFinite()) {
        throw std::invalid_argument("MetricGrid::sample: point must be finite");
    }
    Eigen::Vector3d local = (x.cwiseMax(bbox_min_).cwiseMin(bbox_max_) - bbox_min_)
                                .cwiseQuotient(spacing_);
    Eigen::Vector3i base;
    Eigen::Vector3d fraction;
    for (int axis = 0; axis < 3; ++axis) {
        const int last_cell = resolution_[axis] - 2;
        base[axis] = std::min(static_cast<int>(std::floor(local[axis])), last_cell);
        fraction[axis] = local[axis] - static_cast<double>(base[axis]);
    }

    Eigen::Matrix3d log_metric = Eigen::Matrix3d::Zero();
    for (int dk = 0; dk <= 1; ++dk) {
        const double wk = dk == 0 ? 1.0 - fraction.z() : fraction.z();
        for (int dj = 0; dj <= 1; ++dj) {
            const double wj = dj == 0 ? 1.0 - fraction.y() : fraction.y();
            for (int di = 0; di <= 1; ++di) {
                const double wi = di == 0 ? 1.0 - fraction.x() : fraction.x();
                const double weight = wi * wj * wk;
                if (weight == 0.0) {
                    continue;
                }
                log_metric += weight * matrix_log(metrics_[index(
                                                  static_cast<std::size_t>(base.x() + di),
                                                  static_cast<std::size_t>(base.y() + dj),
                                                  static_cast<std::size_t>(base.z() + dk))]
                                                      .M);
            }
        }
    }
    return Metric3d(matrix_exp(log_metric));
}

void MetricGrid::fill(const std::function<Metric3d(const Eigen::Vector3d&)>& f) {
    if (!f) {
        throw std::invalid_argument("MetricGrid::fill: function must be callable");
    }
    for (int k = 0; k < resolution_.z(); ++k) {
        for (int j = 0; j < resolution_.y(); ++j) {
            for (int i = 0; i < resolution_.x(); ++i) {
                set(static_cast<std::size_t>(i), static_cast<std::size_t>(j),
                    static_cast<std::size_t>(k),
                    f(node_position(static_cast<std::size_t>(i), static_cast<std::size_t>(j),
                                    static_cast<std::size_t>(k))));
            }
        }
    }
}

mesh::SizeFieldFn MetricGrid::as_size_field() const {
    return [this](const Eigen::Vector3d& x) { return sample(x).isotropic_size(); };
}

Metric3d metric_from_hessian(const Eigen::Matrix3d& H, double eps_target, double h_min,
                             double h_max, double max_aspect) {
    if (!H.allFinite() || !(eps_target > 0.0) || !std::isfinite(eps_target)) {
        throw std::invalid_argument("metric_from_hessian: finite H and eps_target > 0 required");
    }
    if (!(h_min > 0.0) || !(h_max >= h_min) || !std::isfinite(h_min) ||
        !std::isfinite(h_max) || !(max_aspect >= 1.0) || !std::isfinite(max_aspect)) {
        throw std::invalid_argument(
            "metric_from_hessian: need finite 0 < h_min <= h_max and max_aspect >= 1");
    }
    const Eigen::Matrix3d symmetric = 0.5 * (H + H.transpose());
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(symmetric);
    if (eig.info() != Eigen::Success || !eig.eigenvalues().allFinite()) {
        throw std::runtime_error("metric_from_hessian: eigensolve failed");
    }

    const Eigen::Vector3d magnitude = eig.eigenvalues().cwiseAbs();
    const double near_zero = 64.0 * std::numeric_limits<double>::epsilon() *
                             std::max(1.0, magnitude.maxCoeff());
    Eigen::Vector3d h = Eigen::Vector3d::Constant(h_max);
    for (int axis = 0; axis < 3; ++axis) {
        if (magnitude[axis] > near_zero) {
            // c=1 for linear interpolation; its conventional shape constant is
            // absorbed into eps_target so the user controls the error scale.
            h[axis] = std::sqrt(eps_target / magnitude[axis]);
        }
    }
    return Metric3d::from_axes(h, eig.eigenvectors()).clamped(h_min, h_max, max_aspect);
}

void limit_gradation(MetricGrid& grid, double beta, int max_sweeps) {
    if (!(beta > 0.0) || !std::isfinite(beta)) {
        throw std::invalid_argument("limit_gradation: beta must be finite and positive");
    }
    if (max_sweeps < 0) {
        throw std::invalid_argument("limit_gradation: max_sweeps must be non-negative");
    }

    auto update = [&](std::size_t from, std::size_t to, const Eigen::Vector3d& edge,
                      bool& changed) {
        const Metric3d source = grid.metrics_[from];
        const double metric_edge = source.length(edge);
        const double growth = 1.0 + beta * metric_edge;
        if (!std::isfinite(growth)) {
            throw std::overflow_error("limit_gradation: metric edge growth overflowed");
        }
        const double factor = 1.0 / (growth * growth);
        if (!(factor > 0.0) || !std::isfinite(factor)) {
            throw std::overflow_error("limit_gradation: span metric underflowed");
        }
        const Metric3d span(source.M * factor);
        if (dominates(grid.metrics_[to].M, span.M)) {
            return;
        }
        const Metric3d replacement = grid.metrics_[to].intersect(span);
        if ((replacement.M - grid.metrics_[to].M).norm() > 1e-12) {
            grid.metrics_[to] = replacement;
            changed = true;
        }
    };

    const int nx = grid.resolution_.x();
    const int ny = grid.resolution_.y();
    const int nz = grid.resolution_.z();
    auto raw_index = [=](int i, int j, int k) {
        return (static_cast<std::size_t>(k) * static_cast<std::size_t>(ny) +
                static_cast<std::size_t>(j)) *
                   static_cast<std::size_t>(nx) +
               static_cast<std::size_t>(i);
    };

    for (int sweep = 0; sweep < max_sweeps; ++sweep) {
        bool changed = false;
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const std::size_t from = raw_index(i, j, k);
                    if (i + 1 < nx) {
                        update(from, raw_index(i + 1, j, k),
                               Eigen::Vector3d{grid.spacing_.x(), 0.0, 0.0}, changed);
                    }
                    if (j + 1 < ny) {
                        update(from, raw_index(i, j + 1, k),
                               Eigen::Vector3d{0.0, grid.spacing_.y(), 0.0}, changed);
                    }
                    if (k + 1 < nz) {
                        update(from, raw_index(i, j, k + 1),
                               Eigen::Vector3d{0.0, 0.0, grid.spacing_.z()}, changed);
                    }
                }
            }
        }
        for (int k = nz - 1; k >= 0; --k) {
            for (int j = ny - 1; j >= 0; --j) {
                for (int i = nx - 1; i >= 0; --i) {
                    const std::size_t from = raw_index(i, j, k);
                    if (i > 0) {
                        update(from, raw_index(i - 1, j, k),
                               Eigen::Vector3d{-grid.spacing_.x(), 0.0, 0.0}, changed);
                    }
                    if (j > 0) {
                        update(from, raw_index(i, j - 1, k),
                               Eigen::Vector3d{0.0, -grid.spacing_.y(), 0.0}, changed);
                    }
                    if (k > 0) {
                        update(from, raw_index(i, j, k - 1),
                               Eigen::Vector3d{0.0, 0.0, -grid.spacing_.z()}, changed);
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }
}

double complexity(const MetricGrid& grid) {
    const double cell_volume = grid.spacing_.x() * grid.spacing_.y() * grid.spacing_.z();
    double integral = 0.0;
    for (int k = 0; k + 1 < grid.resolution_.z(); ++k) {
        for (int j = 0; j + 1 < grid.resolution_.y(); ++j) {
            for (int i = 0; i + 1 < grid.resolution_.x(); ++i) {
                const Eigen::Vector3d centre =
                    grid.bbox_min_ + grid.spacing_.cwiseProduct(
                                         Eigen::Vector3d{static_cast<double>(i) + 0.5,
                                                         static_cast<double>(j) + 0.5,
                                                         static_cast<double>(k) + 0.5});
                integral += grid.sample(centre).volume_density() * cell_volume;
            }
        }
    }
    return integral;
}

void normalize_complexity(MetricGrid& grid, double target_complexity, double h_min,
                          double h_max, double max_aspect) {
    if (!(target_complexity > 0.0) || !std::isfinite(target_complexity)) {
        throw std::invalid_argument(
            "normalize_complexity: target complexity must be finite and positive");
    }
    const double current = complexity(grid);
    if (!(current > 0.0) || !std::isfinite(current)) {
        throw std::runtime_error("normalize_complexity: current complexity is not positive finite");
    }
    const double scale = std::exp((2.0 / 3.0) *
                                  (std::log(target_complexity) - std::log(current)));
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        throw std::overflow_error("normalize_complexity: metric scale is not representable");
    }
    for (Metric3d& metric : grid.metrics_) {
        metric = Metric3d(metric.M * scale).clamped(h_min, h_max, max_aspect);
    }
}

} // namespace polymesh::adapt
