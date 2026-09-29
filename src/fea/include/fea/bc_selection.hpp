// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Default boundary-condition selection: which boundary nodes/faces a fixture or
// load lands on when the caller names a box or nothing at all (cantilever ends),
// and the energy-conjugate load over that selection. Built on the region and
// face primitives in fea/traction.hpp.

#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace polymesh::fea {

/// cos(~45°): a face counts as aligned with an axis/direction when |n·d| is at
/// least this. Used for end faces (d = ±x) and pressure faces (d = load dir).
inline constexpr double kSelectionNormalMinDot = 0.7;

/// How much load is applied over a selection, and where it points.
struct SurfaceLoadSpec {
    Eigen::Vector3d dir{0.0, 1.0, 0.0}; // unit, default +y (historical CLI load)
    double force = 1000.0;              // total resultant, N (historical default)
    double traction_pa = 0.0;           // pressure magnitude, Pa
    bool traction_mode = false;         // true: traction_pa × area; false: force
};

/// A fixture or load selection on the boundary surface, with its provenance.
struct BcSelection {
    std::vector<std::uint32_t> nodes;
    std::vector<SurfaceFace> faces;
    std::size_t slab_nodes = 0; // what the plain 0.51·h slab captured
    bool face_fallback = false; // slab was degenerate → normal-aligned faces
    double fallback_band = 0.0; // end band as a fraction of the x extent
    bool from_box = false;
    // Set for a box selection: `faces` is then the set the box *touches*, and
    // the load is integrated over their intersection with this region instead of
    // over whole faces. A slab or normal-aligned fallback selection has no
    // region — those are face sets by construction, not a volume of space.
    std::optional<LoadRegion> region;
};

/// Node count a default selection must reach to count as a face rather than a
/// point/edge clamp: max(12, ceil(2% of `n_boundary_nodes`)).
std::size_t sane_selection_minimum(std::size_t n_boundary_nodes);

/// Number of distinct nodes carried by `faces` (the mesh's boundary node count
/// when `faces` is `boundary_surface_faces(mesh)`).
std::size_t count_boundary_nodes(const std::vector<SurfaceFace>& faces);

/// One cantilever end: `end` -1 = min-x (default fixture), +1 = max-x (default
/// load). A `box` wins (boundary nodes inside it, faces touching it); otherwise
/// the x-slab of depth `tol`, widening to ±x-aligned faces in an end band when
/// the slab is below `sane_selection_minimum`. Only boundary nodes are selected.
BcSelection select_cantilever_end(const NodalMesh& mesh,
                                  const std::vector<SurfaceFace>& all_faces,
                                  std::size_t n_boundary_nodes,
                                  const std::optional<LoadRegion>& box, double xmin,
                                  double xmax, double tol, int end);

/// Faces of `box_faces` whose normal satisfies |n·direction| >= the alignment
/// threshold: drops side-wall strips a pressure box grazes. |dot| because
/// mixed-element boundary windings are not uniformly outward. Order preserved.
std::vector<SurfaceFace> pressure_aligned_faces(const NodalMesh& mesh,
                                                const std::vector<SurfaceFace>& box_faces,
                                                const Eigen::Vector3d& direction);

/// Empty when fully fixing `fixed_nodes` removes all six rigid-body modes
/// (>= 3 non-collinear nodes); otherwise a human-readable reason it does not
/// (too few nodes, coincident, or collinear).
std::string constraint_defect(const NodalMesh& mesh,
                              const std::vector<std::uint32_t>& fixed_nodes);

/// Energy-conjugate 3N load for `spec` over `faces` (clipped to `region` when
/// set; `exact_pressure_area` overrides the mesh area for traction), falling back
/// to an even split over `fallback_nodes` when no face has area. Prints one
/// `load:` line to `report`; throws std::runtime_error prefixed `what` on failure.
Eigen::VectorXd
assemble_selection_load(const NodalMesh& mesh, const std::vector<SurfaceFace>& faces,
                        std::span<const std::uint32_t> fallback_nodes,
                        const SurfaceLoadSpec& spec, const char* what, std::FILE* report,
                        const std::optional<LoadRegion>& region,
                        std::optional<double> exact_pressure_area = std::nullopt);

} // namespace polymesh::fea
