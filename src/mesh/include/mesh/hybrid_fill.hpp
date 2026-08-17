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
#include "mesh/tet_fill.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Core>

#include <span>

namespace polymesh::mesh {

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
/// values are desired edge lengths in metres and are clamped at the lattice
/// cell-budget floor before deriving L0/L1/L2.
/// `curvature_turn_deg` > 0 enables the per-cell turning-angle criterion:
/// cells where the surface turns more than that angle per bulk cell (h·κ)
/// marks L1; more than twice it marks L2 — contiguous, inert on flats.
/// `fit` carries the exact BRep oracle plus the topology used to hard-pin
/// sharp edges and CAD vertices (ADR-0035); null keeps the tessellated path.
GradedTetFillOutput graded_tet_fill_surface(
    const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
    const Eigen::Vector3d& bbox_max, double h, int skin_layers = 2,
    std::span<const geom::SharpEdge> features = {}, double feature_band = 0.0,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double curvature_turn_deg = 0.0, const BoundaryFit* fit = nullptr,
    const SizeFieldFn& size_field = {});

} // namespace polymesh::mesh
