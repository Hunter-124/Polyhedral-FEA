// SPDX-License-Identifier: BSD-3-Clause
#include "fea/stress.hpp"

#include "fea/shape.hpp"

#include <Eigen/Dense>

#include <cmath>

namespace polymesh::fea {

std::vector<Stress> recover_nodal_stress(const NodalMesh& mesh, const Material& material,
                                         const Eigen::VectorXd& u) {
    const auto d = material.d_matrix();
    std::vector<Stress> stress(mesh.nodes.size(), Stress::Zero());
    std::vector<int> hits(mesh.nodes.size(), 0);

    for (const auto& element : mesh.elements) {
        if (element.type == ElementType::kPolyVem) {
            continue; // constant-strain VEM stress recovered via ZZ path later
        }
        const auto ref = reference_nodes(element.type);
        Eigen::Matrix<double, Eigen::Dynamic, 3> x(element.nodes.size(), 3);
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[element.nodes[a]].transpose();
        }
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            const auto shape = eval_shape(element.type, ref[a]);
            const Eigen::Matrix3d jac = shape.dn.transpose() * x;
            const Eigen::Matrix3d jac_inv = jac.inverse();
            const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx =
                shape.dn * jac_inv.transpose();

            Eigen::Matrix<double, 6, 1> eps = Eigen::Matrix<double, 6, 1>::Zero();
            for (std::size_t b = 0; b < element.nodes.size(); ++b) {
                const auto bi = static_cast<Eigen::Index>(b);
                const Eigen::Vector3d ub =
                    u.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[b]));
                eps[0] += dndx(bi, 0) * ub[0];
                eps[1] += dndx(bi, 1) * ub[1];
                eps[2] += dndx(bi, 2) * ub[2];
                eps[3] += dndx(bi, 2) * ub[1] + dndx(bi, 1) * ub[2];
                eps[4] += dndx(bi, 2) * ub[0] + dndx(bi, 0) * ub[2];
                eps[5] += dndx(bi, 1) * ub[0] + dndx(bi, 0) * ub[1];
            }
            stress[element.nodes[a]] += d * eps;
            ++hits[element.nodes[a]];
        }
    }
    for (std::size_t i = 0; i < stress.size(); ++i) {
        if (hits[i] > 0) {
            stress[i] /= hits[i];
        }
    }
    return stress;
}

double von_mises(const Stress& s) {
    const double sxx = s[0], syy = s[1], szz = s[2];
    const double syz = s[3], sxz = s[4], sxy = s[5];
    return std::sqrt(0.5 * ((sxx - syy) * (sxx - syy) + (syy - szz) * (syy - szz) +
                            (szz - sxx) * (szz - sxx)) +
                     3.0 * (sxy * sxy + syz * syz + sxz * sxz));
}

} // namespace polymesh::fea
