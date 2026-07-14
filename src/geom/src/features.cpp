// SPDX-License-Identifier: BSD-3-Clause
#include "geom/features.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::geom {
namespace {

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

[[maybe_unused]] double point_segment_distance(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                                               const Eigen::Vector3d& b) {
    const Eigen::Vector3d ab = b - a;
    const double denom = ab.squaredNorm();
    if (denom == 0.0) {
        return (p - a).norm();
    }
    const double t = std::clamp((p - a).dot(ab) / denom, 0.0, 1.0);
    return (p - (a + t * ab)).norm();
}

// Uniform grid over sharp-edge AABBs for accelerated closest-feature queries
// (mirrors mesh::SurfaceGrid). A real CAD part has ~10^4 sharp edges; brute
// force per boundary node dominates snap/smooth without this.
struct EdgeGrid {
    Eigen::Vector3d origin = Eigen::Vector3d::Zero();
    Eigen::Vector3d cell = Eigen::Vector3d::Ones();
    int nx = 1, ny = 1, nz = 1;
    std::vector<std::vector<std::size_t>> bins;

    int flat(int i, int j, int k) const { return (k * ny + j) * nx + i; }

    void build(const TriSurface& surface, std::span<const SharpEdge> edges) {
        bins.clear();
        if (edges.empty() || surface.vertices.empty()) {
            return;
        }
        Eigen::Vector3d bmin = surface.vertices[0];
        Eigen::Vector3d bmax = surface.vertices[0];
        bool any = false;
        for (const auto& e : edges) {
            if (e.v0 >= surface.vertices.size() || e.v1 >= surface.vertices.size()) {
                continue;
            }
            bmin = bmin.cwiseMin(surface.vertices[e.v0]).cwiseMin(surface.vertices[e.v1]);
            bmax = bmax.cwiseMax(surface.vertices[e.v0]).cwiseMax(surface.vertices[e.v1]);
            any = true;
        }
        if (!any) {
            return;
        }
        const Eigen::Vector3d extent = (bmax - bmin).cwiseMax(Eigen::Vector3d::Constant(1e-12));
        const double pad = 1e-6 * extent.norm() + 1e-12;
        bmin.array() -= pad;
        bmax.array() += pad;
        const double ne_d = static_cast<double>(std::max<std::size_t>(1, edges.size() / 4));
        const int res = std::clamp(static_cast<int>(std::cbrt(ne_d)), 4, 64);
        nx = ny = nz = res;
        origin = bmin;
        cell = (bmax - bmin)
                   .cwiseQuotient(Eigen::Vector3d(nx, ny, nz))
                   .cwiseMax(Eigen::Vector3d::Constant(1e-30));
        bins.assign(static_cast<std::size_t>(nx * ny * nz), {});
        for (std::size_t idx = 0; idx < edges.size(); ++idx) {
            const auto& e = edges[idx];
            if (e.v0 >= surface.vertices.size() || e.v1 >= surface.vertices.size()) {
                continue;
            }
            const Eigen::Vector3d& A = surface.vertices[e.v0];
            const Eigen::Vector3d& B = surface.vertices[e.v1];
            const Eigen::Vector3d lomin = (A.cwiseMin(B) - origin).cwiseQuotient(cell);
            const Eigen::Vector3d lomax = (A.cwiseMax(B) - origin).cwiseQuotient(cell);
            const int i0 = std::clamp(static_cast<int>(std::floor(lomin[0])), 0, nx - 1);
            const int j0 = std::clamp(static_cast<int>(std::floor(lomin[1])), 0, ny - 1);
            const int k0 = std::clamp(static_cast<int>(std::floor(lomin[2])), 0, nz - 1);
            const int i1 = std::clamp(static_cast<int>(std::floor(lomax[0])), 0, nx - 1);
            const int j1 = std::clamp(static_cast<int>(std::floor(lomax[1])), 0, ny - 1);
            const int k1 = std::clamp(static_cast<int>(std::floor(lomax[2])), 0, nz - 1);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        bins[static_cast<std::size_t>(flat(i, j, k))].push_back(idx);
                    }
                }
            }
        }
    }

    ClosestOnFeature query(const TriSurface& surface, std::span<const SharpEdge> edges,
                           const Eigen::Vector3d& p) const {
        ClosestOnFeature best;
        if (bins.empty()) {
            return best;
        }
        auto consider = [&](std::size_t idx) {
            const auto& e = edges[idx];
            const Eigen::Vector3d& a = surface.vertices[e.v0];
            const Eigen::Vector3d& b = surface.vertices[e.v1];
            const Eigen::Vector3d ab = b - a;
            const double denom = ab.squaredNorm();
            Eigen::Vector3d q = a;
            if (denom > 0.0) {
                const double t = std::clamp((p - a).dot(ab) / denom, 0.0, 1.0);
                q = a + t * ab;
            }
            const double d = (p - q).norm();
            if (d < best.distance) {
                best.distance = d;
                best.point = q;
            }
        };
        const Eigen::Vector3d local = (p - origin).cwiseQuotient(cell);
        const int ic = std::clamp(static_cast<int>(std::floor(local[0])), 0, nx - 1);
        const int jc = std::clamp(static_cast<int>(std::floor(local[1])), 0, ny - 1);
        const int kc = std::clamp(static_cast<int>(std::floor(local[2])), 0, nz - 1);
        const int max_r = std::max({nx, ny, nz});
        const double min_cell = std::min({cell[0], cell[1], cell[2]});
        for (int r = 0; r <= max_r; ++r) {
            const int i0 = std::max(0, ic - r), i1 = std::min(nx - 1, ic + r);
            const int j0 = std::max(0, jc - r), j1 = std::min(ny - 1, jc + r);
            const int k0 = std::max(0, kc - r), k1 = std::min(nz - 1, kc + r);
            for (int k = k0; k <= k1; ++k) {
                for (int j = j0; j <= j1; ++j) {
                    for (int i = i0; i <= i1; ++i) {
                        if (r > 0) {
                            const bool on_shell = (i == i0 || i == i1 || j == j0 ||
                                                   j == j1 || k == k0 || k == k1);
                            if (!on_shell) {
                                continue;
                            }
                        }
                        for (const std::size_t idx :
                             bins[static_cast<std::size_t>(flat(i, j, k))]) {
                            consider(idx);
                        }
                    }
                }
            }
            if (r > 0 && std::isfinite(best.distance) &&
                best.distance <= static_cast<double>(r) * min_cell) {
                break;
            }
        }
        return best;
    }
};

ClosestOnFeature closest_on_features_brute(const Eigen::Vector3d& p, const TriSurface& surface,
                                           std::span<const SharpEdge> edges) {
    ClosestOnFeature best;
    for (const auto& e : edges) {
        if (e.v0 >= surface.vertices.size() || e.v1 >= surface.vertices.size()) {
            continue;
        }
        const Eigen::Vector3d& a = surface.vertices[e.v0];
        const Eigen::Vector3d& b = surface.vertices[e.v1];
        const Eigen::Vector3d ab = b - a;
        const double denom = ab.squaredNorm();
        Eigen::Vector3d q = a;
        if (denom > 0.0) {
            const double t = std::clamp((p - a).dot(ab) / denom, 0.0, 1.0);
            q = a + t * ab;
        }
        const double d = (p - q).norm();
        if (d < best.distance) {
            best.distance = d;
            best.point = q;
        }
    }
    return best;
}

const EdgeGrid& feature_grid_for(const TriSurface& surface, std::span<const SharpEdge> edges) {
    thread_local const SharpEdge* cached_data = nullptr;
    thread_local std::size_t cached_n = 0;
    thread_local const void* cached_surf = nullptr;
    thread_local std::size_t cached_nv = 0;
    thread_local EdgeGrid cached;
    if (cached_data != edges.data() || cached_n != edges.size() ||
        cached_surf != static_cast<const void*>(&surface) ||
        cached_nv != surface.vertices.size()) {
        cached.build(surface, edges);
        cached_data = edges.data();
        cached_n = edges.size();
        cached_surf = static_cast<const void*>(&surface);
        cached_nv = surface.vertices.size();
    }
    return cached;
}

} // namespace

std::vector<SharpEdge> detect_sharp_edges(const TriSurface& surface, double sharp_angle_deg) {
    surface.validate();
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

    const double cos_thresh = std::cos(sharp_angle_deg * 3.14159265358979323846 / 180.0);
    std::vector<SharpEdge> out;
    for (const auto& [key, tris] : edge_tris) {
        if (tris.size() == 1) {
            out.push_back(SharpEdge{key.first, key.second, 0.0});
            continue;
        }
        if (tris.size() != 2) {
            continue; // non-manifold — treat as feature
        }
        const Eigen::Vector3d n0 = tri_normal(surface, tris[0]);
        const Eigen::Vector3d n1 = tri_normal(surface, tris[1]);
        const double c = std::clamp(n0.dot(n1), -1.0, 1.0);
        // Flat ⇒ cos≈1. Sharp crease ⇒ cos smaller.
        if (c < cos_thresh) {
            out.push_back(SharpEdge{key.first, key.second, std::acos(c)});
        }
    }
    return out;
}

double distance_to_features(const Eigen::Vector3d& p, const TriSurface& surface,
                            std::span<const SharpEdge> edges) {
    return closest_on_features(p, surface, edges).distance;
}

ClosestOnFeature closest_on_features(const Eigen::Vector3d& p, const TriSurface& surface,
                                     std::span<const SharpEdge> edges) {
    if (edges.size() < 32) {
        return closest_on_features_brute(p, surface, edges);
    }
    const auto& g = feature_grid_for(surface, edges);
    if (g.bins.empty()) {
        return closest_on_features_brute(p, surface, edges);
    }
    ClosestOnFeature best = g.query(surface, edges, p);
    if (!std::isfinite(best.distance)) {
        return closest_on_features_brute(p, surface, edges);
    }
    return best;
}

} // namespace polymesh::geom
