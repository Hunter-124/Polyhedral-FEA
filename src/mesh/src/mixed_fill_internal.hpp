// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private phases of mesh::mixed_fill_surface, shared by the mixed_fill*.cpp
// translation units only. Not part of the public mesh API.

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/mixed_fill.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace polymesh::mesh::detail::mixed {

/// Hex8 face loops (local corners), outward for a positively oriented hex.
/// Face f borders the face neighbour at offset kFaceNbr[f].
inline constexpr std::array<std::array<int, 4>, 6> kHexFaces{{
    {{0, 3, 2, 1}},
    {{4, 5, 6, 7}},
    {{0, 1, 5, 4}},
    {{2, 3, 7, 6}},
    {{0, 4, 7, 3}},
    {{1, 2, 6, 5}},
}};

inline constexpr std::array<std::array<int, 3>, 6> kFaceNbr{{
    {{0, 0, -1}},
    {{0, 0, 1}},
    {{0, -1, 0}},
    {{0, 1, 0}},
    {{-1, 0, 0}},
    {{1, 0, 0}},
}};

inline void poll_cancel(const std::function<void()>& cancel_check) {
    if (cancel_check) {
        cancel_check();
    }
}

/// Coarse lattice state shared by the classification, 2:1 closure and
/// emission phases. Per-cell arrays are indexed by `idx(i, j, k)`.
struct MixedLattice {
    FeatureAwareClassification classification;
    double h_budget = 0.0; // min_h_for_cell_budget floor (0 when unconstrained)
    double h_cell = 0.0;   // bulk cell edge, grid.max_edge()
    int nx = 0;
    int ny = 0;
    int nz = 0;
    std::vector<int> dist; // face hops to the free surface; -1 outside
    int max_dist = 0;
    bool size_adaptive = false; // feature/seed/curvature/size-field drivers present
    std::vector<char> is_fine;
    std::vector<char> is_feature_skin;
    std::vector<char> is_seed_skin;
    std::vector<char> is_transition;

    [[nodiscard]] std::size_t idx(int i, int j, int k) const {
        return classification.grid.index(i, j, k);
    }

    [[nodiscard]] bool inb(int i, int j, int k) const {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               classification.inside[idx(i, j, k)];
    }

    [[nodiscard]] bool cell_fine(int i, int j, int k) const {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               classification.inside[idx(i, j, k)] && is_fine[idx(i, j, k)];
    }

    /// Coarse lattice edge starting at node (a,b,c) along `axis` carries a
    /// hanging mid iff any of its four incident cells is fine.
    [[nodiscard]] bool edge_split(int a, int b, int c, int axis) const {
        for (int u = -1; u <= 0; ++u) {
            for (int v = -1; v <= 0; ++v) {
                int ci = a, cj = b, ck = c;
                if (axis == 0) {
                    cj += u;
                    ck += v;
                } else if (axis == 1) {
                    ci += u;
                    ck += v;
                } else {
                    ci += u;
                    cj += v;
                }
                if (cell_fine(ci, cj, ck)) {
                    return true;
                }
            }
        }
        return false;
    }
};

/// Apex-fan cell group (2:1 closure fan or plain-mode skin fan): its private
/// apex node, the corners of the lattice cell it sits in, and the cell range
/// [first, end) it owns at emission time.
struct FanSpan {
    std::uint32_t apex;
    std::array<std::uint32_t, 8> corners;
    std::size_t first;
    std::size_t end;
};

void orient_pyramid_winding(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes);
void normalize_pyramid_diagonal(MixedCell& pyr, const std::vector<Eigen::Vector3d>& nodes);
void emit_cell_pyramids(MixedFillOutput& out, const std::array<std::uint32_t, 8>& c,
                        std::uint32_t apex);
double closed_poly_volume(const std::vector<Eigen::Vector3d>& coords,
                          const std::vector<std::vector<std::uint32_t>>& faces);

/// Classify the coarse lattice (budget-clamped h) and compute face-hop
/// distance to the free surface.
MixedLattice classify_mixed_lattice(const geom::TriSurface& surface,
                                    const Eigen::Vector3d& bbox_min,
                                    const Eigen::Vector3d& bbox_max, double h,
                                    const SizeFieldFn& size_field,
                                    bool local_surface_classification,
                                    const std::function<void()>& cancel_check);

/// Mark fine (h/2) cells from skin, size field, feature/seed/curvature bands
/// and mixed child samples. Sets `lat.size_adaptive` and the field/skin stats.
void mark_fine_cells(MixedLattice& lat, MixedFillOutput& out, const geom::TriSurface& surface,
                     int skin_layers, std::span<const geom::SharpEdge> features,
                     double feature_band, std::span<const Eigen::Vector3d> curvature_seeds,
                     double seed_band, double curvature_turn_deg,
                     const SizeFieldFn& size_field, const std::function<void()>& cancel_check);

/// Close the conforming 2:1 interface (size-adaptive lattices only) and apply
/// the uniform-h/2 budget fallback. Always sizes `lat.is_transition`.
void close_transitions(MixedLattice& lat, MixedFillOutput& out, bool native_poly_transitions,
                       const std::function<void()>& cancel_check);

/// Emit hex / fine-child / transition / skin cells and boundary quads in
/// lattice order. Returns the apex fans for the shell-apex passes.
std::vector<FanSpan> emit_mixed_cells(const MixedLattice& lat, MixedFillOutput& out,
                                      bool native_poly_transitions,
                                      const std::function<void()>& cancel_check);

/// Pre-snap shell apex placement for the product-FE path (ADR-0013).
void place_shell_apexes(MixedFillOutput& out, const std::vector<FanSpan>& fan_spans,
                        const geom::TriSurface& surface,
                        std::span<const geom::SharpEdge> features,
                        bool native_poly_transitions, double h_cell,
                        const std::function<void()>& cancel_check);

/// Snap boundary nodes onto the surface, then re-place fan apexes against the
/// actual snapped bases. Requires non-empty `out.boundary_quads`.
void snap_mixed_boundary(MixedFillOutput& out, const std::vector<FanSpan>& fan_spans,
                         const geom::TriSurface& surface,
                         std::span<const geom::SharpEdge> features, double h_cell,
                         const std::function<void()>& cancel_check);

} // namespace polymesh::mesh::detail::mixed
