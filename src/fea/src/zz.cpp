// SPDX-License-Identifier: BSD-3-Clause
#include "fea/zz.hpp"

#include "fea/shape.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <map>
#include <vector>

namespace polymesh::fea {
namespace {

Stress stress_at(const NodalElement& element,
                 const Eigen::Matrix<double, Eigen::Dynamic, 3>& x, const Eigen::VectorXd& u,
                 const Eigen::Matrix<double, 6, 6>& d, const Eigen::Vector3d& xi) {
    const auto shape = eval_shape(element.type, xi);
    const Eigen::Matrix3d jac = shape.dn.transpose() * x;
    const Eigen::Matrix3d jac_inv = jac.inverse();
    const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = shape.dn * jac_inv.transpose();
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
    return d * eps;
}

Eigen::Vector3d element_centroid(const NodalMesh& mesh, const NodalElement& el) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (auto n : el.nodes) {
        c += mesh.nodes[n];
    }
    return c / static_cast<double>(el.nodes.size());
}

} // namespace

ZzRecovery recover_zz(const NodalMesh& mesh, const Material& material,
                      const Eigen::VectorXd& u) {
    const auto d = material.d_matrix();
    const auto n_nodes = mesh.nodes.size();
    const auto n_elem = mesh.elements.size();

    // Element centroid stress (superconvergent sampling points for linear elements).
    std::vector<Stress> el_stress(n_elem, Stress::Zero());
    std::vector<Eigen::Vector3d> el_cent(n_elem);
    for (std::size_t e = 0; e < n_elem; ++e) {
        const auto& el = mesh.elements[e];
        el_cent[e] = element_centroid(mesh, el);
        if (el.type == ElementType::kPolyVem) {
            el_stress[e].setZero();
            continue;
        }
        // Pyramid: average tet-split stresses (matches stiffness assembly).
        if (el.type == ElementType::kPyramid5 && el.nodes.size() == 5) {
            const auto& n = el.nodes;
            auto tet_stress = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c,
                                  std::uint32_t apex) -> Stress {
                NodalElement tet{ElementType::kTet4, {a, b, c, apex}};
                const Eigen::Vector3d& pa = mesh.nodes[a];
                const Eigen::Vector3d& pb = mesh.nodes[b];
                const Eigen::Vector3d& pc = mesh.nodes[c];
                const Eigen::Vector3d& pd = mesh.nodes[apex];
                const double vol = (pb - pa).dot((pc - pa).cross(pd - pa));
                if (vol < 0.0) {
                    tet.nodes = {a, c, b, apex};
                }
                Eigen::Matrix<double, Eigen::Dynamic, 3> x(4, 3);
                for (std::size_t i = 0; i < 4; ++i) {
                    x.row(static_cast<Eigen::Index>(i)) =
                        mesh.nodes[tet.nodes[i]].transpose();
                }
                const auto ref = reference_nodes(ElementType::kTet4);
                Eigen::Vector3d xi = Eigen::Vector3d::Zero();
                for (const auto& r : ref) {
                    xi += r;
                }
                xi /= static_cast<double>(ref.size());
                return stress_at(tet, x, u, d, xi);
            };
            el_stress[e] = 0.5 * (tet_stress(n[0], n[1], n[2], n[4]) +
                                  tet_stress(n[0], n[2], n[3], n[4]));
            continue;
        }
        Eigen::Matrix<double, Eigen::Dynamic, 3> x(el.nodes.size(), 3);
        for (std::size_t a = 0; a < el.nodes.size(); ++a) {
            x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[el.nodes[a]].transpose();
        }
        // Reference centroid: average of reference nodes.
        const auto ref = reference_nodes(el.type);
        Eigen::Vector3d xi = Eigen::Vector3d::Zero();
        for (const auto& r : ref) {
            xi += r;
        }
        xi /= static_cast<double>(ref.size());
        el_stress[e] = stress_at(el, x, u, d, xi);
    }

    // Node → incident elements.
    std::vector<std::vector<std::size_t>> incident(n_nodes);
    for (std::size_t e = 0; e < n_elem; ++e) {
        for (auto n : mesh.elements[e].nodes) {
            incident[n].push_back(e);
        }
    }

    // Per-node least-squares fit of stress components: σ(x) ≈ a0 + a1 x + a2 y + a3 z
    ZzRecovery out;
    out.nodal_stress.assign(n_nodes, Stress::Zero());
    for (std::size_t n = 0; n < n_nodes; ++n) {
        const auto& patch = incident[n];
        if (patch.empty()) {
            continue;
        }
        // Need ≥4 samples for linear fit; fall back to average.
        if (patch.size() < 4) {
            Stress acc = Stress::Zero();
            for (auto e : patch) {
                acc += el_stress[e];
            }
            out.nodal_stress[n] = acc / static_cast<double>(patch.size());
            continue;
        }
        const auto m = static_cast<Eigen::Index>(patch.size());
        Eigen::MatrixXd A(m, 4);
        Eigen::MatrixXd B(m, 6);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto e = patch[static_cast<std::size_t>(i)];
            const auto& c = el_cent[e];
            A(i, 0) = 1.0;
            A(i, 1) = c[0];
            A(i, 2) = c[1];
            A(i, 3) = c[2];
            B.row(i) = el_stress[e].transpose();
        }
        // Solve (A^T A) X = A^T B for 4x6 coefficient matrix.
        const Eigen::MatrixXd ata = A.transpose() * A;
        const Eigen::MatrixXd atb = A.transpose() * B;
        const Eigen::MatrixXd coeff = ata.ldlt().solve(atb);
        const Eigen::Vector3d& p = mesh.nodes[n];
        Eigen::RowVector4d row;
        row << 1.0, p[0], p[1], p[2];
        out.nodal_stress[n] = (row * coeff).transpose();
    }

    // Element indicators: ||σ* - σ_h||_energy-ish via stress L2 at centroid.
    out.element_eta.assign(n_elem, 0.0);
    double sum_sq = 0.0;
    for (std::size_t e = 0; e < n_elem; ++e) {
        const auto& el = mesh.elements[e];
        Stress star = Stress::Zero();
        for (auto n : el.nodes) {
            star += out.nodal_stress[n];
        }
        star /= static_cast<double>(el.nodes.size());
        const Stress diff = star - el_stress[e];
        // Energy-like: (1/2) e : C^{-1} : e ≈ use ||diff||^2 scaled (C positive definite).
        const double eta = diff.norm();
        out.element_eta[e] = eta;
        sum_sq += eta * eta;
    }
    out.global_eta = std::sqrt(sum_sq);
    return out;
}

} // namespace polymesh::fea
