// SPDX-License-Identifier: BSD-3-Clause
#include "fea/element_validity.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "fea/vem.hpp"
#include "mesh/cell_validity.hpp"

#include <Eigen/Dense>

namespace polymesh::fea {
namespace {

Eigen::Matrix<double, Eigen::Dynamic, 3> coords_of(const NodalMesh& mesh,
                                                   const NodalElement& element) {
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(static_cast<Eigen::Index>(element.nodes.size()),
                                               3);
    for (std::size_t a = 0; a < element.nodes.size(); ++a) {
        x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[element.nodes[a]].transpose();
    }
    return x;
}

bool rule_positive(ElementType type, const Eigen::Matrix<double, Eigen::Dynamic, 3>& x) {
    for (const auto& qp : default_rule(type)) {
        const auto shape = eval_shape(type, qp.xi);
        if (shape.dn.rows() != x.rows()) {
            return false;
        }
        const Eigen::Matrix3d jac = shape.dn.transpose() * x;
        if (!(jac.determinant() > 0.0)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool element_jacobians_positive(const NodalMesh& mesh, const NodalElement& element) {
    for (const auto ni : element.nodes) {
        if (ni >= mesh.nodes.size() || !mesh.nodes[ni].allFinite()) {
            return false;
        }
    }
    if (element.type == ElementType::kPolyVem) {
        // A polyhedral cell carries no isoparametric map, so "integrable" here
        // means the divergence-theorem volume its VEM projector integrates over
        // is positive. Reporting these unconditionally valid is what let a
        // repair pass move nodes freely inside a packed-poly mesh: cvt_poly's
        // worst boundary node went from 0.503 h to 1.799 h with no gate at all.
        if (element.faces.empty()) {
            return false;
        }
        std::vector<Eigen::Vector3d> coords;
        coords.reserve(element.nodes.size());
        for (const auto ni : element.nodes) {
            coords.push_back(mesh.nodes[ni]);
        }
        return poly_volume(coords, element.faces) > 0.0;
    }
    if (element.type == ElementType::kPyramid5 && element.nodes.size() == 5) {
        // The pyramid is integrated as the two tets of the shared-face-consistent
        // split, so that is what has to be positive — the pyramid's own rule is
        // never used.
        const auto& n = element.nodes;
        const std::array<std::array<int, 4>, 2> split =
            mesh::validity::pyramid_split_diagonal(mesh.nodes[n[0]], mesh.nodes[n[1]],
                                                   mesh.nodes[n[2]], mesh.nodes[n[3]]) == 1
                ? std::array<std::array<int, 4>, 2>{{{{1, 2, 3, 4}}, {{1, 3, 0, 4}}}}
                : std::array<std::array<int, 4>, 2>{{{{0, 1, 2, 4}}, {{0, 2, 3, 4}}}};
        for (const auto& tet : split) {
            Eigen::Matrix<double, Eigen::Dynamic, 3> x(4, 3);
            for (std::size_t a = 0; a < 4; ++a) {
                x.row(static_cast<Eigen::Index>(a)) =
                    mesh.nodes[n[static_cast<std::size_t>(tet[a])]].transpose();
            }
            if (!rule_positive(ElementType::kTet4, x)) {
                return false;
            }
        }
        return true;
    }
    return rule_positive(element.type, coords_of(mesh, element));
}

bool star_jacobians_positive(const NodalMesh& mesh,
                             std::span<const std::uint32_t> incident_elements) {
    for (const auto ei : incident_elements) {
        if (ei >= mesh.elements.size()) {
            return false;
        }
        if (!element_jacobians_positive(mesh, mesh.elements[ei])) {
            return false;
        }
    }
    return true;
}

} // namespace polymesh::fea
