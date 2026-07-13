// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_lloyd.hpp"

#include <algorithm>
#include <cmath>
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
    std::vector<Eigen::Vector3d> others;
    others.reserve(sites.size() > 0 ? sites.size() - 1 : 0);

    for (int it = 0; it < max_iters; ++it) {
        double max_move = 0.0;
        double sum_vol = 0.0;

        for (std::size_t i = 0; i < sites.size(); ++i) {
            if (sites[i].fixed) {
                next[i] = result.positions[i];
                continue;
            }

            others.clear();
            for (std::size_t j = 0; j < sites.size(); ++j) {
                if (j != i) {
                    others.push_back(result.positions[j]);
                }
            }

            auto centroid = restricted_voronoi_centroid(
                domain, result.positions[i], others, params.size_at,
                params.h_floor);
            if (!centroid.has_value()) {
                next[i] = result.positions[i];
                continue;
            }

            // Keep free sites inside the domain box (restricted CVT).
            Eigen::Vector3d p = *centroid;
            p = p.cwiseMax(domain.min).cwiseMin(domain.max);
            max_move = std::max(max_move, (p - result.positions[i]).norm());
            next[i] = p;

            if (auto cell = restricted_voronoi_cell(domain, result.positions[i],
                                                    others)) {
                sum_vol += cell->volume;
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
