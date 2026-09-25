// SPDX-License-Identifier: BSD-3-Clause
#include "poly_mesh_geometry.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace polymesh::mesh::detail {
namespace {

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

Point2 project_2d(const Eigen::Vector3d& p, int drop_axis) {
    if (drop_axis == 0) {
        return {p.y(), p.z()};
    }
    if (drop_axis == 1) {
        return {p.x(), p.z()};
    }
    return {p.x(), p.y()};
}

double orient_2d(const Point2& a, const Point2& b, const Point2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool point_on_segment(const Point2& p, const Point2& a, const Point2& b, double linear_tol,
                      double area_tol) {
    if (std::abs(orient_2d(a, b, p)) > area_tol) {
        return false;
    }
    return p.x >= std::min(a.x, b.x) - linear_tol && p.x <= std::max(a.x, b.x) + linear_tol &&
           p.y >= std::min(a.y, b.y) - linear_tol && p.y <= std::max(a.y, b.y) + linear_tol;
}

bool segments_intersect(const Point2& a, const Point2& b, const Point2& c, const Point2& d,
                        double linear_tol, double area_tol) {
    const double o1 = orient_2d(a, b, c);
    const double o2 = orient_2d(a, b, d);
    const double o3 = orient_2d(c, d, a);
    const double o4 = orient_2d(c, d, b);
    if (((o1 > area_tol && o2 < -area_tol) || (o1 < -area_tol && o2 > area_tol)) &&
        ((o3 > area_tol && o4 < -area_tol) || (o3 < -area_tol && o4 > area_tol))) {
        return true;
    }
    return (std::abs(o1) <= area_tol && point_on_segment(c, a, b, linear_tol, area_tol)) ||
           (std::abs(o2) <= area_tol && point_on_segment(d, a, b, linear_tol, area_tol)) ||
           (std::abs(o3) <= area_tol && point_on_segment(a, c, d, linear_tol, area_tol)) ||
           (std::abs(o4) <= area_tol && point_on_segment(b, c, d, linear_tol, area_tol));
}

bool segments_cross_strictly(const Point2& a, const Point2& b, const Point2& c,
                             const Point2& d, double area_tol) {
    const double o1 = orient_2d(a, b, c);
    const double o2 = orient_2d(a, b, d);
    const double o3 = orient_2d(c, d, a);
    const double o4 = orient_2d(c, d, b);
    return ((o1 > area_tol && o2 < -area_tol) || (o1 < -area_tol && o2 > area_tol)) &&
           ((o3 > area_tol && o4 < -area_tol) || (o3 < -area_tol && o4 > area_tol));
}

bool point_in_polygon_strict(const Point2& point, const std::vector<Point2>& polygon,
                             double linear_tol, double area_tol) {
    bool inside = false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point2& a = polygon[i];
        const Point2& b = polygon[(i + 1) % polygon.size()];
        if (point_on_segment(point, a, b, linear_tol, area_tol)) {
            return false;
        }
        if ((a.y > point.y) != (b.y > point.y)) {
            const double x = a.x + (point.y - a.y) * (b.x - a.x) / (b.y - a.y);
            if (x > point.x) {
                inside = !inside;
            }
        }
    }
    return inside;
}

double triangle_intersection_area(const std::array<Point2, 3>& subject,
                                  const std::array<Point2, 3>& clip, double area_tol) {
    std::vector<Point2> polygon(subject.begin(), subject.end());
    const double clip_orientation = orient_2d(clip[0], clip[1], clip[2]);
    if (std::abs(clip_orientation) <= area_tol) {
        return 0.0;
    }
    const double sign = clip_orientation > 0.0 ? 1.0 : -1.0;
    for (std::size_t edge = 0; edge < clip.size() && !polygon.empty(); ++edge) {
        const Point2& a = clip[edge];
        const Point2& b = clip[(edge + 1) % clip.size()];
        std::vector<Point2> clipped;
        clipped.reserve(polygon.size() + 1);
        Point2 previous = polygon.back();
        double previous_distance = sign * orient_2d(a, b, previous);
        for (const Point2& current : polygon) {
            const double current_distance = sign * orient_2d(a, b, current);
            const bool previous_inside = previous_distance >= -area_tol;
            const bool current_inside = current_distance >= -area_tol;
            if (previous_inside != current_inside) {
                const double denominator = previous_distance - current_distance;
                if (std::abs(denominator) > area_tol) {
                    const double t = previous_distance / denominator;
                    clipped.push_back({previous.x + t * (current.x - previous.x),
                                       previous.y + t * (current.y - previous.y)});
                }
            }
            if (current_inside) {
                clipped.push_back(current);
            }
            previous = current;
            previous_distance = current_distance;
        }
        polygon = std::move(clipped);
    }
    if (polygon.empty()) {
        return 0.0;
    }
    double twice_area = 0.0;
    const Point2 area_origin = polygon.front();
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point2& a = polygon[i];
        const Point2& b = polygon[(i + 1) % polygon.size()];
        twice_area += (a.x - area_origin.x) * (b.y - area_origin.y) -
                      (a.y - area_origin.y) * (b.x - area_origin.x);
    }
    return 0.5 * std::abs(twice_area);
}

} // namespace

FaceGeometry face_geometry(const PolyMesh& mesh, const Face& face) {
    FaceGeometry out;
    const Eigen::Vector3d origin = mesh.vertices[face.vertices.front()];
    out.min = origin;
    out.max = origin;
    Eigen::Vector3d centroid_offset = Eigen::Vector3d::Zero();
    for (const VertexId vertex : face.vertices) {
        const Eigen::Vector3d& point = mesh.vertices[vertex];
        centroid_offset += point - origin;
        out.min = out.min.cwiseMin(point);
        out.max = out.max.cwiseMax(point);
    }
    out.centroid = origin + centroid_offset / static_cast<double>(face.vertices.size());
    for (std::size_t i = 0; i < face.vertices.size(); ++i) {
        const Eigen::Vector3d a = mesh.vertices[face.vertices[i]] - origin;
        const Eigen::Vector3d b =
            mesh.vertices[face.vertices[(i + 1) % face.vertices.size()]] - origin;
        out.area += a.cross(b);
    }
    out.area *= 0.5;
    for (std::size_t i = 0; i < face.vertices.size(); ++i) {
        for (std::size_t j = i + 1; j < face.vertices.size(); ++j) {
            out.diameter = std::max(
                out.diameter,
                (mesh.vertices[face.vertices[i]] - mesh.vertices[face.vertices[j]]).norm());
        }
    }
    const Eigen::Vector3d abs_area = out.area.cwiseAbs();
    if (abs_area.x() >= abs_area.y() && abs_area.x() >= abs_area.z()) {
        out.drop_axis = 0;
    } else if (abs_area.y() >= abs_area.z()) {
        out.drop_axis = 1;
    }
    return out;
}

bool polygon_is_simple(const PolyMesh& mesh, const Face& face, const FaceGeometry& geometry) {
    const std::size_t n = face.vertices.size();
    const double linear_tol = 1e-10 * std::max(geometry.diameter, 1e-30);
    const double area_tol = 1e-12 * std::max(geometry.diameter * geometry.diameter, 1e-60);
    std::vector<Point2> points;
    points.reserve(n);
    for (const VertexId vertex : face.vertices) {
        points.push_back(project_2d(mesh.vertices[vertex], geometry.drop_axis));
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t inext = (i + 1) % n;
        for (std::size_t j = i + 1; j < n; ++j) {
            const std::size_t jnext = (j + 1) % n;
            if (i == j || inext == j || jnext == i) {
                continue;
            }
            if (segments_intersect(points[i], points[inext], points[j], points[jnext],
                                   linear_tol, area_tol)) {
                return false;
            }
        }
    }
    return true;
}

std::vector<std::array<VertexId, 3>> triangulate_simple_polygon(const PolyMesh& mesh,
                                                                const Face& face) {
    if (face.vertices.size() == 3) {
        return {{face.vertices[0], face.vertices[1], face.vertices[2]}};
    }
    const FaceGeometry geometry = face_geometry(mesh, face);
    if (!polygon_is_simple(mesh, face, geometry)) {
        return {};
    }
    std::vector<Point2> points;
    points.reserve(face.vertices.size());
    for (const VertexId vertex : face.vertices) {
        points.push_back(project_2d(mesh.vertices[vertex], geometry.drop_axis));
    }
    double signed_area = 0.0;
    const Point2 area_origin = points.front();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point2& a = points[i];
        const Point2& b = points[(i + 1) % points.size()];
        signed_area += (a.x - area_origin.x) * (b.y - area_origin.y) -
                       (a.y - area_origin.y) * (b.x - area_origin.x);
    }
    const double orientation = signed_area >= 0.0 ? 1.0 : -1.0;
    const double area_tol = 1e-12 * std::max(geometry.diameter * geometry.diameter, 1e-60);
    std::vector<std::size_t> remaining(face.vertices.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) {
        remaining[i] = i;
    }
    std::vector<std::array<VertexId, 3>> triangles;
    triangles.reserve(face.vertices.size() - 2);
    while (remaining.size() > 3) {
        bool clipped = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::size_t previous =
                remaining[(i + remaining.size() - 1) % remaining.size()];
            const std::size_t current = remaining[i];
            const std::size_t next = remaining[(i + 1) % remaining.size()];
            if (orientation * orient_2d(points[previous], points[current], points[next]) <=
                area_tol) {
                continue;
            }
            bool blocked = false;
            for (const std::size_t candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) {
                    continue;
                }
                const double a = orientation * orient_2d(points[previous], points[current],
                                                         points[candidate]);
                const double b =
                    orientation * orient_2d(points[current], points[next], points[candidate]);
                const double c =
                    orientation * orient_2d(points[next], points[previous], points[candidate]);
                if (a >= -area_tol && b >= -area_tol && c >= -area_tol) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                continue;
            }
            triangles.push_back(
                {face.vertices[previous], face.vertices[current], face.vertices[next]});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            return {};
        }
    }
    if (orientation *
            orient_2d(points[remaining[0]], points[remaining[1]], points[remaining[2]]) <=
        area_tol) {
        return {};
    }
    triangles.push_back({face.vertices[remaining[0]], face.vertices[remaining[1]],
                         face.vertices[remaining[2]]});
    return triangles;
}

bool segment_hits_face_interior(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                                const PolyMesh& mesh, const Face& face,
                                const FaceGeometry& geometry, double linear_tol) {
    const Eigen::Vector3d normal = geometry.area.normalized();
    const double da = (a - geometry.centroid).dot(normal);
    const double db = (b - geometry.centroid).dot(normal);
    if ((da > linear_tol && db > linear_tol) || (da < -linear_tol && db < -linear_tol)) {
        return false;
    }
    const double denominator = da - db;
    if (std::abs(denominator) <= linear_tol) {
        return false;
    }
    const double t = da / denominator;
    const double segment_length = (b - a).norm();
    const double parameter_tol =
        std::min(0.25, linear_tol / std::max(segment_length, linear_tol));
    if (t <= parameter_tol || t >= 1.0 - parameter_tol) {
        return false;
    }
    const Eigen::Vector3d hit = a + t * (b - a);
    std::vector<Point2> polygon;
    polygon.reserve(face.vertices.size());
    for (const VertexId vertex : face.vertices) {
        polygon.push_back(project_2d(mesh.vertices[vertex], geometry.drop_axis));
    }
    const double area_tol = 1e-12 * std::max(geometry.diameter * geometry.diameter, 1e-60);
    return point_in_polygon_strict(project_2d(hit, geometry.drop_axis), polygon, linear_tol,
                                   area_tol);
}

bool coplanar_polygons_overlap(const PolyMesh& mesh, const Face& a, const FaceGeometry& ga,
                               const Face& b, const FaceGeometry& gb, double linear_tol) {
    const Eigen::Vector3d na = ga.area.normalized();
    const Eigen::Vector3d nb = gb.area.normalized();
    if (na.cross(nb).norm() > 1e-10 ||
        std::abs((gb.centroid - ga.centroid).dot(na)) > linear_tol) {
        return false;
    }
    std::vector<Point2> pa;
    std::vector<Point2> pb;
    for (const VertexId vertex : a.vertices) {
        pa.push_back(project_2d(mesh.vertices[vertex], ga.drop_axis));
    }
    for (const VertexId vertex : b.vertices) {
        pb.push_back(project_2d(mesh.vertices[vertex], ga.drop_axis));
    }
    const double diameter = std::max(ga.diameter, gb.diameter);
    const double area_tol = 1e-12 * std::max(diameter * diameter, 1e-60);
    for (std::size_t i = 0; i < pa.size(); ++i) {
        for (std::size_t j = 0; j < pb.size(); ++j) {
            if (segments_cross_strictly(pa[i], pa[(i + 1) % pa.size()], pb[j],
                                        pb[(j + 1) % pb.size()], area_tol)) {
                return true;
            }
        }
    }
    for (const Point2& point : pa) {
        if (point_in_polygon_strict(point, pb, linear_tol, area_tol)) {
            return true;
        }
    }
    for (const Point2& point : pb) {
        if (point_in_polygon_strict(point, pa, linear_tol, area_tol)) {
            return true;
        }
    }

    // Collinear boundaries can enclose a thin positive-area overlap without a
    // strict edge crossing or a source vertex in the other polygon.  Clip the
    // polygons' ear triangles pairwise to measure that remaining case.
    const auto triangles_a = triangulate_simple_polygon(mesh, a);
    const auto triangles_b = triangulate_simple_polygon(mesh, b);
    for (const auto& ta : triangles_a) {
        const std::array<Point2, 3> projected_a{
            project_2d(mesh.vertices[ta[0]], ga.drop_axis),
            project_2d(mesh.vertices[ta[1]], ga.drop_axis),
            project_2d(mesh.vertices[ta[2]], ga.drop_axis)};
        for (const auto& tb : triangles_b) {
            const std::array<Point2, 3> projected_b{
                project_2d(mesh.vertices[tb[0]], ga.drop_axis),
                project_2d(mesh.vertices[tb[1]], ga.drop_axis),
                project_2d(mesh.vertices[tb[2]], ga.drop_axis)};
            if (triangle_intersection_area(projected_a, projected_b, area_tol) > area_tol) {
                return true;
            }
        }
    }
    return false;
}

bool point_in_cell_strict(const Eigen::Vector3d& point, const PolyMesh& mesh, const Cell& cell,
                          CellId cell_id, const std::vector<FaceGeometry>& geometry,
                          double linear_tol) {
    // Boundary contact is legitimate between adjacent cells and is therefore
    // explicitly excluded from the strict containment result.
    for (const FaceId face_id : cell.faces) {
        const Face& face = mesh.faces[face_id];
        const FaceGeometry& fg = geometry[face_id];
        const Eigen::Vector3d normal = fg.area.normalized();
        if (std::abs((point - fg.centroid).dot(normal)) > linear_tol) {
            continue;
        }
        std::vector<Point2> polygon;
        polygon.reserve(face.vertices.size());
        for (const VertexId vertex : face.vertices) {
            polygon.push_back(project_2d(mesh.vertices[vertex], fg.drop_axis));
        }
        const Point2 projected = project_2d(point, fg.drop_axis);
        const double area_tol =
            1e-12 * std::max(fg.diameter * fg.diameter, linear_tol * linear_tol);
        if (point_in_polygon_strict(projected, polygon, linear_tol, area_tol)) {
            return false;
        }
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            if (point_on_segment(projected, polygon[i], polygon[(i + 1) % polygon.size()],
                                 linear_tol, area_tol)) {
                return false;
            }
        }
    }

    // The oriented solid angle works for any closed simple polyhedron, not
    // merely convex RVD cells.  Face orientation is reversed for a neighbour.
    double solid_angle = 0.0;
    for (const FaceId face_id : cell.faces) {
        const Face& face = mesh.faces[face_id];
        const double orientation = face.owner == cell_id ? 1.0 : -1.0;
        for (const auto& triangle : triangulate_simple_polygon(mesh, face)) {
            const Eigen::Vector3d a = mesh.vertices[triangle[0]] - point;
            const Eigen::Vector3d b = mesh.vertices[triangle[1]] - point;
            const Eigen::Vector3d c = mesh.vertices[triangle[2]] - point;
            const double la = a.norm();
            const double lb = b.norm();
            const double lc = c.norm();
            if (std::min({la, lb, lc}) <= linear_tol) {
                return false;
            }
            const double denominator =
                la * lb * lc + a.dot(b) * lc + b.dot(c) * la + c.dot(a) * lb;
            solid_angle += orientation * 2.0 * std::atan2(a.dot(b.cross(c)), denominator);
        }
    }
    return std::abs(solid_angle) > 2.0 * 3.14159265358979323846;
}

} // namespace polymesh::mesh::detail
