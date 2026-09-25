// SPDX-License-Identifier: BSD-3-Clause
// Boundary loop resolution: collect every element face loop, drop exactly paired
// and opposing-partition interior faces, then repair tears in the shell.
#include "boundary_loops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace polymesh::fea::detail::boundary {
namespace {

void collect_element_loops(const NodalElement& el, std::vector<Loop>& loops) {
    const auto& n = el.nodes;
    auto quad = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        loops.push_back({a, b, c, d});
    };
    auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        loops.push_back({a, b, c});
    };
    switch (el.type) {
    case ElementType::kTet4:
    case ElementType::kTet10:
        if (n.size() < 4) {
            return;
        }
        tri(n[0], n[2], n[1]);
        tri(n[0], n[1], n[3]);
        tri(n[0], n[3], n[2]);
        tri(n[1], n[2], n[3]);
        break;
    case ElementType::kHex8:
    case ElementType::kHex20:
        if (n.size() < 8) {
            return;
        }
        quad(n[0], n[3], n[2], n[1]);
        quad(n[4], n[5], n[6], n[7]);
        quad(n[0], n[1], n[5], n[4]);
        quad(n[1], n[2], n[6], n[5]);
        quad(n[2], n[3], n[7], n[6]);
        quad(n[3], n[0], n[4], n[7]);
        break;
    case ElementType::kPrism6:
        if (n.size() < 6) {
            return;
        }
        tri(n[0], n[2], n[1]);
        tri(n[3], n[4], n[5]);
        quad(n[0], n[1], n[4], n[3]);
        quad(n[1], n[2], n[5], n[4]);
        quad(n[2], n[0], n[3], n[5]);
        break;
    case ElementType::kPyramid5:
        if (n.size() < 5) {
            return;
        }
        quad(n[0], n[1], n[2], n[3]);
        tri(n[0], n[1], n[4]);
        tri(n[1], n[2], n[4]);
        tri(n[2], n[3], n[4]);
        tri(n[3], n[0], n[4]);
        break;
    case ElementType::kPolyVem:
        for (const auto& face : el.faces) {
            if (face.size() < 3) {
                continue;
            }
            Loop loop;
            loop.reserve(face.size());
            bool valid = true;
            for (const std::uint32_t local : face) {
                if (local >= n.size()) {
                    valid = false;
                    break;
                }
                loop.push_back(n[local]);
            }
            if (valid) {
                loops.push_back(std::move(loop));
            }
        }
        break;
    }
}

bool sanitize_loop(const NodalMesh& mesh, Loop& loop) {
    Loop clean;
    clean.reserve(loop.size());
    for (const std::uint32_t node : loop) {
        if (node >= mesh.nodes.size()) {
            return false;
        }
        if (clean.empty() || clean.back() != node) {
            clean.push_back(node);
        }
    }
    if (clean.size() > 1 && clean.front() == clean.back()) {
        clean.pop_back();
    }
    if (clean.size() < 3) {
        return false;
    }
    Loop key = clean;
    std::sort(key.begin(), key.end());
    key.erase(std::unique(key.begin(), key.end()), key.end());
    if (key.size() != clean.size()) {
        return false;
    }
    loop = std::move(clean);
    return true;
}

Loop loop_key(const Loop& loop) {
    Loop key = loop;
    std::sort(key.begin(), key.end());
    return key;
}

} // namespace

std::vector<Loop> resolve_boundary_loops(const NodalMesh& mesh) {
    std::vector<Loop> loops;
    std::vector<Loop> element_loops;
    for (const NodalElement& element : mesh.elements) {
        element_loops.clear();
        collect_element_loops(element, element_loops);
        for (Loop& loop : element_loops) {
            if (sanitize_loop(mesh, loop)) {
                loops.push_back(std::move(loop));
            }
        }
    }

    std::vector<bool> active(loops.size(), true);
    std::map<Loop, std::vector<std::size_t>> exact_owners;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        exact_owners[loop_key(loops[i])].push_back(i);
    }
    for (const auto& [key, owners] : exact_owners) {
        (void)key;
        if (owners.size() > 1) {
            for (const std::size_t owner : owners) {
                active[owner] = false;
            }
        }
    }

    EdgeOwners edge_owners;
    NodeNeighbors neighbors;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (!active[i]) {
            continue;
        }
        for (std::size_t corner = 0; corner < loops[i].size(); ++corner) {
            const std::uint32_t a = loops[i][corner];
            const std::uint32_t b = loops[i][(corner + 1) % loops[i].size()];
            edge_owners[edge(a, b)].push_back(i);
            neighbors[a].insert(b);
            neighbors[b].insert(a);
        }
    }

    // Loops deactivated above are genuinely interior: matched exactly by another
    // element's face. `suppress_opposing_partition` below is a GEOMETRIC judgement
    // and can be wrong; when it is, a real exterior facet disappears and the shell
    // has a hole in it. Remember which loops were exactly paired.
    std::vector<bool> paired_out(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        paired_out[i] = !active[i];
    }

    std::vector<std::size_t> partition_targets;
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (active[i] &&
            (loops[i].size() > 3 ||
             triangle_has_partition_hint(mesh, i, loops, edge_owners, neighbors))) {
            partition_targets.push_back(i);
        }
    }
    std::sort(partition_targets.begin(), partition_targets.end(),
              [&](std::size_t a, std::size_t b) { return loops[a].size() > loops[b].size(); });
    for (const std::size_t target : partition_targets) {
        suppress_opposing_partition(mesh, target, loops, active);
    }

    // Suppression proposes, the shell disposes: revive only loops that step
    // removed (never exactly paired interior faces) and that touch an edge not
    // used exactly twice. Reviving strictly reduces once-used edges, so it ends
    // (ADR-0030).
    for (int repair = 0; repair < 8; ++repair) {
        std::map<Edge, int> edge_use;
        for (std::size_t i = 0; i < loops.size(); ++i) {
            if (!active[i]) {
                continue;
            }
            for (std::size_t c = 0; c < loops[i].size(); ++c) {
                ++edge_use[edge(loops[i][c], loops[i][(c + 1) % loops[i].size()])];
            }
        }
        std::set<Edge> torn;
        for (const auto& [e, count] : edge_use) {
            if (count != 2) {
                torn.insert(e);
            }
        }
        if (torn.empty()) {
            break;
        }
        std::size_t revived = 0;
        for (std::size_t i = 0; i < loops.size(); ++i) {
            if (active[i] || paired_out[i]) {
                continue;
            }
            bool touches = false;
            for (std::size_t c = 0; c < loops[i].size() && !touches; ++c) {
                touches =
                    torn.count(edge(loops[i][c], loops[i][(c + 1) % loops[i].size()])) > 0;
            }
            if (touches) {
                active[i] = true;
                ++revived;
            }
        }
        if (revived == 0) {
            break;
        }
    }

    std::vector<Loop> boundary;
    boundary.reserve(loops.size());
    for (std::size_t i = 0; i < loops.size(); ++i) {
        if (active[i]) {
            boundary.push_back(std::move(loops[i]));
        }
    }
    return boundary;
}

} // namespace polymesh::fea::detail::boundary
