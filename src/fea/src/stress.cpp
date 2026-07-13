// SPDX-License-Identifier: BSD-3-Clause
#include "fea/stress.hpp"

#include "fea/backend.hpp"
#include "fea/shape.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(POLYMESH_WITH_OPENMP)
#include <omp.h>
#endif

namespace polymesh::fea {

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
    std::vector<std::vector<Stress>> thr_stress(
        static_cast<std::size_t>(nthreads),
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
                continue; // constant-strain VEM stress recovered via ZZ path later
            }
            const auto ref = reference_nodes(element.type);
            Eigen::Matrix<double, Eigen::Dynamic, 3> x(element.nodes.size(), 3);
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                x.row(static_cast<Eigen::Index>(a)) =
                    mesh.nodes[element.nodes[a]].transpose();
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

double tet4_aspect_quality(const Eigen::Vector3d& a, const Eigen::Vector3d& b,
                           const Eigen::Vector3d& c, const Eigen::Vector3d& d) {
    const double v = std::abs((b - a).dot((c - a).cross(d - a)) / 6.0);
    if (v <= 0.0) {
        return 0.0;
    }
    const double e[6] = {(a - b).norm(), (a - c).norm(), (a - d).norm(),
                         (b - c).norm(), (b - d).norm(), (c - d).norm()};
    const double emax = *std::max_element(e, e + 6);
    if (emax <= 0.0) {
        return 0.0;
    }
    constexpr double kNorm = 6.0 * 1.4142135623730951;
    return std::min(1.0, kNorm * v / (emax * emax * emax));
}

} // namespace

std::vector<ElementCentroidStress>
recover_element_centroid_stress(const NodalMesh& mesh, const Material& material,
                                const Eigen::VectorXd& u) {
    const auto dmat = material.d_matrix();
    std::vector<ElementCentroidStress> out;
    out.reserve(mesh.elements.size());

    for (std::uint32_t ei = 0; ei < static_cast<std::uint32_t>(mesh.elements.size()); ++ei) {
        const auto& element = mesh.elements[ei];
        if (element.type == ElementType::kPolyVem || element.nodes.empty()) {
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
        sample.volume = std::abs(det); // reference-weight proxy; ok for ranking filters
        if (element.type == ElementType::kTet4 && element.nodes.size() >= 4) {
            sample.quality = tet4_aspect_quality(
                mesh.nodes[element.nodes[0]], mesh.nodes[element.nodes[1]],
                mesh.nodes[element.nodes[2]], mesh.nodes[element.nodes[3]]);
            // True tet volume for area-weight-ish volume metrics.
            sample.volume = std::abs((mesh.nodes[element.nodes[1]] - mesh.nodes[element.nodes[0]])
                                         .dot((mesh.nodes[element.nodes[2]] -
                                               mesh.nodes[element.nodes[0]])
                                                  .cross(mesh.nodes[element.nodes[3]] -
                                                         mesh.nodes[element.nodes[0]]))) /
                            6.0;
        } else {
            sample.quality = 1.0;
        }
        out.push_back(sample);
    }
    return out;
}

} // namespace polymesh::fea
