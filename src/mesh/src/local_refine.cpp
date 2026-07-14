// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/local_refine.hpp"

#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace {

using EdgeKey = std::pair<std::uint32_t, std::uint32_t>;

struct EdgeHash {
    std::size_t operator()(const EdgeKey& e) const noexcept {
        // Splitmix-style mix of endpoint indices.
        std::size_t x = static_cast<std::size_t>(e.first);
        x ^= static_cast<std::size_t>(e.second) + 0x9e3779b97f4a7c15ULL + (x << 6) + (x >> 2);
        return x;
    }
};

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

void tet_edges(const std::array<std::uint32_t, 4>& t, EdgeKey out[6]) {
    out[0] = make_edge(t[0], t[1]);
    out[1] = make_edge(t[0], t[2]);
    out[2] = make_edge(t[0], t[3]);
    out[3] = make_edge(t[1], t[2]);
    out[4] = make_edge(t[1], t[3]);
    out[5] = make_edge(t[2], t[3]);
}

void erase_tet_from_edge(std::vector<std::size_t>& list, std::size_t ti) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] == ti) {
            list[i] = list.back();
            list.pop_back();
            return;
        }
    }
}

// Sorted free-surface triangle key (unpaired tet faces).
using FreeFaceKey = std::array<std::uint32_t, 3>;

FreeFaceKey make_free_face(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    FreeFaceKey f{{a, b, c}};
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    if (f[1] > f[2]) {
        std::swap(f[1], f[2]);
    }
    if (f[0] > f[1]) {
        std::swap(f[0], f[1]);
    }
    return f;
}

struct FreeFaceHash {
    std::size_t operator()(const FreeFaceKey& f) const noexcept {
        std::size_t h = f[0];
        h ^= static_cast<std::size_t>(f[1]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(f[2]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace

TetFillOutput local_refine_tets(std::vector<Eigen::Vector3d> nodes,
                                std::vector<std::array<std::uint32_t, 4>> tets,
                                std::span<const std::size_t> marked, LocalRefineStats* stats,
                                const geom::TriSurface* surface) {
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

    // Live tet flags (tombstones for parents that are replaced in-place as child0
    // stay live; we only ever append child1 — no kill-without-replace path).
    std::vector<char> alive(tets.size(), 1);

    // Edge → incident live tet indices. O(degree) updates per bisection instead of
    // O(n) full-mesh scans (old path rebuilt the entire tet list every split).
    std::unordered_map<EdgeKey, std::vector<std::size_t>, EdgeHash> edge_tets;
    edge_tets.reserve(tets.size() * 3);

    auto link_tet = [&](std::size_t ti) {
        EdgeKey es[6];
        tet_edges(tets[ti], es);
        for (const auto& e : es) {
            edge_tets[e].push_back(ti);
        }
    };
    auto unlink_tet = [&](std::size_t ti) {
        EdgeKey es[6];
        tet_edges(tets[ti], es);
        for (const auto& e : es) {
            auto it = edge_tets.find(e);
            if (it == edge_tets.end()) {
                continue;
            }
            erase_tet_from_edge(it->second, ti);
            if (it->second.empty()) {
                edge_tets.erase(it);
            }
        }
    };

    for (std::size_t i = 0; i < tets.size(); ++i) {
        link_tet(i);
    }

    // Free-surface faces (count==1) for optional surface-aware midpoints.
    // Projecting only free edges avoids collapsing interior chords near thin walls.
    std::unordered_set<FreeFaceKey, FreeFaceHash> free_faces;
    std::unordered_set<EdgeKey, EdgeHash> free_edges;
    if (surface != nullptr && !surface->triangles.empty()) {
        std::unordered_map<FreeFaceKey, int, FreeFaceHash> face_count;
        face_count.reserve(tets.size() * 2);
        static constexpr int kFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
        for (const auto& t : tets) {
            for (const auto& fv : kFaces) {
                ++face_count[make_free_face(t[static_cast<std::size_t>(fv[0])],
                                            t[static_cast<std::size_t>(fv[1])],
                                            t[static_cast<std::size_t>(fv[2])])];
            }
        }
        free_faces.reserve(face_count.size() / 2 + 8);
        free_edges.reserve(face_count.size());
        for (const auto& [fk, c] : face_count) {
            if (c != 1) {
                continue;
            }
            free_faces.insert(fk);
            free_edges.insert(make_edge(fk[0], fk[1]));
            free_edges.insert(make_edge(fk[1], fk[2]));
            free_edges.insert(make_edge(fk[0], fk[2]));
        }
    }

    std::unordered_map<EdgeKey, std::uint32_t, EdgeHash> midpoints;
    midpoints.reserve(remaining.size() * 4 + 16);
    auto midpoint_of = [&](EdgeKey e) -> std::uint32_t {
        const auto [it, inserted] =
            midpoints.try_emplace(e, static_cast<std::uint32_t>(nodes.size()));
        if (inserted) {
            const bool on_free = surface != nullptr && free_edges.count(e) > 0;
            Eigen::Vector3d mid = 0.5 * (nodes[e.first] + nodes[e.second]);
            if (on_free) {
                // Chord of a convex free edge lies outside the solid (into holes);
                // place the mid on the STL so post-snap unsnap is less likely.
                mid = closest_on_surface(*surface, mid).point;
                ++local_stats.n_surface_mids;
            }
            const std::uint32_t mid_id = it->second;
            nodes.push_back(mid);
            // Split free faces that used this edge: (a,b,c) → (a,m,c)+(m,b,c).
            if (on_free) {
                free_edges.erase(e);
                free_edges.insert(make_edge(e.first, mid_id));
                free_edges.insert(make_edge(e.second, mid_id));
                std::vector<FreeFaceKey> hit;
                for (const auto& fk : free_faces) {
                    const bool has_a = (fk[0] == e.first || fk[1] == e.first || fk[2] == e.first);
                    const bool has_b =
                        (fk[0] == e.second || fk[1] == e.second || fk[2] == e.second);
                    if (has_a && has_b) {
                        hit.push_back(fk);
                    }
                }
                for (const auto& fk : hit) {
                    free_faces.erase(fk);
                    std::uint32_t c = fk[0];
                    if (c == e.first || c == e.second) {
                        c = fk[1];
                    }
                    if (c == e.first || c == e.second) {
                        c = fk[2];
                    }
                    free_faces.insert(make_free_face(e.first, mid_id, c));
                    free_faces.insert(make_free_face(mid_id, e.second, c));
                    free_edges.insert(make_edge(mid_id, c));
                }
            }
        }
        return it->second;
    };

    // Each iteration bisects one terminal LEPP edge (all live sharers at once).
    const std::size_t max_iters = tets.size() * 64 + 1024 + remaining.size() * 8;
    for (std::size_t iter = 0; iter < max_iters && !remaining.empty(); ++iter) {
        // Prefer lowest remaining index among still-live tets.
        std::size_t seed = static_cast<std::size_t>(-1);
        for (const auto m : remaining) {
            if (m < alive.size() && alive[m] && (seed == static_cast<std::size_t>(-1) || m < seed)) {
                seed = m;
            }
        }
        if (seed == static_cast<std::size_t>(-1)) {
            remaining.clear();
            break;
        }

        // LEPP walk to a terminal edge (edge whose all live sharers have it as longest).
        std::size_t walk = seed;
        EdgeKey edge = longest_edge(tets[walk], nodes);
        for (int lepp = 0; lepp < 4096; ++lepp) {
            bool moved = false;
            const auto it = edge_tets.find(edge);
            if (it == edge_tets.end()) {
                break;
            }
            for (const auto n : it->second) {
                if (n >= alive.size() || !alive[n]) {
                    continue;
                }
                if (!tet_has_edge(tets[n], edge)) {
                    continue; // stale index safety
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

        // Live sharers of the terminal edge.
        std::vector<std::size_t> sharers;
        sharers.reserve(8);
        if (const auto it = edge_tets.find(edge); it != edge_tets.end()) {
            for (const auto n : it->second) {
                if (n < alive.size() && alive[n] && tet_has_edge(tets[n], edge)) {
                    sharers.push_back(n);
                }
            }
        }
        if (sharers.empty()) {
            remaining.erase(seed);
            continue;
        }

        const std::uint32_t mid = midpoint_of(edge);

        // Choose a midpoint whose bisection keeps every live sharer's children
        // strictly positive. Free-surface projection — or a sliver child left by
        // an earlier split — can invert a tet; fall back to the Euclidean chord,
        // and if even that degenerates, skip this terminal edge entirely. No
        // partial split → the mesh stays conforming and valid; the sliver region
        // just stays coarser instead of crashing the whole mesh (ADR-0016).
        auto children_ok = [&](const Eigen::Vector3d& pos) {
            nodes[mid] = pos;
            for (const auto i : sharers) {
                if (i >= alive.size() || !alive[i] || !tet_has_edge(tets[i], edge)) {
                    continue;
                }
                std::uint32_t c = 0, d = 0;
                int found = 0;
                for (const auto v : tets[i]) {
                    if (v == edge.first || v == edge.second) {
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
                    return false;
                }
                const auto ch0 = orient_positive({edge.first, mid, c, d}, nodes);
                const auto ch1 = orient_positive({mid, edge.second, c, d}, nodes);
                const double v0 = tet_signed_volume(nodes[ch0[0]], nodes[ch0[1]],
                                                    nodes[ch0[2]], nodes[ch0[3]]);
                const double v1 = tet_signed_volume(nodes[ch1[0]], nodes[ch1[1]],
                                                    nodes[ch1[2]], nodes[ch1[3]]);
                if (!(v0 > 0.0) || !(v1 > 0.0)) {
                    return false;
                }
            }
            return true;
        };
        const Eigen::Vector3d chord = 0.5 * (nodes[edge.first] + nodes[edge.second]);
        const Eigen::Vector3d projected = nodes[mid];
        bool can_split = children_ok(projected);
        if (!can_split && (projected - chord).squaredNorm() > 1e-30) {
            can_split = children_ok(chord); // leaves nodes[mid]=chord when true
        }
        if (!can_split) {
            nodes[mid] = projected; // orphan midpoint node compacted downstream
            remaining.erase(seed);
            ++local_stats.n_skipped_slivers;
            continue;
        }

        // Bisect every sharer: replace slot with child0, append child1.
        // Copy sharers first — link/unlink mutates edge_tets lists.
        const std::vector<std::size_t> share_copy = sharers;
        for (const auto i : share_copy) {
            if (i >= alive.size() || !alive[i]) {
                continue;
            }
            if (!tet_has_edge(tets[i], edge)) {
                continue;
            }
            const auto children = bisect_tet(tets[i], edge, mid, nodes);
            unlink_tet(i);
            tets[i] = children[0];
            link_tet(i);
            remaining.erase(i);

            const std::size_t j = tets.size();
            tets.push_back(children[1]);
            alive.push_back(1);
            link_tet(j);
            // One-level marks: parent satisfied; children unmarked.
            ++local_stats.n_bisections;
        }
    }

    if (!remaining.empty()) {
        // Drop marks on dead/orphaned indices; if any live marks remain, fail.
        for (auto it = remaining.begin(); it != remaining.end();) {
            if (*it >= alive.size() || !alive[*it]) {
                it = remaining.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!remaining.empty()) {
        throw ValidityError(std::format("local_refine_tets: failed to clear marks ({} left)",
                                        remaining.size()));
    }

    // Compact live tets.
    std::vector<std::array<std::uint32_t, 4>> compact;
    compact.reserve(tets.size());
    for (std::size_t i = 0; i < tets.size(); ++i) {
        if (alive[i]) {
            compact.push_back(tets[i]);
        }
    }

    TetFillOutput out;
    out.nodes = std::move(nodes);
    out.tets = std::move(compact);
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
                                LocalRefineStats* stats, const geom::TriSurface* surface) {
    return local_refine_tets(mesh.nodes, mesh.tets, marked, stats, surface);
}

} // namespace polymesh::mesh
