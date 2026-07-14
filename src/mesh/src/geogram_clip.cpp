// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/geogram_clip.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
// Upstream amalgam is not -Wpedantic / -Wconversion clean; silence at the include.
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
std::once_flag g_geo_once;

void init_geogram_once() {
    GEO::initialize(GEO::GEOGRAM_INSTALL_ALL);
}
#endif

}  // namespace

bool geogram_available() noexcept {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    return true;
#else
    return false;
#endif
}

void geogram_ensure_initialized() {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    std::call_once(g_geo_once, init_geogram_once);
#endif
}

std::optional<ClippedCell> clip_convex_cell(const ClipBox& box,
                                            std::span<const ClipPlane> planes) {
#if defined(POLYMESH_WITH_GEOGRAM) && POLYMESH_WITH_GEOGRAM
    geogram_ensure_initialized();

    if ((box.max.array() <= box.min.array()).any()) {
        return std::nullopt;
    }

    VBW::ConvexCell cell;
    cell.init_with_box(box.min.x(), box.min.y(), box.min.z(), box.max.x(),
                       box.max.y(), box.max.z());

    for (const ClipPlane& pl : planes) {
        cell.clip_by_plane(VBW::make_vec4(pl.a, pl.b, pl.c, pl.d));
    }

    // Empty / fully clipped cells have no valid triangles.
    if (cell.empty()) {
        ClippedCell out;
        out.empty = true;
        out.volume = 0.0;
        return out;
    }

    cell.compute_geometry();
    const VBW::vec3 b = cell.barycenter();

    ClippedCell out;
    out.volume = cell.volume();
    out.barycenter = Eigen::Vector3d(b.x, b.y, b.z);
    out.n_triangles = static_cast<std::size_t>(cell.nb_t());
    out.n_planes = static_cast<std::size_t>(cell.nb_v());
    out.empty = (out.volume <= 0.0);
    return out;
#else
    (void)box;
    (void)planes;
    return std::nullopt;
#endif
}

std::optional<ClippedCell> unit_cube_cell() {
    return clip_convex_cell(ClipBox{}, {});
}

// ---- SiteGrid (neighbour-restricted Voronoi clipping) -----------------------

void SiteGrid::build(std::span<const Eigen::Vector3d> pts, double cell_edge) {
    n_pts_ = pts.size();
    cell_ = (cell_edge > 1e-30) ? cell_edge : 1.0;
    inv_cell_ = 1.0 / cell_;
    cell_start_.clear();
    items_.clear();
    if (n_pts_ == 0) {
        nx_ = ny_ = nz_ = 0;
        max_ring_ = 0;
        return;
    }

    Eigen::Vector3d lo = pts[0];
    Eigen::Vector3d hi = pts[0];
    for (const auto& p : pts) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    origin_ = lo;
    const Eigen::Vector3d ext = (hi - lo).cwiseMax(0.0);
    nx_ = std::max(1, static_cast<int>(std::floor(ext.x() * inv_cell_)) + 1);
    ny_ = std::max(1, static_cast<int>(std::floor(ext.y() * inv_cell_)) + 1);
    nz_ = std::max(1, static_cast<int>(std::floor(ext.z() * inv_cell_)) + 1);
    max_ring_ = std::max({nx_, ny_, nz_});

    const std::size_t ncells = static_cast<std::size_t>(nx_) *
                               static_cast<std::size_t>(ny_) *
                               static_cast<std::size_t>(nz_);
    // Counting sort into CSR: cell_start_ holds per-bucket counts, then offsets.
    cell_start_.assign(ncells + 1, 0);
    auto bucket = [&](const Eigen::Vector3d& p) -> std::size_t {
        const int ix = clampi(static_cast<int>((p.x() - origin_.x()) * inv_cell_), nx_ - 1);
        const int iy = clampi(static_cast<int>((p.y() - origin_.y()) * inv_cell_), ny_ - 1);
        const int iz = clampi(static_cast<int>((p.z() - origin_.z()) * inv_cell_), nz_ - 1);
        return (static_cast<std::size_t>(iz) * static_cast<std::size_t>(ny_) +
                static_cast<std::size_t>(iy)) *
                   static_cast<std::size_t>(nx_) +
               static_cast<std::size_t>(ix);
    };
    for (const auto& p : pts) {
        ++cell_start_[bucket(p) + 1];
    }
    for (std::size_t c = 0; c < ncells; ++c) {
        cell_start_[c + 1] += cell_start_[c];
    }
    items_.resize(n_pts_);
    std::vector<std::uint32_t> cursor(cell_start_.begin(), cell_start_.end() - 1);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const std::size_t c = bucket(pts[i]);
        items_[cursor[c]++] = static_cast<std::uint32_t>(i);
    }
}

void SiteGrid::ring(const Eigen::Vector3d& p, int r, std::vector<std::uint32_t>& out) const {
    if (n_pts_ == 0 || r < 0) {
        return;
    }
    const int cx = clampi(static_cast<int>((p.x() - origin_.x()) * inv_cell_), nx_ - 1);
    const int cy = clampi(static_cast<int>((p.y() - origin_.y()) * inv_cell_), ny_ - 1);
    const int cz = clampi(static_cast<int>((p.z() - origin_.z()) * inv_cell_), nz_ - 1);
    auto emit = [&](int ix, int iy, int iz) {
        if (ix < 0 || ix >= nx_ || iy < 0 || iy >= ny_ || iz < 0 || iz >= nz_) {
            return;
        }
        const std::size_t c = (static_cast<std::size_t>(iz) * static_cast<std::size_t>(ny_) +
                               static_cast<std::size_t>(iy)) *
                                  static_cast<std::size_t>(nx_) +
                              static_cast<std::size_t>(ix);
        for (std::uint32_t k = cell_start_[c]; k < cell_start_[c + 1]; ++k) {
            out.push_back(items_[k]);
        }
    };
    if (r == 0) {
        emit(cx, cy, cz);
        return;
    }
    // Chebyshev shell: buckets with max(|dx|,|dy|,|dz|) == r. Walk the six faces
    // of the (2r+1)^3 cube without revisiting interior cells.
    for (int dz = -r; dz <= r; ++dz) {
        const bool z_face = (dz == -r || dz == r);
        for (int dy = -r; dy <= r; ++dy) {
            const bool y_face = (dy == -r || dy == r);
            if (z_face || y_face) {
                for (int dx = -r; dx <= r; ++dx) {
                    emit(cx + dx, cy + dy, cz + dz);
                }
            } else {
                emit(cx - r, cy + dy, cz + dz);
                emit(cx + r, cy + dy, cz + dz);
            }
        }
    }
}

}  // namespace polymesh::mesh
