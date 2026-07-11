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
#include <unordered_map>
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

// Exterior triangular faces of a tet mesh (appear once). Returns node ids.
std::vector<std::uint32_t>
tet_boundary_nodes(const std::vector<std::array<std::uint32_t, 4>>& tets) {
    struct FaceKey {
        std::uint32_t a, b, c;
        bool operator==(const FaceKey& o) const { return a == o.a && b == o.b && c == o.c; }
    };
    struct FaceHash {
        std::size_t operator()(const FaceKey& f) const noexcept {
            std::size_t h = f.a;
            h ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };
    auto make_key = [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        std::array<std::uint32_t, 3> v{{i, j, k}};
        std::sort(v.begin(), v.end());
        return FaceKey{v[0], v[1], v[2]};
    };
    std::unordered_map<FaceKey, int, FaceHash> count;
    count.reserve(tets.size() * 2);
    static constexpr int kFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    for (const auto& t : tets) {
        for (const auto& f : kFaces) {
            ++count[make_key(t[static_cast<std::size_t>(f[0])],
                             t[static_cast<std::size_t>(f[1])],
                             t[static_cast<std::size_t>(f[2])])];
        }
    }
    std::unordered_set<std::uint32_t> nodes;
    nodes.reserve(count.size());
    for (const auto& [key, c] : count) {
        if (c == 1) {
            nodes.insert(key.a);
            nodes.insert(key.b);
            nodes.insert(key.c);
        }
    }
    return {nodes.begin(), nodes.end()};
}

} // namespace

GradedTetFillOutput
graded_tet_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                        const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                        std::span<const geom::SharpEdge> features, double feature_band,
                        std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                        double curvature_turn_deg) {
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
    if (!(curvature_turn_deg > 0.0)) {
        curvature_turn_deg = 0.0;
    }
    const Eigen::Vector3d extent = bbox_max - bbox_min;
    if (extent.minCoeff() <= 0.0) {
        throw ValidityError("graded_tet_fill_surface: empty bbox");
    }

    // Coarse-primary lattice at target h. Multi-level LEB (ADR-0018):
    //   L0 bulk ~ h, L1 feature/skin ~ h/2, L2 high-κ seeds ~ h/4.
    constexpr int subdiv = 2; // max LEB depth (L2)
    constexpr std::size_t kGradedMaxCells = 48 * 1024;
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kGradedMaxCells, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    const CartesianGrid grid = make_bbox_grid(bbox_min, bbox_max, h_use);
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double hc = grid.max_edge();
    [[maybe_unused]] const double hf = 0.5 * hc;
    const auto inside = classify_cells_inside(surface, grid);
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    const auto inb = [&](int i, int j, int k) {
        return i >= 0 && i < nx && j >= 0 && j < ny && k >= 0 && k < nz &&
               inside[idx(i, j, k)];
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

    // Skin depth: never eat more than half the interior.
    // When feature/seed grading is on, skip free-surface hop flood — otherwise
    // the whole exterior becomes L1 and the adaptive size field is invisible
    // (everything looks the same size in the free-surface wireframe). Plain
    // graded (no geo drivers) still skins so unit boxes get an L1 shell.
    const int skin_cap = std::max(1, (max_dist + 1) / 2);
    const bool have_geo_drivers =
        (feature_band > 0.0) || (seed_band > 0.0) || (curvature_turn_deg > 0.0);
    const int skin_thresh = have_geo_drivers ? 0 : std::min(skin_layers, skin_cap);

    // refine_level: 0=bulk, 1=L1 (~h/2), 2=L2 (~h/4 near high-κ seeds)
    std::vector<std::uint8_t> refine_level(inside.size(), 0);
    std::vector<char> is_feature(inside.size(), 0);
    std::vector<char> is_seed(inside.size(), 0);
    std::vector<char> is_l1(inside.size(), 0);
    std::vector<char> is_l2(inside.size(), 0);

    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (skin_thresh > 0 && dist[c] >= 0 && dist[c] < skin_thresh) {
            is_l1[c] = 1;
            refine_level[c] = 1;
        }
    }
    // Features → L1 band (hole rims, creases).
    stamp_feature_cells(is_l1, &is_feature, nx, ny, nz, grid, surface, features, feature_band);
    // Seeds (a-posteriori adapt) → L2 superfine.
    stamp_seed_cells(is_l2, &is_seed, nx, ny, nz, grid, refine_seeds, seed_band);
    // Per-cell turning-angle criterion: h·κ > θ → L1, > 2θ → L2 (angle-adaptive,
    // contiguous along curved walls, inert on flats).
    if (curvature_turn_deg > 0.0) {
        stamp_curvature_cells(is_l1, &is_l2, nullptr, nx, ny, nz, grid, surface,
                              curvature_turn_deg * 3.14159265358979323846 / 180.0);
    }
    // Feature core → L2 so hole rims get two LEB levels (~h/4).
    if (feature_band > 0.0 && !features.empty()) {
        std::vector<char> feature_core(inside.size(), 0);
        stamp_feature_cells(feature_core, nullptr, nx, ny, nz, grid, surface, features,
                            0.75 * feature_band);
        for (std::size_t c = 0; c < inside.size(); ++c) {
            if (feature_core[c]) {
                is_l2[c] = 1;
            }
        }
    }

    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            refine_level[c] = 0;
            is_feature[c] = 0;
            is_seed[c] = 0;
            is_l1[c] = 0;
            is_l2[c] = 0;
            continue;
        }
        if (is_l1[c] || is_feature[c]) {
            refine_level[c] = std::max<std::uint8_t>(refine_level[c], 1);
        }
        if (is_l2[c] || is_seed[c]) {
            refine_level[c] = 2;
        }
    }

    bool any_l2 = false;
    for (auto lv : refine_level) {
        if (lv >= 2) {
            any_l2 = true;
            break;
        }
    }

    GradedTetFillOutput out;
    out.h_coarse = hc;
    // L1 → ~h/2, L2 → ~h/4 (report deepest active level).
    out.h_fine = any_l2 ? 0.25 * hc : 0.5 * hc;
    out.skin_layers = skin_layers;
    out.subdivision = subdiv;
    out.mesh.h = out.h_fine;

    // Uniform coarse Kuhn lattice, then multi-level LEB (ADR-0018).
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

    auto emit_face_quad = [&](int i, int j, int k, int face) {
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
        for (int qn = 0; qn < 4; ++qn) {
            const auto& c = corners[static_cast<std::size_t>(qn)];
            quad[static_cast<std::size_t>(qn)] = node_at(i + c[0], j + c[1], k + c[2]);
        }
        out.mesh.boundary_quads.push_back(quad);
    };

    std::size_t n_l2_cells = 0;
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[idx(i, j, k)]) {
                    continue;
                }
                const auto id = idx(i, j, k);
                emit_cube_tets(i, j, k);
                if (refine_level[id] > 0) {
                    ++out.n_fine_cells;
                    if (is_feature[id]) {
                        ++out.n_feature_cells;
                    }
                    if (is_seed[id] || refine_level[id] >= 2) {
                        ++out.n_seed_cells;
                    }
                    if (refine_level[id] >= 2) {
                        ++n_l2_cells;
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
    (void)n_l2_cells;

    if (out.mesh.tets.empty()) {
        throw ValidityError("graded_tet_fill_surface: no interior cells");
    }

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

    // Multi-level LEB: pass 1 marks level≥1, pass 2 marks level≥2.
    constexpr std::size_t kLebTetBudget = 200'000;
    auto run_leb_for_min_level = [&](std::uint8_t min_level) {
        if (out.mesh.tets.size() > kLebTetBudget) {
            return;
        }
        std::vector<std::size_t> marked;
        marked.reserve(out.mesh.tets.size() / 4 + 8);
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            const auto& n = out.mesh.tets[ti];
            const Eigen::Vector3d c = 0.25 * (out.mesh.nodes[n[0]] + out.mesh.nodes[n[1]] +
                                              out.mesh.nodes[n[2]] + out.mesh.nodes[n[3]]);
            const int cid = cell_of_point(c);
            if (cid >= 0 && static_cast<std::size_t>(cid) < refine_level.size() &&
                refine_level[static_cast<std::size_t>(cid)] >= min_level) {
                marked.push_back(ti);
            }
        }
        if (marked.empty()) {
            return;
        }
        LocalRefineStats st;
        // S1: project free-surface LEB mids onto STL (avoid hole-void chords).
        auto refined = local_refine_tets(std::move(out.mesh.nodes), std::move(out.mesh.tets),
                                         marked, &st, &surface);
        out.mesh.nodes = std::move(refined.nodes);
        out.mesh.tets = std::move(refined.tets);
    };

    // Pre-LEB snap of lattice corners so LEB mid-edges start closer to the CAD
    // (cleaner hole rims; midpoints of two on-surface nodes ≈ on surface).
    {
        std::vector<std::uint32_t> pre_snap = tet_boundary_nodes(out.mesh.tets);
        if (!pre_snap.empty()) {
            std::unordered_set<std::uint32_t> bset(pre_snap.begin(), pre_snap.end());
            std::vector<std::size_t> skin_tets;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                const auto& n = out.mesh.tets[ti];
                if (bset.count(n[0]) || bset.count(n[1]) || bset.count(n[2]) ||
                    bset.count(n[3])) {
                    skin_tets.push_back(ti);
                }
            }
            const double vol_eps = 1e-14 * hc * hc * hc;
            snap_boundary_nodes(
                surface, out.mesh.nodes, pre_snap, hc,
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
                /*max_move_frac=*/1.05, /*passes=*/5, features);
            for (auto& n : out.mesh.tets) {
                const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                                   out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                if (v < 0.0) {
                    std::swap(n[1], n[2]);
                }
            }
        }
    }

    if (out.n_fine_cells > 0) {
        run_leb_for_min_level(1);
        if (any_l2) {
            run_leb_for_min_level(2);
        }
    }

    // CRITICAL: recollect free-surface nodes *after* LEB so mid-edge nodes on
    // the hole rim actually get snapped. Only unpaired-face nodes (S3) — do not
    // merge stale pre-LEB lattice quads (those can include interior/non-skin
    // corners after refine). Run as a reusable round: the S5 void carve below
    // exposes fresh (never-snapped) lattice faces that need a second round.
    const auto snap_round = [&]() {
        std::vector<std::uint32_t> snap_nodes = tet_boundary_nodes(out.mesh.tets);
        if (snap_nodes.empty()) {
            return;
        }
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
        const double vol_eps = 1e-14 * hc * hc * hc;
        auto collect_invert = [&](std::set<std::uint32_t>& offenders) {
            for (const auto ti : skin_tets) {
                const auto& n = out.mesh.tets[ti];
                const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                                   out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                if (v > vol_eps) {
                    continue;
                }
                offenders.insert(n.begin(), n.end());
            }
        };
        // Strong budget so LEB mid-edges on holes leave the Cartesian stair.
        // Use max(hc, ~cell diagonal) scale via frac>1; soft-unsnap keeps quality.
        snap_boundary_nodes(surface, out.mesh.nodes, snap_nodes, hc, collect_invert,
                            /*max_move_frac=*/1.15, /*passes=*/7, features);
        // Per-node accept/reject re-project for residual outliers (hole kinks).
        // Full projection; keep only if no skin tet inverts.
        {
            const double thr = 0.08 * hc;
            for (auto ni : snap_nodes) {
                if (ni >= out.mesh.nodes.size()) {
                    continue;
                }
                const auto cp = closest_on_surface(surface, out.mesh.nodes[ni]);
                if (!(cp.distance > thr) || cp.distance > 2.5 * hc) {
                    continue;
                }
                const Eigen::Vector3d saved = out.mesh.nodes[ni];
                // Full projection first, then partial fractions — a partial move
                // still shrinks the residual when the full one inverts a tet.
                static constexpr double kFracs[] = {1.0, 0.6, 0.35};
                for (const double frac : kFracs) {
                    out.mesh.nodes[ni] = saved + frac * (cp.point - saved);
                    bool ok = true;
                    for (const auto ti : skin_tets) {
                        const auto& n = out.mesh.tets[ti];
                        if (n[0] != ni && n[1] != ni && n[2] != ni && n[3] != ni) {
                            continue;
                        }
                        const double v =
                            tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                              out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                        if (v <= vol_eps) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        break;
                    }
                    out.mesh.nodes[ni] = saved;
                }
            }
        }
        for (auto& n : out.mesh.tets) {
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
    };
    snap_round();

    // S4 sliver-cap collapse: snapping all four corners of a skin tet onto a
    // curved surface leaves a near-flat cap that unsnap cannot cure without
    // reopening the residual. Collapse the cap's shortest edge (conforming;
    // the dead node merges into the survivor) when every incident tet stays
    // valid and no new cap appears.
    const auto repair_round = [&]() {
        constexpr double kCapAspect = 0.05;  // caps live far below Kuhn ~0.27
        constexpr double kKeepAspect = 0.04; // incident tets must stay above
        constexpr int kCollapsePasses = 5;
        const double vol_eps = 1e-14 * hc * hc * hc;
        const auto aspect_of = [&](const std::array<std::uint32_t, 4>& n) {
            const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
            const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
            const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
            const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
            const double v = std::abs(tet_signed_volume(a, b, c, d));
            const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                          (b - c).norm(), (b - d).norm(), (c - d).norm()});
            if (emax <= 0.0) {
                return 0.0;
            }
            return std::min(1.0, 6.0 * 1.4142135623730951 * v / (emax * emax * emax));
        };
        std::vector<std::uint32_t> node_remap(out.mesh.nodes.size());
        for (std::uint32_t ni = 0; ni < node_remap.size(); ++ni) {
            node_remap[ni] = ni;
        }
        bool collapsed_any = false;
        for (int pass = 0; pass < kCollapsePasses; ++pass) {
            const auto bvec = tet_boundary_nodes(out.mesh.tets);
            const std::unordered_set<std::uint32_t> bset(bvec.begin(), bvec.end());
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> incident;
            incident.reserve(out.mesh.nodes.size());
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                for (const auto ni : out.mesh.tets[ti]) {
                    incident[ni].push_back(ti);
                }
            }
            std::vector<char> removed(out.mesh.tets.size(), 0);
            bool any = false;
            const auto try_collapse = [&](std::uint32_t dead, std::uint32_t surv) {
                // Never pull the surface inward: a boundary node may only merge
                // into another boundary node.
                if (bset.count(dead) && !bset.count(surv)) {
                    return false;
                }
                const auto it = incident.find(dead);
                if (it == incident.end()) {
                    return false;
                }
                for (const auto tj : it->second) {
                    if (removed[tj]) {
                        continue;
                    }
                    auto probe = out.mesh.tets[tj];
                    const double aspect_before = aspect_of(probe);
                    bool has_surv = false;
                    for (auto& nn : probe) {
                        if (nn == surv) {
                            has_surv = true;
                        }
                        if (nn == dead) {
                            nn = surv;
                        }
                    }
                    if (has_surv) {
                        continue; // degenerates away with the collapse
                    }
                    const double v =
                        tet_signed_volume(out.mesh.nodes[probe[0]], out.mesh.nodes[probe[1]],
                                          out.mesh.nodes[probe[2]], out.mesh.nodes[probe[3]]);
                    // Reject inversions outright; otherwise accept when the
                    // neighbor stays healthy — or at least does not get worse
                    // (sliver clusters heal stepwise).
                    const double aspect_after = aspect_of(probe);
                    const bool was_healthy = aspect_before >= kKeepAspect;
                    if (v <= 0.0 || (was_healthy && v <= vol_eps) ||
                        (aspect_after < kKeepAspect && aspect_after < aspect_before)) {
                        return false;
                    }
                }
                for (const auto tj : it->second) {
                    if (removed[tj]) {
                        continue;
                    }
                    auto& t = out.mesh.tets[tj];
                    bool has_surv = false;
                    for (const auto nn : t) {
                        if (nn == surv) {
                            has_surv = true;
                            break;
                        }
                    }
                    if (has_surv) {
                        removed[tj] = 1;
                        continue;
                    }
                    for (auto& nn : t) {
                        if (nn == dead) {
                            nn = surv;
                        }
                    }
                    incident[surv].push_back(tj);
                }
                node_remap[dead] = surv;
                any = true;
                collapsed_any = true;
                return true;
            };

            // Phase A — void juts: boundary nodes whose projection the snap had
            // to reject (hole-rim stair chords poking into the void) merge into
            // an adjacent on-surface boundary node instead of leaving a spike.
            for (const auto ni : bvec) {
                if (node_remap[ni] != ni) {
                    continue;
                }
                const double resid = closest_on_surface(surface, out.mesh.nodes[ni]).distance;
                if (resid <= 0.15 * hc) {
                    continue;
                }
                const auto it = incident.find(ni);
                if (it == incident.end()) {
                    continue;
                }
                std::vector<std::pair<double, std::uint32_t>> cand;
                for (const auto tj : it->second) {
                    if (removed[tj]) {
                        continue;
                    }
                    for (const auto nn : out.mesh.tets[tj]) {
                        if (nn == ni || !bset.count(nn)) {
                            continue;
                        }
                        cand.push_back({(out.mesh.nodes[nn] - out.mesh.nodes[ni]).norm(), nn});
                    }
                }
                std::sort(cand.begin(), cand.end());
                cand.erase(std::unique(cand.begin(), cand.end(),
                                       [](const auto& x, const auto& y) {
                                           return x.second == y.second;
                                       }),
                           cand.end());
                for (const auto& [len, surv] : cand) {
                    if (closest_on_surface(surface, out.mesh.nodes[surv]).distance >
                        0.05 * hc) {
                        continue;
                    }
                    if (try_collapse(ni, surv)) {
                        break;
                    }
                }
            }

            // Phase B — sliver caps: collapse the shortest viable edge.
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                if (removed[ti] || aspect_of(out.mesh.tets[ti]) >= kCapAspect) {
                    continue;
                }
                const auto tet = out.mesh.tets[ti];
                // Candidate edges by ascending length.
                std::array<std::pair<double, std::array<std::uint32_t, 2>>, 6> edges_len{};
                int ne = 0;
                for (int p = 0; p < 4; ++p) {
                    for (int q2 = p + 1; q2 < 4; ++q2) {
                        const auto a = tet[static_cast<std::size_t>(p)];
                        const auto b = tet[static_cast<std::size_t>(q2)];
                        edges_len[static_cast<std::size_t>(ne++)] = {
                            (out.mesh.nodes[a] - out.mesh.nodes[b]).norm(), {a, b}};
                    }
                }
                std::sort(edges_len.begin(), edges_len.end(),
                          [](const auto& x, const auto& y) { return x.first < y.first; });
                bool done = false;
                for (int e = 0; e < ne && !done; ++e) {
                    for (int dir = 0; dir < 2 && !done; ++dir) {
                        const std::uint32_t dead = edges_len[static_cast<std::size_t>(e)]
                                                       .second[static_cast<std::size_t>(dir)];
                        const std::uint32_t surv =
                            edges_len[static_cast<std::size_t>(e)]
                                .second[static_cast<std::size_t>(1 - dir)];
                        if (node_remap[dead] != dead || node_remap[surv] != surv) {
                            continue;
                        }
                        done = try_collapse(dead, surv);
                    }
                }
            }
            if (any) {
                std::size_t w = 0;
                for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                    if (!removed[ti]) {
                        out.mesh.tets[w++] = out.mesh.tets[ti];
                    }
                }
                out.mesh.tets.resize(w);
            } else {
                break;
            }
        }
        if (collapsed_any) {
            const auto resolve = [&](std::uint32_t ni) {
                while (node_remap[ni] != ni) {
                    ni = node_remap[ni];
                }
                return ni;
            };
            for (auto& q : out.mesh.boundary_quads) {
                for (auto& ni : q) {
                    ni = resolve(ni);
                }
            }
        }

        // S5 void carve: nodes the snap could not place on the surface (their
        // projection inverts skin tets — hole-void stair chords) poke into CAD
        // holes. Peel the tets of those *pre-identified* jut nodes as they gain
        // free faces, until the juts drop out of the mesh. Newly exposed nodes
        // are not juts, so the peel cannot run away into the bulk.
        // Also peels *flat caps the collapse could not cure* (aspect below the
        // scorecard floor): a cap that survives S4 is wedged between healthy
        // tets; with a free face it is a zero-thickness skin flake — removing
        // it exposes those healthy faces with negligible volume change.
        {
            constexpr int kCarvePasses = 4;
            constexpr double kPeelAspect = 0.03; // < kKeepAspect: only true flakes
            const double node_thr = 0.15 * hc;
            const auto aspect_peel = [&](const std::array<std::uint32_t, 4>& n) {
                const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
                const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
                const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
                const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
                const double v = std::abs(tet_signed_volume(a, b, c, d));
                const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                              (b - c).norm(), (b - d).norm(), (c - d).norm()});
                if (emax <= 0.0) {
                    return true;
                }
                return 6.0 * 1.4142135623730951 * v / (emax * emax * emax) < kPeelAspect;
            };
            std::unordered_set<std::uint32_t> jut;
            for (const auto ni : tet_boundary_nodes(out.mesh.tets)) {
                if (closest_on_surface(surface, out.mesh.nodes[ni]).distance > node_thr) {
                    jut.insert(ni);
                }
            }
            for (int pass = 0; pass < kCarvePasses; ++pass) {
                // Free faces per tet (faces appearing once across the mesh).
                struct FKey {
                    std::uint32_t a, b, c;
                    bool operator==(const FKey& o) const {
                        return a == o.a && b == o.b && c == o.c;
                    }
                };
                struct FHash {
                    std::size_t operator()(const FKey& f) const noexcept {
                        std::size_t h2 = f.a;
                        h2 ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL +
                              (h2 << 6) + (h2 >> 2);
                        h2 ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL +
                              (h2 << 6) + (h2 >> 2);
                        return h2;
                    }
                };
                static constexpr int kTFaces[4][3] = {
                    {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
                std::unordered_map<FKey, int, FHash> fcount;
                fcount.reserve(out.mesh.tets.size() * 2);
                const auto fkey = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    std::array<std::uint32_t, 3> v{{x, y, z}};
                    std::sort(v.begin(), v.end());
                    return FKey{v[0], v[1], v[2]};
                };
                for (const auto& t : out.mesh.tets) {
                    for (const auto& f : kTFaces) {
                        ++fcount[fkey(t[static_cast<std::size_t>(f[0])],
                                      t[static_cast<std::size_t>(f[1])],
                                      t[static_cast<std::size_t>(f[2])])];
                    }
                }
                std::vector<char> kill(out.mesh.tets.size(), 0);
                std::size_t n_kill = 0;
                for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                    const auto& t = out.mesh.tets[ti];
                    bool has_jut = false;
                    for (const auto ni : t) {
                        if (jut.count(ni)) {
                            has_jut = true;
                            break;
                        }
                    }
                    if (!has_jut && !aspect_peel(t)) {
                        continue;
                    }
                    bool has_free_face = false;
                    for (const auto& f : kTFaces) {
                        if (fcount[fkey(t[static_cast<std::size_t>(f[0])],
                                        t[static_cast<std::size_t>(f[1])],
                                        t[static_cast<std::size_t>(f[2])])] == 1) {
                            has_free_face = true;
                            break;
                        }
                    }
                    if (has_free_face) {
                        kill[ti] = 1;
                        ++n_kill;
                    }
                }
                if (n_kill == 0 || n_kill >= out.mesh.tets.size()) {
                    break;
                }
                std::size_t w = 0;
                for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                    if (!kill[ti]) {
                        out.mesh.tets[w++] = out.mesh.tets[ti];
                    }
                }
                out.mesh.tets.resize(w);
            }
        }
    };
    repair_round();
    // The carve exposes fresh lattice faces that were never snapped — run a
    // second snap + repair round so the new boundary reaches the surface too.
    snap_round();
    repair_round();

    // S6 tangential smoothing: snap places nodes *on* the surface but keeps
    // their lattice-stair spacing, which reads as sawtooth on curved walls and
    // hole rims. Relax boundary nodes toward their boundary-neighbor centroid
    // and re-project (crease nodes relax along the crease), reverting any move
    // that inverts a tet.
    {
        struct FaceKey {
            std::uint32_t a, b, c;
            bool operator==(const FaceKey& o) const {
                return a == o.a && b == o.b && c == o.c;
            }
        };
        struct FaceHash {
            std::size_t operator()(const FaceKey& f) const noexcept {
                std::size_t h2 = f.a;
                h2 ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL + (h2 << 6) +
                      (h2 >> 2);
                h2 ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL + (h2 << 6) +
                      (h2 >> 2);
                return h2;
            }
        };
        static constexpr int kTFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
        std::unordered_map<FaceKey, std::array<std::uint32_t, 3>, FaceHash> once;
        std::unordered_map<FaceKey, int, FaceHash> fcount;
        fcount.reserve(out.mesh.tets.size() * 2);
        const auto fkey = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            std::array<std::uint32_t, 3> v{{x, y, z}};
            std::sort(v.begin(), v.end());
            return FaceKey{v[0], v[1], v[2]};
        };
        for (const auto& t : out.mesh.tets) {
            for (const auto& f : kTFaces) {
                const auto k0 = t[static_cast<std::size_t>(f[0])];
                const auto k1 = t[static_cast<std::size_t>(f[1])];
                const auto k2 = t[static_cast<std::size_t>(f[2])];
                const auto key = fkey(k0, k1, k2);
                if (++fcount[key] == 1) {
                    once[key] = {k0, k1, k2};
                }
            }
        }
        std::vector<std::array<std::uint32_t, 4>> free_faces;
        free_faces.reserve(once.size() / 2);
        for (const auto& [key, tri] : once) {
            if (fcount[key] == 1) {
                free_faces.push_back({tri[0], tri[1], tri[2], tri[2]});
            }
        }
        const double vol_eps = 1e-14 * hc * hc * hc;
        smooth_boundary_nodes(
            surface, out.mesh.nodes, free_faces, hc,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto& n : out.mesh.tets) {
                    const double v = tet_signed_volume(out.mesh.nodes[n[0]],
                                                       out.mesh.nodes[n[1]],
                                                       out.mesh.nodes[n[2]],
                                                       out.mesh.nodes[n[3]]);
                    if (v <= vol_eps) {
                        offenders.insert(n.begin(), n.end());
                    }
                }
            },
            /*passes=*/3, /*relax=*/0.5, features);
        for (auto& n : out.mesh.tets) {
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
    }
    // Smoothing can thin an already-marginal cap — one more collapse round.
    repair_round();

    // Rebuild boundary quads as exterior tris padded for pipeline display
    // (quad[3]=quad[2] for pure tris is OK — pipeline may re-extract).
    // Keep original lattice quads when present; append nothing if already set.
    check_tet_fill_geometry(out.mesh);
    return out;
}

} // namespace polymesh::mesh
