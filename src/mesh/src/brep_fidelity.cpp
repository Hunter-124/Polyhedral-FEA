// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/brep_fidelity.hpp"

#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numbers>
#include <numeric>
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

/// Walk step that keeps at most `cap` of `n` items. 0/oversized caps walk all.
std::size_t sample_stride(std::size_t n, std::size_t cap) {
    if (cap == 0 || n <= cap) {
        return 1;
    }
    return (n + cap - 1) / cap;
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
    // Sorted ascending: summing smallest-first keeps the mean stable for the
    // heavily skewed distance samples this feeds (many ~0, a few large).
    out.mean = std::accumulate(finite.begin(), finite.end(), 0.0) /
               static_cast<double>(finite.size());
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
    double mesh_volume, std::size_t max_reference_samples, std::size_t max_boundary_samples) {
    BRepGeometryFidelity out;
    out.brep = geom::inspect_brep(model);
    if (!out.brep.available) {
        return out;
    }
    out.available = true;
    const double bbox_diagonal = model.bbox_diagonal();

    // The mesh-to-BRep direction draws from three sources — boundary nodes,
    // face centroids, boundary-edge midpoints — whose counts sit at roughly
    // 1:2:3 on a closed triangulated boundary. Node samples are ~0 by
    // construction (the mesher projects boundary nodes onto the BRep) while
    // centroid and midpoint samples carry the real deviation, so the MIX
    // decides the reported mean and quantiles. Striding each source against
    // the same cap independently would let them cross the cap at different
    // mesh sizes and silently change that mix with h — exactly the variable
    // these numbers have to be comparable across. One stride, taken from the
    // largest source, keeps the mix fixed at every resolution.
    const std::vector<std::uint32_t> boundary_nodes = valid_boundary_nodes(nodes, free_faces);

    std::set<std::pair<std::uint32_t, std::uint32_t>> boundary_edges;
    for (const auto& face : free_faces) {
        const int count = (face[3] == face[2]) ? 3 : 4;
        for (int i = 0; i < count; ++i) {
            const std::uint32_t a = face[i];
            const std::uint32_t b = face[(i + 1) % count];
            if (valid_node(a, nodes) && valid_node(b, nodes) && a != b) {
                boundary_edges.emplace(std::min(a, b), std::max(a, b));
            }
        }
    }

    const std::size_t stride = sample_stride(
        std::max({boundary_nodes.size(), free_faces.size(), boundary_edges.size()}),
        max_boundary_samples);

    std::vector<double> mesh_to_brep;
    std::vector<double> node_to_brep;
    mesh_to_brep.reserve(boundary_nodes.size() / stride + free_faces.size() / stride +
                         boundary_edges.size() / stride + 3);
    node_to_brep.reserve(boundary_nodes.size() / stride + 1);

    // POLYMESH_FIDELITY_DUMP=<path>: write the 64 worst mesh->BRep samples as
    // "kind distance_m x y z" lines so a tail regression can be located on the
    // part instead of inferred from quantiles. Diagnostic only; unset = off.
    struct WorstSample {
        double distance;
        char kind;
        Eigen::Vector3d point;
    };
    std::vector<WorstSample> worst_samples;
    const char* const dump_path = std::getenv("POLYMESH_FIDELITY_DUMP");
    const auto note_sample = [&](double distance, char kind, const Eigen::Vector3d& p) {
        if (dump_path != nullptr) {
            worst_samples.push_back({distance, kind, p});
        }
    };

    for (std::size_t i = 0; i < boundary_nodes.size(); i += stride) {
        if (const auto projected =
                geom::project_point_on_surface(model, nodes[boundary_nodes[i]])) {
            mesh_to_brep.push_back(projected->distance);
            node_to_brep.push_back(projected->distance);
            note_sample(projected->distance, 'n', nodes[boundary_nodes[i]]);
        }
    }

    std::vector<double> normal_angles;
    normal_angles.reserve(free_faces.size() / stride + 1);
    for (std::size_t face_index = 0; face_index < free_faces.size(); face_index += stride) {
        const auto& face = free_faces[face_index];
        const int count = (face[3] == face[2]) ? 3 : 4;
        bool valid = true;
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int i = 0; i < count; ++i) {
            valid = valid && valid_node(face[i], nodes);
            if (valid) {
                centroid += nodes[face[i]];
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
        note_sample(projected->distance, 'c', centroid);

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

    std::size_t edge_index = 0;
    for (const auto& [a, b] : boundary_edges) {
        if (edge_index++ % stride != 0) {
            continue;
        }
        const Eigen::Vector3d midpoint = 0.5 * (nodes[a] + nodes[b]);
        if (const auto projected = geom::project_point_on_surface(model, midpoint)) {
            mesh_to_brep.push_back(projected->distance);
            note_sample(projected->distance, 'm', midpoint);
        }
    }
    out.mesh_boundary_samples_to_brep_surface =
        summarize_distances(mesh_to_brep, h, bbox_diagonal);
    out.mesh_boundary_nodes_to_brep_surface =
        summarize_distances(node_to_brep, h, bbox_diagonal);
    out.mesh_boundary_normal_angle_to_brep_normal = summarize_samples(normal_angles);

    if (dump_path != nullptr) {
        std::sort(worst_samples.begin(), worst_samples.end(),
                  [](const WorstSample& x, const WorstSample& y) {
                      return x.distance > y.distance;
                  });
        if (std::FILE* file = std::fopen(dump_path, "w")) {
            const std::size_t limit = std::min<std::size_t>(worst_samples.size(), 64);
            for (std::size_t i = 0; i < limit; ++i) {
                const WorstSample& s = worst_samples[i];
                std::fprintf(file, "%c %.9g %.9g %.9g %.9g\n", s.kind, s.distance, s.point.x(),
                             s.point.y(), s.point.z());
            }
            std::fclose(file);
        }
    }

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
            if (!p.allFinite()) {
                continue;
            }
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& edge : topology.edges) {
                if (edge.feature != geom::CadEdgeFeature::kSharp) {
                    continue;
                }
                if (const auto exact = geom::project_point_on_edge(model, edge.id, p)) {
                    nearest = std::min(nearest, exact->distance);
                }
            }
            if (std::isfinite(nearest)) {
                mesh_feature_to_brep.push_back(nearest);
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

GeometryCompleteness evaluate_geometry_completeness(const geom::CadModel& model,
                                                    double mesh_volume,
                                                    double relative_volume_tolerance) {
    GeometryCompleteness out;
    out.mesh_volume = mesh_volume;
    out.relative_volume_tolerance = relative_volume_tolerance;
    const auto brep = geom::inspect_brep(model);
    if (!brep.available || !brep.valid || !brep.closed || brep.solid_count == 0 ||
        !(brep.volume > 0.0) || !std::isfinite(brep.volume) ||
        !(relative_volume_tolerance > 0.0) || !std::isfinite(relative_volume_tolerance)) {
        return out;
    }
    out.available = true;
    out.brep_volume = brep.volume;
    if (!(mesh_volume >= 0.0) || !std::isfinite(mesh_volume)) {
        out.relative_volume_error = std::numeric_limits<double>::infinity();
        return out;
    }
    out.relative_volume_error = std::abs(mesh_volume - brep.volume) / brep.volume;
    out.complete = out.relative_volume_error <= relative_volume_tolerance;
    return out;
}

double boundary_surface_volume(const std::vector<Eigen::Vector3d>& nodes,
                               const std::vector<FreeFace>& free_faces) {
    if (nodes.empty()) {
        return 0.0;
    }
    // Divergence form about nodes.front(): the origin shift keeps the signed
    // tetra volumes small for parts far from the world origin.
    const Eigen::Vector3d origin = nodes.front();
    const auto tri_volume = [&](std::uint32_t ia, std::uint32_t ib, std::uint32_t ic) {
        if (ia >= nodes.size() || ib >= nodes.size() || ic >= nodes.size()) {
            return 0.0;
        }
        const Eigen::Vector3d a = nodes[ia] - origin;
        const Eigen::Vector3d b = nodes[ib] - origin;
        const Eigen::Vector3d c = nodes[ic] - origin;
        return a.dot(b.cross(c)) / 6.0;
    };
    double signed_volume = 0.0;
    for (const auto& face : free_faces) {
        signed_volume += tri_volume(face[0], face[1], face[2]);
        if (face[3] != face[2]) {
            signed_volume += tri_volume(face[0], face[2], face[3]);
        }
    }
    return std::abs(signed_volume);
}

std::vector<geom::MeshEdgeSegment>
mesh_dihedral_feature_segments(const std::vector<Eigen::Vector3d>& nodes,
                               const std::vector<FreeFace>& free_faces,
                               double sharp_angle_deg) {
    using Edge = std::pair<std::uint32_t, std::uint32_t>;
    std::map<Edge, std::vector<Eigen::Vector3d>> edge_normals;
    for (const auto& face : free_faces) {
        const std::size_t count = face[3] == face[2] ? 3 : 4;
        bool valid = true;
        for (std::size_t i = 0; i < count; ++i) {
            valid = valid && face[i] < nodes.size();
        }
        if (!valid) {
            continue;
        }
        Eigen::Vector3d normal =
            (nodes[face[1]] - nodes[face[0]]).cross(nodes[face[2]] - nodes[face[0]]);
        const double norm = normal.norm();
        if (!(norm > 1e-15)) {
            continue;
        }
        normal /= norm;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t a = face[i];
            std::uint32_t b = face[(i + 1) % count];
            if (a == b) {
                continue;
            }
            if (a > b) {
                std::swap(a, b);
            }
            edge_normals[{a, b}].push_back(normal);
        }
    }

    const double threshold = sharp_angle_deg * std::numbers::pi / 180.0;
    std::vector<geom::MeshEdgeSegment> segments;
    segments.reserve(edge_normals.size());
    for (const auto& [edge, normals] : edge_normals) {
        // Exactly two incident faces: a manifold edge with a defined dihedral.
        if (normals.size() != 2) {
            continue;
        }
        // Angle between the two facet PLANES, which is what a crease is. The
        // signed dot product measured the angle between two winding-dependent
        // normals instead, so a pair of neighbouring faces that the boundary
        // extraction happened to wind oppositely read as a 180° crease. That
        // over-detection is the whole of the "spurious mesh creases" ADR-0035
        // §5(b) recorded as an open mesher defect: on icecream_cone/graded it
        // reported 177 feature segments where the geometry has 47, and the
        // phantom ones are scattered over smooth walls, which is why their
        // distance to the nearest sharp BRep edge came out at 0.81 of the
        // bounding-box diagonal.
        const double cosine = std::clamp(std::abs(normals[0].dot(normals[1])), 0.0, 1.0);
        if (std::acos(cosine) < threshold) {
            continue;
        }
        segments.push_back({nodes[edge.first], nodes[edge.second]});
    }
    return segments;
}

BrepFidelitySummary summarize_brep_fidelity(const BRepGeometryFidelity& report) {
    BrepFidelitySummary out;
    if (!report.available) {
        return out;
    }
    const auto& forward = report.mesh_boundary_samples_to_brep_surface.over_bbox_diagonal;
    const auto& reverse = report.brep_surface_samples_to_mesh_boundary.over_bbox_diagonal;
    out.n_samples = forward.count + reverse.count;
    if (out.n_samples == 0) {
        return out;
    }
    double mean_sum = 0.0;
    std::size_t directions = 0;
    for (const SampleDistribution* d : {&forward, &reverse}) {
        if (d->count > 0) {
            mean_sum += d->mean;
            ++directions;
        }
    }
    out.available = true;
    out.chamfer_mean = mean_sum / static_cast<double>(directions);
    out.dist_p95 = std::max(forward.p95, reverse.p95);
    out.dist_p99 = std::max(forward.p99, reverse.p99);
    out.dist_max = std::max(forward.max, reverse.max);
    out.normal_angle_p95_rad = report.mesh_boundary_normal_angle_to_brep_normal.p95;
    out.rel_volume_err =
        report.has_relative_volume_error ? report.mesh_vs_brep_relative_volume_error : 0.0;
    return out;
}

BrepFidelitySummary brep_fidelity_summary(const geom::CadModel& model,
                                          const std::vector<Eigen::Vector3d>& nodes,
                                          const std::vector<FreeFace>& free_faces, double h,
                                          std::size_t max_samples) {
    if (free_faces.empty()) {
        return {};
    }
    const auto segments = mesh_dihedral_feature_segments(nodes, free_faces);
    const double mesh_volume = boundary_surface_volume(nodes, free_faces);
    return summarize_brep_fidelity(evaluate_brep_geometry_fidelity(
        model, nodes, free_faces, segments, h, mesh_volume, max_samples, max_samples));
}

} // namespace polymesh::mesh
