// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/brep_fidelity.hpp"

#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace polymesh::mesh {
namespace {

double interpolated_quantile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double position = q * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + fraction * (sorted[upper] - sorted[lower]);
}

double point_segment_distance(const Eigen::Vector3d& p, const geom::MeshEdgeSegment& segment) {
    const Eigen::Vector3d ab = segment.b - segment.a;
    const double len2 = ab.squaredNorm();
    if (!(len2 > 0.0) || !std::isfinite(len2)) {
        return (p - segment.a).norm();
    }
    const double t = std::clamp((p - segment.a).dot(ab) / len2, 0.0, 1.0);
    return (p - (segment.a + t * ab)).norm();
}

bool valid_node(std::uint32_t i, const std::vector<Eigen::Vector3d>& nodes) {
    return static_cast<std::size_t>(i) < nodes.size() && nodes[i].allFinite();
}

void append_triangle(geom::TriSurface& surface, std::uint32_t a, std::uint32_t b,
                     std::uint32_t c) {
    if (a == b || b == c || c == a || !valid_node(a, surface.vertices) ||
        !valid_node(b, surface.vertices) || !valid_node(c, surface.vertices)) {
        return;
    }
    if ((surface.vertices[b] - surface.vertices[a])
            .cross(surface.vertices[c] - surface.vertices[a])
            .squaredNorm() <= 1e-30) {
        return;
    }
    surface.triangles.push_back({a, b, c});
}

geom::TriSurface boundary_surface(const std::vector<Eigen::Vector3d>& nodes,
                                  const std::vector<FreeFace>& free_faces) {
    geom::TriSurface surface;
    surface.vertices = nodes;
    surface.triangles.reserve(free_faces.size() * 2);
    for (const auto& face : free_faces) {
        append_triangle(surface, face[0], face[1], face[2]);
        if (face[3] != face[2]) {
            append_triangle(surface, face[0], face[2], face[3]);
        }
    }
    return surface;
}

std::vector<std::uint32_t> valid_boundary_nodes(const std::vector<Eigen::Vector3d>& nodes,
                                                const std::vector<FreeFace>& free_faces) {
    std::vector<std::uint32_t> result;
    for (const std::uint32_t i : free_face_nodes(free_faces)) {
        if (valid_node(i, nodes)) {
            result.push_back(i);
        }
    }
    return result;
}

} // namespace

SampleDistribution summarize_samples(std::span<const double> samples) {
    std::vector<double> finite;
    finite.reserve(samples.size());
    double rms_scale = 0.0;
    double rms_sum = 1.0;
    for (const double sample : samples) {
        if (!std::isfinite(sample)) {
            continue;
        }
        finite.push_back(sample);
        const double magnitude = std::abs(sample);
        if (magnitude != 0.0) {
            if (rms_scale < magnitude) {
                const double ratio = rms_scale / magnitude;
                rms_sum = 1.0 + rms_sum * ratio * ratio;
                rms_scale = magnitude;
            } else {
                const double ratio = magnitude / rms_scale;
                rms_sum += ratio * ratio;
            }
        }
    }
    if (finite.empty()) {
        return {};
    }

    std::sort(finite.begin(), finite.end());
    SampleDistribution out;
    out.count = finite.size();
    out.rms = rms_scale == 0.0
                  ? 0.0
                  : rms_scale * std::sqrt(rms_sum / static_cast<double>(finite.size()));
    out.p95 = interpolated_quantile(finite, 0.95);
    out.p99 = interpolated_quantile(finite, 0.99);
    out.max = finite.back();
    return out;
}

DistanceDistribution summarize_distances(std::span<const double> distances, double h,
                                         double bbox_diagonal) {
    std::vector<double> finite;
    finite.reserve(distances.size());
    for (const double distance : distances) {
        if (std::isfinite(distance) && distance >= 0.0) {
            finite.push_back(distance);
        }
    }

    DistanceDistribution out;
    out.metres = summarize_samples(finite);
    const auto normalized = [&finite](double scale) {
        if (!(scale > 0.0) || !std::isfinite(scale)) {
            return SampleDistribution{};
        }
        std::vector<double> values;
        values.reserve(finite.size());
        for (const double distance : finite) {
            values.push_back(distance / scale);
        }
        return summarize_samples(values);
    };
    out.over_h = normalized(h);
    out.over_bbox_diagonal = normalized(bbox_diagonal);
    return out;
}

BRepGeometryFidelity evaluate_brep_geometry_fidelity(
    const geom::CadModel& model, const std::vector<Eigen::Vector3d>& nodes,
    const std::vector<FreeFace>& free_faces,
    const std::vector<geom::MeshEdgeSegment>& mesh_feature_segments, double h,
    double mesh_volume, std::size_t max_reference_samples) {
    BRepGeometryFidelity out;
    out.brep = geom::inspect_brep(model);
    if (!out.brep.available) {
        return out;
    }
    out.available = true;
    const double bbox_diagonal = model.bbox_diagonal();

    const std::vector<std::uint32_t> boundary_nodes = valid_boundary_nodes(nodes, free_faces);
    std::vector<double> mesh_to_brep;
    mesh_to_brep.reserve(boundary_nodes.size() + free_faces.size() * 5);
    for (const std::uint32_t i : boundary_nodes) {
        if (const auto projected = geom::project_point_on_surface(model, nodes[i])) {
            mesh_to_brep.push_back(projected->distance);
        }
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> boundary_edges;
    std::vector<double> normal_angles;
    normal_angles.reserve(free_faces.size());
    for (const auto& face : free_faces) {
        const int count = (face[3] == face[2]) ? 3 : 4;
        bool valid = true;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int i = 0; i < count; ++i) {
            valid = valid && valid_node(face[i], nodes);
            if (valid_node(face[i], nodes)) {
                centroid += nodes[face[i]];
            }
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % count];
            if (valid_node(a, nodes) && valid_node(b, nodes) && a != b) {
                boundary_edges.emplace(std::min(a, b), std::max(a, b));
            }
        }
        if (!valid) {
            continue;
        }
        centroid /= static_cast<double>(count);
        const auto projected = geom::project_point_on_surface(model, centroid);
        if (!projected) {
            continue;
        }
        mesh_to_brep.push_back(projected->distance);

        Eigen::Vector3d mesh_normal =
            (nodes[face[1]] - nodes[face[0]]).cross(nodes[face[2]] - nodes[face[0]]);
        const double mesh_norm = mesh_normal.norm();
        const double brep_norm = projected->normal.norm();
        if (mesh_norm > 1e-15 && brep_norm > 1e-15) {
            mesh_normal /= mesh_norm;
            const Eigen::Vector3d brep_normal = projected->normal / brep_norm;
            const double cosine = std::clamp(std::abs(mesh_normal.dot(brep_normal)), 0.0, 1.0);
            normal_angles.push_back(std::acos(cosine));
        }
    }
    for (const auto& [a, b] : boundary_edges) {
        const Eigen::Vector3d midpoint = 0.5 * (nodes[a] + nodes[b]);
        if (const auto projected = geom::project_point_on_surface(model, midpoint)) {
            mesh_to_brep.push_back(projected->distance);
        }
    }
    out.mesh_boundary_samples_to_brep_surface =
        summarize_distances(mesh_to_brep, h, bbox_diagonal);
    out.mesh_boundary_normal_angle_to_brep_normal = summarize_samples(normal_angles);

    const geom::TriSurface mesh_boundary = boundary_surface(nodes, free_faces);
    if (!mesh_boundary.triangles.empty()) {
        const geom::BRepSurfaceSamples reference =
            geom::sample_brep_surface(model, max_reference_samples);
        out.brep_surface_sample_face_count = reference.face_count;
        out.brep_surface_uv_attempt_count = reference.uv_attempt_count;
        out.brep_surface_fallback_vertex_count = reference.fallback_vertex_count;
        std::vector<double> brep_reference_to_mesh;
        brep_reference_to_mesh.reserve(reference.points.size());
        for (const Eigen::Vector3d& point : reference.points) {
            brep_reference_to_mesh.push_back(
                closest_on_surface(mesh_boundary, point).distance);
        }
        out.brep_surface_samples_to_mesh_boundary =
            summarize_distances(brep_reference_to_mesh, h, bbox_diagonal);
    }

    const geom::CadTopology topology = geom::extract_topology(model, 24);
    const geom::ChordalEdgeMetrics chordal =
        geom::chordal_edge_metrics_segments(topology, mesh_feature_segments, true);
    out.mesh_feature_segment_count = static_cast<std::size_t>(chordal.n_segments);
    out.max_mesh_feature_segment_midpoint_to_sharp_brep_edge = chordal.max_chordal;
    out.max_sharp_edge_chordal_efficiency = chordal.max_efficiency;

    std::vector<double> mesh_feature_to_brep;
    mesh_feature_to_brep.reserve(mesh_feature_segments.size() * 3);
    for (const auto& segment : mesh_feature_segments) {
        const Eigen::Vector3d points[] = {segment.a, 0.5 * (segment.a + segment.b), segment.b};
        for (const Eigen::Vector3d& p : points) {
            if (p.allFinite()) {
                if (const auto closest = geom::closest_edge(topology, p, true)) {
                    mesh_feature_to_brep.push_back(closest->distance);
                }
            }
        }
    }
    out.mesh_feature_segment_samples_to_sharp_brep_edges =
        summarize_distances(mesh_feature_to_brep, h, bbox_diagonal);

    std::vector<double> brep_edge_to_mesh_feature;
    if (!mesh_feature_segments.empty()) {
        for (const geom::CadEdge& edge : topology.edges) {
            if (edge.feature != geom::CadEdgeFeature::kSharp) {
                continue;
            }
            for (const Eigen::Vector3d& p : edge.samples) {
                double nearest = std::numeric_limits<double>::infinity();
                for (const geom::MeshEdgeSegment& segment : mesh_feature_segments) {
                    nearest = std::min(nearest, point_segment_distance(p, segment));
                }
                if (std::isfinite(nearest)) {
                    brep_edge_to_mesh_feature.push_back(nearest);
                }
            }
        }
    }
    out.sharp_brep_edge_samples_to_mesh_feature_segments =
        summarize_distances(brep_edge_to_mesh_feature, h, bbox_diagonal);

    std::vector<double> brep_vertex_to_mesh;
    brep_vertex_to_mesh.reserve(topology.vertices.size());
    if (!boundary_nodes.empty()) {
        for (const geom::CadVertex& vertex : topology.vertices) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const std::uint32_t i : boundary_nodes) {
                nearest = std::min(nearest, (vertex.position - nodes[i]).norm());
            }
            if (std::isfinite(nearest)) {
                brep_vertex_to_mesh.push_back(nearest);
            }
        }
    }
    out.brep_vertices_to_mesh_boundary_nodes =
        summarize_distances(brep_vertex_to_mesh, h, bbox_diagonal);

    if (out.brep.valid && out.brep.closed && out.brep.solid_count > 0 &&
        std::isfinite(mesh_volume) && mesh_volume >= 0.0 && std::isfinite(out.brep.volume) &&
        out.brep.volume > 0.0) {
        out.has_relative_volume_error = true;
        out.mesh_vs_brep_relative_volume_error =
            std::abs(mesh_volume - out.brep.volume) / out.brep.volume;
    }
    return out;
}

} // namespace polymesh::mesh
