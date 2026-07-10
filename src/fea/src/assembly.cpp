// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"

#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "fea/vem.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <array>
#include <format>
#include <vector>

namespace polymesh::fea {
namespace {

/// Node coordinates of one element as an n x 3 matrix.
Eigen::Matrix<double, Eigen::Dynamic, 3> element_coords(const NodalMesh& mesh,
                                                        const NodalElement& element) {
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(element.nodes.size(), 3);
    for (std::size_t a = 0; a < element.nodes.size(); ++a) {
        x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[element.nodes[a]].transpose();
    }
    return x;
}

/// Strain-displacement matrix B (6 x 3n) in Voigt order
/// (xx, yy, zz, yz, xz, xy) with engineering shear strains, from physical
/// shape-function gradients (n x 3).
Eigen::MatrixXd b_matrix(const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx) {
    const Eigen::Index n = dndx.rows();
    Eigen::MatrixXd b = Eigen::MatrixXd::Zero(6, 3 * n);
    for (Eigen::Index a = 0; a < n; ++a) {
        const double dx = dndx(a, 0);
        const double dy = dndx(a, 1);
        const double dz = dndx(a, 2);
        b(0, 3 * a + 0) = dx;
        b(1, 3 * a + 1) = dy;
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 1) = dz; // gamma_yz = dv/dz + dw/dy
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 0) = dz; // gamma_xz = du/dz + dw/dx
        b(4, 3 * a + 2) = dx;
        b(5, 3 * a + 0) = dy; // gamma_xy = du/dy + dv/dx
        b(5, 3 * a + 1) = dx;
    }
    return b;
}

} // namespace

Eigen::MatrixXd element_stiffness(const NodalMesh& mesh, const NodalElement& element,
                                  const Material& material) {
    if (element.type == ElementType::kPolyVem) {
        PolyCell cell;
        cell.nodes = element.nodes;
        cell.faces = element.faces;
        return vem_poly_stiffness(mesh, cell, material);
    }
    // Pyramid5: two tet4s (base diagonal 0-2 + apex). Flip-aware scatter keeps
    // local DOF order consistent with the stiffness matrix. Hex8 stays true
    // isoparametric trilinear (GATE 1 freeze). Product hybrid path expands hex
    // cores to six pyramids so all faces share this diagonal convention
    // (ADR-0013); mixed isoparametric-hex + pyramid is nonconforming.
    if (element.type == ElementType::kPyramid5 && element.nodes.size() == 5) {
        const auto& n = element.nodes;
        Eigen::MatrixXd k = Eigen::MatrixXd::Zero(15, 15);
        auto add_tet = [&](std::array<int, 4> loc) {
            std::array<std::uint32_t, 4> ids{
                n[static_cast<std::size_t>(loc[0])], n[static_cast<std::size_t>(loc[1])],
                n[static_cast<std::size_t>(loc[2])], n[static_cast<std::size_t>(loc[3])]};
            const Eigen::Vector3d& pa = mesh.nodes[ids[0]];
            const Eigen::Vector3d& pb = mesh.nodes[ids[1]];
            const Eigen::Vector3d& pc = mesh.nodes[ids[2]];
            const Eigen::Vector3d& pd = mesh.nodes[ids[3]];
            if ((pb - pa).dot((pc - pa).cross(pd - pa)) < 0.0) {
                std::swap(ids[1], ids[2]);
                std::swap(loc[1], loc[2]);
            }
            NodalElement tet{ElementType::kTet4, {ids[0], ids[1], ids[2], ids[3]}};
            const Eigen::MatrixXd kt = element_stiffness(mesh, tet, material);
            for (int a = 0; a < 4; ++a) {
                for (int b = 0; b < 4; ++b) {
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            k(3 * loc[static_cast<std::size_t>(a)] + i,
                              3 * loc[static_cast<std::size_t>(b)] + j) +=
                                kt(3 * a + i, 3 * b + j);
                        }
                    }
                }
            }
        };
        add_tet({{0, 1, 2, 4}});
        add_tet({{0, 2, 3, 4}});
        return k;
    }
    const auto x = element_coords(mesh, element);
    const auto d = material.d_matrix();
    const Eigen::Index ndof = 3 * x.rows();
    Eigen::MatrixXd k = Eigen::MatrixXd::Zero(ndof, ndof);
    for (const auto& qp : default_rule(element.type)) {
        const auto shape = eval_shape(element.type, qp.xi);
        // Jacobian J(r,c) = d x_c / d xi_r.
        const Eigen::Matrix3d jac = shape.dn.transpose() * x;
        const double det = jac.determinant();
        if (det <= 0.0) {
            throw FeaError(std::format(
                "element_stiffness: non-positive Jacobian ({:.3e}) — inverted element", det));
        }
        // dN/dx = dN/dxi * J^{-T}. The inverse is materialized first: Eigen 5's
        // evaluator recurses infinitely on the nested inverse().transpose()
        // expression (stack overflow).
        const Eigen::Matrix3d jac_inv = jac.inverse();
        const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = shape.dn * jac_inv.transpose();
        const auto b = b_matrix(dndx);
        k.noalias() += b.transpose() * d * b * (det * qp.weight);
    }
    return k;
}

Eigen::SparseMatrix<double> assemble_stiffness(const NodalMesh& mesh,
                                               const Material& material) {
    mesh.check_validity();
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    std::vector<Eigen::Triplet<double>> triplets;
    for (const auto& element : mesh.elements) {
        const auto k = element_stiffness(mesh, element, material);
        const auto n = static_cast<Eigen::Index>(element.nodes.size());
        for (Eigen::Index a = 0; a < n; ++a) {
            for (Eigen::Index b = 0; b < n; ++b) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        const auto row = 3 * static_cast<Eigen::Index>(
                                                 element.nodes[static_cast<std::size_t>(a)]) +
                                         i;
                        const auto col = 3 * static_cast<Eigen::Index>(
                                                 element.nodes[static_cast<std::size_t>(b)]) +
                                         j;
                        triplets.emplace_back(row, col, k(3 * a + i, 3 * b + j));
                    }
                }
            }
        }
    }
    Eigen::SparseMatrix<double> global(ndof, ndof);
    global.setFromTriplets(triplets.begin(), triplets.end());
    return global;
}

Eigen::VectorXd assemble_body_load(const NodalMesh& mesh, const BodyForce& body_force) {
    mesh.check_validity();
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (const auto& element : mesh.elements) {
        if (element.type == ElementType::kPolyVem) {
            // Lumped consistent load: equal share of volume * b(centroid).
            std::vector<Eigen::Vector3d> coords;
            coords.reserve(element.nodes.size());
            Eigen::Vector3d c = Eigen::Vector3d::Zero();
            for (auto id : element.nodes) {
                coords.push_back(mesh.nodes[id]);
                c += mesh.nodes[id];
            }
            c /= static_cast<double>(element.nodes.size());
            const double vol = poly_volume(coords, element.faces);
            const Eigen::Vector3d b = body_force(c);
            const Eigen::Vector3d share =
                b * (vol / static_cast<double>(element.nodes.size()));
            for (auto id : element.nodes) {
                f.segment<3>(3 * static_cast<Eigen::Index>(id)) += share;
            }
            continue;
        }
        const auto x = element_coords(mesh, element);
        // Elevated rule: body-force fields (e.g. manufactured solutions) are
        // often higher-degree than the stiffness integrand, and consistent
        // loads must not become the accuracy bottleneck.
        std::vector<QuadraturePoint> rule;
        if (element.type == ElementType::kTet4 || element.type == ElementType::kTet10) {
            rule = tet_rule(4);
        } else if (element.type == ElementType::kPrism6 ||
                   element.type == ElementType::kPyramid5) {
            rule = default_rule(element.type);
        } else {
            rule = hex_rule(4);
        }
        for (const auto& qp : rule) {
            const auto shape = eval_shape(element.type, qp.xi);
            const Eigen::Matrix3d jac = shape.dn.transpose() * x;
            const double det = jac.determinant();
            if (det <= 0.0) {
                throw FeaError("assemble_body_load: non-positive Jacobian");
            }
            const Eigen::Vector3d point = x.transpose() * shape.n;
            const Eigen::Vector3d b = body_force(point);
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                f.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[a])) +=
                    shape.n[static_cast<Eigen::Index>(a)] * b * (det * qp.weight);
            }
        }
    }
    return f;
}

} // namespace polymesh::fea
