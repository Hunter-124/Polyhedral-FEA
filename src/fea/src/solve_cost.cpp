// SPDX-License-Identifier: BSD-3-Clause
#include "fea/solve_cost.hpp"

#include "fea/solve.hpp"

#include <Eigen/OrderingMethods>

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <vector>

namespace polymesh::fea {
namespace {

constexpr int kEmpty = -1;

class DisjointSet {
  public:
    explicit DisjointSet(int size) : parent_(static_cast<std::size_t>(size)) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    [[nodiscard]] int find(int node) {
        int root = node;
        while (parent_[static_cast<std::size_t>(root)] != root) {
            root = parent_[static_cast<std::size_t>(root)];
        }
        while (parent_[static_cast<std::size_t>(node)] != node) {
            const int next = parent_[static_cast<std::size_t>(node)];
            parent_[static_cast<std::size_t>(node)] = root;
            node = next;
        }
        return root;
    }

    void link_path(int node, int root) {
        while (parent_[static_cast<std::size_t>(node)] != root) {
            const int next = parent_[static_cast<std::size_t>(node)];
            parent_[static_cast<std::size_t>(node)] = root;
            if (next == node) {
                break;
            }
            node = next;
        }
    }

  private:
    std::vector<int> parent_;
};

Eigen::SparseMatrix<double>
normalized_symmetric_pattern(const Eigen::SparseMatrix<double>& input) {
    if (input.rows() != input.cols()) {
        throw FeaError("analyze_solve_cost: reduced pattern must be square");
    }
    if (input.rows() > static_cast<Eigen::Index>(std::numeric_limits<int>::max())) {
        throw FeaError("analyze_solve_cost: reduced pattern exceeds Eigen's int index range");
    }

    std::vector<Eigen::Triplet<double>> entries;
    entries.reserve(static_cast<std::size_t>(2 * input.nonZeros() + input.rows()));
    for (Eigen::Index col = 0; col < input.outerSize(); ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(input, col); it; ++it) {
            entries.emplace_back(it.row(), it.col(), 1.0);
            if (it.row() != it.col()) {
                entries.emplace_back(it.col(), it.row(), 1.0);
            }
        }
    }
    for (Eigen::Index i = 0; i < input.rows(); ++i) {
        entries.emplace_back(i, i, 1.0);
    }

    Eigen::SparseMatrix<double> pattern(input.rows(), input.cols());
    pattern.setFromTriplets(entries.begin(), entries.end(),
                            [](double, double) { return 1.0; });
    pattern.makeCompressed();
    return pattern;
}

std::vector<int> elimination_forest(const Eigen::SparseMatrix<double>& ordered) {
    const int size = static_cast<int>(ordered.rows());
    std::vector<int> parent(static_cast<std::size_t>(size), kEmpty);
    std::vector<int> ancestor(static_cast<std::size_t>(size), kEmpty);

    // Liu's path-compressed elimination-tree construction. Only the strict
    // upper pattern is visited; the input is structurally symmetric.
    for (int col = 0; col < size; ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(ordered, col); it; ++it) {
            int row = static_cast<int>(it.row());
            if (row >= col) {
                continue;
            }
            while (row != kEmpty && row < col) {
                const int next = ancestor[static_cast<std::size_t>(row)];
                ancestor[static_cast<std::size_t>(row)] = col;
                if (next == kEmpty) {
                    parent[static_cast<std::size_t>(row)] = col;
                }
                row = next;
            }
        }
    }
    return parent;
}

std::vector<int> postorder_forest(const std::vector<int>& parent) {
    const int size = static_cast<int>(parent.size());
    std::vector<int> first_child(static_cast<std::size_t>(size), kEmpty);
    std::vector<int> last_child(static_cast<std::size_t>(size), kEmpty);
    std::vector<int> next_sibling(static_cast<std::size_t>(size), kEmpty);
    for (int node = 0; node < size; ++node) {
        const int p = parent[static_cast<std::size_t>(node)];
        if (p == kEmpty) {
            continue;
        }
        int& first = first_child[static_cast<std::size_t>(p)];
        int& last = last_child[static_cast<std::size_t>(p)];
        if (first == kEmpty) {
            first = node;
        } else {
            next_sibling[static_cast<std::size_t>(last)] = node;
        }
        last = node;
    }

    std::vector<int> post;
    std::vector<int> stack;
    post.reserve(static_cast<std::size_t>(size));
    stack.reserve(static_cast<std::size_t>(size));
    for (int root = 0; root < size; ++root) {
        if (parent[static_cast<std::size_t>(root)] != kEmpty) {
            continue;
        }
        stack.push_back(root);
        while (!stack.empty()) {
            const int node = stack.back();
            int& child = first_child[static_cast<std::size_t>(node)];
            if (child != kEmpty) {
                const int next = child;
                child = next_sibling[static_cast<std::size_t>(next)];
                stack.push_back(next);
            } else {
                post.push_back(node);
                stack.pop_back();
            }
        }
    }
    if (post.size() != parent.size()) {
        throw FeaError("analyze_solve_cost: invalid elimination forest");
    }
    return post;
}

std::vector<int> ldlt_column_counts(const Eigen::SparseMatrix<double>& ordered,
                                    const std::vector<int>& parent) {
    const int size = static_cast<int>(ordered.rows());
    std::vector<int> higher_outer(static_cast<std::size_t>(size + 1), 0);
    for (int col = 1; col < size; ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(ordered, col); it; ++it) {
            if (it.row() < col) {
                ++higher_outer[static_cast<std::size_t>(static_cast<int>(it.row())) + 1];
            }
        }
    }
    std::partial_sum(higher_outer.begin(), higher_outer.end(), higher_outer.begin());
    std::vector<int> higher_inner(static_cast<std::size_t>(higher_outer.back()));
    std::vector<int> cursor(static_cast<std::size_t>(size), 0);
    for (int col = 1; col < size; ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(ordered, col); it; ++it) {
            const int row = static_cast<int>(it.row());
            if (row < col) {
                const int slot = higher_outer[static_cast<std::size_t>(row)] +
                                 cursor[static_cast<std::size_t>(row)]++;
                higher_inner[static_cast<std::size_t>(slot)] = col;
            }
        }
    }

    const std::vector<int> post = postorder_forest(parent);
    std::vector<int> counts(static_cast<std::size_t>(size), 1);
    for (int node = 0; node < size; ++node) {
        const int p = parent[static_cast<std::size_t>(node)];
        if (p != kEmpty) {
            counts[static_cast<std::size_t>(p)] = 0;
        }
    }

    DisjointSet sets(size);
    std::vector<int> previous_leaf(static_cast<std::size_t>(size), kEmpty);
    for (const int node : post) {
        const std::size_t node_index = static_cast<std::size_t>(node);
        counts[node_index] += higher_outer[node_index + 1] - higher_outer[node_index];
        for (int offset = higher_outer[node_index]; offset < higher_outer[node_index + 1];
             ++offset) {
            const int higher = higher_inner[static_cast<std::size_t>(offset)];
            const int previous = previous_leaf[static_cast<std::size_t>(higher)];
            if (previous != kEmpty) {
                const int representative = sets.find(previous);
                sets.link_path(previous, representative);
                --counts[static_cast<std::size_t>(representative)];
            }
            previous_leaf[static_cast<std::size_t>(higher)] = node;
        }
        const int p = parent[static_cast<std::size_t>(node)];
        if (p != kEmpty) {
            sets.link_path(node, p);
        }
    }

    for (int node = 0; node < size; ++node) {
        const int p = parent[static_cast<std::size_t>(node)];
        if (p != kEmpty) {
            counts[static_cast<std::size_t>(p)] += counts[static_cast<std::size_t>(node)] - 1;
        }
        // SimplicialLDLT stores a unit-lower factor and omits its diagonal.
        --counts[static_cast<std::size_t>(node)];
    }
    return counts;
}

Eigen::SparseMatrix<double> free_dof_pattern(const NodalMesh& mesh, const Dirichlet& dirichlet,
                                             const LinearConstraints* constraints) {
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    const bool has_constraints = constraints != nullptr && !constraints->empty();
    if (has_constraints) {
        constraints->validate(ndof);
    }

    const auto is_slave = [&](Eigen::Index dof) {
        return has_constraints && constraints->is_slave(static_cast<std::uint32_t>(dof));
    };
    std::vector<Eigen::Index> original_to_system(static_cast<std::size_t>(ndof), -1);
    Eigen::Index system_size = 0;
    for (Eigen::Index dof = 0; dof < ndof; ++dof) {
        if (!is_slave(dof)) {
            original_to_system[static_cast<std::size_t>(dof)] = system_size++;
        }
    }

    std::vector<char> prescribed(static_cast<std::size_t>(system_size), 0);
    for (const auto& [dof, value] : dirichlet.dof_values) {
        static_cast<void>(value);
        if (dof < 0 || dof >= ndof || is_slave(dof)) {
            throw FeaError("analyze_solve_cost: invalid prescribed DOF");
        }
        prescribed[static_cast<std::size_t>(
            original_to_system[static_cast<std::size_t>(dof)])] = 1;
    }
    std::vector<Eigen::Index> system_to_free(static_cast<std::size_t>(system_size), -1);
    Eigen::Index nfree = 0;
    for (Eigen::Index dof = 0; dof < system_size; ++dof) {
        if (prescribed[static_cast<std::size_t>(dof)] == 0) {
            system_to_free[static_cast<std::size_t>(dof)] = nfree++;
        }
    }

    std::map<std::uint32_t, const LinearConstraint*> slave_entries;
    if (has_constraints) {
        for (const auto& constraint : constraints->entries()) {
            slave_entries.emplace(constraint.slave_dof, &constraint);
        }
    }
    const auto append_free_images = [&](Eigen::Index original,
                                        std::vector<Eigen::Index>& local) {
        if (!is_slave(original)) {
            const Eigen::Index system = original_to_system[static_cast<std::size_t>(original)];
            const Eigen::Index free = system_to_free[static_cast<std::size_t>(system)];
            if (free >= 0) {
                local.push_back(free);
            }
            return;
        }
        const auto entry = slave_entries.find(static_cast<std::uint32_t>(original));
        if (entry == slave_entries.end()) {
            throw FeaError("analyze_solve_cost: missing linear-constraint entry");
        }
        for (const auto& [master, weight] : entry->second->masters) {
            if (weight == 0.0) {
                continue;
            }
            const Eigen::Index system = original_to_system[static_cast<std::size_t>(master)];
            const Eigen::Index free = system_to_free[static_cast<std::size_t>(system)];
            if (free >= 0) {
                local.push_back(free);
            }
        }
    };

    // One reserve for the whole build. Reserving `entries.size() + k` per element
    // asks for an exact capacity every iteration, which defeats the vector's
    // geometric growth and reallocates-and-copies the whole buffer once per
    // element: the 60k-tet10 l_bracket case in tests/test_brep_fidelity.cpp spent
    // over 30 minutes copying triplets instead of the ~1 s the pattern build
    // costs. Every element contributes at most (3 * nodes)^2 entries, since the
    // local DOF list is deduplicated and prescribed DOFs are dropped, so this is
    // an upper bound unless linear constraints expand a slave into several
    // masters -- and then normal geometric growth takes over.
    std::size_t estimated_entries = static_cast<std::size_t>(nfree);
    for (const auto& element : mesh.elements) {
        const std::size_t width = 3 * element.nodes.size();
        estimated_entries += width * width;
    }
    std::vector<Eigen::Triplet<double>> entries;
    entries.reserve(estimated_entries);
    for (const auto& element : mesh.elements) {
        std::vector<Eigen::Index> local;
        local.reserve(3 * element.nodes.size());
        for (const std::uint32_t node : element.nodes) {
            if (node >= mesh.nodes.size()) {
                throw FeaError("analyze_solve_cost: element node out of range");
            }
            for (int axis = 0; axis < 3; ++axis) {
                append_free_images(3 * static_cast<Eigen::Index>(node) + axis, local);
            }
        }
        std::sort(local.begin(), local.end());
        local.erase(std::unique(local.begin(), local.end()), local.end());
        for (const Eigen::Index row : local) {
            for (const Eigen::Index col : local) {
                entries.emplace_back(row, col, 1.0);
            }
        }
    }
    for (Eigen::Index dof = 0; dof < nfree; ++dof) {
        entries.emplace_back(dof, dof, 1.0);
    }

    Eigen::SparseMatrix<double> pattern(nfree, nfree);
    pattern.setFromTriplets(entries.begin(), entries.end(),
                            [](double, double) { return 1.0; });
    pattern.makeCompressed();
    return pattern;
}

} // namespace

SolveCostEstimate analyze_solve_cost(const Eigen::SparseMatrix<double>& pattern_only_kff) {
    SolveCostEstimate out;
    const Eigen::SparseMatrix<double> pattern = normalized_symmetric_pattern(pattern_only_kff);
    out.nfree = pattern.rows();
    out.pattern_nnz = static_cast<std::uint64_t>(pattern.nonZeros());
    if (pattern.rows() == 0) {
        return out;
    }

    Eigen::AMDOrdering<int> amd;
    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> inverse_permutation;
    amd(pattern, inverse_permutation);
    const Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> permutation =
        inverse_permutation.inverse();
    Eigen::SparseMatrix<double> ordered = permutation * pattern * permutation.transpose();
    ordered.makeCompressed();

    const std::vector<int> parent = elimination_forest(ordered);
    const std::vector<int> counts = ldlt_column_counts(ordered, parent);
    for (const int strict_lower_count : counts) {
        if (strict_lower_count < 0) {
            throw FeaError("analyze_solve_cost: negative factor column count");
        }
        out.factor_nnz += static_cast<std::uint64_t>(strict_lower_count);
        const double cholesky_column_size = static_cast<double>(strict_lower_count) + 1.0;
        out.factor_flops += cholesky_column_size * cholesky_column_size;
    }

    std::uint64_t ichol_lower_nnz = 0;
    for (Eigen::Index col = 0; col < pattern.outerSize(); ++col) {
        for (Eigen::SparseMatrix<double>::InnerIterator it(pattern, col); it; ++it) {
            if (it.row() >= it.col()) {
                ++ichol_lower_nnz;
            }
        }
    }
    const double nfree = static_cast<double>(out.nfree);
    out.cg_flops_per_iter = 2.0 * static_cast<double>(out.pattern_nnz) +
                            2.0 * static_cast<double>(ichol_lower_nnz) + 10.0 * nfree;
    out.cg_bytes_per_iter = 12.0 * static_cast<double>(out.pattern_nnz) + 6.0 * 8.0 * nfree;
    return out;
}

double estimate_direct_solve_bytes(const SolveCostEstimate& estimate) {
    // CSR pattern/value stream once; strict-L value/index stream once to form
    // and twice to solve; diagonal and dense-vector streams.
    return 12.0 * static_cast<double>(estimate.pattern_nnz) +
           36.0 * static_cast<double>(estimate.factor_nnz) +
           40.0 * static_cast<double>(estimate.nfree);
}
SolveCostEstimate analyze_solve_cost(const NodalMesh& mesh, const Dirichlet& dirichlet,
                                     const LinearConstraints* constraints) {
    return analyze_solve_cost(free_dof_pattern(mesh, dirichlet, constraints));
}

} // namespace polymesh::fea
