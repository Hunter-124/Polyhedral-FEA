// SPDX-License-Identifier: BSD-3-Clause
#include "geom/indicators.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace polymesh::geom {
namespace {

constexpr double kPi = 3.14159265358979323846;

Eigen::Vector3d tri_normal(const TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    const Eigen::Vector3d n = ab.cross(ac);
    const double len = n.norm();
    if (len > 0.0) {
        return Eigen::Vector3d(n / len);
    }
    return Eigen::Vector3d(0.0, 0.0, 1.0);
}

double tri_area(const TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    return 0.5 * ab.cross(ac).norm();
}

/// Möller–Trumbore; returns true on hit with t > t_min along unit/non-unit dir.
bool ray_triangle_hit(const Eigen::Vector3d& orig, const Eigen::Vector3d& dir,
                      const Eigen::Vector3d& v0, const Eigen::Vector3d& v1,
                      const Eigen::Vector3d& v2, double t_min, double& t_out) {
    constexpr double kEps = 1e-12;
    const Eigen::Vector3d e1 = v1 - v0;
    const Eigen::Vector3d e2 = v2 - v0;
    const Eigen::Vector3d pvec = dir.cross(e2);
    const double det = e1.dot(pvec);
    if (std::abs(det) < kEps) {
        return false;
    }
    const double inv_det = 1.0 / det;
    const Eigen::Vector3d tvec = orig - v0;
    const double u = tvec.dot(pvec) * inv_det;
    if (u < 0.0 || u > 1.0) {
        return false;
    }
    const Eigen::Vector3d qvec = tvec.cross(e1);
    const double v = dir.dot(qvec) * inv_det;
    if (v < 0.0 || u + v > 1.0) {
        return false;
    }
    const double t = e2.dot(qvec) * inv_det;
    if (t <= t_min) {
        return false;
    }
    t_out = t;
    return true;
}

} // namespace

VertexCurvature estimate_vertex_curvature(const TriSurface& surface) {
    surface.validate();
    const std::size_t nv = surface.vertices.size();
    VertexCurvature out;
    out.kappa.assign(nv, 0.0);
    if (nv == 0 || surface.triangles.empty()) {
        return out;
    }

    // 1-ring area (barycentric: area/3 per corner) and integrated turning.
    std::vector<double> ring_area(nv, 0.0);
    std::vector<double> integrated(nv, 0.0);

    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const double a = tri_area(surface, t);
        const auto& tri = surface.triangles[t];
        for (int k = 0; k < 3; ++k) {
            ring_area[tri[static_cast<std::size_t>(k)]] += a / 3.0;
        }
    }

    using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<EdgeKey, std::vector<std::size_t>> edge_tris;
    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const auto& tri = surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            auto a = tri[static_cast<std::size_t>(e)];
            auto b = tri[static_cast<std::size_t>((e + 1) % 3)];
            if (a > b) {
                std::swap(a, b);
            }
            edge_tris[{a, b}].push_back(t);
        }
    }

    for (const auto& [key, tris] : edge_tris) {
        if (tris.size() != 2) {
            continue; // boundary or non-manifold: no smooth turning term
        }
        const Eigen::Vector3d n0 = tri_normal(surface, tris[0]);
        const Eigen::Vector3d n1 = tri_normal(surface, tris[1]);
        const double c = std::clamp(n0.dot(n1), -1.0, 1.0);
        const double theta = std::acos(c); // 0 = flat
        const double elen =
            (surface.vertices[key.first] - surface.vertices[key.second]).norm();
        const double contrib = theta * elen;
        integrated[key.first] += contrib;
        integrated[key.second] += contrib;
    }

    for (std::size_t i = 0; i < nv; ++i) {
        const double A = ring_area[i];
        if (A > 1e-30) {
            // |H| ≈ (Σ θ‖e‖) / (4 A) — order-correct for spheres (H = 1/R).
            out.kappa[i] = integrated[i] / (4.0 * A);
        }
    }
    return out;
}

VertexThickness estimate_local_thickness(const TriSurface& surface, double eps_scale) {
    surface.validate();
    const std::size_t nv = surface.vertices.size();
    VertexThickness out;
    out.thickness.assign(nv, std::numeric_limits<double>::infinity());
    if (nv == 0 || surface.triangles.empty()) {
        return out;
    }

    // Area-weighted vertex normals + mean edge length for epsilon.
    std::vector<Eigen::Vector3d> vnormal(nv, Eigen::Vector3d::Zero());
    double edge_len_sum = 0.0;
    std::size_t edge_count = 0;
    for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
        const auto& tri = surface.triangles[t];
        const double a = tri_area(surface, t);
        const Eigen::Vector3d n = tri_normal(surface, t);
        for (int k = 0; k < 3; ++k) {
            vnormal[tri[static_cast<std::size_t>(k)]] += a * n;
            const auto i0 = tri[static_cast<std::size_t>(k)];
            const auto i1 = tri[static_cast<std::size_t>((k + 1) % 3)];
            edge_len_sum += (surface.vertices[i0] - surface.vertices[i1]).norm();
            ++edge_count;
        }
    }
    const double mean_edge =
        edge_count > 0 ? edge_len_sum / static_cast<double>(edge_count) : 1.0;
    const double eps = std::max(eps_scale * mean_edge, 1e-12);

    for (std::size_t i = 0; i < nv; ++i) {
        Eigen::Vector3d n = vnormal[i];
        const double nlen = n.norm();
        if (nlen < 1e-30) {
            continue;
        }
        n /= nlen;
        // Inward: origin just inside, direction -n (assumes outward normals).
        const Eigen::Vector3d orig = surface.vertices[i] - n * eps;
        const Eigen::Vector3d dir = -n;

        double best_t = std::numeric_limits<double>::infinity();
        for (std::size_t t = 0; t < surface.triangles.size(); ++t) {
            const auto& tri = surface.triangles[t];
            // Skip triangles incident to this vertex (self-hit / grazing).
            if (tri[0] == i || tri[1] == i || tri[2] == i) {
                continue;
            }
            double hit_t = 0.0;
            if (ray_triangle_hit(orig, dir, surface.vertices[tri[0]], surface.vertices[tri[1]],
                                 surface.vertices[tri[2]], 0.5 * eps, hit_t)) {
                best_t = std::min(best_t, hit_t);
            }
        }
        if (std::isfinite(best_t)) {
            // Report thickness from the surface vertex (origin was offset by eps).
            out.thickness[i] = best_t + eps;
        }
    }
    return out;
}

std::uint32_t nearest_vertex_index(const TriSurface& surface, const Eigen::Vector3d& p) {
    if (surface.vertices.empty()) {
        return 0;
    }
    std::uint32_t best = 0;
    double best_d2 = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < surface.vertices.size(); ++i) {
        const double d2 = (surface.vertices[i] - p).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best = static_cast<std::uint32_t>(i);
        }
    }
    return best;
}

double distance_to_surface_vertices(const TriSurface& surface, const Eigen::Vector3d& p) {
    if (surface.vertices.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    const auto i = nearest_vertex_index(surface, p);
    return (surface.vertices[i] - p).norm();
}

} // namespace polymesh::geom
