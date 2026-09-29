// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "fea/bc_selection.hpp"
#include "fea/traction.hpp"
#include "geom/cad_topology.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace polymesh::pipeline {

// The nodal integrator still supplies the energy-conjugate distribution; this
// only replaces the resultant's area. If no unambiguous CAD face matches,
// callers use the integrated mesh area.
std::optional<double> cad_pressure_area(const Model& model, const fea::LoadRegion& box,
                                        const Eigen::Vector3d& direction) {
    if (!model.cad.has_value()) {
        return std::nullopt;
    }
    const auto topo = geom::extract_topology(*model.cad, 16);
    double area = 0.0;
    for (const auto& face : topo.faces) {
        if (face.kind != geom::CadSurfaceKind::kPlane) {
            continue;
        }
        std::vector<Eigen::Vector3d> points;
        for (const auto edge_id : face.edge_ids) {
            if (edge_id < topo.edges.size()) {
                const auto& samples = topo.edges[edge_id].samples;
                points.insert(points.end(), samples.begin(), samples.end());
            }
        }
        if (points.size() < 3) {
            continue;
        }
        const bool in_box = std::all_of(points.begin(), points.end(), [&](const auto& p) {
            return p.x() >= box.lo.x() && p.x() <= box.hi.x() && p.y() >= box.lo.y() &&
                   p.y() <= box.hi.y() && p.z() >= box.lo.z() && p.z() <= box.hi.z();
        });
        if (!in_box) {
            continue;
        }
        Eigen::Vector3d n = Eigen::Vector3d::Zero();
        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
            n = (points[i] - points[0]).cross(points[i + 1] - points[0]);
            if (n.norm() > 1e-15) {
                n.normalize();
                break;
            }
        }
        if (std::abs(n.dot(direction)) >= fea::kSelectionNormalMinDot) {
            area += face.area;
        }
    }
    return area > 0.0 ? std::optional<double>(area) : std::nullopt;
}

} // namespace polymesh::pipeline
