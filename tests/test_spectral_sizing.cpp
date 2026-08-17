// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/spectral_sizing.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <stdexcept>
#include <vector>

using Eigen::Vector3d;
namespace spectral = polymesh::adapt::spectral;

namespace {

constexpr double kPi = 3.14159265358979323846;

// h = 0.05 + 0.45 * prod_a sin^2(pi * i_a / 16): a smooth bump whose spectrum
// is exactly 27 modes (each axis contributes bins {0, 1, 15}), so a 99.9%
// energy truncation is an identity on it.
spectral::Grid3d make_bump_grid() {
    spectral::Grid3d g;
    g.dims = {16, 16, 16};
    g.origin = Vector3d::Zero();
    g.spacing = Vector3d::Constant(1.0 / 15.0);
    g.values.resize(16 * 16 * 16);
    for (int k = 0; k < 16; ++k) {
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i < 16; ++i) {
                const double bx = std::sin(kPi * static_cast<double>(i) / 16.0);
                const double by = std::sin(kPi * static_cast<double>(j) / 16.0);
                const double bz = std::sin(kPi * static_cast<double>(k) / 16.0);
                g.at(i, j, k) = 0.05 + 0.45 * bx * bx * by * by * bz * bz;
            }
        }
    }
    return g;
}

// h = 0.1 + 0.05 * x_hat + a * checkerboard. The ramp is a pure axis ramp
// (15 non-DC modes); the checkerboard is exactly the (8,8,8) Nyquist mode.
spectral::Grid3d make_ripple_grid(double amplitude) {
    spectral::Grid3d g;
    g.dims = {16, 16, 16};
    g.origin = Vector3d::Zero();
    g.spacing = Vector3d::Constant(1.0 / 15.0);
    g.values.resize(16 * 16 * 16);
    for (int k = 0; k < 16; ++k) {
        for (int j = 0; j < 16; ++j) {
            for (int i = 0; i < 16; ++i) {
                const double checker = ((i + j + k) % 2 == 0) ? 1.0 : -1.0;
                g.at(i, j, k) =
                    0.1 + 0.05 * (static_cast<double>(i) / 15.0) + amplitude * checker;
            }
        }
    }
    return g;
}

double checker_projection(const spectral::Grid3d& g) {
    double acc = 0.0;
    for (int k = 0; k < g.dims[2]; ++k) {
        for (int j = 0; j < g.dims[1]; ++j) {
            for (int i = 0; i < g.dims[0]; ++i) {
                const double c = ((i + j + k) % 2 == 0) ? 1.0 : -1.0;
                acc += c * g.at(i, j, k);
            }
        }
    }
    return std::abs(acc) / static_cast<double>(g.values.size());
}

double grid_mean(const spectral::Grid3d& g) {
    double acc = 0.0;
    for (const double v : g.values) {
        acc += v;
    }
    return acc / static_cast<double>(g.values.size());
}

} // namespace

TEST_CASE("fft of a delta is flat", "[spectral]") {
    std::vector<std::complex<double>> a(16, {0.0, 0.0});
    a[0] = {1.0, 0.0};
    spectral::fft_inplace(a, false);
    for (const auto& v : a) {
        CHECK(std::abs(v) == Catch::Approx(1.0).margin(1e-12));
    }
}

TEST_CASE("fft of a single cosine mode has two half-height spikes", "[spectral]") {
    constexpr std::size_t n = 16;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = {std::cos(2.0 * kPi * 2.0 * static_cast<double>(i) / static_cast<double>(n)),
                0.0};
    }
    spectral::fft_inplace(a, false);
    for (std::size_t k = 0; k < n; ++k) {
        const double expect = (k == 2 || k == n - 2) ? 0.5 * static_cast<double>(n) : 0.0;
        CHECK(std::abs(a[k]) == Catch::Approx(expect).margin(1e-9));
    }
}

TEST_CASE("fft inverse round-trips and parseval holds", "[spectral]") {
    constexpr std::size_t n = 16;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i);
        a[i] = {0.03 * x * x - 0.5 * x + 0.25, 0.1 * std::sin(0.7 * x)};
    }
    const std::vector<std::complex<double>> original = a;
    double norm_x = 0.0;
    for (const auto& v : a) {
        norm_x += std::norm(v);
    }
    spectral::fft_inplace(a, false);
    double norm_f = 0.0;
    for (const auto& v : a) {
        norm_f += std::norm(v);
    }
    CHECK(norm_f / static_cast<double>(n) == Catch::Approx(norm_x).epsilon(1e-12));
    spectral::fft_inplace(a, true);
    double max_err = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        max_err = std::max(max_err, std::abs(a[i] - original[i]));
    }
    CHECK(max_err < 1e-12);
}

TEST_CASE("fft rejects non power-of-two sizes", "[spectral]") {
    std::vector<std::complex<double>> bad3(3);
    std::vector<std::complex<double>> bad12(12);
    std::vector<std::complex<double>> bad1(1);
    std::vector<std::complex<double>> bad0;
    CHECK_THROWS_AS(spectral::fft_inplace(bad3, false), std::invalid_argument);
    CHECK_THROWS_AS(spectral::fft_inplace(bad12, true), std::invalid_argument);
    CHECK_THROWS_AS(spectral::fft_inplace(bad1, false), std::invalid_argument);
    CHECK_THROWS_AS(spectral::fft_inplace(bad0, false), std::invalid_argument);
}

TEST_CASE("lowpass_signal guards copy the input and zero the report", "[spectral]") {
    const std::vector<double> s2{0.0, 1.0};
    const std::vector<double> v2{3.0, 4.0};
    spectral::FilterReport report;
    const auto out2 = spectral::lowpass_signal(s2, v2, 0.999, &report);
    CHECK(out2 == v2);
    CHECK(report.modes_total == 0);
    CHECK(report.modes_kept == 0);
    CHECK(report.energy_total == 0.0);

    const std::vector<double> s4{0.0, 1.0, 2.0, 3.0};
    const std::vector<double> vnan{1.0, std::nan(""), 2.0, 3.0};
    const auto out_nan = spectral::lowpass_signal(s4, vnan, 0.999, &report);
    CHECK(out_nan.size() == vnan.size());
    CHECK(report.modes_total == 0);
}

TEST_CASE("lowpass_signal strips a weak high-frequency ripple", "[spectral]") {
    // Stations at cell midpoints so both cosines land exactly on reflected
    // bins: smooth = bins {8,120}, ripple = bins {50,78} of the length-128
    // even extension. The ripple carries (0.005/0.3)^2 = 2.8e-4 of the AC
    // energy, below the 1e-3 tail a 99.9% truncation may drop, so an honest
    // dominant-mode truncation removes it.
    constexpr std::size_t m = 64;
    std::vector<double> stations(m), values(m), smooth(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double s = 2.0 * kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(m);
        stations[i] = s;
        smooth[i] = 1.0 + 0.3 * std::cos(8.0 * s);
        values[i] = smooth[i] + 0.005 * std::cos(25.0 * s);
    }
    spectral::FilterReport report;
    const auto out = spectral::lowpass_signal(stations, values, 0.999, &report);
    REQUIRE(out.size() == m);
    double err_in = 0.0;
    double err_out = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
        err_in = std::max(err_in, std::abs(values[i] - smooth[i]));
        err_out = std::max(err_out, std::abs(out[i] - smooth[i]));
    }
    CHECK(err_in == Catch::Approx(0.005).epsilon(0.01));
    CHECK(err_out < 1e-9);
    CHECK(err_out * 20.0 < err_in);   // at least 20x closer to the smooth part
    CHECK(report.modes_total == 127); // reflected length 128, DC excluded
    CHECK(report.modes_kept >= 2);
    CHECK(report.modes_kept < report.modes_total);
    CHECK(report.energy_kept >= 0.999 * report.energy_total * (1.0 - 1e-12));
}

TEST_CASE("grid3d indexes x-fastest and samples trilinearly", "[spectral]") {
    spectral::Grid3d g;
    g.dims = {4, 2, 8};
    g.origin = Vector3d::Zero();
    g.spacing = Vector3d::Ones();
    g.values.resize(4 * 2 * 8, 0.0);
    CHECK(g.index(1, 0, 0) == 1);
    CHECK(g.index(0, 1, 0) == 4);
    CHECK(g.index(0, 0, 1) == 8);
    g.at(3, 1, 7) = 42.0;
    CHECK(g.at(3, 1, 7) == 42.0);
    CHECK(g.max_value() == 42.0);
    CHECK(g.min_value() == 0.0);

    spectral::Grid3d c;
    c.dims = {2, 2, 2};
    c.origin = Vector3d::Zero();
    c.spacing = Vector3d::Ones();
    c.values.resize(8);
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                c.at(i, j, k) = static_cast<double>(i + 2 * j + 4 * k);
            }
        }
    }
    CHECK(c.sample(Vector3d{0.5, 0.5, 0.5}) == Catch::Approx(3.5).margin(1e-15));
    CHECK(c.sample(Vector3d{1.0, 1.0, 1.0}) == Catch::Approx(7.0).margin(1e-15));
    // Constant outside: clamps to the nearest face.
    CHECK(c.sample(Vector3d{5.0, 0.5, 0.5}) == Catch::Approx(4.0).margin(1e-15));
    CHECK(c.sample(Vector3d{-3.0, 0.5, 0.5}) == Catch::Approx(3.0).margin(1e-15));
}

TEST_CASE("sample_field_grid pads, snaps to pow2, and median-fills", "[spectral]") {
    const auto field = [](const Vector3d& p) { return p.x() < 0.0 ? std::nan("") : 2.5; };
    const spectral::Grid3d g = spectral::sample_field_grid(field, Vector3d{0.0, 0.0, 0.0},
                                                           Vector3d{1.0, 1.0, 1.0}, 0.3);
    for (int a = 0; a < 3; ++a) {
        CHECK(g.dims[a] == 8); // ceil(1.6/0.3)+1 = 7 -> next pow2 = 8
        CHECK(g.spacing[a] <= 0.3);
        CHECK(g.origin[a] == Catch::Approx(-0.3));
    }
    // Every NaN sample (x < 0 covers some padded nodes) is the median 2.5.
    for (const double v : g.values) {
        CHECK(v == 2.5);
    }
    const auto all_nan = [](const Vector3d&) { return std::nan(""); };
    CHECK_THROWS_AS(
        spectral::sample_field_grid(all_nan, Vector3d::Zero(), Vector3d::Ones(), 0.3),
        std::runtime_error);
    const auto one = [](const Vector3d&) { return 1.0; };
    CHECK_THROWS_AS(spectral::sample_field_grid(one, Vector3d::Zero(), Vector3d::Ones(), 0.0),
                    std::invalid_argument);
}

TEST_CASE("lowpass_grid_energy strips a checkerboard ripple", "[spectral]") {
    // The ripple carries (2e-4)^2 / var(ramp) of the AC energy: ~1.7e-5,
    // far below the 1e-3 tail of a 99.9% truncation, so the honest dominant-
    // mode selection drops the Nyquist mode and keeps all 15 ramp modes.
    spectral::Grid3d g = make_ripple_grid(0.0002);
    const double proj_in = checker_projection(g);
    const double mean_in = grid_mean(g);
    const double min_in = g.min_value();
    const double max_in = g.max_value();
    CHECK(proj_in == Catch::Approx(0.0002).epsilon(1e-9));

    const spectral::FilterReport report = spectral::lowpass_grid_energy(g, 0.999);
    const double proj_out = checker_projection(g);
    CHECK(proj_out * 5.0 <= proj_in); // ripple shrinks at least 5x
    CHECK(std::abs(grid_mean(g) - mean_in) <= 0.05 * mean_in);
    CHECK(g.min_value() >= min_in - 1e-15);
    CHECK(g.max_value() <= max_in + 1e-15);
    CHECK(report.modes_total == 4095);
    CHECK(report.modes_kept < report.modes_total);
    CHECK(report.energy_kept >= 0.999 * report.energy_total * (1.0 - 1e-12));
}

TEST_CASE("predict_element_count integrates vol over h cubed", "[spectral]") {
    spectral::Grid3d g;
    g.dims = {8, 8, 8};
    g.origin = Vector3d::Zero();
    g.spacing = Vector3d::Constant(0.25);
    g.values.assign(8 * 8 * 8, 0.5);
    CHECK(spectral::predict_element_count(g) == Catch::Approx(64.0).margin(1e-9));
    g.at(0, 0, 0) = -1.0;         // skipped: non-positive
    g.at(1, 0, 0) = std::nan(""); // skipped: non-finite
    CHECK(spectral::predict_element_count(g) == Catch::Approx(510.0 * 0.125).margin(1e-9));
}

TEST_CASE("enforce_element_budget with zero budget is a no-op", "[spectral]") {
    spectral::Grid3d g = make_bump_grid();
    const std::vector<double> before_values = g.values;
    const spectral::BudgetResult r = spectral::enforce_element_budget(g, 0, 0.999, 0.05, 0.5);
    CHECK(g.values == before_values);
    CHECK(r.budget_met);
    CHECK(r.h_scale == 1.0);
    CHECK(r.predicted_before > 0.0);
    CHECK(r.predicted_after == r.predicted_before);
    CHECK(r.filter.modes_total == 0);
}

TEST_CASE("enforce_element_budget scales h onto the target", "[spectral]") {
    // Loose clamp: scaling is exact for the vol/h^3 model.
    {
        spectral::Grid3d g = make_bump_grid();
        const double floor = 0.5 * g.min_value();
        const double ceil = 2.0 * g.max_value();
        const double before = spectral::predict_element_count(g);
        const auto max_elems = static_cast<std::size_t>(before / 2.0);
        const spectral::BudgetResult r =
            spectral::enforce_element_budget(g, max_elems, 0.999, floor, ceil);
        const double expected_scale =
            std::pow(r.predicted_before / static_cast<double>(max_elems), 1.0 / 3.0);
        CHECK(r.h_scale == Catch::Approx(expected_scale).epsilon(0.01));
        CHECK(r.predicted_after <= static_cast<double>(max_elems) * (1.0 + 1e-9));
        CHECK(r.budget_met);
        CHECK(g.min_value() >= floor);
        CHECK(g.max_value() <= ceil);
    }
    // Over-tight clamp (the pre-filter extremes): scaling past h_ceil is
    // clamped, the prediction lands back over budget, and budget_met says so.
    {
        spectral::Grid3d g = make_bump_grid();
        const double floor = g.min_value();
        const double ceil = g.max_value();
        const double before = spectral::predict_element_count(g);
        const auto max_elems = static_cast<std::size_t>(before / 64.0);
        const spectral::BudgetResult r =
            spectral::enforce_element_budget(g, max_elems, 0.999, floor, ceil);
        const double expected_scale =
            std::pow(r.predicted_before / static_cast<double>(max_elems), 1.0 / 3.0);
        CHECK(r.h_scale == Catch::Approx(expected_scale).epsilon(0.01));
        CHECK(g.max_value() == Catch::Approx(ceil).margin(1e-15)); // clamp engaged
        CHECK(r.predicted_after > static_cast<double>(max_elems));
        CHECK(!r.budget_met);
    }
}

TEST_CASE("spectral filtering is bit-identical across runs", "[spectral]") {
    spectral::Grid3d g1 = make_bump_grid();
    spectral::Grid3d g2 = make_bump_grid();
    const auto max_elems = static_cast<std::size_t>(spectral::predict_element_count(g1) / 2.0);
    const spectral::BudgetResult r1 =
        spectral::enforce_element_budget(g1, max_elems, 0.999, 0.01, 2.0);
    const spectral::BudgetResult r2 =
        spectral::enforce_element_budget(g2, max_elems, 0.999, 0.01, 2.0);
    CHECK(g1.values == g2.values);
    CHECK(r1.h_scale == r2.h_scale);
    CHECK(r1.predicted_after == r2.predicted_after);

    const std::vector<double> s{0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
    const std::vector<double> v{1.0, 1.3, 0.8, 1.1, 1.0, 0.9, 1.2, 1.05};
    CHECK(spectral::lowpass_signal(s, v, 0.99) == spectral::lowpass_signal(s, v, 0.99));
}

TEST_CASE("grid sizing field samples the grid and clamps to its range", "[spectral]") {
    spectral::Grid3d g;
    g.dims = {2, 2, 2};
    g.origin = Vector3d::Zero();
    g.spacing = Vector3d::Ones();
    g.values.resize(8);
    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                g.at(i, j, k) = static_cast<double>(i + 2 * j + 4 * k);
            }
        }
    }
    const spectral::GridSizingField field(g);
    CHECK(field.size_at(Vector3d{0.5, 0.5, 0.5}) ==
          Catch::Approx(g.sample(Vector3d{0.5, 0.5, 0.5})).margin(1e-15));
    CHECK(field.size_at(Vector3d{0.25, 0.5, 0.75}) ==
          Catch::Approx(g.sample(Vector3d{0.25, 0.5, 0.75})).margin(1e-15));
    // Outside the box: nearest-face value, still inside [min, max].
    CHECK(field.size_at(Vector3d{5.0, 0.5, 0.5}) == Catch::Approx(4.0).margin(1e-15));
    CHECK(field.size_at(Vector3d{-3.0, 0.5, 0.5}) == Catch::Approx(3.0).margin(1e-15));
    CHECK(field.size_at(Vector3d{100.0, 100.0, 100.0}) <= g.max_value());
    CHECK(field.size_at(Vector3d{-100.0, -100.0, -100.0}) >= g.min_value());
}
