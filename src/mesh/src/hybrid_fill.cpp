// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

// Kuhn 6-tet split of a unit cube with corners numbered as hex8.
constexpr std::array<std::array<int, 4>, 6> kCubeTets{{
    {{0, 1, 2, 6}},
    {{0, 2, 3, 6}},
    {{0, 1, 5, 6}},
    {{0, 3, 7, 6}},
    {{0, 4, 5, 6}},
    {{0, 4, 7, 6}},
}};

constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

} // namespace

GradedTetFillOutput
graded_tet_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                        const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                        std::span<const geom::SharpEdge> features, double feature_band,
                        std::span<const Eigen::Vector3d> refine_seeds, double seed_band) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("graded_tet_fill_surface: h must be positive");
    }
    if (skin_layers < 1) {
        skin_layers = 1;
    }
    if (!(feature_band > 0.0) || features.empty()) {
        feature_band = 0.0;
    }
    if (!(seed_band > 0.0) || refine_seeds.empty()) {
        seed_band = 0.0;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("graded_tet_fill_surface: empty bbox");
    }

    // Coarse-primary lattice at target h (classify cost matches tet/hybrid).
    // LEB refine on fine-marked cells (ADR-0018). Cap cells below the global
    // 512k budget: LEB+snap on multi-million-tet meshes is not interactive.
    constexpr int subdiv = 2;
    constexpr std::size_t kGradedMaxCells = 48 * 1024; // ~6 tets/cell → under LEB budget
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kGradedMaxCells, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h_use);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double hc = grid.max_edge();
    const double hf = 0.5 * hc;
    const auto inside = classify_cells_inside(surface, grid);
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz && inside[idx(i, j, k)];
    };

    // Face-only boundary distance (coarse hops).
    std::vector<int> dist(inside.size(), -1);
    std::queue<std::array<int, 3>> q;
    int max_dist = 0;
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (const auto& o : kFaceNbr) {
                    if (!inb(i + o[0], j + o[1], k + o[2])) {
                        boundary = true;
                        break;
                    }
                }
                if (boundary) {
                    dist[idx(i, j, k)] = 0;
                    q.push({i, j, k});
                }
            }
        }
    }
    while (!q.empty()) {
        const auto c = q.front();
        q.pop();
        const int d0 = dist[idx(c[0], c[1], c[2])];
        max_dist = std::max(max_dist, d0);
        for (const auto& o : kFaceNbr) {
            const int ni = c[0] + o[0], nj = c[1] + o[1], nk = c[2] + o[2];
            if (!inb(ni, nj, nk)) {
                continue;
            }
            auto& dn = dist[idx(ni, nj, nk)];
            if (dn < 0 || dn > d0 + 1) {
                dn = d0 + 1;
                q.push({ni, nj, nk});
            }
        }
    }

    // Skin depth in coarse cells; never eat more than half the interior.
    const int skin_cap = std::max(1, (max_dist + 1) / 2);
    const int skin_thresh = std::min(skin_layers, skin_cap);

    std::vector<char> is_fine(inside.size(), 0);
    std::vector<char> is_feature(inside.size(), 0);
    std::vector<char> is_seed(inside.size(), 0);
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (dist[c] >= 0 && dist[c] < skin_thresh) {
            is_fine[c] = 1;
        }
    }
    stamp_feature_cells(is_fine, &is_feature, nx, ny, nz, grid, surface, features, feature_band);
    stamp_seed_cells(is_fine, &is_seed, nx, ny, nz, grid, refine_seeds, seed_band);
    // Only interior cells may be refined.
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            is_fine[c] = 0;
            is_feature[c] = 0;
            is_seed[c] = 0;
        }
    }

    GradedTetFillOutput out;
    out.h_coarse = hc;
    out.h_fine = hf;
    out.skin_layers = skin_layers;
    out.subdivision = subdiv;
    out.mesh.h = hf;

    // ADR-0018: emit a *uniform* coarse Kuhn lattice first, then LEB-refine tets
    // whose centroids lie in fine-marked cells. LEPP closure keeps the mesh
    // face-conforming (no 2:1 hanging nodes). Old path emitted coarse step-2
    // Kuhn next to fine 2×2×2 Kuhn → nonconforming mid-edge nodes.
    std::map<std::array<int, 3>, std::uint32_t> node_ids;
    const auto node_at = [&](int i, int j, int k) {
        const auto [it, fresh] = node_ids.try_emplace(
            std::array<int, 3>{i, j, k}, static_cast<std::uint32_t>(out.mesh.nodes.size()));
        if (fresh) {
            out.mesh.nodes.push_back(grid.node(i, j, k));
        }
        return it->second;
    };

    auto emit_cube_tets = [&](int i, int j, int k) {
        const std::array<std::uint32_t, 8> c{{
            node_at(i, j, k),
            node_at(i + 1, j, k),
            node_at(i + 1, j + 1, k),
            node_at(i, j + 1, k),
            node_at(i, j, k + 1),
            node_at(i + 1, j, k + 1),
            node_at(i + 1, j + 1, k + 1),
            node_at(i, j + 1, k + 1),
        }};
        for (const auto& t : kCubeTets) {
            std::array<std::uint32_t, 4> n{
                {c[static_cast<std::size_t>(t[0])], c[static_cast<std::size_t>(t[1])],
                 c[static_cast<std::size_t>(t[2])], c[static_cast<std::size_t>(t[3])]}};
            double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                         out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
                v = -v;
            }
            if (v > 0.0) {
                out.mesh.tets.push_back(n);
            }
        }
    };

    // Exterior lattice quads for snap / display (before LEB; rebuilt after).
    auto emit_face_quad = [&](int i, int j, int k, int face) {
        // face: 0=-x 1=+x 2=-y 3=+y 4=-z 5=+z
        std::array<std::array<int, 3>, 4> corners{};
        switch (face) {
        case 0:
            corners = {{{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}}};
            break;
        case 1:
            corners = {{{1, 0, 0}, {1, 0, 1}, {1, 1, 1}, {1, 1, 0}}};
            break;
        case 2:
            corners = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}}};
            break;
        case 3:
            corners = {{{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}}};
            break;
        case 4:
            corners = {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}};
            break;
        default:
            corners = {{{0, 0, 1}, {0, 1, 1}, {1, 1, 1}, {1, 0, 1}}};
            break;
        }
        std::array<std::uint32_t, 4> quad{};
        for (int q = 0; q < 4; ++q) {
            const auto& c = corners[static_cast<std::size_t>(q)];
            quad[static_cast<std::size_t>(q)] = node_at(i + c[0], j + c[1], k + c[2]);
        }
        out.mesh.boundary_quads.push_back(quad);
    };

    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto id = idx(i, j, k);
                emit_cube_tets(i, j, k);
                if (is_fine[id]) {
                    ++out.n_fine_cells;
                    if (is_feature[id]) {
                        ++out.n_feature_cells;
                    }
                    if (is_seed[id]) {
                        ++out.n_seed_cells;
                    }
                } else {
                    ++out.n_coarse_cells;
                }
                for (int f = 0; f < 6; ++f) {
                    if (!inb(i + kFaceNbr[f][0], j + kFaceNbr[f][1], k + kFaceNbr[f][2])) {
                        emit_face_quad(i, j, k, f);
                    }
                }
            }
        }
    }

    if (out.mesh.tets.empty()) {
        throw ValidityError("graded_tet_fill_surface: no interior cells");
    }

    // Map a point to the coarse cell that contains it (clamped).
    const auto cell_of_point = [&](const Eigen::Vector3d& p) -> int {
        const Eigen::Vector3d local = p - grid.origin;
        int i = static_cast<int>(std::floor(local[0] / grid.cell[0]));
        int j = static_cast<int>(std::floor(local[1] / grid.cell[1]));
        int k = static_cast<int>(std::floor(local[2] / grid.cell[2]));
        i = std::clamp(i, 0, nx - 1);
        j = std::clamp(j, 0, ny - 1);
        k = std::clamp(k, 0, nz - 1);
        return static_cast<int>(idx(i, j, k));
    };

    // LEB refine tets in fine cells. Prefer lattice boundary nodes for snap
    // (already collected); LEB clears topology-accurate quads but we keep the
    // node ids that were on the free surface.
    std::vector<std::uint32_t> snap_nodes;
    {
        std::set<std::uint32_t> bset;
        for (const auto& q : out.mesh.boundary_quads) {
            bset.insert(q.begin(), q.end());
        }
        snap_nodes.assign(bset.begin(), bset.end());
    }

    const bool any_fine = out.n_fine_cells > 0;
    // LEB is O(n log n) with large constants; skip grading on huge lattices so
    // auto-coarsened tiny-h still returns a conforming coarse mesh quickly.
    constexpr std::size_t kLebTetBudget = 120'000;
    if (any_fine && out.mesh.tets.size() <= kLebTetBudget) {
        // Large-but-under-budget: one LEB pass. Small: up to subdiv.
        const int passes =
            (out.mesh.tets.size() > 40'000) ? 1 : subdiv;
        for (int pass = 0; pass < passes; ++pass) {
            std::vector<std::size_t> marked;
            marked.reserve(out.mesh.tets.size() / 4 + 8);
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                const auto& n = out.mesh.tets[ti];
                const Eigen::Vector3d c =
                    0.25 * (out.mesh.nodes[n[0]] + out.mesh.nodes[n[1]] + out.mesh.nodes[n[2]] +
                            out.mesh.nodes[n[3]]);
                const int cid = cell_of_point(c);
                if (cid >= 0 && static_cast<std::size_t>(cid) < is_fine.size() &&
                    is_fine[static_cast<std::size_t>(cid)]) {
                    marked.push_back(ti);
                }
            }
            if (marked.empty()) {
                break;
            }
            LocalRefineStats st;
            auto refined = local_refine_tets(std::move(out.mesh.nodes), std::move(out.mesh.tets),
                                             marked, &st);
            out.mesh.nodes = std::move(refined.nodes);
            out.mesh.tets = std::move(refined.tets);
            // Keep pre-LEB lattice boundary_quads: corner node ids remain valid
            // (LEB only appends midpoints). Pipeline may replace with exterior
            // faces for display; tests use quads for snap residual checks.
        }
    }

    if (!snap_nodes.empty()) {
        // Drop any snap node indices that somehow went out of range (should not).
        snap_nodes.erase(std::remove_if(snap_nodes.begin(), snap_nodes.end(),
                                        [&](std::uint32_t i) {
                                            return i >= out.mesh.nodes.size();
                                        }),
                         snap_nodes.end());
        std::vector<std::size_t> skin_tets;
        skin_tets.reserve(snap_nodes.size() * 4);
        {
            std::unordered_set<std::uint32_t> bset(snap_nodes.begin(), snap_nodes.end());
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                const auto& n = out.mesh.tets[ti];
                if (bset.count(n[0]) || bset.count(n[1]) || bset.count(n[2]) ||
                    bset.count(n[3])) {
                    skin_tets.push_back(ti);
                }
            }
        }
        const double vol_eps = 1e-14 * hf * hf * hf;
        snap_boundary_nodes(
            surface, out.mesh.nodes, snap_nodes, hf,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto ti : skin_tets) {
                    const auto& n = out.mesh.tets[ti];
                    const double v =
                        tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                          out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                    if (v > vol_eps) {
                        continue;
                    }
                    offenders.insert(n.begin(), n.end());
                }
            },
            /*max_move_frac=*/0.85, /*passes=*/3);
        for (auto& n : out.mesh.tets) {
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
        // Restore simple boundary quads from pre-LEB lattice nodes for region map.
        if (out.mesh.boundary_quads.empty() && snap_nodes.size() >= 3) {
            // Pipeline replaces these via extract_boundary_faces; leave empty OK.
        }
    }

    check_tet_fill_geometry(out.mesh);
    return out;
}

} // namespace polymesh::mesh
