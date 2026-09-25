// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Helpers shared between the pipeline translation units. Private to
// src/pipeline/src; not part of the public `pipeline/` include tree.

#include "pipeline/scene.hpp"

#include "adapt/graded_sizing.hpp"
#include "fea/nodal_mesh.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/mirror.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace polymesh::pipeline::detail {

// --- a-priori sizing (refinement_plan.cpp); also used by SolveJob ----------

/// Finest-wins spatial decimation: keep one source (min h) per cubic cell of
/// side `cell`. Preserves the size field (where the mesh must be fine) while
/// capping seed count, so the gradient limiter and ball-grading meshers do not
/// choke on ~1 seed per surface vertex (tens of thousands on a real CAD part).
///
/// Buckets are anchored on the source set's own bbox centre, not on world zero.
/// A world-anchored lattice puts an arbitrary cell wall somewhere in the part, so
/// a source and its exact mirror can fall in cells of different widths relative
/// to the symmetry plane and one of the pair survives decimation alone. The
/// centre-anchored partition maps onto itself under reflection about any bbox
/// mid-plane, and min-h is commutative, so the surviving set is mirror-symmetric
/// whenever the input is.
std::vector<adapt::SizeSource> decimate_sources(std::vector<adapt::SizeSource> src,
                                                double cell);

/// BRep topology for the sizing reads, or an empty topology when the model has
/// no CAD (STL / .msh input) or OCC cannot walk it. Walking the BRep once and
/// sharing the result keeps face-curvature and edge-curvature sizing on the
/// same sample stations.
geom::CadTopology cad_sizing_topology(const Model& model);

/// Chordal size sources along curved CAD edges with FFT-denoised curvature
/// (ADR-0034). OCC BRepLProp κ samples carry parameterization noise; the
/// energy-truncated inverse FFT recovers the smooth κ(s), and the emitted
/// source size follows the constant-relative-sag rule h = c/κ.
/// Flat edge runs (κ below the noise floor after denoise) emit nothing.
std::vector<adapt::SizeSource> spectral_edge_sources(const geom::CadTopology& topo,
                                                     double h_min_geo, double h_coarse,
                                                     SpectralSizingReport& report);

/// Spectral wrap of a fused size field (ADR-0034): sample on a Cartesian grid,
/// energy-truncate the spectrum (insignificant fine bands merge), re-impose
/// the geometry-only demand (elementwise min — trimming can never blur a real
/// feature), then optionally land the predicted element count on `budget`
/// with one uniform h scale. `geo_field` must be the geometry-sources-only
/// sizing (no BC/error seeds); pass an empty fn when no floor is wanted.
mesh::SizeFieldFn apply_spectral_sizing(const Model& model, const mesh::SizeFieldFn& field,
                                        const mesh::SizeFieldFn& geo_field, double h_fine,
                                        std::size_t budget, SpectralSizingReport& report);

// --- ship gates on the delivered mesh (ship_gate.cpp); used by volume_mesh --

/// Final ship gate: relax the cells that are actually being emitted.
///
/// Every mesher gates its OWN cells during snap, with its own predicate over
/// its own intermediate zoo. What ships is `fea::NodalMesh`, and the measure
/// the product reports is `fea::cell_quality` — the pyramid a hybrid fill
/// gated as a pyramid may ship as two assembly tets, an LEB child may be
/// carved after its last gate, and neither is re-measured. This sweep closes
/// that gap the way ADR-0033 requires: measure the cell that ships, and if it
/// is below the floor, give it room by relaxing its INTERIOR nodes only
/// (boundary nodes carry the exact-BRep placement and must not move).
///
/// Returns the number of cells still below the floor, which the caller reports
/// rather than hides.
std::size_t
relax_cells_below_shape_floor(fea::NodalMesh& mesh,
                              std::span<const std::array<std::uint32_t, 4>> boundary_faces,
                              double floor_value, int rounds = 4);

/// Boundary conformity on the mesh that actually ships (ADR-0035).
///
/// Every mesher snaps *its own* boundary set — the lattice skin it built. What
/// ships is `fea::extract_boundary_faces(out.mesh)`, the true element exterior,
/// and the two are not the same set: a fan tet peeled after the snap, a pyramid
/// shipped as two assembly tets, or an LEB child carved late can expose a node
/// that was interior when the snap ran and is on the free surface when the mesh
/// leaves. Those nodes were never candidates for projection.
///
/// This closes the loop for every mesher at once: project the true exterior
/// through the same owner-aware oracle, accept a move only when every incident
/// cell keeps `fea::cell_quality` at or above the shared floor (the measure the
/// product reports, not each mesher's internal predicate), and open room by
/// relaxing interior star nodes when the first attempt is refused.
///
/// The acceptance test is absolute rather than "improves": a move that only
/// improves a near-degenerate cell still ships an element the solver refuses
/// after p-elevation. A node that cannot be placed above the floor stays where
/// it is and is counted.
struct ExteriorConformStats {
    std::size_t n_candidates = 0;
    std::size_t n_moved = 0;
    std::size_t n_relax_rescued = 0;
    std::size_t n_hex_fanned = 0; // hexes fanned into pyramids to free a node
    std::size_t n_left = 0;       // still off the BRep by more than 1e-9 h
    std::size_t n_edge_pinned = 0;
    std::size_t n_edge_chains = 0;
    std::size_t n_pin_rejected = 0;
    std::size_t n_connected_edges = 0;
    std::string connected_edge_census;
    double edge_pass_ms = 0.0;       // cost of the exact sharp-edge recovery pass
    std::size_t n_kink_relieved = 0; // face nodes slid to lower a facet kink
    double worst_residual = 0.0;
    std::uint32_t worst_node = 0;
    Eigen::Vector3d worst_position = Eigen::Vector3d::Zero();
    bool reverted = false; // whole pass rolled back by the exit invariant
};

ExteriorConformStats conform_true_exterior(
    fea::NodalMesh& mesh, std::span<const std::array<std::uint32_t, 4>> boundary_faces,
    mesh::BoundaryProjectionContext* projection, const mesh::BoundaryFit* fit, double h,
    double floor_value, const mesh::MirrorFrame* mirror);

struct BoundaryShellTopology {
    std::size_t n_edges = 0;
    /// Used by one face: a hole in the shell.
    std::size_t n_open = 0;
    /// Used by three or more: two boundary patches occupying the same place.
    std::size_t n_nonmanifold = 0;
};

/// Edge-use census over the free-face set. A closed 2-manifold uses every edge
/// exactly twice; anything else is a tear or a duplicated skin, and neither
/// shows up in a volume comparison.
BoundaryShellTopology
boundary_shell_topology(const std::vector<std::array<std::uint32_t, 4>>& faces);

/// Replace the `geometry_<stage>_volume` token in a mesher note (append it when
/// absent) with the current assessment.
void replace_geometry_volume_note(std::string& note, std::string_view stage,
                                  const GeometryVolumeAssessment& assessment);

/// Throw `GeometryVolumeLimitError` ("feature unresolved at h=") when an exact
/// CAD feature face has no aligned delivered boundary patch within the
/// `kGeometryFeatureResolutionOverH` limit; sub-resolution faces are noted as
/// absorbed instead.
void enforce_feature_resolution(const Model& model, VolumeMeshOutput& output,
                                double requested_h, double delivered_h);

} // namespace polymesh::pipeline::detail
