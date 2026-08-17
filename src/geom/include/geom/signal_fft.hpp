// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Discrete Fourier tools shared by the sizing pipeline (ADR-0034) and the
// boundary feature-pin pass (ADR-0035).
//
// These live in geom rather than adapt because `adapt` links `mesh`, so a mesh
// pass cannot reach an adapt header without a dependency cycle. The transform
// itself is pure numerics with no geometry dependency; `adapt::spectral`
// re-exports these names so existing callers are unchanged.
//
// Everything is double-only and deterministic (no randomness, index-stable
// tie-breaking in the mode ranking).

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::geom {

/// What an energy truncation did.
struct FilterReport {
    std::size_t modes_total = 0;  // modes considered (DC excluded)
    std::size_t modes_kept = 0;   // modes retained by the truncation
    double energy_total = 0.0;    // Σ|F(k)|² over considered modes
    double energy_kept = 0.0;     // energy retained
    double energy_fraction = 0.0; // requested capture fraction
};

/// In-place iterative radix-2 Cooley–Tukey FFT. `a.size()` must be a power of
/// two ≥ 2. inverse=true applies the 1/N-scaled inverse. Throws
/// std::invalid_argument on a non-power-of-two size.
void fft_inplace(std::vector<std::complex<double>>& a, bool inverse);

/// Keep DC plus the smallest dominant-|F(k)| set capturing `energy_fraction`
/// of the non-DC spectral energy; zero the rest. Deterministic: ties break on
/// index. `energy_fraction` is clamped into (0, 1]; non-finite disables the
/// truncation.
FilterReport truncate_modes(std::vector<std::complex<double>>& f, double energy_fraction);

/// Smooth a 1-D signal sampled at `stations` (need not be uniform): resample
/// uniformly, even-reflect to suppress the periodic-wrap discontinuity, FFT,
/// keep the dominant modes capturing `energy_fraction` of spectral energy,
/// inverse, then interpolate back to the original stations. Output size equals
/// input size. Fewer than 3 samples (or non-finite input) returns a copy of
/// `values` with a zeroed report.
std::vector<double> lowpass_signal(std::span<const double> stations,
                                   std::span<const double> values, double energy_fraction,
                                   FilterReport* report = nullptr);

/// Periodic variant for closed chains: no even reflection (the signal already
/// wraps), so a circle's coordinate signal keeps its two true modes instead of
/// gaining reflection harmonics. `stations` must be strictly increasing and
/// `values` is treated as one full period (the caller must NOT repeat the
/// first sample at the end). Same guards as `lowpass_signal`.
std::vector<double> lowpass_signal_periodic(std::span<const double> stations,
                                            std::span<const double> values,
                                            double energy_fraction,
                                            FilterReport* report = nullptr);

} // namespace polymesh::geom
