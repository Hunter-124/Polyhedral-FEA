// SPDX-License-Identifier: BSD-3-Clause
// Boundary census and shell-guarded deletion for the graded tet fill.
#include "hybrid_fill_internal.hpp"
#include "topology_keys.hpp"

#include "mesh/fill_progress.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh::detail {
namespace {

/// Undirected edge packed as (min << 32) | max.
std::uint64_t pack_edge(std::uint32_t x, std::uint32_t y) {
    return x < y ? (static_cast<std::uint64_t>(x) << 32) | y
                 : (static_cast<std::uint64_t>(y) << 32) | x;
}

} // namespace

std::vector<std::uint32_t>
tet_boundary_nodes(const std::vector<std::array<std::uint32_t, 4>>& tets,
                   const std::vector<Eigen::Vector3d>& nodes) {
    std::unordered_map<TriKey, int, TriKeyMixHash> count;
    count.reserve(tets.size() * 2);
    for (std::size_t work_done = 0; const auto& t : tets) {
        if (active_fill_progress != nullptr) fill_progress_poll(work_done++, tets.size());
        for (const auto& f : kTetFaces) {
            ++count[sorted_tri_key(t[static_cast<std::size_t>(f[0])],
                                   t[static_cast<std::size_t>(f[1])],
                                   t[static_cast<std::size_t>(f[2])])];
        }
    }
    std::unordered_set<std::uint32_t> nodes_set;
    nodes_set.reserve(count.size());
    for (std::size_t work_done = 0; const auto& [key, c] : count) {
        if (active_fill_progress != nullptr) fill_progress_poll(work_done++, count.size());
        if (c == 1) {
            nodes_set.insert(key[0]);
            nodes_set.insert(key[1]);
            nodes_set.insert(key[2]);
        }
    }
    std::vector<std::uint32_t> out(nodes_set.begin(), nodes_set.end());
    // Mirror-canonical order, not bucket order and not ascending node id: this
    // list is the iteration order of snap_round's boundary snap and per-node
    // re-project, where each node moves, tests its skin tets, and reverts on
    // inversion — so an earlier node's accepted move changes whether a later one
    // inverts. Bucket order differs across standard libraries (ADR-0032) and node
    // ids do not mirror (ADR-0036).
    //
    // Sorting on distance-from-centre per axis, quantised so a mirror pair keys
    // bit-identically, makes a node and its mirror image adjacent in the order
    // with the same predecessor set up to reflection. Ties fall back to node id so
    // the order stays total and platform-independent.
    if (out.size() > 1) {
        Eigen::Vector3d lo = nodes[out.front()];
        Eigen::Vector3d hi = lo;
        for (const auto ni : out) {
            fill_progress_poll();
            lo = lo.cwiseMin(nodes[ni]);
            hi = hi.cwiseMax(nodes[ni]);
        }
        const Eigen::Vector3d center = 0.5 * (lo + hi);
        const double quantum = 1e-9 * (hi - lo).norm();
        const double inv_q = quantum > 0.0 ? 1.0 / quantum : 0.0;
        std::vector<std::array<long long, 3>> key(nodes.size());
        for (const auto ni : out) {
            fill_progress_poll();
            const Eigen::Vector3d d = (nodes[ni] - center).cwiseAbs() * inv_q;
            key[ni] = {static_cast<long long>(d.x()), static_cast<long long>(d.y()),
                       static_cast<long long>(d.z())};
        }
        std::sort(out.begin(), out.end(), [&](std::uint32_t a, std::uint32_t b) {
            return key[a] != key[b] ? key[a] < key[b] : a < b;
        });
    }
    return out;
}

TetShellTopology tet_shell_topology(const std::vector<std::array<std::uint32_t, 4>>& tets) {
    std::unordered_map<TriKey, int, TriKeyMixHash> face_use;
    face_use.reserve(tets.size() * 2);
    for (std::size_t work_done = 0; const auto& t : tets) {
        if (active_fill_progress != nullptr) fill_progress_poll(work_done++, tets.size());
        for (const auto& f : kTetFaces) {
            ++face_use[sorted_tri_key(t[static_cast<std::size_t>(f[0])],
                                      t[static_cast<std::size_t>(f[1])],
                                      t[static_cast<std::size_t>(f[2])])];
        }
    }
    std::unordered_map<std::uint64_t, int> edge_use;
    edge_use.reserve(face_use.size());
    for (std::size_t work_done = 0; const auto& [face, count] : face_use) {
        if (active_fill_progress != nullptr) fill_progress_poll(work_done++, face_use.size());
        if (count != 1) {
            continue;
        }
        ++edge_use[pack_edge(face[0], face[1])];
        ++edge_use[pack_edge(face[0], face[2])];
        ++edge_use[pack_edge(face[1], face[2])];
    }
    TetShellTopology out;
    for (std::size_t work_done = 0; const auto& [edge, count] : edge_use) {
        if (active_fill_progress != nullptr) fill_progress_poll(work_done++, edge_use.size());
        if (count != 2) {
            out.torn.insert(edge);
        }
    }
    out.n_torn_edges = out.torn.size();
    return out;
}

bool restrict_kill_to_shell(const std::vector<std::array<std::uint32_t, 4>>& tets,
                            std::vector<char>& kill, int max_rounds) {
    const auto entry = tet_shell_topology(tets);
    const auto proposed = kill;
    const std::size_t n_proposed =
        static_cast<std::size_t>(std::count(kill.begin(), kill.end(), static_cast<char>(1)));
    // A slit is local: the repair may not grow the deletion without bound.
    const std::size_t kill_ceiling = 3 * n_proposed + 64;

    std::vector<std::array<std::uint32_t, 4>> survivors;
    std::vector<std::size_t> survivor_index;
    survivors.reserve(tets.size());
    survivor_index.reserve(tets.size());
    const auto fresh_tears = [&]() {
        survivors.clear();
        survivor_index.clear();
        for (std::size_t ti = 0; ti < tets.size(); ++ti) {
            fill_progress_poll();
            if (!kill[ti]) {
                survivors.push_back(tets[ti]);
                survivor_index.push_back(ti);
            }
        }
        std::unordered_set<std::uint64_t> fresh;
        for (const auto e : tet_shell_topology(survivors).torn) {
            fill_progress_poll();
            if (entry.torn.count(e) == 0) {
                fresh.insert(e);
            }
        }
        return fresh;
    };
    const auto touches_any = [&](const std::array<std::uint32_t, 4>& t,
                                 const std::unordered_set<std::uint64_t>& edges) {
        for (std::size_t i = 0; i < 4; ++i) {
            for (std::size_t j = i + 1; j < 4; ++j) {
                if (edges.count(pack_edge(t[i], t[j])) != 0) {
                    return true;
                }
            }
        }
        return false;
    };

    std::size_t killed = n_proposed;
    for (int round = 0; round < max_rounds; ++round) {
        fill_progress_poll();
        const auto fresh = fresh_tears();
        if (fresh.empty()) {
            return true;
        }
        // Free-face count per survivor: a survivor at a fresh tear carrying two
        // or more exposed faces is the stranded spike.
        std::unordered_map<std::uint64_t, int> face_use;
        face_use.reserve(survivors.size() * 2);
        const auto face_hash = [](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            std::array<std::uint32_t, 3> v{{a, b, c}};
            std::sort(v.begin(), v.end());
            std::uint64_t h = v[0];
            h = h * 1000003U + v[1];
            h = h * 1000003U + v[2];
            return h;
        };
        for (std::size_t work_done = 0; const auto& t : survivors) {
            if (active_fill_progress != nullptr) fill_progress_poll(work_done++, survivors.size());
            for (const auto& f : kTetFaces) {
                ++face_use[face_hash(t[static_cast<std::size_t>(f[0])],
                                     t[static_cast<std::size_t>(f[1])],
                                     t[static_cast<std::size_t>(f[2])])];
            }
        }
        std::size_t extended = 0;
        for (std::size_t si = 0; si < survivors.size(); ++si) {
            fill_progress_poll();
            const auto& t = survivors[si];
            if (!touches_any(t, fresh)) {
                continue;
            }
            int free_faces = 0;
            for (const auto& f : kTetFaces) {
                if (face_use[face_hash(t[static_cast<std::size_t>(f[0])],
                                       t[static_cast<std::size_t>(f[1])],
                                       t[static_cast<std::size_t>(f[2])])] == 1) {
                    ++free_faces;
                }
            }
            if (free_faces >= 2) {
                kill[survivor_index[si]] = 1;
                ++extended;
            }
        }
        killed += extended;
        if (extended == 0 || killed > kill_ceiling) {
            break;
        }
    }

    // Extension did not close it. Fall back to backfilling the minimum.
    kill = proposed;
    for (int round = 0; round < max_rounds; ++round) {
        fill_progress_poll();
        const auto fresh = fresh_tears();
        if (fresh.empty()) {
            return true;
        }
        std::size_t revived = 0;
        for (std::size_t ti = 0; ti < tets.size(); ++ti) {
            fill_progress_poll();
            if (kill[ti] && touches_any(tets[ti], fresh)) {
                kill[ti] = 0;
                ++revived;
            }
        }
        if (revived == 0) {
            break;
        }
    }
    if (fresh_tears().empty()) {
        return true;
    }
    std::fill(kill.begin(), kill.end(), 0);
    return false;
}

} // namespace polymesh::mesh::detail
