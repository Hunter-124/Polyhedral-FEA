// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/surface_metrics.hpp"

#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace {

constexpr double kPi = 3.14159265358979323846;

double clamp01(double x) {
    if (x < 0.0) {
        return 0.0;
    }
    if (x > 1.0) {
        return 1.0;
    }
    return x;
}

double tet_aspect_local(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                        const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    const Eigen::Vector3d ab = b - a;
    const Eigen::Vector3d ac = c - a;
    const Eigen::Vector3d ad = d - a;
    const double v = std::abs(ab.dot(ac.cross(ad)) / 6.0);
    if (v <= 0.0) {
        return 0.0;
    }
    const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                  (b - c).norm(), (b - d).norm(), (c - d).norm()});
    if (emax <= 0.0) {
        return 0.0;
    }
    constexpr double kNorm = 6.0 * 1.4142135623730951;
    return std::min(1.0, kNorm * v / (emax * emax * emax));
}

Eigen::Vector3d unit_or_z(const Eigen::Vector3d& d) {
    const double n = d.norm();
    if (n < 1e-15) {
        return Eigen::Vector3d::UnitZ();
    }
    return d / n;
}

/// Radial distance from axis and angle in plane orthogonal to axis.
void cyl_coords(const Eigen::Vector3d& p, const CircularFeature& c, double& r,
                double& theta) {
    const Eigen::Vector3d ax = unit_or_z(c.axis_dir);
    const Eigen::Vector3d d = p - c.axis_point;
    const Eigen::Vector3d radial = d - ax * d.dot(ax);
    r = radial.norm();
    // Orthonormal plane basis from axis.
    const Eigen::Vector3d e1 = ax.unitOrthogonal();
    const Eigen::Vector3d e2 = ax.cross(e1);
    if (r < 1e-15) {
        theta = 0.0;
        return;
    }
    theta = std::atan2(radial.dot(e2), radial.dot(e1));
}

} // namespace

std::vector<std::uint32_t> free_face_nodes(const std::vector<FreeFace>& free_faces) {
    std::unordered_set<std::uint32_t> uniq;
    uniq.reserve(free_faces.size() * 2);
    for (const auto& f : free_faces) {
        const int n = (f[3] == f[2]) ? 3 : 4;
        for (int i = 0; i < n; ++i) {
            uniq.insert(f[static_cast<std::size_t>(i)]);
        }
    }
    return {uniq.begin(), uniq.end()};
}

CurvedMeshMetrics evaluate_curved_mesh_quality(
    const geom::TriSurface& surface, const std::vector<Eigen::Vector3d>& nodes,
    const std::vector<FreeFace>& free_faces, double h, double mesh_volume,
    double ref_volume, const CircularFeature* circular,
    const std::vector<std::array<std::uint32_t, 4>>* tets) {
    CurvedMeshMetrics m;
    if (nodes.empty() || !(h > 0.0) || !std::isfinite(h)) {
        return m;
    }

    const auto bnodes = free_face_nodes(free_faces);
    m.n_boundary_nodes = bnodes.size();
    if (!bnodes.empty()) {
        const auto conf = surface_conformity(surface, nodes, bnodes);
        m.m1_max = conf.max_distance;
        m.m1_mean = conf.mean_distance;
    }

    // M2: edge midpoints + face centroids of free faces.
    double m2_sum = 0.0;
    auto sample = [&](const Eigen::Vector3d& p) {
        const double d = closest_on_surface(surface, p).distance;
        m.m2_max = std::max(m.m2_max, d);
        m2_sum += d;
        ++m.n_face_samples;
    };
    for (const auto& f : free_faces) {
        const bool tri = (f[3] == f[2]);
        const int nv = tri ? 3 : 4;
        bool ok = true;
        for (int i = 0; i < nv; ++i) {
            if (f[static_cast<std::size_t>(i)] >= nodes.size()) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        for (int i = 0; i < nv; ++i) {
            const auto a = f[static_cast<std::size_t>(i)];
            const auto b = f[static_cast<std::size_t>((i + 1) % nv)];
            if (a == b) {
                continue;
            }
            sample(0.5 * (nodes[a] + nodes[b]));
        }
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        for (int i = 0; i < nv; ++i) {
            c += nodes[f[static_cast<std::size_t>(i)]];
        }
        c /= static_cast<double>(nv);
        sample(c);
    }
    if (m.n_face_samples > 0) {
        m.m2_mean = m2_sum / static_cast<double>(m.n_face_samples);
    }

    if (mesh_volume > 0.0 && ref_volume > 0.0 && std::isfinite(mesh_volume) &&
        std::isfinite(ref_volume)) {
        m.has_volume = true;
        m.m3_rel_volume_err = std::abs(mesh_volume - ref_volume) / ref_volume;
    }

    if (circular != nullptr && circular->radius > 0.0 && std::isfinite(circular->radius)) {
        m.has_circular = true;
        const double band =
            (circular->select_band > 0.0) ? circular->select_band : 0.75 * h;
        std::vector<double> thetas;
        thetas.reserve(bnodes.size());
        double max_rad_err = 0.0;
        for (auto ni : bnodes) {
            if (ni >= nodes.size()) {
                continue;
            }
            double r = 0.0, th = 0.0;
            cyl_coords(nodes[ni], *circular, r, th);
            if (std::abs(r - circular->radius) > band) {
                continue;
            }
            max_rad_err = std::max(max_rad_err, std::abs(r - circular->radius));
            thetas.push_back(th);
            ++m.n_circular_nodes;
        }
        if (m.n_circular_nodes > 0) {
            m.m4_radial_rel = max_rad_err / circular->radius;
            std::sort(thetas.begin(), thetas.end());
            double max_gap = 0.0;
            for (std::size_t i = 0; i + 1 < thetas.size(); ++i) {
                max_gap = std::max(max_gap, thetas[i + 1] - thetas[i]);
            }
            // Wrap-around gap.
            if (thetas.size() >= 2) {
                const double wrap = (thetas.front() + 2.0 * kPi) - thetas.back();
                max_gap = std::max(max_gap, wrap);
            } else if (thetas.size() == 1) {
                max_gap = 2.0 * kPi;
            }
            m.m5_max_azimuth_gap = max_gap;
        } else {
            // No nodes selected: treat as full gap / bad radial coverage.
            m.m4_radial_rel = 1.0;
            m.m5_max_azimuth_gap = 2.0 * kPi;
        }
    }

    if (tets != nullptr && !tets->empty() && !bnodes.empty()) {
        std::unordered_set<std::uint32_t> bset(bnodes.begin(), bnodes.end());
        m.m6_min_boundary_aspect = std::numeric_limits<double>::infinity();
        for (const auto& t : *tets) {
            bool touch = false;
            for (auto n : t) {
                if (bset.count(n)) {
                    touch = true;
                    break;
                }
            }
            if (!touch) {
                continue;
            }
            if (t[0] >= nodes.size() || t[1] >= nodes.size() || t[2] >= nodes.size() ||
                t[3] >= nodes.size()) {
                continue;
            }
            const double asp =
                tet_aspect_local(nodes[t[0]], nodes[t[1]], nodes[t[2]], nodes[t[3]]);
            m.m6_min_boundary_aspect = std::min(m.m6_min_boundary_aspect, asp);
            ++m.n_boundary_tets;
        }
        if (m.n_boundary_tets == 0) {
            m.m6_min_boundary_aspect = 1.0;
        } else {
            m.has_tet_aspect = true;
            if (!std::isfinite(m.m6_min_boundary_aspect)) {
                m.m6_min_boundary_aspect = 0.0;
            }
        }
    }

    // Composite: weights favor face-sample residual (M2) and radial error (M4).
    // w = {M1:0.12, M2:0.30, M3:0.12, M4:0.28, M5:0.10, M6:0.08}
    const double inv_h = 1.0 / h;
    const double s1 = 1.0 - clamp01(m.m1_max * inv_h);
    const double s2 = 1.0 - clamp01(m.m2_max * inv_h);
    const double s3 = m.has_volume ? (1.0 - clamp01(m.m3_rel_volume_err)) : 1.0;
    const double s4 = m.has_circular ? (1.0 - clamp01(m.m4_radial_rel)) : 1.0;
    const double s5 =
        m.has_circular ? (1.0 - clamp01(m.m5_max_azimuth_gap / kPi)) : 1.0;
    const double s6 = m.has_tet_aspect ? clamp01(m.m6_min_boundary_aspect) : 1.0;

    double w1 = 0.12, w2 = 0.30, w3 = 0.12, w4 = 0.28, w5 = 0.10, w6 = 0.08;
    // Renormalize if optional metrics inactive so score stays in [0,1] meaningfully.
    if (!m.has_volume) {
        w3 = 0.0;
    }
    if (!m.has_circular) {
        w4 = 0.0;
        w5 = 0.0;
    }
    if (!m.has_tet_aspect) {
        w6 = 0.0;
    }
    const double wsum = w1 + w2 + w3 + w4 + w5 + w6;
    if (wsum > 0.0) {
        m.composite_score =
            (w1 * s1 + w2 * s2 + w3 * s3 + w4 * s4 + w5 * s5 + w6 * s6) / wsum;
    }
    return m;
}

} // namespace polymesh::mesh
