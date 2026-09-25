// SPDX-License-Identifier: BSD-3-Clause
#include "fea/zz.hpp"

#include "fea/backend.hpp"
#include "fea/cell_quality.hpp"
#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "fea/vem.hpp"
#include "recovered_strain.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#if defined(POLYMESH_WITH_OPENMP)
#include <omp.h>
#endif

namespace polymesh::fea {
namespace {

Stress stress_at(const NodalElement& element,
                 const Eigen::Matrix<double, Eigen::Dynamic, 3>& x, const Eigen::VectorXd& u,
                 const Eigen::Matrix<double, 6, 6>& d, const Eigen::Vector3d& xi) {
    const auto shape = eval_shape(element.type, xi);
    const Eigen::Matrix3d jac = shape.dn.transpose() * x;
    const Eigen::Matrix3d jac_inv = jac.inverse();
    const Eigen::Matrix<double, Eigen::Dynamic, 3> dndx = shape.dn * jac_inv.transpose();
    const Eigen::Matrix<double, 6, 1> eps = detail::strain_from_gradients(element, dndx, u);
    return d * eps;
}

Eigen::Vector3d element_centroid(const NodalMesh& mesh, const NodalElement& el) {
    Eigen::Vector3d c = Eigen::Vector3d::Zero();
    for (auto n : el.nodes) {
        c += mesh.nodes[n];
    }
    return c / static_cast<double>(el.nodes.size());
}

/// Element volume, m³: the weight that turns the stress-jump norm into an energy.
/// Unlike `fea::element_volume`, FEM types integrate |det J| with the stiffness
/// rule (`default_rule`), not the over-integrated curved-cell rule.
double recovery_volume(const NodalMesh& mesh, const NodalElement& el,
                       const std::vector<QuadraturePoint>& rule) {
    if (el.type == ElementType::kPolyVem) {
        return element_volume(mesh, el);
    }
    Eigen::Matrix<double, Eigen::Dynamic, 3> x(el.nodes.size(), 3);
    for (std::size_t a = 0; a < el.nodes.size(); ++a) {
        x.row(static_cast<Eigen::Index>(a)) = mesh.nodes[el.nodes[a]].transpose();
    }
    double volume = 0.0;
    for (const auto& qp : rule) {
        const auto shape = eval_shape(el.type, qp.xi);
        volume += qp.weight * std::abs((shape.dn.transpose() * x).determinant());
    }
    return volume;
}

} // namespace

ZzRecovery recover_zz(const NodalMesh& mesh, const Material& material,
                      const Eigen::VectorXd& u) {
    init_runtime_performance();
    const auto d = material.d_matrix();
    const auto n_nodes = mesh.nodes.size();
    const auto n_elem = mesh.elements.size();

    // Element centroid stress (superconvergent sampling points for linear elements)
    // and element volume (the weight that makes η an energy, not a stress norm).
    std::vector<Stress> el_stress(n_elem, Stress::Zero());
    std::vector<Eigen::Vector3d> el_cent(n_elem);
    std::vector<double> el_vol(n_elem, 0.0);
    // One quadrature rule per element type present, built once up front.
    std::array<std::vector<QuadraturePoint>, 7> rules;
    for (const auto& el : mesh.elements) {
        const auto ti = static_cast<std::size_t>(el.type);
        if (el.type != ElementType::kPolyVem && rules[ti].empty()) {
            rules[ti] = default_rule(el.type);
        }
    }
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (std::ptrdiff_t e = 0; e < static_cast<std::ptrdiff_t>(n_elem); ++e) {
        const auto eu = static_cast<std::size_t>(e);
        const auto& el = mesh.elements[eu];
        el_cent[eu] = element_centroid(mesh, el);
        el_vol[eu] = recovery_volume(mesh, el, rules[static_cast<std::size_t>(el.type)]);
        if (el.type == ElementType::kPolyVem) {
            // Constant/centroid VEM projected strain → stress (same projector as
            // the stiffness).
            const int order = vem_infer_order(el.nodes.size(), el.faces);
            std::vector<Eigen::Vector3d> coords;
            coords.reserve(el.nodes.size());
            Eigen::VectorXd u_elem(3 * static_cast<Eigen::Index>(el.nodes.size()));
            for (std::size_t a = 0; a < el.nodes.size(); ++a) {
                coords.push_back(mesh.nodes[el.nodes[a]]);
                u_elem.segment<3>(3 * static_cast<Eigen::Index>(a)) =
                    u.segment<3>(3 * static_cast<Eigen::Index>(el.nodes[a]));
            }
            el_stress[eu] = d * vem_projected_strain(coords, el.faces, u_elem, order);
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
        el_stress[eu] = stress_at(el, x, u, d, xi);
    }

    // Node → incident elements, in compressed form: counting pass, then fill in
    // ascending element order so each patch keeps element order (the patch mean
    // and the least-squares fit below are order-sensitive at roundoff level).
    std::vector<std::uint32_t> incident_offsets(n_nodes + 1, 0);
    for (const auto& element : mesh.elements) {
        for (const auto node : element.nodes) {
            ++incident_offsets[node + 1];
        }
    }
    for (std::size_t i = 0; i < n_nodes; ++i) {
        incident_offsets[i + 1] += incident_offsets[i];
    }
    std::vector<std::uint32_t> incident_elements(incident_offsets.back());
    {
        std::vector<std::uint32_t> cursor(incident_offsets.begin(), incident_offsets.end() - 1);
        for (std::size_t e = 0; e < n_elem; ++e) {
            for (const auto node : mesh.elements[e].nodes) {
                incident_elements[cursor[node]++] = static_cast<std::uint32_t>(e);
            }
        }
    }

    // Per-node least-squares fit of stress components: σ(x) ≈ a0 + a·(x − x̄).
    //
    // The fit is written in the patch's OWN frame — coordinates measured from
    // the patch's mean sample point and divided by the patch radius — not in
    // absolute metres. In absolute coordinates the design matrix [1, x, y, z] of
    // a patch of diameter h at distance R from the origin has columns constant
    // to within h/R, so it is numerically rank-deficient by construction and
    // extrapolating its null-space slope to |x| ≈ R amplifies it by R/h. Every
    // node on a flat lattice face makes it EXACTLY singular: its incident
    // element centroids are coplanar.
    //
    // In the patch frame the constant column is orthogonal to the three
    // coordinate columns, so a0 is the patch mean whatever the rank is, and the
    // SVD's minimum-norm solve zeroes the linear part along any direction the
    // patch cannot resolve. A second guard is still required for sparse edge
    // patches: four or five samples can formally span the four-coefficient
    // basis while leaving an extrapolation with enormous statistical leverage.
    // Bound the L2 gain from sample noise to the recovered node; a fit that
    // would amplify it by more than two falls back to the same patch average as
    // the no-linear-part path.
    constexpr double kMaxExtrapolationGain = 2.0;
    ZzRecovery out;
    out.nodal_stress.assign(n_nodes, Stress::Zero());
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (std::ptrdiff_t n = 0; n < static_cast<std::ptrdiff_t>(n_nodes); ++n) {
        const auto nu = static_cast<std::size_t>(n);
        const std::span<const std::uint32_t> patch(
            incident_elements.data() + incident_offsets[nu],
            incident_offsets[nu + 1] - incident_offsets[nu]);
        if (patch.empty()) {
            continue;
        }
        const Eigen::Vector3d& p = mesh.nodes[nu];
        Stress mean_stress = Stress::Zero();
        Eigen::Vector3d mean_off = Eigen::Vector3d::Zero();
        double radius = 0.0;
        for (auto e : patch) {
            mean_stress += el_stress[e];
            const Eigen::Vector3d off = el_cent[e] - p;
            mean_off += off;
            radius = std::max(radius, off.norm());
        }
        const auto count = static_cast<double>(patch.size());
        mean_stress /= count;
        mean_off /= count;
        if (patch.size() < 4 || !(radius > 0.0)) {
            out.nodal_stress[nu] = mean_stress; // no linear part to resolve
            continue;
        }
        const auto m = static_cast<Eigen::Index>(patch.size());
        Eigen::MatrixXd A(m, 4);
        Eigen::MatrixXd B(m, 6);
        for (Eigen::Index i = 0; i < m; ++i) {
            const auto e = patch[static_cast<std::size_t>(i)];
            const Eigen::Vector3d off = (el_cent[e] - p - mean_off) / radius;
            A(i, 0) = 1.0;
            A(i, 1) = off[0];
            A(i, 2) = off[1];
            A(i, 3) = off[2];
            B.row(i) = el_stress[e].transpose();
        }
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeThinU | Eigen::ComputeThinV);
        // Rank tolerance, relative to the largest singular value: a direction
        // counts as sampled only when the patch spreads in it by ~1% of its own
        // radius. Anything thinner is the flat-face / thin-layer null space and
        // its slope is noise divided by ~0. Thresholds of 1e-3…1e-2 are stable;
        // from 5e-2 real slopes start being discarded.
        svd.setThreshold(1e-2);
        // For A = UΣVᵀ, the node prediction's sample weights are
        // row·VΣ⁻¹Uᵀ. U does not change their L2 norm, so evaluate the gain in
        // the four-dimensional basis without allocating an m-entry weight row.
        Eigen::RowVector4d row;
        row << 1.0, -mean_off[0] / radius, -mean_off[1] / radius, -mean_off[2] / radius;
        const Eigen::RowVector4d basis_weights = row * svd.matrixV();
        double extrapolation_gain2 = 0.0;
        for (Eigen::Index i = 0; i < svd.rank(); ++i) {
            const double scaled = basis_weights[i] / svd.singularValues()[i];
            extrapolation_gain2 += scaled * scaled;
        }
        if (!(extrapolation_gain2 <= kMaxExtrapolationGain * kMaxExtrapolationGain)) {
            out.nodal_stress[nu] = mean_stress;
            continue;
        }
        const Eigen::MatrixXd coeff = svd.solve(B);
        // Extrapolate from the patch mean point back to the node itself.
        const Stress fit = (row * coeff).transpose();
        out.nodal_stress[nu] = fit.allFinite() ? fit : mean_stress;
    }

    // Element indicators, energy norm of the recovered stress jump (ZZ):
    //   η_e² = ∫_e (σ*−σ_h)ᵀ D⁻¹ (σ*−σ_h) dV ≈ V_e · dᵀ D⁻¹ d   (centroid rule)
    // divided by the same norm of the FE stress itself so both the per-element
    // indicator and the global number are dimensionless *relative* errors:
    //   η_e = sqrt(η_e² / Σ_f V_f σ_hᵀ D⁻¹ σ_h),  η = sqrt(Σ_e η_e²).
    // Dörfler marking is invariant to the common denominator, so the marking
    // ordering stays a pure energy-share ranking.
    const Eigen::Matrix<double, 6, 6> d_inv = d.inverse();
    out.element_eta.assign(n_elem, 0.0);
    double sum_sq = 0.0;
    double ref_sq = 0.0;
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static) reduction(+ : sum_sq, ref_sq)
#endif
    for (std::ptrdiff_t e = 0; e < static_cast<std::ptrdiff_t>(n_elem); ++e) {
        const auto eu = static_cast<std::size_t>(e);
        const auto& el = mesh.elements[eu];
        Stress star = Stress::Zero();
        for (auto n : el.nodes) {
            star += out.nodal_stress[n];
        }
        star /= static_cast<double>(el.nodes.size());
        const Stress diff = star - el_stress[eu];
        const double e_sq = el_vol[eu] * diff.dot(d_inv * diff);
        out.element_eta[eu] = e_sq; // squared; normalized below
        sum_sq += e_sq;
        ref_sq += el_vol[eu] * el_stress[eu].dot(d_inv * el_stress[eu]);
    }
    if (ref_sq > 0.0) {
        for (auto& eta : out.element_eta) {
            eta = std::sqrt(std::max(0.0, eta) / ref_sq);
        }
        out.global_eta = std::sqrt(std::max(0.0, sum_sq) / ref_sq);
    } else {
        // No strain energy in the solution (u ≡ 0): no relative error to report.
        std::fill(out.element_eta.begin(), out.element_eta.end(), 0.0);
        out.global_eta = 0.0;
    }
    return out;
}

} // namespace polymesh::fea
