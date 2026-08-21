// SPDX-License-Identifier: BSD-3-Clause
#include "fea/stress.hpp"

#include "fea/backend.hpp"
#include "fea/cell_quality.hpp"
#include "fea/shape.hpp"
#include "fea/vem.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(POLYMESH_WITH_OPENMP)
#include <omp.h>
#endif

namespace polymesh::fea {

namespace {

/// Stress of one polyhedral VEM cell from its own projected strain.
///
/// The VEM k=1 space has no per-node shape-function gradient to differentiate,
/// so the cell-wise polynomial projection is the strain: exactly the operator
/// the consistency term of `vem_poly_stiffness` integrates, so σ = D ε(Π u) is
/// the stress the element actually carries. Scattering it to every vertex of the
/// cell and averaging there matches what the isoparametric path does with its
/// nodal samples, so an all-VEM mesh gets the same nodal-averaged field instead
/// of the zeros it used to report.
Stress poly_vem_cell_stress(const NodalMesh& mesh, const NodalElement& element,
                            const Eigen::Matrix<double, 6, 6>& d, const Eigen::VectorXd& u) {
    const auto n = element.nodes.size();
    std::vector<Eigen::Vector3d> coords;
    coords.reserve(n);
    Eigen::VectorXd u_elem(3 * static_cast<Eigen::Index>(n));
    for (std::size_t a = 0; a < n; ++a) {
        coords.push_back(mesh.nodes[element.nodes[a]]);
        u_elem.segment<3>(3 * static_cast<Eigen::Index>(a)) =
            u.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[a]));
    }
    const int order = vem_infer_order(n, element.faces);
    return d * vem_projected_strain(coords, element.faces, u_elem, order);
}

} // namespace

std::vector<Stress> recover_nodal_stress(const NodalMesh& mesh, const Material& material,
                                         const Eigen::VectorXd& u) {
    init_runtime_performance();
    const auto d = material.d_matrix();
    const auto n_nodes = mesh.nodes.size();
    const auto n_elem = mesh.elements.size();
    std::vector<Stress> stress(n_nodes, Stress::Zero());
    std::vector<int> hits(n_nodes, 0);

#if defined(POLYMESH_WITH_OPENMP)
    // Thread-local buffers then ordered merge — same averages as serial (double).
    // Pre-size outside the parallel region (no concurrent resize).
    const int nthreads = std::max(1, omp_get_max_threads());
    std::vector<std::vector<Stress>> thr_stress(static_cast<std::size_t>(nthreads),
                                                std::vector<Stress>(n_nodes, Stress::Zero()));
    std::vector<std::vector<int>> thr_hits(static_cast<std::size_t>(nthreads),
                                           std::vector<int>(n_nodes, 0));
#pragma omp parallel
    {
        const int tid = std::min(omp_get_thread_num(), nthreads - 1);
        auto& local_s = thr_stress[static_cast<std::size_t>(tid)];
        auto& local_h = thr_hits[static_cast<std::size_t>(tid)];
#pragma omp for schedule(static)
        for (std::ptrdiff_t e = 0; e < static_cast<std::ptrdiff_t>(n_elem); ++e) {
            const auto& element = mesh.elements[static_cast<std::size_t>(e)];
            if (element.type == ElementType::kPolyVem) {
                if (element.nodes.empty() || element.faces.empty()) {
                    continue;
                }
                const Stress s = poly_vem_cell_stress(mesh, element, d, u);
                if (!s.allFinite()) {
                    continue;
                }
                for (std::uint32_t nid : element.nodes) {
                    local_s[nid] += s;
                    ++local_h[nid];
                }
                continue;
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
                local_s[element.nodes[a]] += d * eps;
                ++local_h[element.nodes[a]];
            }
        }
    }
    for (std::size_t t = 0; t < thr_stress.size(); ++t) {
        for (std::size_t i = 0; i < n_nodes; ++i) {
            stress[i] += thr_stress[t][i];
            hits[i] += thr_hits[t][i];
        }
    }
#else
    for (const auto& element : mesh.elements) {
        if (element.type == ElementType::kPolyVem) {
            if (element.nodes.empty() || element.faces.empty()) {
                continue;
            }
            const Stress s = poly_vem_cell_stress(mesh, element, d, u);
            if (!s.allFinite()) {
                continue;
            }
            for (std::uint32_t nid : element.nodes) {
                stress[nid] += s;
                ++hits[nid];
            }
            continue;
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
#endif
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

namespace {

/// Reference-space sample at element "center" (not a nodal corner).
Eigen::Vector3d reference_centroid(ElementType type) {
    switch (type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        return {0.25, 0.25, 0.25};
    case ElementType::kHex8:
    case ElementType::kHex20:
    case ElementType::kPyramid5:
    case ElementType::kPrism6:
        return {0.0, 0.0, 0.0};
    case ElementType::kPolyVem:
    default:
        return {0.0, 0.0, 0.0};
    }
}

/// Measured shape quality for stress-sample filtering: `cell_quality`, with an
/// unmeasurable cell reported as 0 so a quality floor never trusts a cell nobody
/// measured (a NaN would silently pass every `q < floor` test).
double sample_quality(const NodalMesh& mesh, const NodalElement& element) {
    const double q = cell_quality(mesh, element);
    return std::isfinite(q) ? q : 0.0;
}

} // namespace

namespace {

/// Constant-strain LSQ on poly vertices: fit u ≈ a + G (x − c), strain = sym(G).
bool poly_vem_constant_strain(const NodalMesh& mesh, const NodalElement& element,
                              const Eigen::VectorXd& u, Eigen::Matrix<double, 6, 1>& eps,
                              Eigen::Vector3d& centroid) {
    const auto n = element.nodes.size();
    if (n < 4) {
        return false;
    }
    centroid.setZero();
    for (std::uint32_t nid : element.nodes) {
        centroid += mesh.nodes[nid];
    }
    centroid /= static_cast<double>(n);

    // Design matrix for each component: [1, dx, dy, dz] → 4 params per component.
    // Stacked 3n × 12 system for (a_x,G_xx,G_xy,G_xz, a_y,..., a_z,...).
    Eigen::MatrixXd A = Eigen::MatrixXd::Zero(static_cast<Eigen::Index>(3 * n), 12);
    Eigen::VectorXd b = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(3 * n));
    for (std::size_t i = 0; i < n; ++i) {
        const Eigen::Vector3d dx = mesh.nodes[element.nodes[i]] - centroid;
        const Eigen::Vector3d ui =
            u.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[i]));
        for (int c = 0; c < 3; ++c) {
            const Eigen::Index row =
                static_cast<Eigen::Index>(3 * i + static_cast<std::size_t>(c));
            A(row, 4 * c + 0) = 1.0;
            A(row, 4 * c + 1) = dx.x();
            A(row, 4 * c + 2) = dx.y();
            A(row, 4 * c + 3) = dx.z();
            b(row) = ui[c];
        }
    }
    const Eigen::VectorXd p = A.colPivHouseholderQr().solve(b);
    if (!p.allFinite()) {
        return false;
    }
    // G rows: grad of u_x, u_y, u_z
    const double gxx = p(1), gxy = p(2), gxz = p(3);
    const double gyx = p(5), gyy = p(6), gyz = p(7);
    const double gzx = p(9), gzy = p(10), gzz = p(11);
    eps[0] = gxx;
    eps[1] = gyy;
    eps[2] = gzz;
    eps[3] = gyz + gzy; // engineering shear γ_yz
    eps[4] = gxz + gzx; // γ_xz
    eps[5] = gxy + gyx; // γ_xy
    return true;
}

} // namespace

std::vector<ElementCentroidStress> recover_element_centroid_stress(const NodalMesh& mesh,
                                                                   const Material& material,
                                                                   const Eigen::VectorXd& u) {
    const auto dmat = material.d_matrix();
    std::vector<ElementCentroidStress> out;
    out.reserve(mesh.elements.size());

    for (std::uint32_t ei = 0; ei < static_cast<std::uint32_t>(mesh.elements.size()); ++ei) {
        const auto& element = mesh.elements[ei];
        if (element.nodes.empty()) {
            continue;
        }

        if (element.type == ElementType::kPolyVem) {
            Eigen::Matrix<double, 6, 1> eps = Eigen::Matrix<double, 6, 1>::Zero();
            Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
            if (!poly_vem_constant_strain(mesh, element, u, eps, centroid)) {
                continue;
            }
            ElementCentroidStress sample;
            sample.stress = dmat * eps;
            sample.centroid = centroid;
            sample.element_index = ei;
            sample.quality = sample_quality(mesh, element);
            sample.volume = element_volume(mesh, element);
            out.push_back(sample);
            continue;
        }

        Eigen::Matrix<double, Eigen::Dynamic, 3> x(element.nodes.size(), 3);
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[element.nodes[a]].transpose();
            centroid += mesh.nodes[element.nodes[a]];
        }
        centroid /= static_cast<double>(element.nodes.size());

        const Eigen::Vector3d xi = reference_centroid(element.type);
        const auto shape = eval_shape(element.type, xi);
        const Eigen::Matrix3d jac = shape.dn.transpose() * x;
        const double det = jac.determinant();
        if (!(std::abs(det) > 1e-30)) {
            continue;
        }
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

        ElementCentroidStress sample;
        sample.stress = dmat * eps;
        sample.centroid = centroid;
        sample.element_index = ei;
        // Was `std::abs(det)`: |det J| at a single reference point, with the
        // reference domain's own measure dropped. Exact for tet4 only because
        // the line below overrode it, and coincidentally exact for prism6
        // (reference volume 1); 0.125x true for hex8/hex20 and a non-constant
        // ~0.09x for pyramid5. Nothing read `.volume` yet, so it never reached
        // the advisor's labels, but a volume-weighted average over these
        // samples would have been silently wrong per element type.
        sample.volume = element_volume(mesh, element);
        sample.quality = sample_quality(mesh, element);
        out.push_back(sample);
    }
    return out;
}

} // namespace polymesh::fea
