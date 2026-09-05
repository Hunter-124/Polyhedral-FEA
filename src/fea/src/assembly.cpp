// SPDX-License-Identifier: BSD-3-Clause
#include "fea/assembly.hpp"

#include "fea/backend.hpp"
#include "fea/quadrature.hpp"
#include "fea/shape.hpp"
#include "fea/vem.hpp"

#include "mesh/cell_validity.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <string>
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
    // Pyramid5: two tet4s split along the base diagonal chosen by
    // mesh::validity::pyramid_split_diagonal — a base-quad-only rule (apex-
    // dependent per-cell choices made the shared-face triangulations mismatch:
    // non-conforming field, constant-strain patch test 1e-12 → 2e-5). The snap
    // validity gates use the same function, so gate and integrator agree.
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
        // TEMP-DIAG removed; choose the shared-face-consistent diagonal.
        if (mesh::validity::pyramid_split_diagonal(mesh.nodes[n[0]], mesh.nodes[n[1]],
                                                   mesh.nodes[n[2]], mesh.nodes[n[3]]) == 1) {
            add_tet({{1, 2, 3, 4}});
            add_tet({{1, 3, 0, 4}});
        } else {
            add_tet({{0, 1, 2, 4}});
            add_tet({{0, 2, 3, 4}});
        }
        return k;
    }
    // Hex8: always GATE-1 isoparametric trilinear. Hybrid zoo product FE expands
    // lattice hex → pyramids (ADR-0012 v3 / ADR-0013) so mixed meshes no longer
    // force Kuhn-PL assembly of bulk hex.
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

namespace {

/// Compressed node-to-node adjacency of a mesh, including each node itself:
/// exactly the block pattern the stiffness matrix has. Built by counting then
/// filling, so it allocates its final size once instead of growing 3N vectors.
struct NodeAdjacency {
    std::vector<std::uint64_t> offsets; // n_nodes + 1
    std::vector<std::uint32_t> neighbours;
};

NodeAdjacency build_node_adjacency(const NodalMesh& mesh) {
    const auto n_nodes = mesh.nodes.size();
    NodeAdjacency adj;
    adj.offsets.assign(n_nodes + 1, 0);
    for (const auto& element : mesh.elements) {
        const auto n = static_cast<std::uint64_t>(element.nodes.size());
        for (const auto node : element.nodes) {
            adj.offsets[static_cast<std::size_t>(node) + 1] += n;
        }
    }
    for (std::size_t i = 0; i < n_nodes; ++i) {
        adj.offsets[i + 1] += adj.offsets[i];
    }
    std::vector<std::uint32_t> raw(static_cast<std::size_t>(adj.offsets.back()));
    std::vector<std::uint64_t> cursor(adj.offsets.begin(), adj.offsets.end() - 1);
    for (const auto& element : mesh.elements) {
        for (const auto row : element.nodes) {
            auto& at = cursor[row];
            for (const auto col : element.nodes) {
                raw[static_cast<std::size_t>(at++)] = col;
            }
        }
    }
    // Sort/unique each node's list, then compact. The compaction is in place
    // over the same buffer because unique counts never exceed raw counts.
    std::vector<std::uint64_t> unique_offsets(n_nodes + 1, 0);
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (std::ptrdiff_t i = 0; i < static_cast<std::ptrdiff_t>(n_nodes); ++i) {
        const auto iu = static_cast<std::size_t>(i);
        const auto begin = raw.begin() + static_cast<std::ptrdiff_t>(adj.offsets[iu]);
        const auto end = raw.begin() + static_cast<std::ptrdiff_t>(adj.offsets[iu + 1]);
        std::sort(begin, end);
        unique_offsets[iu + 1] =
            static_cast<std::uint64_t>(std::distance(begin, std::unique(begin, end)));
    }
    for (std::size_t i = 0; i < n_nodes; ++i) {
        unique_offsets[i + 1] += unique_offsets[i];
    }
    adj.neighbours.resize(static_cast<std::size_t>(unique_offsets.back()));
    for (std::size_t i = 0; i < n_nodes; ++i) {
        const auto src = raw.begin() + static_cast<std::ptrdiff_t>(adj.offsets[i]);
        const auto count = unique_offsets[i + 1] - unique_offsets[i];
        std::copy(src, src + static_cast<std::ptrdiff_t>(count),
                  adj.neighbours.begin() + static_cast<std::ptrdiff_t>(unique_offsets[i]));
    }
    adj.offsets = std::move(unique_offsets);
    return adj;
}

} // namespace

Eigen::SparseMatrix<double> assemble_stiffness(const NodalMesh& mesh,
                                               const Material& material) {
    init_runtime_performance();
    mesh.check_validity();
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    const auto ne = static_cast<std::ptrdiff_t>(mesh.elements.size());

    // The pattern is built first, from connectivity alone, and the element
    // matrices are accumulated straight into it. The alternative — one
    // Eigen::Triplet per local entry — costs 16 bytes for every one of the
    // sum(3n * 3n) contributions before a single value is summed: 750 MB of
    // triplets on a 52k-cell tet10 mesh, twice over during the merge, plus the
    // counting sort inside setFromTriplets. The pattern is 12 MB for the same
    // mesh and the sum happens in place.
    const auto adj = build_node_adjacency(mesh);
    const auto n_nodes = mesh.nodes.size();
    Eigen::SparseMatrix<double> global(ndof, ndof);
    const auto nnz = 9 * adj.offsets.back();
    if (nnz > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw FeaError(std::format(
            "assemble_stiffness: {} stiffness nonzeros exceed the {} that Eigen's "
            "int sparse index can address; coarsen the mesh",
            nnz, std::numeric_limits<int>::max()));
    }
    global.resizeNonZeros(static_cast<Eigen::Index>(nnz));
    int* const outer = global.outerIndexPtr();
    int* const inner = global.innerIndexPtr();
    double* const values = global.valuePtr();
    // Column-major storage of a symmetric pattern: column 3*node+axis holds
    // rows 3*neighbour+0..2 for each neighbour in ascending order, so the
    // inner indices come out sorted without a second pass.
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static)
#endif
    for (std::ptrdiff_t node = 0; node < static_cast<std::ptrdiff_t>(n_nodes); ++node) {
        const auto nu = static_cast<std::size_t>(node);
        const auto first = adj.offsets[nu];
        const auto count = adj.offsets[nu + 1] - first;
        for (int axis = 0; axis < 3; ++axis) {
            const auto column = 3 * static_cast<std::size_t>(nu) + static_cast<std::size_t>(axis);
            auto at = 9 * first + 3 * count * static_cast<std::uint64_t>(axis);
            outer[column] = static_cast<int>(at);
            for (std::uint64_t k = 0; k < count; ++k) {
                const auto neighbour = adj.neighbours[static_cast<std::size_t>(first + k)];
                for (int component = 0; component < 3; ++component) {
                    inner[at] = 3 * static_cast<int>(neighbour) + component;
                    values[at] = 0.0;
                    ++at;
                }
            }
        }
    }
    outer[static_cast<std::size_t>(ndof)] = static_cast<int>(nnz);

    // Where element e's (a, b) node block starts in `values`. One binary search
    // per node pair serves all nine of its scalar entries.
    const auto block_at = [&](std::uint32_t row_node, std::uint32_t col_node) {
        const auto first = adj.offsets[col_node];
        const auto last = adj.offsets[col_node + 1];
        const auto begin = adj.neighbours.begin() + static_cast<std::ptrdiff_t>(first);
        const auto found =
            std::lower_bound(begin, adj.neighbours.begin() + static_cast<std::ptrdiff_t>(last),
                             row_node);
        return 9 * first + 3 * static_cast<std::uint64_t>(std::distance(begin, found));
    };

    // Element matrices are computed a CHUNK at a time in parallel into a
    // bounded scratch buffer, then scattered SERIALLY in element order. Two
    // consequences: peak scratch is one chunk instead of one dense matrix per
    // element, and each matrix entry is summed in element order regardless of
    // thread count, so the assembled K is bit-for-bit identical on any host —
    // and identical to what the previous triplet merge produced, since that
    // merge also summed in element order.
    std::size_t max_edof = 0;
    for (const auto& element : mesh.elements) {
        max_edof = std::max(max_edof, 3 * element.nodes.size());
    }
    constexpr std::size_t kChunkScratchBytes = 32ULL << 20;
    const std::size_t per_element_bytes = std::max<std::size_t>(1, max_edof * max_edof) * 8;
    const auto chunk = static_cast<std::ptrdiff_t>(
        std::clamp<std::size_t>(kChunkScratchBytes / per_element_bytes, 1,
                                static_cast<std::size_t>(std::max<std::ptrdiff_t>(ne, 1))));
    std::vector<Eigen::MatrixXd> scratch(static_cast<std::size_t>(chunk));
    std::vector<std::string> failures(static_cast<std::size_t>(chunk));
    for (std::ptrdiff_t begin = 0; begin < ne; begin += chunk) {
        const std::ptrdiff_t end = std::min(ne, begin + chunk);
#if defined(POLYMESH_WITH_OPENMP)
#pragma omp parallel for schedule(static)
#endif
        for (std::ptrdiff_t e = begin; e < end; ++e) {
            const auto slot = static_cast<std::size_t>(e - begin);
            failures[slot].clear();
            try {
                scratch[slot] =
                    element_stiffness(mesh, mesh.elements[static_cast<std::size_t>(e)],
                                      material);
            } catch (const std::exception& ex) {
                failures[slot] = ex.what();
            }
        }
        for (std::ptrdiff_t e = begin; e < end; ++e) {
            const auto slot = static_cast<std::size_t>(e - begin);
            // Lowest element index wins, so a mesh with several bad cells
            // always reports the same one.
            if (!failures[slot].empty()) {
                throw FeaError(failures[slot]);
            }
            const auto& element = mesh.elements[static_cast<std::size_t>(e)];
            const auto& k = scratch[slot];
            const auto n = element.nodes.size();
            for (std::size_t a = 0; a < n; ++a) {
                for (std::size_t b = 0; b < n; ++b) {
                    const auto base = block_at(element.nodes[a], element.nodes[b]);
                    const auto stride =
                        3 * (adj.offsets[element.nodes[b] + 1] - adj.offsets[element.nodes[b]]);
                    for (int i = 0; i < 3; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            // Column 3*node_b+j starts `stride` entries after
                            // column 3*node_b+j-1 within the same node block.
                            values[base + static_cast<std::uint64_t>(j) * stride +
                                   static_cast<std::uint64_t>(i)] +=
                                k(static_cast<Eigen::Index>(3 * a + static_cast<std::size_t>(i)),
                                  static_cast<Eigen::Index>(3 * b + static_cast<std::size_t>(j)));
                        }
                    }
                }
            }
        }
    }
    return global;
}

Eigen::VectorXd assemble_body_load(const NodalMesh& mesh, const BodyForce& body_force) {
    mesh.check_validity();
    Eigen::VectorXd f =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    for (const auto& element : mesh.elements) {
        if (element.type == ElementType::kPolyVem) {
            std::vector<Eigen::Vector3d> coords;
            coords.reserve(element.nodes.size());
            for (auto id : element.nodes) {
                coords.push_back(mesh.nodes[id]);
            }
            const int order = vem_infer_order(element.nodes.size(), element.faces);
            const Eigen::VectorXd fe = vem_body_load(coords, element.faces, body_force, order);
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                f.segment<3>(3 * static_cast<Eigen::Index>(element.nodes[a])) +=
                    fe.segment<3>(3 * static_cast<Eigen::Index>(a));
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
