// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_lloyd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "Delaunay_psm.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

namespace polymesh::mesh {
namespace {

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM

/// Bisector halfspace: points closer to `site` than to `other`.
/// Keep (site - other)·(x - mid) ≥ 0.
ClipPlane bisector_keep_site(const Eigen::Vector3d& site,
                             const Eigen::Vector3d& other) {
    const Eigen::Vector3d n = site - other;
    const Eigen::Vector3d mid = 0.5 * (site + other);
    ClipPlane pl;
    pl.a = n.x();
    pl.b = n.y();
    pl.c = n.z();
    pl.d = -n.dot(mid);
    return pl;
}

/// Build restricted Voronoi cell; returns false if empty / dead.
bool build_rvd_cell(VBW::ConvexCell& cell, const ClipBox& box,
                    const Eigen::Vector3d& site,
                    std::span<const Eigen::Vector3d> others) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());
    for (const Eigen::Vector3d& o : others) {
        const Eigen::Vector3d d = site - o;
        if (d.squaredNorm() < 1e-30) {
            continue;  // coincident — skip (caller should unique sites)
        }
        const ClipPlane pl = bisector_keep_site(site, o);
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d));
        if (cell.empty()) {
            return false;
        }
    }
    return !cell.empty();
}

inline Eigen::Vector3d to_eigen(VBW::vec3 v) {
    return Eigen::Vector3d(v.x, v.y, v.z);
}

inline double tet_volume_abs(const VBW::vec3& p, const VBW::vec3& q,
                             const VBW::vec3& r, const VBW::vec3& s) {
    // Same orientation formula as Geogram tet_volume (absolute).
    const double Ux = q.x - p.x, Uy = q.y - p.y, Uz = q.z - p.z;
    const double Vx = r.x - p.x, Vy = r.y - p.y, Vz = r.z - p.z;
    const double Wx = s.x - p.x, Wy = s.y - p.y, Wz = s.z - p.z;
    const double UVx = Uy * Vz - Uz * Vy;
    const double UVy = Uz * Vx - Ux * Vz;
    const double UVz = Ux * Vy - Uy * Vx;
    return std::fabs(UVx * Wx + UVy * Wy + UVz * Wz) / 6.0;
}

/// Density-weighted mass / first moment over the VBW tet fan (ρ at tet bary).
void density_mg(const VBW::ConvexCell& cell, const SizeFieldFn& size_at,
                double h_floor, double& mass, Eigen::Vector3d& mg) {
    mass = 0.0;
    mg.setZero();

    const bool uniform = !size_at;

    VBW::ushort t_origin = VBW::END_OF_LIST;
    for (VBW::index_t v = 0; v < cell.nb_v(); ++v) {
        if (cell.vertex_triangle(v) == VBW::END_OF_LIST) {
            continue;
        }
        if (t_origin == VBW::END_OF_LIST) {
            t_origin = static_cast<VBW::ushort>(cell.vertex_triangle(v));
            continue;
        }
        VBW::ushort t1t2[2];
        VBW::index_t cur = 0;
        VBW::index_t t = cell.vertex_triangle(v);
        VBW::index_t count = 0;
        do {
            if (cur < 2) {
                t1t2[cur] = static_cast<VBW::ushort>(t);
            } else {
                const VBW::vec3 p = cell.triangle_point(t_origin);
                const VBW::vec3 q = cell.triangle_point(t1t2[0]);
                const VBW::vec3 r = cell.triangle_point(t1t2[1]);
                const VBW::vec3 s = cell.triangle_point(static_cast<VBW::ushort>(t));
                const double vol = tet_volume_abs(p, q, r, s);
                if (vol > 0.0) {
                    const Eigen::Vector3d bary(
                        0.25 * (p.x + q.x + r.x + s.x),
                        0.25 * (p.y + q.y + r.y + s.y),
                        0.25 * (p.z + q.z + r.z + s.z));
                    double rho = 1.0;
                    if (!uniform) {
                        rho = density_from_size(size_at(bary), h_floor);
                    }
                    const double w = rho * vol;
                    mass += w;
                    mg += w * bary;
                }
                t1t2[1] = static_cast<VBW::ushort>(t);
            }
            ++cur;
            const VBW::index_t lv = cell.triangle_find_vertex(t, v);
            t = cell.triangle_adjacent(t, (lv + 1) % 3);
            ++count;
            if (count > 100000) {
                break;
            }
        } while (t != cell.vertex_triangle(v));
    }
}

/// Neighbour-restricted RVD cell: clip `positions[self]` against sites gathered
/// from `grid` ring by ring, stopping when the security radius guarantees no
/// farther site can cut the cell. Produces the exact restricted Voronoi cell
/// (same as clipping against all sites) in O(neighbours) instead of O(N).
bool build_rvd_cell_grid(VBW::ConvexCell& cell, const ClipBox& box,
                         std::span<const Eigen::Vector3d> positions,
                         std::size_t self, const SiteGrid& grid,
                         std::vector<std::uint32_t>& ring_buf) {
    if ((box.max.array() <= box.min.array()).any()) {
        return false;
    }
    cell.clear();
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());
    const Eigen::Vector3d& s = positions[self];
    const VBW::vec3 sc = VBW::make_vec3(s.x(), s.y(), s.z());
    const double g = grid.cell_edge();
    const int kmax = grid.max_ring();
    for (int k = 0; k <= kmax; ++k) {
        ring_buf.clear();
        grid.ring(s, k, ring_buf);
        for (std::uint32_t j : ring_buf) {
            if (static_cast<std::size_t>(j) == self) {
                continue;
            }
            const Eigen::Vector3d d = s - positions[j];
            if (d.squaredNorm() < 1e-30) {
                continue;  // coincident — skip (caller should unique sites)
            }
            const ClipPlane pl = bisector_keep_site(s, positions[j]);
            cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d));
            if (cell.empty()) {
                return false;
            }
        }
        if (k >= 1) {
            // Any unclipped site is at Euclidean distance ≥ k·g; its bisector
            // sits ≥ k·g/2 from the site. If that exceeds the cell's farthest
            // vertex, no farther site can cut it — done (security radius).
            const double R2 = cell.squared_radius(sc);
            const double dmin = static_cast<double>(k) * g;
            if (4.0 * R2 <= dmin * dmin) {
                break;
            }
        }
    }
    return !cell.empty();
}

#endif  // POLYMESH_WITH_GEOGRAM

}  // namespace

std::optional<ClippedCell> restricted_voronoi_cell(
    const ClipBox& box, const Eigen::Vector3d& site,
    std::span<const Eigen::Vector3d> others) {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    geogram_ensure_initialized();
    VBW::ConvexCell cell;
    if (!build_rvd_cell(cell, box, site, others)) {
        ClippedCell out;
        out.empty = true;
        out.volume = 0.0;
        return out;
    }
    cell.compute_geometry();
    const VBW::vec3 b = cell.barycenter();
    ClippedCell out;
    out.volume = cell.volume();
    out.barycenter = to_eigen(b);
    out.n_triangles = static_cast<std::size_t>(cell.nb_t());
    out.n_planes = static_cast<std::size_t>(cell.nb_v());
    out.empty = (out.volume <= 0.0);
    return out;
#else
    (void)box;
    (void)site;
    (void)others;
    return std::nullopt;
#endif
}

std::optional<Eigen::Vector3d> restricted_voronoi_centroid(
    const ClipBox& box, const Eigen::Vector3d& site,
    std::span<const Eigen::Vector3d> others, const SizeFieldFn& size_at,
    double h_floor) {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    geogram_ensure_initialized();
    VBW::ConvexCell cell;
    if (!build_rvd_cell(cell, box, site, others)) {
        return std::nullopt;
    }
    cell.compute_geometry();
    if (cell.empty()) {
        return std::nullopt;
    }

    if (!size_at) {
        return to_eigen(cell.barycenter());
    }

    double mass = 0.0;
    Eigen::Vector3d mg = Eigen::Vector3d::Zero();
    density_mg(cell, size_at, h_floor, mass, mg);
    if (!(mass > 0.0) || !std::isfinite(mass)) {
        return to_eigen(cell.barycenter());
    }
    return mg / mass;
#else
    (void)box;
    (void)site;
    (void)others;
    (void)size_at;
    (void)h_floor;
    return std::nullopt;
#endif
}

CvtLloydResult lloyd_cvt(const ClipBox& domain, std::span<const CvtSite> sites,
                         const CvtLloydParams& params) {
    CvtLloydResult result;
    result.positions.resize(sites.size());
    for (std::size_t i = 0; i < sites.size(); ++i) {
        result.positions[i] = sites[i].pos;
    }

    result.stats.n_sites = sites.size();
    result.stats.domain_diag = (domain.max - domain.min).norm();
    for (const CvtSite& s : sites) {
        if (s.fixed) {
            ++result.stats.n_fixed;
        } else {
            ++result.stats.n_free;
        }
    }

#if !(defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM)
    result.stats.geogram_ok = false;
    return result;
#else
    if (!geogram_available() || sites.empty()) {
        result.stats.geogram_ok = geogram_available();
        return result;
    }
    result.stats.geogram_ok = true;
    geogram_ensure_initialized();

    const double tol =
        params.move_tol_rel * std::max(result.stats.domain_diag, 1e-30);
    const int max_iters = std::max(params.max_iters, 0);

    std::vector<Eigen::Vector3d> next = result.positions;

    // Bucket edge ≈ mean site spacing so a cell's Voronoi neighbours sit within
    // a couple of Chebyshev rings. Correctness is independent of this value
    // (security radius exhausts the grid); it only tunes how much pruning helps.
    const Eigen::Vector3d ext = (domain.max - domain.min).cwiseMax(1e-30);
    const double box_vol = ext.x() * ext.y() * ext.z();
    double g_edge = std::cbrt(box_vol / static_cast<double>(std::max<std::size_t>(1, sites.size())));
    if (!(g_edge > 0.0)) {
        g_edge = 0.25 * result.stats.domain_diag;
    }
    g_edge = std::max(g_edge, 1e-9 * std::max(result.stats.domain_diag, 1e-30));

    for (int it = 0; it < max_iters; ++it) {
        double max_move = 0.0;
        double sum_vol = 0.0;

        SiteGrid grid;
        grid.build(result.positions, g_edge);
        const auto n = static_cast<std::ptrdiff_t>(sites.size());

#pragma omp parallel
        {
            VBW::ConvexCell cell;
            std::vector<std::uint32_t> ring_buf;
#pragma omp for schedule(dynamic, 64) reduction(max : max_move) reduction(+ : sum_vol)
            for (std::ptrdiff_t ii = 0; ii < n; ++ii) {
                const auto i = static_cast<std::size_t>(ii);
                if (sites[i].fixed) {
                    next[i] = result.positions[i];
                    continue;
                }
                if (!build_rvd_cell_grid(cell, domain, result.positions, i, grid,
                                         ring_buf)) {
                    next[i] = result.positions[i];
                    continue;
                }
                cell.compute_geometry();
                if (cell.empty()) {
                    next[i] = result.positions[i];
                    continue;
                }

                Eigen::Vector3d c;
                if (!params.size_at) {
                    const VBW::vec3 b = cell.barycenter();
                    c = Eigen::Vector3d(b.x, b.y, b.z);
                } else {
                    double mass = 0.0;
                    Eigen::Vector3d mg = Eigen::Vector3d::Zero();
                    density_mg(cell, params.size_at, params.h_floor, mass, mg);
                    if (mass > 0.0 && std::isfinite(mass)) {
                        c = mg / mass;
                    } else {
                        const VBW::vec3 b = cell.barycenter();
                        c = Eigen::Vector3d(b.x, b.y, b.z);
                    }
                }

                // Keep free sites inside the domain box (restricted CVT).
                c = c.cwiseMax(domain.min).cwiseMin(domain.max);
                max_move = std::max(max_move, (c - result.positions[i]).norm());
                next[i] = c;
                sum_vol += cell.volume();
            }
        }

        result.positions.swap(next);
        result.stats.n_iters = it + 1;
        result.stats.max_move = max_move;
        result.stats.sum_cell_volume = sum_vol;

        if (max_move <= tol) {
            result.stats.converged = true;
            break;
        }
    }

    return result;
#endif
}

std::vector<CvtSite> seed_lattice_sites(const ClipBox& box, int n_side) {
    std::vector<CvtSite> out;
    n_side = std::max(n_side, 1);
    const Eigen::Vector3d ext = (box.max - box.min).cwiseMax(1e-30);
    out.reserve(static_cast<std::size_t>(n_side * n_side * n_side));
    for (int i = 0; i < n_side; ++i) {
        for (int j = 0; j < n_side; ++j) {
            for (int k = 0; k < n_side; ++k) {
                const double u = (static_cast<double>(i) + 0.5) /
                                 static_cast<double>(n_side);
                const double v = (static_cast<double>(j) + 0.5) /
                                 static_cast<double>(n_side);
                const double w = (static_cast<double>(k) + 0.5) /
                                 static_cast<double>(n_side);
                CvtSite s;
                s.pos = box.min + Eigen::Vector3d(u * ext.x(), v * ext.y(),
                                                  w * ext.z());
                s.fixed = false;
                out.push_back(s);
            }
        }
    }
    return out;
}

}  // namespace polymesh::mesh
