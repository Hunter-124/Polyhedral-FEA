// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/spectral_sizing.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace polymesh::adapt::spectral {

namespace {

[[nodiscard]] std::size_t next_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

[[nodiscard]] std::size_t floor_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p <= n / 2) {
        p <<= 1;
    }
    return p;
}

/// energy_fraction lives in (0, 1]; non-finite requests disable truncation.
[[nodiscard]] double clamp_fraction(double f) {
    if (!std::isfinite(f)) {
        return 1.0;
    }
    return std::clamp(f, std::numeric_limits<double>::epsilon(), 1.0);
}

/// Transform every line along `axis` (axis lengths are powers of two, so no
/// padding is ever needed). Length-1 axes are an identity transform.
void transform_axis(std::vector<std::complex<double>>& f, const std::array<int, 3>& dims,
                    int axis, bool inverse) {
    const std::size_t len = static_cast<std::size_t>(dims[axis]);
    if (len == 1) {
        return;
    }
    std::size_t stride = 1;
    for (int a = 0; a < axis; ++a) {
        stride *= static_cast<std::size_t>(dims[a]);
    }
    const std::size_t block = stride * len;
    std::vector<std::complex<double>> line(len);
    for (std::size_t start = 0; start < f.size(); start += block) {
        for (std::size_t off = 0; off < stride; ++off) {
            const std::size_t base = start + off;
            for (std::size_t m = 0; m < len; ++m) {
                line[m] = f[base + m * stride];
            }
            fft_inplace(line, inverse);
            for (std::size_t m = 0; m < len; ++m) {
                f[base + m * stride] = line[m];
            }
        }
    }
}

[[nodiscard]] double median_of(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t mid = v.size() / 2;
    if (v.size() % 2 != 0) {
        return v[mid];
    }
    return 0.5 * (v[mid - 1] + v[mid]);
}

} // namespace

std::size_t Grid3d::index(int i, int j, int k) const {
    return static_cast<std::size_t>(i) +
           static_cast<std::size_t>(dims[0]) *
               (static_cast<std::size_t>(j) +
                static_cast<std::size_t>(dims[1]) * static_cast<std::size_t>(k));
}

double& Grid3d::at(int i, int j, int k) { return values[index(i, j, k)]; }

double Grid3d::at(int i, int j, int k) const { return values[index(i, j, k)]; }

double Grid3d::sample(const Eigen::Vector3d& p) const {
    std::array<int, 3> lo_i{};
    std::array<int, 3> hi_i{};
    std::array<double, 3> t{};
    for (int a = 0; a < 3; ++a) {
        if (dims[a] <= 1) {
            lo_i[a] = 0;
            hi_i[a] = 0;
            t[a] = 0.0;
            continue;
        }
        const double max_coord = static_cast<double>(dims[a] - 1);
        const double coord = std::clamp((p[a] - origin[a]) / spacing[a], 0.0, max_coord);
        const int i = std::min(static_cast<int>(std::floor(coord)), dims[a] - 2);
        lo_i[a] = i;
        hi_i[a] = i + 1;
        t[a] = coord - static_cast<double>(i);
    }
    double result = 0.0;
    for (int dz = 0; dz < 2; ++dz) {
        const double wz = dz != 0 ? t[2] : 1.0 - t[2];
        for (int dy = 0; dy < 2; ++dy) {
            const double wy = dy != 0 ? t[1] : 1.0 - t[1];
            for (int dx = 0; dx < 2; ++dx) {
                const double wx = dx != 0 ? t[0] : 1.0 - t[0];
                result +=
                    wx * wy * wz *
                    at(lo_i[0] + dx * (hi_i[0] - lo_i[0]), lo_i[1] + dy * (hi_i[1] - lo_i[1]),
                       lo_i[2] + dz * (hi_i[2] - lo_i[2]));
            }
        }
    }
    return result;
}

double Grid3d::min_value() const { return *std::min_element(values.begin(), values.end()); }

double Grid3d::max_value() const { return *std::max_element(values.begin(), values.end()); }

Grid3d sample_field_grid(const std::function<double(const Eigen::Vector3d&)>& field,
                         const Eigen::Vector3d& lo, const Eigen::Vector3d& hi,
                         double target_spacing, int max_dim) {
    if (!(target_spacing > 0.0) || !std::isfinite(target_spacing)) {
        throw std::invalid_argument("sample_field_grid: target_spacing must be positive");
    }
    // The grid contract is power-of-two dims; the clamp ceiling snaps down so
    // the invariant survives a non-power-of-two max_dim.
    const std::size_t cap =
        max_dim >= 8 ? floor_pow2(static_cast<std::size_t>(max_dim)) : std::size_t{8};

    Grid3d grid;
    for (int a = 0; a < 3; ++a) {
        if (!std::isfinite(lo[a]) || !std::isfinite(hi[a]) || !(hi[a] >= lo[a])) {
            throw std::invalid_argument("sample_field_grid: need finite lo <= hi");
        }
        const double pad_lo = lo[a] - target_spacing; // one cell each side
        const double pad_hi = hi[a] + target_spacing;
        const double range = pad_hi - pad_lo;
        const double needed = std::ceil(range / target_spacing) + 1.0;
        const std::size_t n =
            std::clamp(next_pow2(static_cast<std::size_t>(needed)), std::size_t{8}, cap);
        grid.dims[a] = static_cast<int>(n);
        grid.origin[a] = pad_lo;
        grid.spacing[a] = range / static_cast<double>(n - 1);
    }

    const std::size_t total = static_cast<std::size_t>(grid.dims[0]) *
                              static_cast<std::size_t>(grid.dims[1]) *
                              static_cast<std::size_t>(grid.dims[2]);
    grid.values.resize(total);
    std::vector<double> finite;
    finite.reserve(total);
    for (int k = 0; k < grid.dims[2]; ++k) {
        for (int j = 0; j < grid.dims[1]; ++j) {
            for (int i = 0; i < grid.dims[0]; ++i) {
                const Eigen::Vector3d p{
                    grid.origin[0] + static_cast<double>(i) * grid.spacing[0],
                    grid.origin[1] + static_cast<double>(j) * grid.spacing[1],
                    grid.origin[2] + static_cast<double>(k) * grid.spacing[2]};
                const double v = field(p);
                grid.at(i, j, k) = v;
                if (std::isfinite(v)) {
                    finite.push_back(v);
                }
            }
        }
    }
    if (finite.empty()) {
        throw std::runtime_error("sample_field_grid: field produced no finite samples");
    }
    const double fill = median_of(std::move(finite));
    for (double& v : grid.values) {
        if (!std::isfinite(v)) {
            v = fill;
        }
    }
    return grid;
}

FilterReport lowpass_grid_energy(Grid3d& grid, double energy_fraction) {
    const double v_min = grid.min_value();
    const double v_max = grid.max_value();
    const double frac = clamp_fraction(energy_fraction);

    std::vector<std::complex<double>> f(grid.values.size());
    for (std::size_t i = 0; i < grid.values.size(); ++i) {
        f[i] = {grid.values[i], 0.0};
    }
    for (int a = 0; a < 3; ++a) {
        transform_axis(f, grid.dims, a, false);
    }
    FilterReport report = truncate_modes(f, frac);
    for (int a = 0; a < 3; ++a) {
        transform_axis(f, grid.dims, a, true);
    }
    for (std::size_t i = 0; i < grid.values.size(); ++i) {
        grid.values[i] = std::clamp(f[i].real(), v_min, v_max);
    }
    return report;
}

double predict_element_count(const Grid3d& h_field) {
    const double vol_cell = h_field.spacing[0] * h_field.spacing[1] * h_field.spacing[2];
    double sum = 0.0;
    for (const double h : h_field.values) {
        if (!std::isfinite(h) || h <= 0.0) {
            continue;
        }
        sum += vol_cell / (h * h * h);
    }
    return sum;
}

BudgetResult enforce_element_budget(Grid3d& h_field, std::size_t max_elems,
                                    double energy_fraction, double h_floor, double h_ceil) {
    BudgetResult result;
    result.predicted_before = predict_element_count(h_field);
    if (max_elems == 0) {
        result.predicted_after = result.predicted_before;
        return result;
    }
    const double budget = static_cast<double>(max_elems);
    result.filter = lowpass_grid_energy(h_field, energy_fraction);
    const double predicted = predict_element_count(h_field);
    if (predicted > budget) {
        result.h_scale = std::pow(predicted / budget, 1.0 / 3.0);
        for (double& v : h_field.values) {
            v *= result.h_scale;
        }
    }
    for (double& v : h_field.values) {
        v = std::clamp(v, h_floor, h_ceil);
    }
    result.predicted_after = predict_element_count(h_field);
    // Quadrature and pow() rounding leave O(1e-15) noise on an exactly-met
    // budget; the slack is far below anything a real violation could hide in.
    result.budget_met = result.predicted_after <= budget * (1.0 + 1e-12);
    return result;
}

double GridSizingField::size_at(const Eigen::Vector3d& point) const {
    return std::clamp(grid_.sample(point), grid_.min_value(), grid_.max_value());
}

} // namespace polymesh::adapt::spectral
