// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/poly_mesh.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <vector>

namespace polymesh::mesh {
namespace {

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

struct FaceGeometry {
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    Eigen::Vector3d area = Eigen::Vector3d::Zero();
    Eigen::Vector3d min = Eigen::Vector3d::Zero();
    Eigen::Vector3d max = Eigen::Vector3d::Zero();
    double diameter = 0.0;
    int drop_axis = 2;
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

} // namespace

void PolyMesh::check_validity() const {
    const auto nv = static_cast<std::uint32_t>(vertices.size());
    const auto nc = static_cast<std::uint32_t>(cells.size());
    std::vector<int> face_cell_references(faces.size(), 0);
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        if (face.vertices.size() < 3) {
            throw ValidityError(std::format("face {} has fewer than 3 vertices", f));
        }
        for (const auto v : face.vertices) {
            if (v >= nv) {
                throw ValidityError(
                    std::format("face {} references out-of-range vertex {}", f, v));
            }
        }
        if (face.owner >= nc) {
            throw ValidityError(
                std::format("face {} references out-of-range owner cell {}", f, face.owner));
        }
        if (face.neighbour && *face.neighbour >= nc) {
            throw ValidityError(std::format(
                "face {} references out-of-range neighbour cell {}", f, *face.neighbour));
        }
        if (face.neighbour && *face.neighbour == face.owner) {
            throw ValidityError(std::format("face {} has the same owner and neighbour cell {}",
                                            f, face.owner));
        }
    }
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const Cell& cell = cells[c];
        if (cell.faces.size() < 4) {
            throw ValidityError(
                std::format("cell {} has fewer than 4 faces (cannot bound a volume)", c));
        }
        std::set<FaceId> unique_faces;
        for (const auto f : cell.faces) {
            if (f >= faces.size()) {
                throw ValidityError(
                    std::format("cell {} references out-of-range face {}", c, f));
            }
            if (!unique_faces.insert(f).second) {
                throw ValidityError(std::format("cell {} lists face {} more than once", c, f));
            }
            const Face& face = faces[f];
            const auto cid = static_cast<CellId>(c);
            if (face.owner != cid && face.neighbour != cid) {
                throw ValidityError(std::format(
                    "cell {} lists face {} which does not reference it back", c, f));
            }
            ++face_cell_references[f];
        }
    }
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const int expected = faces[f].neighbour ? 2 : 1;
        if (face_cell_references[f] != expected) {
            throw ValidityError(
                std::format("face {} is listed by {} cells but ownership requires {}", f,
                            face_cell_references[f], expected));
        }
    }
}

void PolyMesh::triangulate_boundary_incident_faces() {
    check_validity();
    std::vector<char> boundary_vertex(vertices.size(), 0);
    for (const Face& face : faces) {
        if (face.neighbour) {
            continue;
        }
        for (const VertexId vertex : face.vertices) {
            boundary_vertex[vertex] = 1;
        }
    }

    std::vector<Face> triangulated;
    triangulated.reserve(faces.size());
    std::vector<std::vector<FaceId>> cell_faces(cells.size());
    const auto append = [&](Face face) {
        const FaceId id = static_cast<FaceId>(triangulated.size());
        cell_faces[face.owner].push_back(id);
        if (face.neighbour) {
            cell_faces[*face.neighbour].push_back(id);
        }
        triangulated.push_back(std::move(face));
    };
    for (const Face& face : faces) {
        const bool touches_boundary =
            std::any_of(face.vertices.begin(), face.vertices.end(),
                        [&](VertexId vertex) { return boundary_vertex[vertex] != 0; });
        if (!touches_boundary || face.vertices.size() <= 3) {
            append(face);
            continue;
        }
        const auto triangles = triangulate_simple_polygon(*this, face);
        if (triangles.size() + 2 != face.vertices.size()) {
            throw ValidityError(
                "cannot triangulate a boundary-incident polygon without degeneracy");
        }
        for (const auto& triangle : triangles) {
            append(Face{.vertices = {triangle[0], triangle[1], triangle[2]},
                        .owner = face.owner,
                        .neighbour = face.neighbour});
        }
    }
    faces = std::move(triangulated);
    for (std::size_t c = 0; c < cells.size(); ++c) {
        cells[c].faces = std::move(cell_faces[c]);
    }
}

void PolyMesh::check_geometry() const {
    check_validity();
    if (vertices.empty()) {
        return;
    }

    Eigen::Vector3d mesh_min = vertices.front();
    Eigen::Vector3d mesh_max = vertices.front();
    for (const Eigen::Vector3d& vertex : vertices) {
        mesh_min = mesh_min.cwiseMin(vertex);
        mesh_max = mesh_max.cwiseMax(vertex);
    }
    const double mesh_scale = std::max((mesh_max - mesh_min).norm(), 1e-30);
    const double mesh_linear_tol = 2e-9 * mesh_scale;

    std::vector<FaceGeometry> geometry;
    geometry.reserve(faces.size());
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const Face& face = faces[f];
        std::set<VertexId> unique_vertices(face.vertices.begin(), face.vertices.end());
        if (unique_vertices.size() != face.vertices.size()) {
            throw ValidityError(
                std::format("face {} repeats a vertex in its polygon loop", f));
        }
        FaceGeometry fg = face_geometry(*this, face);
        const double area_tol =
            1e-14 * std::max(fg.diameter * fg.diameter, mesh_scale * mesh_scale * 1e-24);
        if (!(fg.diameter > 0.0) || !(fg.area.norm() > area_tol)) {
            throw ValidityError(std::format("face {} has zero geometric area", f));
        }
        const Eigen::Vector3d normal = fg.area.normalized();
        const double planarity_tol = 1e-8 * fg.diameter + mesh_linear_tol;
        for (const VertexId vertex : face.vertices) {
            const double offset = std::abs((vertices[vertex] - fg.centroid).dot(normal));
            if (offset > planarity_tol) {
                throw ValidityError(
                    std::format("face {} is nonplanar by {:.3e} m (tolerance {:.3e} m)", f,
                                offset, planarity_tol));
            }
        }
        if (!polygon_is_simple(*this, face, fg)) {
            throw ValidityError(
                std::format("face {} has a self-intersecting polygon loop", f));
        }
        geometry.push_back(fg);
    }

    // Closed manifold exterior: every undirected edge of the boundary
    // triangulation appears in exactly two exterior faces.
    std::map<std::pair<VertexId, VertexId>, int> boundary_edge_count;
    for (const Face& face : faces) {
        if (face.neighbour) {
            continue;
        }
        for (std::size_t i = 0; i < face.vertices.size(); ++i) {
            VertexId a = face.vertices[i];
            VertexId b = face.vertices[(i + 1) % face.vertices.size()];
            if (a > b) {
                std::swap(a, b);
            }
            ++boundary_edge_count[{a, b}];
        }
    }
    for (const auto& [edge, count] : boundary_edge_count) {
        if (count != 2) {
            throw ValidityError(std::format(
                "boundary edge ({},{}) appears {} times (want 2 for closed manifold)",
                edge.first, edge.second, count));
        }
    }

    struct EdgeUse {
        int count = 0;
        int direction_balance = 0;
        std::vector<std::size_t> incident_faces;
    };
    std::vector<Eigen::Vector3d> cell_mins(cells.size());
    std::vector<Eigen::Vector3d> cell_maxs(cells.size());
    std::vector<std::vector<VertexId>> cell_vertex_ids(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const CellId cid = static_cast<CellId>(c);
        const Cell& cell = cells[c];
        std::map<std::pair<VertexId, VertexId>, EdgeUse> edge_uses;
        std::set<VertexId> cell_vertices;
        for (std::size_t local_face = 0; local_face < cell.faces.size(); ++local_face) {
            const Face& face = faces[cell.faces[local_face]];
            const bool owner = face.owner == cid;
            for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                const std::size_t next = (i + 1) % face.vertices.size();
                const VertexId a = owner ? face.vertices[i] : face.vertices[next];
                const VertexId b = owner ? face.vertices[next] : face.vertices[i];
                const auto key = std::minmax(a, b);
                EdgeUse& use = edge_uses[{key.first, key.second}];
                ++use.count;
                use.direction_balance += a < b ? 1 : -1;
                use.incident_faces.push_back(local_face);
                cell_vertices.insert(a);
                cell_vertices.insert(b);
            }
        }
        if (cell_vertices.size() < 4) {
            throw ValidityError(std::format("cell {} has fewer than 4 geometric vertices", c));
        }
        std::vector<std::vector<std::size_t>> adjacency(cell.faces.size());
        for (const auto& [edge, use] : edge_uses) {
            if (use.count != 2 || use.direction_balance != 0 ||
                use.incident_faces.size() != 2) {
                throw ValidityError(
                    std::format("cell {} edge ({},{}) has {} uses with direction balance {}",
                                c, edge.first, edge.second, use.count, use.direction_balance));
            }
            const std::size_t a = use.incident_faces[0];
            const std::size_t b = use.incident_faces[1];
            adjacency[a].push_back(b);
            adjacency[b].push_back(a);
        }
        std::vector<char> visited(cell.faces.size(), 0);
        std::vector<std::size_t> stack{0};
        visited[0] = 1;
        std::size_t n_visited = 0;
        while (!stack.empty()) {
            const std::size_t current = stack.back();
            stack.pop_back();
            ++n_visited;
            for (const std::size_t next : adjacency[current]) {
                if (!visited[next]) {
                    visited[next] = 1;
                    stack.push_back(next);
                }
            }
        }
        if (n_visited != cell.faces.size()) {
            throw ValidityError(
                std::format("cell {} has disconnected closed-shell components", c));
        }

        for (std::size_t i = 0; i < cell.faces.size(); ++i) {
            const FaceId fa_id = cell.faces[i];
            const Face& fa = faces[fa_id];
            for (std::size_t j = i + 1; j < cell.faces.size(); ++j) {
                const FaceId fb_id = cell.faces[j];
                const Face& fb = faces[fb_id];
                const Eigen::Vector3d face_overlap =
                    geometry[fa_id].max.cwiseMin(geometry[fb_id].max) -
                    geometry[fa_id].min.cwiseMax(geometry[fb_id].min);
                if ((face_overlap.array() < -mesh_linear_tol).any()) {
                    continue;
                }
                if (coplanar_polygons_overlap(*this, fa, geometry[fa_id], fb, geometry[fb_id],
                                              mesh_linear_tol)) {
                    throw ValidityError(std::format(
                        "cell {} faces {} and {} overlap in one plane", c, fa_id, fb_id));
                }
                bool intersects = false;
                for (std::size_t e = 0; e < fa.vertices.size() && !intersects; ++e) {
                    intersects = segment_hits_face_interior(
                        vertices[fa.vertices[e]],
                        vertices[fa.vertices[(e + 1) % fa.vertices.size()]], *this, fb,
                        geometry[fb_id], mesh_linear_tol);
                }
                for (std::size_t e = 0; e < fb.vertices.size() && !intersects; ++e) {
                    intersects = segment_hits_face_interior(
                        vertices[fb.vertices[e]],
                        vertices[fb.vertices[(e + 1) % fb.vertices.size()]], *this, fa,
                        geometry[fa_id], mesh_linear_tol);
                }
                if (intersects) {
                    throw ValidityError(
                        std::format("cell {} faces {} and {} intersect away from shared edges",
                                    c, fa_id, fb_id));
                }
            }
        }

        Eigen::Vector3d cell_min = vertices[*cell_vertices.begin()];
        Eigen::Vector3d cell_max = cell_min;
        for (const VertexId vertex : cell_vertices) {
            cell_min = cell_min.cwiseMin(vertices[vertex]);
            cell_max = cell_max.cwiseMax(vertices[vertex]);
        }
        cell_mins[c] = cell_min;
        cell_maxs[c] = cell_max;
        cell_vertex_ids[c].assign(cell_vertices.begin(), cell_vertices.end());
        double volume = 0.0;
        for (const FaceId face_id : cell.faces) {
            const double sign = faces[face_id].owner == cid ? 1.0 : -1.0;
            volume += sign *
                      (geometry[face_id].centroid - cell_min).dot(geometry[face_id].area) /
                      3.0;
        }
        const double cell_scale = std::max((cell_max - cell_min).norm(), 1e-30);
        const double volume_tol = 1e-14 * cell_scale * cell_scale * cell_scale;
        if (!(volume > volume_tol)) {
            throw ValidityError(std::format(
                "cell {} has non-positive or collapsed volume {:.3e} m^3", c, volume));
        }
        if (cell.kind == CellKind::kTet && cell_vertices.size() != 4) {
            throw ValidityError(
                std::format("tet cell {} does not reference exactly 4 unique vertices", c));
        }
    }

    // A valid cell shell says nothing about how distinct cells occupy space.
    // Prune disjoint (and merely touching) cell boxes, then reject positive-area
    // face overlap, transverse face crossings, and strict containment.
    std::vector<std::size_t> cell_order(cells.size());
    for (std::size_t c = 0; c < cells.size(); ++c) {
        cell_order[c] = c;
    }
    std::sort(cell_order.begin(), cell_order.end(), [&](std::size_t a, std::size_t b) {
        return cell_mins[a].x() < cell_mins[b].x();
    });
    for (std::size_t ai = 0; ai < cell_order.size(); ++ai) {
        const std::size_t a = cell_order[ai];
        for (std::size_t bi = ai + 1; bi < cell_order.size(); ++bi) {
            const std::size_t b = cell_order[bi];
            if (cell_mins[b].x() >= cell_maxs[a].x()) {
                break;
            }
            const Eigen::Vector3d overlap =
                cell_maxs[a].cwiseMin(cell_maxs[b]) - cell_mins[a].cwiseMax(cell_mins[b]);
            const double pair_scale = std::max((cell_maxs[a] - cell_mins[a]).norm(),
                                               (cell_maxs[b] - cell_mins[b]).norm());
            const double pair_linear_tol =
                std::max(mesh_linear_tol, 2e-9 * std::max(pair_scale, 1e-30));
            if ((overlap.array() <= pair_linear_tol).any()) {
                continue;
            }

            const bool topological_neighbours =
                std::any_of(cells[a].faces.begin(), cells[a].faces.end(), [&](FaceId face_id) {
                    return std::find(cells[b].faces.begin(), cells[b].faces.end(), face_id) !=
                           cells[b].faces.end();
                });
            for (const FaceId fa_id : cells[a].faces) {
                const Face& fa = faces[fa_id];
                for (const FaceId fb_id : cells[b].faces) {
                    if (fa_id == fb_id) {
                        continue; // one topological face shared by legitimate neighbours
                    }
                    const Face& fb = faces[fb_id];
                    const Eigen::Vector3d face_overlap =
                        geometry[fa_id].max.cwiseMin(geometry[fb_id].max) -
                        geometry[fa_id].min.cwiseMax(geometry[fb_id].min);
                    if ((face_overlap.array() < -pair_linear_tol).any()) {
                        continue;
                    }
                    if (coplanar_polygons_overlap(*this, fa, geometry[fa_id], fb,
                                                  geometry[fb_id], pair_linear_tol)) {
                        throw ValidityError(
                            std::format("cells {} and {} have overlapping faces {} and {}", a,
                                        b, fa_id, fb_id));
                    }
                    bool intersects = false;
                    for (std::size_t e = 0; e < fa.vertices.size() && !intersects; ++e) {
                        intersects = segment_hits_face_interior(
                            vertices[fa.vertices[e]],
                            vertices[fa.vertices[(e + 1) % fa.vertices.size()]], *this, fb,
                            geometry[fb_id], pair_linear_tol);
                    }
                    for (std::size_t e = 0; e < fb.vertices.size() && !intersects; ++e) {
                        intersects = segment_hits_face_interior(
                            vertices[fb.vertices[e]],
                            vertices[fb.vertices[(e + 1) % fb.vertices.size()]], *this, fa,
                            geometry[fa_id], pair_linear_tol);
                    }
                    if (intersects) {
                        std::size_t shared_vertices = 0;
                        for (const VertexId va : fa.vertices) {
                            shared_vertices += static_cast<std::size_t>(
                                std::find(fb.vertices.begin(), fb.vertices.end(), va) !=
                                fb.vertices.end());
                        }
                        throw ValidityError(std::format(
                            "cells {} and {} have crossing faces {} and {} "
                            "(topological_neighbours={}, shared_vertices={})",
                            a, b, fa_id, fb_id, topological_neighbours, shared_vertices));
                    }
                }
            }

            // After all face pairs have been proven noncrossing, two connected
            // closed shells are either disjoint or one contains the other.
            // One representative vertex from each shell therefore decides
            // containment. Topological neighbours already share their legal
            // contact face and cannot contain one another without another face
            // crossing, so they need no solid-angle evaluation.
            if (!topological_neighbours) {
                const CellId aid = static_cast<CellId>(a);
                const CellId bid = static_cast<CellId>(b);
                const VertexId a_vertex = cell_vertex_ids[a].front();
                if (point_in_cell_strict(vertices[a_vertex], *this, cells[b], bid, geometry,
                                         pair_linear_tol)) {
                    throw ValidityError(
                        std::format("cell {} contains a vertex of cell {}", b, a));
                }
                const VertexId b_vertex = cell_vertex_ids[b].front();
                if (point_in_cell_strict(vertices[b_vertex], *this, cells[a], aid, geometry,
                                         pair_linear_tol)) {
                    throw ValidityError(
                        std::format("cell {} contains a vertex of cell {}", a, b));
                }
            }
        }
    }
}

} // namespace polymesh::mesh
