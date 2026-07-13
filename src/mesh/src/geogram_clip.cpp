// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/geogram_clip.hpp"

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

}  // namespace polymesh::mesh
