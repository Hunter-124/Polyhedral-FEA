// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/surface_metrics.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::mesh {

/// Finite-sample distribution. Quantiles use linear interpolation between
/// sorted samples at q * (n - 1). Empty input has count == 0 and zero values.
struct SampleDistribution {
    std::size_t count = 0;
    double mean = 0.0;
    double rms = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

/// A distance distribution in metres and normalized by both requested scales.
/// A non-positive/non-finite scale leaves the corresponding distribution empty.
struct DistanceDistribution {
    SampleDistribution metres;
    SampleDistribution over_h;
    SampleDistribution over_bbox_diagonal;
};

/// Summarize finite scalar samples. Non-finite inputs are ignored.
[[nodiscard]] SampleDistribution summarize_samples(std::span<const double> samples);

/// Summarize non-negative distances in metres and normalize them by h and the
/// live BRep bounding-box diagonal.
[[nodiscard]] DistanceDistribution summarize_distances(std::span<const double> distances,
                                                       double h, double bbox_diagonal);

/// Sampled fidelity evidence between a volume mesh boundary and a live BRep.
/// Field names state the measured direction; no one-sided result is labelled a
/// Hausdorff distance.
struct BRepGeometryFidelity {
    bool available = false;
    geom::BRepInspection brep;

    DistanceDistribution mesh_boundary_samples_to_brep_surface;
    /// Boundary NODES only, no face centroids or edge midpoints. Nodes are the
    /// only samples a mesher can place exactly on the BRep; centroid and
    /// midpoint samples always carry the linear-facet chord sag h²κ/8, so a
    /// combined statistic cannot tell "the mesher missed the surface" from
    /// "a flat facet spans a curve" (ADR-0035).
    DistanceDistribution mesh_boundary_nodes_to_brep_surface;
    DistanceDistribution brep_surface_samples_to_mesh_boundary;
    std::size_t brep_surface_sample_face_count = 0;
    std::size_t brep_surface_uv_attempt_count = 0;
    std::size_t brep_surface_fallback_vertex_count = 0;

    /// Unoriented angle acos(|n_mesh dot n_BRep|), in radians.
    SampleDistribution mesh_boundary_normal_angle_to_brep_normal;

    DistanceDistribution mesh_feature_segment_samples_to_sharp_brep_edges;
    DistanceDistribution sharp_brep_edge_samples_to_mesh_feature_segments;
    /// Chordal utility evidence, named by its actual sampled direction.
    std::size_t mesh_feature_segment_count = 0;
    double max_mesh_feature_segment_midpoint_to_sharp_brep_edge = 0.0;
    double max_sharp_edge_chordal_efficiency = 0.0;

    /// Nearest boundary-node error for each exact CAD vertex.
    DistanceDistribution brep_vertices_to_mesh_boundary_nodes;

    bool has_relative_volume_error = false;
    double mesh_vs_brep_relative_volume_error = 0.0;
};

inline constexpr double kGeometryCompletenessRelVolumeTolerance = 0.01;

/// Exact-CAD volume guard.  Unlike boundary residuals, this detects a missing
/// or over-cut void even when every boundary face that does exist projects
/// perfectly onto the BRep.
struct GeometryCompleteness {
    bool available = false;
    bool complete = false;
    double brep_volume = 0.0;
    double mesh_volume = 0.0;
    double relative_volume_error = 0.0;
    double relative_volume_tolerance = kGeometryCompletenessRelVolumeTolerance;
};

[[nodiscard]] GeometryCompleteness evaluate_geometry_completeness(
    const geom::CadModel& model, double mesh_volume,
    double relative_volume_tolerance = kGeometryCompletenessRelVolumeTolerance);

/// Evaluate sampled mesh/BRep fidelity using the live BRep projection oracle.
///
/// `free_faces` are exterior triangles/quads (triangle iff f[3] == f[2]).
/// Candidate feature segments should come from classified boundary mesh edges;
/// they are not inferred from arbitrary polygon diagonals here. The BRep-to-mesh
/// direction uses bounded exact trimmed-face samples and never materializes a
/// reference tessellation. `max_reference_samples` is both the storage ceiling
/// and the input to the bounded UV-attempt budget.
///
/// `max_boundary_samples` caps the mesh-to-BRep direction, whose cost is one
/// exact BRep projection per sample. 0 (the diagnostic default) projects every
/// boundary node, face centroid, and boundary-edge midpoint. A positive cap
/// derives ONE stride from the largest of those three sources and applies it to
/// all three, so their ~1:2:3 mix — and therefore the reported mean and
/// quantiles — stays fixed as the mesh is refined. Per-source caps would let
/// the sources saturate at different mesh sizes and make the metric
/// incomparable across h. The total sample count is bounded by 3*(cap + 1).
///
/// Empty models and builds without OpenCASCADE return the deterministic default
/// (`available == false`). Invalid indices/degenerate faces are skipped.
[[nodiscard]] BRepGeometryFidelity evaluate_brep_geometry_fidelity(
    const geom::CadModel& model, const std::vector<Eigen::Vector3d>& nodes,
    const std::vector<FreeFace>& free_faces,
    const std::vector<geom::MeshEdgeSegment>& mesh_feature_segments, double h,
    double mesh_volume, std::size_t max_reference_samples = 10'000,
    std::size_t max_boundary_samples = 0);

/// Absolute enclosed volume of a closed boundary-face shell (divergence form).
/// Quads are split on the 0-2 diagonal; out-of-range indices contribute zero.
/// Empty input returns 0.
[[nodiscard]] double boundary_surface_volume(const std::vector<Eigen::Vector3d>& nodes,
                                             const std::vector<FreeFace>& free_faces);

/// Candidate mesh feature segments: boundary edges shared by exactly two faces
/// whose dihedral angle meets `sharp_angle_deg`. This is the classified-edge
/// input `evaluate_brep_geometry_fidelity` expects; polygon diagonals are never
/// invented here.
[[nodiscard]] std::vector<geom::MeshEdgeSegment>
mesh_dihedral_feature_segments(const std::vector<Eigen::Vector3d>& nodes,
                               const std::vector<FreeFace>& free_faces,
                               double sharp_angle_deg = 30.0);

/// Scale-free condensation of `BRepGeometryFidelity` into the columns the
/// learned mesh advisor trains on. Every distance is a fraction of the BRep
/// bounding-box diagonal, so values are comparable across parts of any size.
///
/// `chamfer_mean` is the symmetric mean point-to-surface distance, averaged
/// over whichever of the two directions produced samples; the quantiles and
/// max are the worse of the two directions. A mesh that hugs the BRep where it
/// has faces but leaves whole faces uncovered therefore still scores badly,
/// because the BRep→mesh direction carries equal weight.
///
/// Both quantiles are reported because `dist_p95` is measured to be
/// degenerate: a conforming mesh has its boundary nodes projected onto the
/// BRep, so well over 95% of the samples are exactly zero and p95 collapses to
/// ~1e-17 on every real part. `dist_p99` is the tail statistic that actually
/// varies with h, and is the one the advisor learns.
struct BrepFidelitySummary {
    bool available = false;
    double chamfer_mean = 0.0;
    double dist_p95 = 0.0;
    double dist_p99 = 0.0;
    double dist_max = 0.0;
    double normal_angle_p95_rad = 0.0;
    double rel_volume_err = 0.0;
    std::size_t n_samples = 0;
};

/// Condense an already-evaluated report. Reports with no samples in either
/// direction stay `available == false`.
[[nodiscard]] BrepFidelitySummary summarize_brep_fidelity(const BRepGeometryFidelity& report);

/// Default per-direction sample budget for the campaign metric. The full
/// diagnostic sweep projects every boundary node, face centroid and edge
/// midpoint onto the exact BRep — thousands of BRepExtrema queries. This cap
/// holds the estimate steady while keeping the metric a small fraction of a
/// campaign run.
inline constexpr std::size_t kCampaignFidelitySamples = 1500;

/// One-call campaign metric: derive feature segments and shell volume from the
/// boundary faces, evaluate against the live BRep under a bounded budget, and
/// condense. Empty faces / empty model return an unavailable summary.
[[nodiscard]] BrepFidelitySummary
brep_fidelity_summary(const geom::CadModel& model, const std::vector<Eigen::Vector3d>& nodes,
                      const std::vector<FreeFace>& free_faces, double h,
                      std::size_t max_samples = kCampaignFidelitySamples);

} // namespace polymesh::mesh
