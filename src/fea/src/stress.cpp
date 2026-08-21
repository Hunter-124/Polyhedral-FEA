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
#include <cstddef>
#include <cstdint>
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

std::vector<double> nodal_scalar_gradient_magnitude(const NodalMesh& mesh,
                                                    const std::vector<double>& nodal,
                                                    std::size_t* n_unresolved) {
    if (n_unresolved != nullptr) {
        *n_unresolved = 0;
    }
    const std::size_t n_nodes = mesh.nodes.size();
    if (nodal.size() != n_nodes) {
        return {};
    }
    std::vector<double> magnitude(n_nodes, 0.0);
    if (n_nodes == 0) {
        return magnitude;
    }

    // Node -> incident element adjacency, CSR (counts, prefix sum, fill). The
    // per-node patch is otherwise a scan of every element, which is O(N·E).
    // Measured on a uniform hex lattice: 20³ (9261 nodes, 8000 elements) takes
    // 2.3 ms through this table against 232 ms for a per-node element scan
    // doing the same patch assembly and 3×3 eigensolve, and 50³ (132651 nodes)
    // takes 34 ms, where the scan's work is another 220× larger.
    std::vector<std::size_t> offset(n_nodes + 1, 0);
    for (const auto& element : mesh.elements) {
        for (const std::uint32_t node : element.nodes) {
            if (node < n_nodes) {
                ++offset[node + 1];
            }
        }
    }
    for (std::size_t i = 0; i < n_nodes; ++i) {
        offset[i + 1] += offset[i];
    }
    std::vector<std::uint32_t> incident(offset[n_nodes], 0);
    {
        std::vector<std::size_t> cursor(offset.begin(), offset.end() - 1);
        for (std::size_t e = 0; e < mesh.elements.size(); ++e) {
            for (const std::uint32_t node : mesh.elements[e].nodes) {
                if (node < n_nodes) {
                    incident[cursor[node]++] = static_cast<std::uint32_t>(e);
                }
            }
        }
    }

    // Rank floor on λ_min/λ_max of the 3×3 normal matrix. Measured on patches
    // rotated off the axes, so a null direction is a cancellation of three
    // nonzero rows rather than an exactly zero row — the hard case:
    //   - rank-deficient (collinear tet, coplanar tet, one-cell-thick lattice
    //     from 2×2 to 16×16): |ratio| ≤ 2.4e-16, and it comes out *negative*
    //     as often as positive, so the test is written as a strict `>`;
    //   - thin but genuinely three-dimensional: a 1000:1 flattened hex lattice
    //     sits at 4.4e-7 and recovers a linear field to 7.5e-12 relative, a
    //     10000:1 one at 4.4e-9 and 5.5e-9 relative;
    //   - well shaped: structured hex 0.12…0.25, Kuhn tet 0.10.
    // 1e-10 sits in the gap: six decades above the round-off a rank-deficient
    // patch produces, one decade below the flattest patch measured that still
    // recovers a real slope.
    constexpr double kRankFloor = 1e-10;

    // `stamp[j] == i` marks node j as already in node i's patch, so a neighbour
    // shared by k elements enters the sum once — the fit is unweighted, and
    // counting multiplicity would silently weight it by valence. One array
    // reused across all nodes: no allocation inside the loop.
    constexpr std::size_t kUnstamped = static_cast<std::size_t>(-1);
    std::vector<std::size_t> stamp(n_nodes, kUnstamped);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver;
    std::size_t unresolved = 0;

    for (std::size_t i = 0; i < n_nodes; ++i) {
        const Eigen::Vector3d& xi = mesh.nodes[i];
        const double si = nodal[i];
        // Offsets are taken from x_i, not from the global origin: posing the fit
        // in absolute coordinates is what made the ZZ patch fit rank deficient
        // for a mesh sitting far from the origin (see zz.cpp).
        Eigen::Matrix3d normal_matrix = Eigen::Matrix3d::Zero();
        Eigen::Vector3d rhs = Eigen::Vector3d::Zero();
        std::size_t patch_size = 0;
        stamp[i] = i;
        for (std::size_t k = offset[i]; k < offset[i + 1]; ++k) {
            for (const std::uint32_t j : mesh.elements[incident[k]].nodes) {
                if (j >= n_nodes || stamp[j] == i) {
                    continue;
                }
                stamp[j] = i;
                const Eigen::Vector3d d = mesh.nodes[j] - xi;
                normal_matrix += d * d.transpose();
                rhs += d * (nodal[j] - si);
                ++patch_size;
            }
        }
        if (patch_size < 3) {
            ++unresolved;
            continue;
        }
        solver.compute(normal_matrix);
        if (solver.info() != Eigen::Success) {
            ++unresolved;
            continue;
        }
        // Ascending, and PSD by construction, so [2] is the largest.
        const Eigen::Vector3d& lambda = solver.eigenvalues();
        if (!(lambda[0] > kRankFloor * lambda[2])) {
            ++unresolved;
            continue;
        }
        // Spectral solve reusing the decomposition the rank test already paid
        // for: g = V Λ⁻¹ Vᵀ rhs, exact for an SPD normal matrix.
        const Eigen::Matrix3d& v = solver.eigenvectors();
        const Eigen::Vector3d g = v * (v.transpose() * rhs).cwiseQuotient(lambda);
        const double norm = g.norm();
        if (!std::isfinite(norm)) {
            ++unresolved;
            continue;
        }
        magnitude[i] = norm;
    }

    if (n_unresolved != nullptr) {
        *n_unresolved = unresolved;
    }
    return magnitude;
}

} // namespace polymesh::fea
