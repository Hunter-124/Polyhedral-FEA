// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Multi-element hybrid lattice fill (SPEC zoo, ADR-0012 v3 / H2, ADR-0019).
//
// Cartesian classification by distance-to-boundary (+ optional feature /
// curvature bands):
//   • bulk (deep)          → hex8 at h
//   • feature / seed fine  → 2×2×2 hex8 at h/2 (true size adaptivity)
//   • 2:1 interface        → fan-split pyramids/tets (default) OR one native
//                            polyhedron per coarse cell (native_poly_transitions)
//   • plain free-surface   → pyramid5 skin at h (when no geo drivers), or hex
//                            when native_poly_transitions is set
//
// Product FE path expands remaining hex → pyramids (ADR-0013) so the solve
// mesh is all-pyramid with matching face diagonals (constant-strain exact).
// Native-poly path (ADR-0019 / fe-vem-assembly) keeps hex as FE and emits
// transition cells as kPolyVem for unified mixed FE+VEM assembly.
// Free-surface bases stay quads (no Kuhn diagonals on the silhouette).
// NOT Delaunay / CAD-fitted (ADR-0015).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/cvt_lloyd.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace polymesh::mesh {
/// Established hybrid-work budget. Public so pipeline pre-flight guards and
/// the fill's internal graded-lattice fallback use one ceiling convention.
inline constexpr std::size_t kHybridMaxElems = 48 * 1024;

enum class MixedCellKind : std::uint8_t {
    kHex8 = 0,
    kPyramid5 = 1,
    kTet4 = 2,
    kPolyVem = 3, // unsplit polyhedron; faces + nodes below
};

struct MixedCell {
    MixedCellKind kind = MixedCellKind::kHex8;
    /// Fixed connectivity for hex8 / pyramid5 / tet4 (first n_nodes entries).
    std::array<std::uint32_t, 8> nodes{};
    std::uint8_t n_nodes = 8;
    /// Variable connectivity for kPolyVem (global node ids + local face loops).
    std::vector<std::uint32_t> poly_nodes;
    std::vector<std::vector<std::uint32_t>> poly_faces;
};

/// Private interior apex and its parent lattice cell. Transition/plain-skin
/// fans expose this so a caller that snaps after product expansion can repair
/// the apex against the actual final base positions without moving the wall.
struct MixedMovableFan {
    std::uint32_t apex = 0;
    std::array<std::uint32_t, 8> corners{};
};

struct MixedFillOutput {
    std::vector<Eigen::Vector3d> nodes; // metres
    std::vector<MixedCell> cells;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    /// Subset of `boundary_quads` created by live/void child interfaces that
    /// were not boundary faces in the original coarse-centre classification.
    std::vector<std::array<std::uint32_t, 4>> local_child_boundary_quads;
    std::vector<MixedMovableFan> movable_fans;
    double h = 0.0;      // bulk cell edge (metres)
    double h_fine = 0.0; // fine cell edge when 2:1 active (≈ h/2), else = h
    std::size_t n_hex = 0;
    std::size_t n_pyramid = 0;
    std::size_t n_tet = 0;
    std::size_t n_poly = 0; // native-poly VEM transition (or other) cells
    double boundary_max_distance = 0.0;
    /// Boundary nodes the exact CAD oracle returned no target for — they are not
    /// moved and cannot appear in `boundary_max_distance`, so they must be counted
    /// separately or the fidelity figure hides them.
    std::size_t n_boundary_no_target = 0;
    /// Boundary nodes left further than 0.2 h from the surface.
    std::size_t n_boundary_residual_tail = 0;
    int skin_layers = 0;
    std::size_t n_feature_skin_cells = 0;
    std::size_t n_fine_cells = 0;       // coarse cells refined to 2×2×2
    std::size_t n_transition_cells = 0; // 2:1 interface cells (fan or poly)
    std::size_t n_level0_cells = 0;
    std::size_t n_level1_cells = 0;
    int classification_refinement_levels = 0;
    double classification_volume_error = 0.0;
    /// Observed requested field range at interior cell centroids (metres).
    double field_h_min = 0.0;
    double field_h_max = 0.0;
    std::size_t n_field_budget_clamped = 0;
    bool native_poly_transitions = false;
};

/// Hybrid zoo: hex bulk + optional 2:1 feature fine + pyramid skin/transition.
/// `skin_layers` ≥ 1 (used only when no feature/seed/size-field drivers).
/// The optional scalar size field requests edge length in metres. It selects
/// only L0 or 2×2×2 L1 cells: 4×4×4 needs the 2:1 closure generalised.
/// `curvature_turn_deg` > 0 enables the per-cell turning-angle criterion:
/// cells where the surface turns more than that angle per bulk cell (h·κ)
/// refine to h/2 — contiguous along curved walls, inert on flats.
/// `native_poly_transitions`: when true, each 2:1 transition coarse cell is
/// one unsplit polyhedron (for VEM) instead of a fan of pyramids/tets, and
/// plain free-surface skin stays hex (no pyramid expand required).
/// `cancel_check`, when supplied, is polled inside lattice classification,
/// transition closure, and emission loops and may throw to cancel the fill.
/// `local_surface_classification` enables one h/2 sampling level whose mixed
/// parents alone are subdivided; it is intended for authoritative CAD topology.
MixedFillOutput mixed_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> curvature_seeds = {}, double seed_band = 0.0,
    bool snap_boundary = true, double curvature_turn_deg = 0.0,
    bool native_poly_transitions = false, const std::function<void()>& cancel_check = {},
    const SizeFieldFn& size_field = {}, bool local_surface_classification = false);

/// Expand every hex8 → 6 pyramid5 (centroid apex). Pyramids/tets/polys pass
/// through. Product FE path for hybrid / hexpyr (constant-strain exact).
MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill);

/// Re-place private transition/skin fan apexes against their current bases.
/// Boundary nodes and connectivity are unchanged. Returns the number moved.
std::size_t repair_mixed_fan_apices(MixedFillOutput& fill, double shape_floor);

} // namespace polymesh::mesh
