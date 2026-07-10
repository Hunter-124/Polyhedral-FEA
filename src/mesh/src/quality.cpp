// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/quality.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

namespace polymesh::mesh {
namespace {

double tet_volume(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
                  const Eigen::Vector3d& d) {
    return (b - a).dot((c - a).cross(d - a)) / 6.0;
}

double tet_aspect(const Eigen::Vector3d& a, const Eigen::Vector3d& b, const Eigen::Vector3d& c,
                  const Eigen::Vector3d& d) {
    const double v = std::abs(tet_volume(a, b, c, d));
    if (v <= 0.0) {
        return 0.0;
    }
    const std::array<double, 6> e{(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                  (b - c).norm(), (b - d).norm(), (c - d).norm()};
    const double emax = *std::max_element(e.begin(), e.end());
    if (emax <= 0.0) {
        return 0.0;
    }
    constexpr double kNorm = 6.0 * 1.4142135623730951;
    return std::min(1.0, kNorm * v / (emax * emax * emax));
}

// Canonical face key: sorted node triple.
using FaceKey = std::array<std::uint32_t, 3>;

FaceKey make_face_key(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    FaceKey f{{a, b, c}};
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    if (f[1] > f[2]) {
        std::swap(f[1], f[2]);
    }
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    return f;
}

} // namespace

std::vector<double> tet4_aspect_ratios(const std::vector<Eigen::Vector3d>& nodes,
                                       const std::vector<std::array<std::uint32_t, 4>>& tets) {
    std::vector<double> out;
    out.reserve(tets.size());
    for (const auto& t : tets) {
        out.push_back(tet_aspect(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]));
    }
    return out;
}

TetQuality summarize_tet4_quality(const std::vector<Eigen::Vector3d>& nodes,
                                  const std::vector<std::array<std::uint32_t, 4>>& tets,
                                  double sliver_threshold) {
    TetQuality q;
    q.min_volume = std::numeric_limits<double>::infinity();
    q.min_aspect = std::numeric_limits<double>::infinity();
    double aspect_sum = 0.0;
    for (const auto& t : tets) {
        const double vol =
            std::abs(tet_volume(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]));
        const double asp = tet_aspect(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]);
        q.min_volume = std::min(q.min_volume, vol);
        q.max_volume = std::max(q.max_volume, vol);
        q.min_aspect = std::min(q.min_aspect, asp);
        aspect_sum += asp;
        if (asp < sliver_threshold) {
            ++q.n_sliver;
        }
    }
    if (tets.empty()) {
        q.min_volume = q.min_aspect = 0.0;
        return q;
    }
    q.mean_aspect = aspect_sum / static_cast<double>(tets.size());
    return q;
}

FaceConformityStats tet4_face_conformity(const std::vector<std::array<std::uint32_t, 4>>& tets) {
    FaceConformityStats s;
    s.n_tet_faces = tets.size() * 4;
    std::map<FaceKey, int> counts;
    static constexpr int kFaceVerts[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : tets) {
        for (const auto& fv : kFaceVerts) {
            const FaceKey key =
                make_face_key(t[static_cast<std::size_t>(fv[0])],
                              t[static_cast<std::size_t>(fv[1])],
                              t[static_cast<std::size_t>(fv[2])]);
            ++counts[key];
        }
    }
    s.n_unique_faces = counts.size();
    for (const auto& [key, c] : counts) {
        (void)key;
        if (c == 1) {
            ++s.n_boundary_faces;
        } else if (c == 2) {
            ++s.n_interior_faces;
        } else {
            ++s.n_nonconforming;
        }
    }
    // Topology-only: cannot see hanging faces (they look like boundary).
    s.is_conforming = (s.n_nonconforming == 0 && s.n_hanging_faces == 0);
    return s;
}

FaceConformityStats tet4_face_conformity(const std::vector<Eigen::Vector3d>& nodes,
                                         const std::vector<std::array<std::uint32_t, 4>>& tets,
                                         const Eigen::Vector3d& bbox_min,
                                         const Eigen::Vector3d& bbox_max,
                                         double surface_margin) {
    FaceConformityStats s;
    s.n_tet_faces = tets.size() * 4;
    std::map<FaceKey, std::pair<int, Eigen::Vector3d>> faces;
    static constexpr int kFaceVerts[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : tets) {
        for (const auto& fv : kFaceVerts) {
            const std::uint32_t a = t[static_cast<std::size_t>(fv[0])];
            const std::uint32_t b = t[static_cast<std::size_t>(fv[1])];
            const std::uint32_t c = t[static_cast<std::size_t>(fv[2])];
            const FaceKey key = make_face_key(a, b, c);
            auto& entry = faces[key];
            if (entry.first == 0) {
                entry.second = (nodes[a] + nodes[b] + nodes[c]) / 3.0;
            }
            ++entry.first;
        }
    }
    s.n_unique_faces = faces.size();
    const double m = std::max(0.0, surface_margin);
    for (const auto& [key, entry] : faces) {
        (void)key;
        const int c = entry.first;
        const Eigen::Vector3d& p = entry.second;
        if (c == 1) {
            ++s.n_boundary_faces;
            // Distance to AABB exterior (0 on faces, positive inside).
            const double dx = std::min(p[0] - bbox_min[0], bbox_max[0] - p[0]);
            const double dy = std::min(p[1] - bbox_min[1], bbox_max[1] - p[1]);
            const double dz = std::min(p[2] - bbox_min[2], bbox_max[2] - p[2]);
            const double dist_out = std::min({dx, dy, dz});
            if (dist_out > m) {
                ++s.n_hanging_faces;
            }
        } else if (c == 2) {
            ++s.n_interior_faces;
        } else {
            ++s.n_nonconforming;
        }
    }
    s.is_conforming = (s.n_nonconforming == 0 && s.n_hanging_faces == 0);
    return s;
}

} // namespace polymesh::mesh
