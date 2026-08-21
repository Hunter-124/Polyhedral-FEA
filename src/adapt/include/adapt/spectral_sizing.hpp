// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Spectral (Fourier) mesh-sizing tools (ADR-0034).
//
// Three honest uses of the discrete Fourier transform in the sizing pipeline:
//
//   1. Signal denoise. Curvature sampled along CAD edges (OCC BRepLProp) is
//      corrupted by parameterization noise; an energy-truncated inverse FFT
//      recovers the smooth κ(s) so edge size sources follow the true curve
//      instead of the noise (chordal sag d = ℓ²κ/8 ⇒ spurious κ spikes emit
//      spurious fine seeds).
//
//   2. Field trimming. A sizing field h(x) sampled on a Cartesian grid has
//      density ρ(x) = 1/h³ (the CVT density contract, mesh/cvt_lloyd.hpp).
//      Modes of F[ρ] carry element-mass in proportion to their energy; an
//      energy-fraction truncation merges insignificant fine bands (noise,
//      sub-seed oscillation) into the surrounding coarse field while modes
//      that carry real element mass are kept verbatim.
//
//   3. Budget targeting. Element count is the complexity integral
//      N ≈ Σ_cells vol/h³. Budgets in the rest of the pipeline are ceilings;
//      here the cap is a *target*: truncate first, then one uniform scale on h
//      lands the prediction on the budget (within grid quadrature error).
//
// Everything is double-only and deterministic (fixed grids, no randomness).

#include "adapt/sizing_field.hpp"
#include "geom/signal_fft.hpp"

#include <Eigen/Core>

#include <array>
#include <complex>
#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace polymesh::adapt::spectral {

// Fourier primitives live in geom (`adapt` links `mesh`, so mesh passes that
// need the transform cannot include an adapt header). Re-exported here so the
// sizing pipeline keeps one spelling.
using geom::fft_inplace;
using geom::FilterReport;
using geom::lowpass_signal;
using geom::lowpass_signal_periodic;
using geom::truncate_modes;

/// Cartesian scalar-field grid, x-fastest. Dims are powers of two so the
/// separable 3-D FFT never needs padding.
struct Grid3d {
    std::array<int, 3> dims{2, 2, 2};
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();  // node (0,0,0)
    Eigen::Vector3d spacing = Eigen::Vector3d::Ones(); // per-axis cell size
    std::vector<double> values;                        // dims product

    [[nodiscard]] std::size_t index(int i, int j, int k) const;
    [[nodiscard]] double& at(int i, int j, int k);
    [[nodiscard]] double at(int i, int j, int k) const;
    /// Trilinear sample; clamped (constant) outside the grid.
    [[nodiscard]] double sample(const Eigen::Vector3d& p) const;
    [[nodiscard]] double min_value() const;
    [[nodiscard]] double max_value() const;
};

/// Sample `field` over [lo, hi] (padded one cell on each side). Per-axis node
/// count is the smallest power of two whose spacing is ≤ `target_spacing`,
/// clamped to [8, max_dim]. Non-finite samples are replaced by the grid median
/// of finite samples (a sizing field must be total; the median is neutral).
Grid3d sample_field_grid(const std::function<double(const Eigen::Vector3d&)>& field,
                         const Eigen::Vector3d& lo, const Eigen::Vector3d& hi,
                         double target_spacing, int max_dim = 64);

/// In-place energy truncation of a grid field: forward separable FFT, keep the
/// smallest set of dominant modes capturing `energy_fraction` of the spectral
/// energy, inverse. Values are afterwards clamped into the grid's original
/// [min, max] range so smoothing can neither invent refinement below the input
/// floor nor coarsen past its ceiling. energy_fraction is clamped to (0, 1].
FilterReport lowpass_grid_energy(Grid3d& grid, double energy_fraction);

/// Predicted element count of a size field: Σ_nodes vol_cell / h(x)³ with
/// vol_cell = Π spacing. Non-positive or non-finite h nodes are skipped (they
/// contribute nothing meaningful to a density integral).
double predict_element_count(const Grid3d& h_field);

struct BudgetResult {
    FilterReport filter;
    double h_scale = 1.0;          // uniform h multiplier applied after truncation
    double predicted_before = 0.0; // Σ vol/h³ on entry
    double predicted_after = 0.0;  // Σ vol/h³ on exit
    bool budget_met = true;        // predicted_after ≤ max_elems (or no budget)
};

/// Turn an element ceiling into a target. With max_elems = 0 this is a no-op.
/// Otherwise: (1) truncate the grid spectrally at `energy_fraction`; (2) if the
/// prediction still exceeds the budget, scale h uniformly by
/// (predicted/max_elems)^{1/3} — exact for the Σvol/h³ model. After both steps
/// values are clamped to [h_floor, h_ceil] (callers pass the pre-filter grid
/// extremes so the budget can never refine into or coarsen out of the input
/// range); a clamp that pushes the prediction back over budget sets
/// budget_met=false rather than hiding it.
BudgetResult enforce_element_budget(Grid3d& h_field, std::size_t max_elems,
                                    double energy_fraction, double h_floor, double h_ceil);

/// SizingField adapter over a filtered grid: trilinear sample clamped to the
/// grid's value range, constant outside the grid box.
class GridSizingField final : public SizingField {
  public:
    explicit GridSizingField(Grid3d grid) : grid_(std::move(grid)) {}
    double size_at(const Eigen::Vector3d& point) const override;
    [[nodiscard]] const Grid3d& grid() const { return grid_; }

  private:
    Grid3d grid_;
};

} // namespace polymesh::adapt::spectral
