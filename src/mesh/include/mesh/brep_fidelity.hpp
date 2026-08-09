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

/// Evaluate sampled mesh/BRep fidelity using the live BRep projection oracle.
///
/// `free_faces` are exterior triangles/quads (triangle iff f[3] == f[2]).
/// Candidate feature segments should come from classified boundary mesh edges;
/// they are not inferred from arbitrary polygon diagonals here. The BRep-to-mesh
/// direction uses bounded exact trimmed-face samples and never materializes a
/// reference tessellation. `max_reference_samples` is both the storage ceiling
/// and the input to the bounded UV-attempt budget.
///
/// Empty models and builds without OpenCASCADE return the deterministic default
/// (`available == false`). Invalid indices/degenerate faces are skipped.
[[nodiscard]] BRepGeometryFidelity evaluate_brep_geometry_fidelity(
    const geom::CadModel& model, const std::vector<Eigen::Vector3d>& nodes,
    const std::vector<FreeFace>& free_faces,
    const std::vector<geom::MeshEdgeSegment>& mesh_feature_segments, double h,
    double mesh_volume, std::size_t max_reference_samples = 10'000);

} // namespace polymesh::mesh
