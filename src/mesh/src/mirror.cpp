// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/mirror.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

/// Quantised coordinate lookup at `quantum` metres. Mirror partners never agree
/// bit-for-bit, so the probe checks the 27 buckets around the query — a single
/// bucket lookup misses a partner that rounds across a bucket wall.
class PointLookup {
  public:
    PointLookup(const std::vector<Eigen::Vector3d>& points, double quantum)
        : points_(points), inv_quantum_(quantum > 0.0 ? 1.0 / quantum : 0.0),
          tolerance_(quantum) {
        buckets_.reserve(points.size() * 2);
        for (std::size_t i = 0; i < points.size(); ++i) {
            buckets_[cell_of(points[i])].push_back(i);
        }
    }

    /// Index of a point within `tolerance` of `p`, or npos.
    [[nodiscard]] std::size_t find(const Eigen::Vector3d& p) const {
        const auto c = cell_of(p);
        std::size_t best = npos;
        double best_d2 = tolerance_ * tolerance_;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const Cell probe{c[0] + dx, c[1] + dy, c[2] + dz};
                    const auto it = buckets_.find(probe);
                    if (it == buckets_.end()) {
                        continue;
                    }
                    for (const auto i : it->second) {
                        const double d2 = (points_[i] - p).squaredNorm();
                        if (d2 <= best_d2) {
                            best_d2 = d2;
                            best = i;
                        }
                    }
                }
            }
        }
        return best;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  private:
    using Cell = std::array<long long, 3>;
    struct CellHash {
        std::size_t operator()(const Cell& c) const noexcept {
            std::size_t h = static_cast<std::size_t>(c[0]);
            h ^= static_cast<std::size_t>(c[1]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= static_cast<std::size_t>(c[2]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    [[nodiscard]] Cell cell_of(const Eigen::Vector3d& p) const {
        return {static_cast<long long>(std::floor(p[0] * inv_quantum_)),
                static_cast<long long>(std::floor(p[1] * inv_quantum_)),
                static_cast<long long>(std::floor(p[2] * inv_quantum_))};
    }

    const std::vector<Eigen::Vector3d>& points_;
    double inv_quantum_ = 0.0;
    double tolerance_ = 0.0;
    std::unordered_map<Cell, std::vector<std::size_t>, CellHash> buckets_;
};

} // namespace

std::size_t MirrorNodeOrbit::CellHash::operator()(const Cell& c) const noexcept {
    std::size_t h = static_cast<std::size_t>(c.x);
    h ^= static_cast<std::size_t>(c.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= static_cast<std::size_t>(c.z) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

MirrorNodeOrbit::Cell MirrorNodeOrbit::cell_of(const Eigen::Vector3d& p) const {
    return {static_cast<long long>(std::floor(p[0] * inv_quantum_)),
            static_cast<long long>(std::floor(p[1] * inv_quantum_)),
            static_cast<long long>(std::floor(p[2] * inv_quantum_))};
}

MirrorNodeOrbit::MirrorNodeOrbit(const MirrorFrame& frame,
                                 const std::vector<Eigen::Vector3d>& nodes, double tol)
    : nodes_(&nodes), frame_(frame), tol_(tol) {
    for (int a = 0; a < 3; ++a) {
        if (frame.plane[static_cast<std::size_t>(a)]) {
            axes_[static_cast<std::size_t>(n_axes_++)] = a;
        }
    }
    if (n_axes_ == 0 || nodes.empty() || !(tol > 0.0)) {
        return;
    }
    active_ = true;
    reflections_ = (1U << static_cast<unsigned>(n_axes_)) - 1U;
    inv_quantum_ = 1.0 / tol;
    buckets_.reserve(nodes.size() * 2);
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        buckets_[cell_of(nodes[i])].push_back(i);
    }
}

Eigen::Vector3d MirrorNodeOrbit::reflect(const Eigen::Vector3d& p, unsigned mask) const {
    Eigen::Vector3d q = p;
    for (int i = 0; i < n_axes_; ++i) {
        if ((mask & (1U << static_cast<unsigned>(i))) == 0) {
            continue;
        }
        const int a = axes_[static_cast<std::size_t>(i)];
        q[a] = 2.0 * frame_.center[a] - p[a];
    }
    return q;
}

std::uint32_t MirrorNodeOrbit::reflected(std::uint32_t node, unsigned mask) const {
    if (!active_ || nodes_ == nullptr || node >= nodes_->size()) {
        return npos;
    }
    const Eigen::Vector3d q = reflect((*nodes_)[node], mask);
    const Cell c = cell_of(q);
    std::uint32_t best = npos;
    double best_d2 = tol_ * tol_;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto it = buckets_.find(Cell{c.x + dx, c.y + dy, c.z + dz});
                if (it == buckets_.end()) {
                    continue;
                }
                for (const auto i : it->second) {
                    const double d2 = ((*nodes_)[i] - q).squaredNorm();
                    if (d2 <= best_d2) {
                        best_d2 = d2;
                        best = i;
                    }
                }
            }
        }
    }
    return best;
}

std::pair<std::uint32_t, unsigned> MirrorNodeOrbit::canonical(std::uint32_t node) const {
    if (!active_ || nodes_ == nullptr || node >= nodes_->size()) {
        return {npos, 0U};
    }
    const Eigen::Vector3d& p = (*nodes_)[node];
    unsigned mask = 0;
    for (int i = 0; i < n_axes_; ++i) {
        const int a = axes_[static_cast<std::size_t>(i)];
        if (p[a] > frame_.center[a]) {
            mask |= 1U << static_cast<unsigned>(i);
        }
    }
    // A reflection is its own inverse, so the same mask maps the canonical point
    // back into this node's octant.
    return {reflected(node, mask), mask};
}

MirrorFrame detect_mirror_frame(const geom::CadModel& cad, const Eigen::Vector3d& bbox_min,
                                const Eigen::Vector3d& bbox_max, double tol_frac) {
    MirrorFrame frame;
    if (cad.empty()) {
        return frame;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    const double diag = extent.norm();
    if (!(diag > 0.0) || !(tol_frac > 0.0)) {
        return frame;
    }
    frame.center = 0.5 * (bbox_min + bbox_max);
    frame.plane_tolerance = 1e-9 * diag;

    // Eight samples per face is enough to catch an asymmetric feature: a feature
    // is its own trimmed face, and `sample_brep_surface` guarantees every face at
    // least one sample, so a boss present on one side and absent on the other is
    // sampled on the side it exists and its reflection lands in mid-air.
    const auto inspection = inspect_brep(cad);
    const std::size_t faces = std::max<std::size_t>(1, inspection.face_count);
    const std::size_t budget = std::clamp<std::size_t>(8 * faces, 256, 4096);
    geom::BRepSurfaceSamples samples;
    try {
        samples = geom::sample_brep_surface(cad, budget);
    } catch (const std::exception&) {
        return frame; // no exact samples: no verified plane, so no fold
    }
    if (samples.points.empty()) {
        return frame;
    }

    const double tol = tol_frac * diag;
    for (int axis = 0; axis < 3; ++axis) {
        double worst = 0.0;
        bool symmetric = true;
        for (const auto& p : samples.points) {
            Eigen::Vector3d q = p;
            q[axis] = 2.0 * frame.center[axis] - p[axis];
            const auto hit = geom::project_point_on_surface(cad, q);
            if (!hit) {
                symmetric = false;
                break;
            }
            worst = std::max(worst, hit->distance);
            if (hit->distance > tol) {
                symmetric = false;
                break;
            }
        }
        if (symmetric) {
            frame.plane[static_cast<std::size_t>(axis)] = true;
            frame.max_residual_over_diag =
                std::max(frame.max_residual_over_diag, worst / diag);
        }
    }
    if (!frame.any()) {
        frame.max_residual_over_diag = 0.0;
    }
    return frame;
}

MirrorFrame detect_mirror_frame(const geom::TriSurface& surface, double tol_frac) {
    MirrorFrame frame;
    if (surface.vertices.empty() || surface.triangles.empty() || !(tol_frac > 0.0)) {
        return frame;
    }
    Eigen::Vector3d lo = surface.vertices.front();
    Eigen::Vector3d hi = lo;
    for (const auto& v : surface.vertices) {
        lo = lo.cwiseMin(v);
        hi = hi.cwiseMax(v);
    }
    const double diag = (hi - lo).norm();
    if (!(diag > 0.0)) {
        return frame;
    }
    frame.center = 0.5 * (lo + hi);
    frame.plane_tolerance = 1e-9 * diag;

    const double tol = tol_frac * diag;
    const PointLookup lookup(surface.vertices, tol);
    std::unordered_set<std::uint64_t> triangle_keys;
    triangle_keys.reserve(surface.triangles.size() * 2);
    const auto triangle_key = [](std::array<std::uint32_t, 3> v) {
        std::sort(v.begin(), v.end());
        std::uint64_t h = v[0];
        h = h * 2654435761ULL + v[1];
        h = h * 2654435761ULL + v[2];
        return h;
    };
    for (const auto& t : surface.triangles) {
        triangle_keys.insert(triangle_key({t[0], t[1], t[2]}));
    }

    for (int axis = 0; axis < 3; ++axis) {
        std::vector<std::size_t> partner(surface.vertices.size(), PointLookup::npos);
        bool symmetric = true;
        double worst = 0.0;
        for (std::size_t i = 0; i < surface.vertices.size() && symmetric; ++i) {
            Eigen::Vector3d q = surface.vertices[i];
            q[axis] = 2.0 * frame.center[axis] - q[axis];
            const std::size_t j = lookup.find(q);
            if (j == PointLookup::npos) {
                symmetric = false;
                break;
            }
            partner[i] = j;
            worst = std::max(worst, (surface.vertices[j] - q).norm());
        }
        for (std::size_t t = 0; t < surface.triangles.size() && symmetric; ++t) {
            const auto& tri = surface.triangles[t];
            const std::array<std::uint32_t, 3> mirrored{
                {static_cast<std::uint32_t>(partner[tri[0]]),
                 static_cast<std::uint32_t>(partner[tri[1]]),
                 static_cast<std::uint32_t>(partner[tri[2]])}};
            if (triangle_keys.count(triangle_key(mirrored)) == 0) {
                symmetric = false;
            }
        }
        if (symmetric) {
            frame.plane[static_cast<std::size_t>(axis)] = true;
            frame.max_residual_over_diag =
                std::max(frame.max_residual_over_diag, worst / diag);
        }
    }
    if (!frame.any()) {
        frame.max_residual_over_diag = 0.0;
    }
    return frame;
}

} // namespace polymesh::mesh
