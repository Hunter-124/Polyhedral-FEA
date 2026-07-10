// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// True local h-refine for tet4 meshes (ADR-0016 / ROADMAP D4).
//
// Strategy: Rivara longest-edge bisection (LEB) with longest-edge propagation
// path (LEPP) closure. Every edge that is split is bisected in *all* tets that
// share it, so the output is conforming tet4 with no hanging nodes and no
// multipoint constraints in assembly.
//
// Units: node coordinates in metres.

#include "geom/tri_surface.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// Counters for tests / mesher notes (optional out-parameter).
struct LocalRefineStats {
    std::size_t n_input_tets = 0;
    std::size_t n_output_tets = 0;
    std::size_t n_marked = 0;
    /// Number of parent tet → 2 children replacements.
    std::size_t n_bisections = 0;
    std::size_t n_new_nodes = 0;
    /// Free-surface midpoints projected onto `surface` (S1 curved residual).
    std::size_t n_surface_mids = 0;
};

/// Longest-edge bisection of a pure tet4 mesh.
///
/// @param nodes Vertex positions (metres); copied then extended with midpoints.
/// @param tets  Each entry four node indices, preferably positive orientation.
/// @param marked Element indices into `tets` to refine (duplicates ignored).
/// @param stats Optional statistics.
/// @param surface Optional CAD/STL: free-surface edge midpoints are projected
///        onto the surface instead of Euclidean chords (reduces hole/void
///        residual after LEB). Nullptr = pure geometric mids (default).
/// @return Refined mesh (`boundary_quads` left empty — topology of the lattice
///         skin is no longer valid after interior splits).
/// @throws ValidityError on empty mesh, out-of-range indices, or non-positive
///         parent volumes that cannot be repaired.
TetFillOutput local_refine_tets(std::vector<Eigen::Vector3d> nodes,
                                std::vector<std::array<std::uint32_t, 4>> tets,
                                std::span<const std::size_t> marked,
                                LocalRefineStats* stats = nullptr,
                                const geom::TriSurface* surface = nullptr);

/// Same as above, taking a `TetFillOutput` (nodes + tets). Boundary quads from
/// the input are **not** preserved.
TetFillOutput local_refine_tets(const TetFillOutput& mesh, std::span<const std::size_t> marked,
                                LocalRefineStats* stats = nullptr,
                                const geom::TriSurface* surface = nullptr);

} // namespace polymesh::mesh
