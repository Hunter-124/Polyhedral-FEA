// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Graded tet fill: multi-level LEB size field (ADR-0018).
//
// Coarse-primary Kuhn lattice at target spacing h. Cells marked L1 (features /
// size field / thick free-surface skin) get one LEB pass (~h/2); L2 (smaller
// field targets, high-κ seeds, and feature core) get a second (~h/4). Thin
// plates skip free-surface hop flood so grading is feature-driven.
// Face-conforming via LEPP. Grid-based, not Delaunay (ADR-0015).

#include "geom/features.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/mirror.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <functional>
#include <span>
#include <string>

namespace polymesh::mesh {

/// Synchronous graded-fill work observation. Counters describe completed/total
/// work units in sub_phase: lattice cells, then nodes/tets/candidates. Zero
/// before work exists is intentional; elements are always live tetrahedra.
struct FillProgress {
    std::size_t cells_done = 0;
    std::size_t cells_total = 0;
    std::size_t elements_so_far = 0;
    std::string sub_phase;
};

struct FillOptions {
    /// Empty disables observation without clock reads or progress allocations.
    /// Called on phase changes and at most five seconds apart in working loops.
    /// Individual synchronous CAD/kernel calls cannot be interrupted.
    std::function<void(const FillProgress&)> on_progress;
};

/// Refinement stopped before another allocation-heavy wave. The measured
/// count lets callers with an automatic size budget retry at a coarser h.
class RefinementLimitError : public ValidityError {
  public:
    RefinementLimitError(std::size_t actual, std::size_t ceiling)
        : ValidityError("graded_tet_fill_surface: memory-derived refinement ceiling exceeded"),
          elements(actual), limit(ceiling) {}
    const std::size_t elements;
    const std::size_t limit;
};

struct GradedTetFillOutput {
    TetFillOutput mesh;    // nodes + tets + boundary quads
    double h_coarse = 0.0; // metres (~ target h when budget allows)
    double h_fine = 0.0;   // metres (~ h_coarse/4 at deepest L2)
    std::size_t n_coarse_cells = 0;
    std::size_t n_fine_cells = 0; // coarse cells marked L1 or L2
    int skin_layers = 0;
    /// Max LEB depth (2 → L2 ≈ h/4).
    int subdivision = 2;
    /// Coarse cells forced fine by feature band.
    std::size_t n_feature_cells = 0;
    /// Coarse cells forced fine by a posteriori / geometry seeds (L2).
    std::size_t n_seed_cells = 0;
    /// Coarse-cell counts after all field/feature/seed marks are fused.
    std::size_t n_level0_cells = 0;
    std::size_t n_level1_cells = 0;
    std::size_t n_level2_cells = 0; // level 2 or deeper protected feature core
    int classification_refinement_levels = 0;
    double classification_volume_error = 0.0;
    /// Observed requested field range at interior cell centroids (metres).
    double field_h_min = 0.0;
    double field_h_max = 0.0;
    std::size_t n_field_budget_clamped = 0;
};

/// Multi-level graded fill. `skin_layers` free-surface hops (skipped on thin
/// parts). Feature/seed bands union with the optional scalar size field; field
/// values are desired edge lengths in metres, independent of the background
/// allocation floor. `max_refinement_tets` is the caller's memory-derived cap (0: uncapped).
/// `curvature_turn_deg` > 0 enables the per-cell turning-angle criterion:
/// cells where the surface turns more than that angle per bulk cell (h·κ)
/// marks L1; more than twice it marks L2 — contiguous, inert on flats.
/// `fit` carries the exact BRep oracle plus the topology used to hard-pin
/// sharp edges and CAD vertices (ADR-0035); null keeps the tessellated path.
/// `mirror` carries the verified reflection symmetry of the geometry
/// (mesh/mirror.hpp): the classification, every cell mark and every geometry
/// query then answer identically for a cell and its mirror image, which is what
/// makes the delivered element pattern mirror-symmetric on a symmetric part.
/// Null leaves every decision on its raw tessellated input.
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double curvature_turn_deg = 0.0, const BoundaryFit* fit = nullptr,
    const SizeFieldFn& size_field = {}, const MirrorFrame* mirror = nullptr,
    std::size_t max_refinement_tets = 0, const FillOptions& options = {});

} // namespace polymesh::mesh
