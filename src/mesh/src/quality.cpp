// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/quality.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>

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

} // namespace polymesh::mesh
