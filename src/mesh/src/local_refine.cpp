// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/local_refine.hpp"

#include "mesh/poly_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <unordered_set>
#include <utility>

namespace polymesh::mesh {
namespace {

using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;

EdgeKey make_edge(std::uint32_t a, std::uint32_t b) {
    return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
}

bool tet_has_edge(const std::array<std::uint32_t, 4>& tet, EdgeKey e) {
    bool has_a = false;
    bool has_b = false;
    for (const auto v : tet) {
        if (v == e.first) {
            has_a = true;
        }
        if (v == e.second) {
            has_b = true;
        }
    }
    return has_a && has_b;
}

/// Longest edge; ties → lexicographically smaller ordered endpoint pair.
EdgeKey longest_edge(const std::array<std::uint32_t, 4>& tet,
                     const std::vector<Eigen::Vector3d>& nodes) {
    EdgeKey best = make_edge(tet[0], tet[1]);
    double best_len2 = (nodes[tet[0]] - nodes[tet[1]]).squaredNorm();
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            const EdgeKey e =
                make_edge(tet[static_cast<std::size_t>(i)], tet[static_cast<std::size_t>(j)]);
            const double len2 = (nodes[e.first] - nodes[e.second]).squaredNorm();
            if (len2 > best_len2 + 1e-30 ||
                (std::abs(len2 - best_len2) <= 1e-30 && e < best)) {
                best_len2 = len2;
                best = e;
            }
        }
    }
    return best;
}

/// Force positive signed volume by swapping two vertices if needed.
std::array<std::uint32_t, 4> orient_positive(const std::array<std::uint32_t, 4>& tet,
                                             const std::vector<Eigen::Vector3d>& nodes) {
    std::array<std::uint32_t, 4> out = tet;
    const double v =
        tet_signed_volume(nodes[out[0]], nodes[out[1]], nodes[out[2]], nodes[out[3]]);
    if (v < 0.0) {
        std::swap(out[0], out[1]);
    }
    return out;
}

/// Bisect tet along edge e at midpoint node `mid`.
std::array<std::array<std::uint32_t, 4>, 2>
bisect_tet(const std::array<std::uint32_t, 4>& tet, EdgeKey e, std::uint32_t mid,
           const std::vector<Eigen::Vector3d>& nodes) {
    std::uint32_t c = 0;
    std::uint32_t d = 0;
    int found = 0;
    for (const auto v : tet) {
        if (v == e.first || v == e.second) {
            continue;
        }
        if (found == 0) {
            c = v;
        } else {
            d = v;
        }
        ++found;
    }
    if (found != 2) {
        throw ValidityError("local_refine_tets: bisect_tet edge not on tet");
    }

    // Children (a,m,c,d) and (m,b,c,d); orient each to positive volume.
    std::array<std::array<std::uint32_t, 4>, 2> children{
        {orient_positive({e.first, mid, c, d}, nodes),
         orient_positive({mid, e.second, c, d}, nodes)}};

    // Sanity: both should be strictly positive for a non-degenerate parent.
    for (const auto& ch : children) {
        const double v =
            tet_signed_volume(nodes[ch[0]], nodes[ch[1]], nodes[ch[2]], nodes[ch[3]]);
        if (!(v > 0.0)) {
            throw ValidityError(
                std::format("local_refine_tets: non-positive child volume {:.3e}", v));
        }
    }
    return children;
}

} // namespace

TetFillOutput local_refine_tets(std::vector<Eigen::Vector3d> nodes,
                                std::vector<std::array<std::uint32_t, 4>> tets,
                                std::span<const std::size_t> marked, LocalRefineStats* stats) {
    LocalRefineStats local_stats;
    local_stats.n_input_tets = tets.size();
    local_stats.n_marked = marked.size();

    if (tets.empty()) {
        throw ValidityError("local_refine_tets: empty tet list");
    }
    if (nodes.empty()) {
        throw ValidityError("local_refine_tets: empty node list");
    }

    const std::size_t n_nodes0 = nodes.size();
    for (std::size_t e = 0; e < tets.size(); ++e) {
        for (const auto idx : tets[e]) {
            if (idx >= nodes.size()) {
                throw ValidityError(
                    std::format("local_refine_tets: tet {} bad node index {}", e, idx));
            }
        }
        // Orient parents positive so children stay well-defined.
        tets[e] = orient_positive(tets[e], nodes);
        const double v = tet_signed_volume(nodes[tets[e][0]], nodes[tets[e][1]],
                                           nodes[tets[e][2]], nodes[tets[e][3]]);
        if (!(v > 0.0)) {
            throw ValidityError(
                std::format("local_refine_tets: tet {} degenerate volume {:.3e}", e, v));
        }
    }

    std::unordered_set<std::size_t> remaining;
    remaining.reserve(marked.size() * 2 + 8);
    for (const auto m : marked) {
        if (m >= tets.size()) {
            throw ValidityError(std::format(
                "local_refine_tets: marked index {} out of range ({})", m, tets.size()));
        }
        remaining.insert(m);
    }
    // Unique mark count for stats.
    local_stats.n_marked = remaining.size();

    if (remaining.empty()) {
        TetFillOutput out;
        out.nodes = std::move(nodes);
        out.tets = std::move(tets);
        local_stats.n_output_tets = out.tets.size();
        if (stats != nullptr) {
            *stats = local_stats;
        }
        return out;
    }

    std::map<EdgeKey, std::uint32_t> midpoints;
    auto midpoint_of = [&](EdgeKey e) -> std::uint32_t {
        const auto [it, inserted] =
            midpoints.try_emplace(e, static_cast<std::uint32_t>(nodes.size()));
        if (inserted) {
            nodes.push_back(0.5 * (nodes[e.first] + nodes[e.second]));
        }
        return it->second;
    };

    // Each iteration bisects one terminal LEPP edge (all sharers at once).
    const std::size_t max_iters = tets.size() * 64 + 1024;
    for (std::size_t iter = 0; iter < max_iters && !remaining.empty(); ++iter) {
        // Lowest remaining mark index (stable).
        const std::size_t seed = *std::min_element(remaining.begin(), remaining.end());
        if (seed >= tets.size()) {
            remaining.erase(seed);
            continue;
        }

        // LEPP walk to a terminal edge.
        std::size_t walk = seed;
        EdgeKey edge = longest_edge(tets[walk], nodes);
        for (int lepp = 0; lepp < 4096; ++lepp) {
            bool moved = false;
            for (std::size_t n = 0; n < tets.size(); ++n) {
                if (!tet_has_edge(tets[n], edge)) {
                    continue;
                }
                const EdgeKey en = longest_edge(tets[n], nodes);
                if (en != edge) {
                    walk = n;
                    edge = en;
                    moved = true;
                    break;
                }
            }
            if (!moved) {
                break;
            }
        }

        // All tets currently sharing the terminal edge.
        std::vector<std::size_t> sharers;
        sharers.reserve(8);
        for (std::size_t n = 0; n < tets.size(); ++n) {
            if (tet_has_edge(tets[n], edge)) {
                sharers.push_back(n);
            }
        }
        if (sharers.empty()) {
            remaining.erase(seed);
            continue;
        }

        const std::uint32_t mid = midpoint_of(edge);
        std::unordered_set<std::size_t> share_set(sharers.begin(), sharers.end());

        std::vector<std::array<std::uint32_t, 4>> new_tets;
        new_tets.reserve(tets.size() + sharers.size());
        std::unordered_set<std::size_t> new_remaining;
        new_remaining.reserve(remaining.size());

        for (std::size_t i = 0; i < tets.size(); ++i) {
            if (!share_set.contains(i)) {
                const std::size_t new_idx = new_tets.size();
                new_tets.push_back(tets[i]);
                if (remaining.contains(i)) {
                    new_remaining.insert(new_idx);
                }
            } else {
                const auto children = bisect_tet(tets[i], edge, mid, nodes);
                new_tets.push_back(children[0]);
                new_tets.push_back(children[1]);
                ++local_stats.n_bisections;
                // One-level marks: parent satisfied; children unmarked.
            }
        }

        tets = std::move(new_tets);
        remaining = std::move(new_remaining);
    }

    if (!remaining.empty()) {
        throw ValidityError(std::format("local_refine_tets: failed to clear marks ({} left)",
                                        remaining.size()));
    }

    TetFillOutput out;
    out.nodes = std::move(nodes);
    out.tets = std::move(tets);
    // boundary_quads intentionally empty — lattice skin invalid after LEB.
    out.h = 0.0;

    local_stats.n_output_tets = out.tets.size();
    local_stats.n_new_nodes = out.nodes.size() - n_nodes0;
    if (stats != nullptr) {
        *stats = local_stats;
    }

    check_tet_fill_geometry(out, 0.0);
    return out;
}

TetFillOutput local_refine_tets(const TetFillOutput& mesh, std::span<const std::size_t> marked,
                                LocalRefineStats* stats) {
    return local_refine_tets(mesh.nodes, mesh.tets, marked, stats);
}

} // namespace polymesh::mesh
