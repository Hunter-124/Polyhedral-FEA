// SPDX-License-Identifier: BSD-3-Clause
#include "geom/signal_fft.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace polymesh::geom {

namespace {

constexpr double kPi = 3.14159265358979323846;

[[nodiscard]] bool is_pow2(std::size_t n) { return n != 0 && (n & (n - 1)) == 0; }

[[nodiscard]] std::size_t next_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
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

[[nodiscard]] double lerp_signal(std::span<const double> stations,
                                 std::span<const double> values, double s) {
    const auto it = std::upper_bound(stations.begin(), stations.end(), s);
    if (it == stations.begin()) {
        return values.front();
    }
    if (it == stations.end()) {
        return values.back();
    }
    const std::size_t hi = static_cast<std::size_t>(it - stations.begin());
    const std::size_t lo = hi - 1;
    const double t = (s - stations[lo]) / (stations[hi] - stations[lo]);
    return values[lo] + t * (values[hi] - values[lo]);
}

/// Shared input guard for both low-pass variants.
[[nodiscard]] bool signal_usable(std::span<const double> stations,
                                 std::span<const double> values) {
    if (stations.size() != values.size() || values.size() < 3) {
        return false;
    }
    if (!(stations.back() > stations.front())) {
        return false;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(stations[i]) || !std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

void fft_inplace(std::vector<std::complex<double>>& a, bool inverse) {
    const std::size_t n = a.size();
    if (n < 2 || !is_pow2(n)) {
        throw std::invalid_argument("fft_inplace: size must be a power of two >= 2");
    }
    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    // Iterative radix-2 Cooley-Tukey. Twiddles via std::polar at full accuracy
    // (no running recurrence): grids are <= 64^3, so speed is a non-issue.
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double base_ang = sign * 2.0 * kPi / static_cast<double>(len);
        const std::size_t half = len / 2;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<double> w =
                    std::polar(1.0, base_ang * static_cast<double>(j));
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
    if (inverse) {
        const double inv_n = 1.0 / static_cast<double>(n);
        for (auto& x : a) {
            x *= inv_n;
        }
    }
}

FilterReport truncate_modes(std::vector<std::complex<double>>& f, double energy_fraction) {
    const double frac = clamp_fraction(energy_fraction);
    FilterReport report;
    report.energy_fraction = frac;
    if (f.size() < 2) {
        return report;
    }
    report.modes_total = f.size() - 1; // DC excluded
    std::vector<double> energy(f.size());
    for (std::size_t k = 0; k < f.size(); ++k) {
        energy[k] = std::norm(f[k]);
    }
    for (std::size_t k = 1; k < f.size(); ++k) {
        report.energy_total += energy[k];
    }
    std::vector<std::size_t> order(f.size() - 1);
    std::iota(order.begin(), order.end(), std::size_t{1});
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return energy[a] > energy[b]; });
    std::vector<bool> keep(f.size(), false);
    keep[0] = true; // DC is always kept verbatim
    const double target = frac * report.energy_total;
    for (std::size_t idx : order) {
        if (report.energy_kept >= target) {
            break;
        }
        keep[idx] = true;
        report.energy_kept += energy[idx];
        ++report.modes_kept;
    }
    for (std::size_t k = 0; k < f.size(); ++k) {
        if (!keep[k]) {
            f[k] = std::complex<double>{0.0, 0.0};
        }
    }
    return report;
}

std::vector<double> lowpass_signal(std::span<const double> stations,
                                   std::span<const double> values, double energy_fraction,
                                   FilterReport* report) {
    if (!signal_usable(stations, values)) {
        if (report != nullptr) {
            *report = FilterReport{};
        }
        return {values.begin(), values.end()};
    }

    const double frac = clamp_fraction(energy_fraction);
    const double s0 = stations.front();
    const double s1 = stations.back();

    // Uniform resample to n = max(8, next_pow2(count)) nodes.
    const std::size_t n = std::max<std::size_t>(8, next_pow2(values.size()));
    std::vector<double> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t =
            s0 + (s1 - s0) * (static_cast<double>(i) / static_cast<double>(n - 1));
        x[i] = lerp_signal(stations, values, t);
    }

    // Even reflection to length 2n suppresses the periodic-wrap discontinuity.
    std::vector<std::complex<double>> f(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        f[i] = {x[i], 0.0};
        f[2 * n - 1 - i] = {x[i], 0.0};
    }
    fft_inplace(f, false);
    const FilterReport local = truncate_modes(f, frac);
    fft_inplace(f, true);

    // Back to the n resample stations, then onto the original stations.
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        y[i] = f[i].real();
    }
    std::vector<double> out(values.size());
    const double scale = static_cast<double>(n - 1) / (s1 - s0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double u = (stations[i] - s0) * scale;
        const std::size_t i0 =
            std::min<std::size_t>(static_cast<std::size_t>(std::floor(u)), n - 2);
        const double t = u - static_cast<double>(i0);
        out[i] = y[i0] + t * (y[i0 + 1] - y[i0]);
    }
    if (report != nullptr) {
        *report = local;
    }
    return out;
}

std::vector<double> lowpass_signal_periodic(std::span<const double> stations,
                                            std::span<const double> values,
                                            double energy_fraction, FilterReport* report) {
    if (!signal_usable(stations, values)) {
        if (report != nullptr) {
            *report = FilterReport{};
        }
        return {values.begin(), values.end()};
    }

    const double frac = clamp_fraction(energy_fraction);
    const double s0 = stations.front();
    // One full period spans past the last station back to the first. The
    // caller passes one period without repeating the wrap sample, so the
    // period length is extrapolated from the mean station spacing.
    const double span = stations.back() - s0;
    const double period =
        span * static_cast<double>(values.size()) / static_cast<double>(values.size() - 1);

    const std::size_t n = std::max<std::size_t>(8, next_pow2(values.size()));
    std::vector<std::complex<double>> f(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = s0 + period * (static_cast<double>(i) / static_cast<double>(n));
        // Wrap-aware lookup: stations beyond the last sample interpolate back
        // to the first, which is what makes this a true periodic transform.
        double v = 0.0;
        if (t <= stations.back()) {
            v = lerp_signal(stations, values, t);
        } else {
            const double u = (t - stations.back()) / (s0 + period - stations.back());
            v = values.back() + u * (values.front() - values.back());
        }
        f[i] = {v, 0.0};
    }
    fft_inplace(f, false);
    const FilterReport local = truncate_modes(f, frac);
    fft_inplace(f, true);

    std::vector<double> out(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const double u = (stations[i] - s0) / period * static_cast<double>(n);
        const std::size_t i0 = static_cast<std::size_t>(std::floor(u)) % n;
        const std::size_t i1 = (i0 + 1) % n;
        const double t = u - std::floor(u);
        out[i] = f[i0].real() + t * (f[i1].real() - f[i0].real());
    }
    if (report != nullptr) {
        *report = local;
    }
    return out;
}

} // namespace polymesh::geom
