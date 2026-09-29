// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to polymesh_mesh: the vendored Geogram PSM include (ADR-0025) and
// the bisector halfspace shared by the Lloyd loop and the Voronoi exports.
// ClipPlane convention: ConvexCell keeps a·x + b·y + c·z + d ≥ 0.

#include "mesh/geogram_clip.hpp"

#include <Eigen/Core>

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

/// Initialize Geogram process state once (safe to call repeatedly).
/// No-op when geogram is not compiled in.
void geogram_ensure_initialized();

namespace detail {

/// Bisector halfspace: points closer to `site` than to `other`.
/// Keep (site - other)·(x - mid) ≥ 0.
inline ClipPlane bisector_keep_site(const Eigen::Vector3d& site,
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

} // namespace detail
} // namespace polymesh::mesh
