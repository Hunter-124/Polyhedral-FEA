// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/lattice_split.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace polymesh::mesh {
namespace {

// Lattice cell → tets: mirror-symmetric alternating 5-tet split
// (mesh/lattice_split.hpp). Replaced the Kuhn 6-tet table, which leaned every
// cell the same way and left the tiling with no mirror symmetry at all.

constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

/// Ordering key for a geometric scalar that must treat a mirrored pair's values
/// as EQUAL, so the tie falls through to the mirror key rather than being
/// decided by rounding noise.
///
/// Measured need: once every stage's decisions are folded (mesh/mirror.hpp), the
/// cylinder lattice reaches the sliver-collapse round at exactly 100/100/100%
/// mirrored tets and leaves it at 99.80/99.21/98.93%, which the tangential
/// smoothing then amplifies to 99.1/92.9/91.2%. The round's inputs tie in exact
/// arithmetic — a cap and its mirror image have the same aspect, the same six
/// edge lengths, the same collapse scores — but their coordinates are reflections
/// computed in floating point, so each quantity differs in the last ulp or two.
/// Comparing those raw values ordered mirrored caps by that noise and collapsed
/// them in unmirrored directions.
///
/// Quantising at 1e-9 of the quantity's own scale is nine orders above the noise
/// and nine below any difference this mesher acts on. It is a quantisation and
/// not an epsilon comparison on purpose: an epsilon comparison is not transitive,
/// and `std::sort` requires a strict weak ordering.
[[nodiscard]] long long tie_key(double value, double scale) {
    if (!std::isfinite(value)) {
        return value > 0.0 ? std::numeric_limits<long long>::max()
                           : std::numeric_limits<long long>::min();
    }
    const double quantum = 1e-9 * (scale > 0.0 ? scale : 1.0);
    return static_cast<long long>(std::llround(value / quantum));
}

// Exterior triangular faces of a tet mesh (appear once). Returns node ids in the
// order the snap/smooth rounds must visit them (see the sort below).
std::vector<std::uint32_t>
tet_boundary_nodes(const std::vector<std::array<std::uint32_t, 4>>& tets,
                   const std::vector<Eigen::Vector3d>& nodes) {
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
    std::unordered_set<std::uint32_t> nodes_set;
    nodes_set.reserve(count.size());
    for (const auto& [key, c] : count) {
        if (c == 1) {
            nodes_set.insert(key.a);
            nodes_set.insert(key.b);
            nodes_set.insert(key.c);
        }
    }
    std::vector<std::uint32_t> out(nodes_set.begin(), nodes_set.end());
    // Mirror-canonical order, not bucket order and not ascending node id: this
    // list is the iteration order of snap_round's boundary snap and per-node
    // re-project, where each node moves, tests its skin tets, and reverts on
    // inversion — so an earlier node's accepted move changes whether a later one
    // inverts. Bucket order alone disagreed across standard libraries (ADR-0032:
    // the 3080 Ti corpus split from gcc on 5 of 24 pairs, stepped_shaft_s2_c0
    // hybrid_zoo 264 vs 200 elements), which ascending node id fixed. Node ids do
    // not mirror, though, so id order gave a node and its mirror image different
    // predecessor sets and the accept/reject outcomes diverged.
    //
    // Sorting on distance-from-centre per axis, quantised so a mirror pair keys
    // bit-identically, makes a node and its mirror image adjacent in the order
    // with the same predecessor set up to reflection. Ties fall back to node id so
    // the order stays total and platform-independent.
    if (out.size() > 1) {
        Eigen::Vector3d lo = nodes[out.front()];
        Eigen::Vector3d hi = lo;
        for (const auto ni : out) {
            lo = lo.cwiseMin(nodes[ni]);
            hi = hi.cwiseMax(nodes[ni]);
        }
        const Eigen::Vector3d center = 0.5 * (lo + hi);
        const double quantum = 1e-9 * (hi - lo).norm();
        const double inv_q = quantum > 0.0 ? 1.0 / quantum : 0.0;
        std::vector<std::array<long long, 3>> key(nodes.size());
        for (const auto ni : out) {
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

/// Edges of the free-face shell that are used a number of times other than two.
///
/// A conforming tet complex has none. Two later steps can create them without
/// changing the mesh's volume or invalidating a single element, which is why
/// nothing caught them for so long: an edge collapse that violates the link
/// condition, and a carve that deletes a tet whose neighbour is then left with
/// two exposed faces meeting at their shared edge. Either way the skin is slit
/// along a line, the two torn patches coincide, and the divergence volume
/// cancels out to the right answer.
struct TetShellTopology {
    std::size_t n_torn_edges = 0;
    /// Packed (min << 32) | max, for the torn edges only.
    std::unordered_set<std::uint64_t> torn;
};

TetShellTopology tet_shell_topology(const std::vector<std::array<std::uint32_t, 4>>& tets) {
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
    const auto make_key = [](std::uint32_t i, std::uint32_t j, std::uint32_t k) {
        std::array<std::uint32_t, 3> v{{i, j, k}};
        std::sort(v.begin(), v.end());
        return FaceKey{v[0], v[1], v[2]};
    };
    static constexpr int kFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
    std::unordered_map<FaceKey, int, FaceHash> face_use;
    face_use.reserve(tets.size() * 2);
    for (const auto& t : tets) {
        for (const auto& f : kFaces) {
            ++face_use[make_key(t[static_cast<std::size_t>(f[0])],
                                t[static_cast<std::size_t>(f[1])],
                                t[static_cast<std::size_t>(f[2])])];
        }
    }
    const auto pack = [](std::uint32_t x, std::uint32_t y) {
        return x < y ? (static_cast<std::uint64_t>(x) << 32) | y
                     : (static_cast<std::uint64_t>(y) << 32) | x;
    };
    std::unordered_map<std::uint64_t, int> edge_use;
    edge_use.reserve(face_use.size());
    for (const auto& [face, count] : face_use) {
        if (count != 1) {
            continue;
        }
        ++edge_use[pack(face.a, face.b)];
        ++edge_use[pack(face.a, face.c)];
        ++edge_use[pack(face.b, face.c)];
    }
    TetShellTopology out;
    for (const auto& [edge, count] : edge_use) {
        if (count != 2) {
            out.torn.insert(edge);
        }
    }
    out.n_torn_edges = out.torn.size();
    return out;
}

/// Restrict a proposed set of tet deletions to those that keep the boundary as
/// intact as it already is.
///
/// Every deletion site in this mesher decides tet-by-tet — centroid in a void
/// child, node in the jut set, aspect below a flake threshold — and none of
/// them could see that removing one tet may leave its neighbour with two
/// exposed faces meeting at their shared edge. Two such neighbours slit the
/// skin along that edge while the volume stays right, because the two torn
/// patches coincide and cancel, so no volume or validity check can find it.
///
/// Each site proposes; this disposes. There are two ways to close a slit and
/// they are not equally good:
///
///  - EXTEND: delete the stranded neighbour too. At a void carve that neighbour
///    is a one-cell spike poking into the hole, so removing it moves the
///    boundary toward the true surface.
///  - REVIVE: put a deleted tet back. That reconnects the survivors through
///    solid, but at a void carve it backfills material INTO the hole.
///
/// Extending is tried first and revival is the fallback, because backfilling
/// measurably degrades the surface: reviving alone left the box_hole bore wall
/// 12% larger and doubled cylinder_prism's graded surface residual (M1max
/// 0.0080 -> 0.0194 h), while extending keeps both at their pre-fix values.
/// Extension is capped so a runaway cannot eat the solid, and if neither
/// converges the whole proposal is dropped — a surviving flake is a quality
/// problem, a torn skin is a correctness one, and the two are not tradeable.
bool restrict_kill_to_shell(const std::vector<std::array<std::uint32_t, 4>>& tets,
                            std::vector<char>& kill, int max_rounds = 8) {
    const auto pack = [](std::uint32_t x, std::uint32_t y) {
        return x < y ? (static_cast<std::uint64_t>(x) << 32) | y
                     : (static_cast<std::uint64_t>(y) << 32) | x;
    };
    static constexpr int kFaces[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
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
            if (!kill[ti]) {
                survivors.push_back(tets[ti]);
                survivor_index.push_back(ti);
            }
        }
        std::unordered_set<std::uint64_t> fresh;
        for (const auto e : tet_shell_topology(survivors).torn) {
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
                if (edges.count(pack(t[i], t[j])) != 0) {
                    return true;
                }
            }
        }
        return false;
    };

    std::size_t killed = n_proposed;
    for (int round = 0; round < max_rounds; ++round) {
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
        for (const auto& t : survivors) {
            for (const auto& f : kFaces) {
                ++face_use[face_hash(t[static_cast<std::size_t>(f[0])],
                                     t[static_cast<std::size_t>(f[1])],
                                     t[static_cast<std::size_t>(f[2])])];
            }
        }
        std::size_t extended = 0;
        for (std::size_t si = 0; si < survivors.size(); ++si) {
            const auto& t = survivors[si];
            if (!touches_any(t, fresh)) {
                continue;
            }
            int free_faces = 0;
            for (const auto& f : kFaces) {
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
        const auto fresh = fresh_tears();
        if (fresh.empty()) {
            return true;
        }
        std::size_t revived = 0;
        for (std::size_t ti = 0; ti < tets.size(); ++ti) {
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

} // namespace

GradedTetFillOutput
graded_tet_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                        const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                        std::span<const geom::SharpEdge> features, double feature_band,
                        std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                        double curvature_turn_deg, const BoundaryFit* fit,
                        const SizeFieldFn& size_field, const MirrorFrame* mirror) {
    BoundaryProjectionContext* projection = fit != nullptr ? fit->projection : nullptr;
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
    // The h/2 classifier is sampling-only. The coarse lattice remains at the
    // requested h; mixed parents are marked for local LEB below. Cell counts are
    // even so the alternating 5-tet split mirrors about the bbox mid-planes.
    const int classification_levels = projection != nullptr ? 1 : 0;
    auto classification = classify_cells_feature_aware(
        surface, bbox_min, bbox_max, h_use, static_cast<long>(kGradedMaxCells),
        /*relative_volume_tolerance=*/0.01, classification_levels, size_field,
        /*even_cells=*/true);
    // Every decision below is taken in one octant and mirrored into the others
    // when the geometry is verified mirror-symmetric. The classification is the
    // first and most consequential of them: which cells hold material decides the
    // whole element pattern, and it is read off a tessellation that is measurably
    // not mirror-symmetric (ADR-0036 §7).
    const CanonicalCellMap cell_orbit = mirror != nullptr
                                            ? canonical_cell_map(classification.grid, *mirror)
                                            : CanonicalCellMap{};
    symmetrise_classification(classification, cell_orbit);
    const CartesianGrid& grid = classification.grid;
    const auto& inside = classification.inside;
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double hc = grid.max_edge();
    [[maybe_unused]] const double hf = 0.5 * hc;
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
    const bool have_geo_drivers = (feature_band > 0.0) || (seed_band > 0.0) ||
                                  (curvature_turn_deg > 0.0) || static_cast<bool>(size_field);
    const int skin_thresh = have_geo_drivers ? 0 : std::min(skin_layers, skin_cap);

    // refine_level: 0=bulk, 1=L1, 2=L2, 3=deep protected-feature core.
    // Level 3 adds bisection waves without deepening a-posteriori/BC seed balls.
    std::vector<std::uint8_t> refine_level(inside.size(), 0);
    std::vector<char> is_feature(inside.size(), 0);
    std::vector<char> is_seed(inside.size(), 0);
    std::vector<char> is_l1(inside.size(), 0);
    std::vector<char> is_l2(inside.size(), 0);
    std::vector<char> is_deep_feature(inside.size(), 0);

    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (skin_thresh > 0 && dist[c] >= 0 && dist[c] < skin_thresh) {
            is_l1[c] = 1;
            refine_level[c] = 1;
        }
    }
    double field_h_min = std::numeric_limits<double>::infinity();
    double field_h_max = 0.0;
    std::size_t n_field_budget_clamped = 0;
    if (size_field) {
        // h_floor is the coarse-lattice budget floor. Requests below it cannot
        // create another lattice scale; LEB remains bounded by its own budget.
        const double h_floor = (h_budget > 0.0) ? h_budget : hc;
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const auto id = idx(i, j, k);
                    if (!inside[id]) {
                        continue;
                    }
                    const Eigen::Vector3d centroid =
                        grid.origin +
                        Eigen::Vector3d{(static_cast<double>(i) + 0.5) * grid.cell[0],
                                        (static_cast<double>(j) + 0.5) * grid.cell[1],
                                        (static_cast<double>(k) + 0.5) * grid.cell[2]};
                    double requested = size_field(centroid);
                    if (!(requested > 0.0) || !std::isfinite(requested)) {
                        requested = h_floor;
                        ++n_field_budget_clamped;
                    } else {
                        field_h_min = std::min(field_h_min, requested);
                        field_h_max = std::max(field_h_max, requested);
                        if (requested < h_floor) {
                            ++n_field_budget_clamped;
                        }
                    }
                    const double h_target = std::max(requested, h_floor);
                    const int level = std::clamp(
                        static_cast<int>(std::lround(std::log2(hc / h_target))), 0, subdiv);
                    refine_level[id] =
                        std::max(refine_level[id], static_cast<std::uint8_t>(level));
                }
            }
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
    // Protected feature core gets two extra LEB waves so hole rims are not
    // visibly polygonal. Keep this tag distinct from curvature/adapt seeds:
    // deepening every seed ball inflated routine CAD solves by an order of
    // magnitude without improving sharp-edge fidelity.
    if (feature_band > 0.0 && !features.empty()) {
        stamp_feature_cells(is_deep_feature, nullptr, nx, ny, nz, grid, surface, features,
                            0.75 * feature_band);
    }

    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            refine_level[c] = 0;
            is_feature[c] = 0;
            is_seed[c] = 0;
            is_l1[c] = 0;
            is_l2[c] = 0;
            is_deep_feature[c] = 0;
            continue;
        }
        if (is_l1[c] || is_feature[c]) {
            refine_level[c] = std::max<std::uint8_t>(refine_level[c], 1);
        }
        if (is_l2[c] || is_seed[c]) {
            refine_level[c] = 2;
        }
        if (is_deep_feature[c]) {
            refine_level[c] = 3;
        }
        if (!classification.child_inside_mask.empty()) {
            const auto mask = classification.child_inside_mask[c];
            if (mask != 0 && mask != std::uint8_t{0xff}) {
                refine_level[c] = std::max<std::uint8_t>(refine_level[c], 1);
                is_feature[c] = 1;
            }
        }
    }

    // Every mark above is stamped from the tessellation — feature edges detected
    // on facet normals, per-cell turning angle from facet triangles, a size field
    // sampled at cell centroids — so a cell and its mirror image can disagree
    // about their own refinement level even on a part whose exact geometry is
    // symmetric. Ablating the curvature stamp alone moved plate_hole from
    // 82/83/97% mirrored tets to 94/95/97%, which is the size of the effect.
    // Take the low-side octant's answer for the whole orbit: on a verified
    // symmetry the true answer is symmetric, so the disagreement is aliasing.
    if (cell_orbit.active()) {
        const auto level_before = refine_level;
        const auto feature_before = is_feature;
        const auto seed_before = is_seed;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            const auto source = cell_orbit.canonical[c];
            refine_level[c] = level_before[source];
            is_feature[c] = feature_before[source];
            is_seed[c] = seed_before[source];
        }
    }

    bool any_l1 = false;
    bool any_l2 = false;
    bool any_deep_feature = false;
    for (auto lv : refine_level) {
        any_l1 = any_l1 || lv >= 1;
        any_l2 = any_l2 || lv >= 2;
        any_deep_feature = any_deep_feature || lv >= 3;
    }

    GradedTetFillOutput out;
    out.h_coarse = hc;
    // Report the deepest active level; an all-L0 field remains at h_coarse.
    out.h_fine = any_l2 ? 0.25 * hc : (any_l1 ? 0.5 * hc : hc);
    out.skin_layers = skin_layers;
    out.subdivision = subdiv;
    out.mesh.h = out.h_fine;
    out.classification_refinement_levels = classification.refinement_levels;
    out.classification_volume_error = classification.relative_volume_error;
    out.field_h_min = std::isfinite(field_h_min) ? field_h_min : 0.0;
    out.field_h_max = field_h_max;
    out.n_field_budget_clamped = n_field_budget_clamped;

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
        for (const auto& t : kLatticeTetsKuhn[lattice_cell_variant(i, j, k)]) {
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
                if (refine_level[id] == 0) {
                    ++out.n_level0_cells;
                } else if (refine_level[id] == 1) {
                    ++out.n_level1_cells;
                } else {
                    ++out.n_level2_cells;
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
                                         marked, &st, &surface, mirror);
        out.mesh.nodes = std::move(refined.nodes);
        out.mesh.tets = std::move(refined.tets);
    };

    // Pre-LEB snap of lattice corners so LEB mid-edges start closer to the CAD
    // (cleaner hole rims; midpoints of two on-surface nodes ≈ on surface).
    {
        std::vector<std::uint32_t> pre_snap =
            tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
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
                /*max_move_frac=*/1.05, /*passes=*/5, features, {}, {},
                /*defer_coupled=*/false, projection, {}, mirror);
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
        if (any_deep_feature) {
            // A single longest-edge split halves volume, not edge length.
            // Additional feature-only waves provide enough curve segments for
            // exact BRep rim projection without deepening ordinary seed balls.
            run_leb_for_min_level(3);
            run_leb_for_min_level(3);
        }
    }
    // Is `p` on the void side of the surface? Answered by the outward normal of
    // the nearest triangle, which is the only inside/outside oracle available
    // at a scale finer than the classifier's lattice samples. A point exactly
    // on the surface, and a point whose nearest triangle is missing, both count
    // as inside: every caller here uses this to authorise DELETING material, so
    // the ambiguous answer has to be the one that keeps it.
    //
    // The whole test runs on the folded point: reflection preserves the sign of
    // (p − cp)·n because it reflects both vectors, so the folded answer is the
    // same answer, and a tet and its mirror image are now condemned or spared
    // together. Deciding each in its own octant is how a carve could take a cell
    // on one side of a symmetric part and leave its mirror image standing.
    const auto outside_solid = [&surface, mirror](const Eigen::Vector3d& raw) {
        const Eigen::Vector3d p = mirror_fold(mirror, raw);
        const auto cp = closest_on_surface(surface, p);
        if (cp.triangle >= surface.triangles.size()) {
            return false;
        }
        const auto& tri = surface.triangles[cp.triangle];
        const Eigen::Vector3d n =
            (surface.vertices[tri[1]] - surface.vertices[tri[0]])
                .cross(surface.vertices[tri[2]] - surface.vertices[tri[0]]);
        return (p - cp.point).dot(n) > 0.0;
    };

    // Distance from `p` to the surface, answered in the canonical octant.
    // Reflection is an isometry, so this is the same distance.
    const auto surface_distance = [&surface, mirror](const Eigen::Vector3d& p) {
        return closest_on_surface(surface, mirror_fold(mirror, p)).distance;
    };

    // The feature-aware classifier's h/2 samples are authoritative inside a
    // mixed coarse parent. LEB has already made those parents conforming; now
    // remove refined tets whose centroids fall in a void child. Without this
    // local carve the classifier can see a bore while the emitted tet mesh
    // remains the original solid coarse cube.
    if (!classification.child_inside_mask.empty()) {
        // The child mask is a cell-CENTRE parity sample on the h/2 lattice, but
        // LEB has already refined these tets to h/4 and finer. Condemning an
        // h/4 tet because one h/2 sample point landed in void is the hole
        // aliasing bug one level down: beside a curved wall the centre of a
        // child can sit outside the solid while most of the child is inside it,
        // and the carve then cuts a slot into the material. Measured on the
        // showcase plate_hole at h=5.6 mm feature-graded: 84 of 672 near-bore
        // boundary nodes ended up off every exact CAD surface, the worst 3.75 mm
        // out — 37% of the bore radius — as fissures open to the top face,
        // which is the ragged crown the hole close-up showed. The uniform mesh
        // of the same part, which has no tets finer than the sample, had none.
        //
        // So the mask proposes and the surface confirms: a tet is carved only
        // when its own centroid is outside the solid, judged against the
        // surface at the tet's own scale rather than the sampler's.
        //
        // Requiring instead that the whole tet lie in void children was tried
        // and is WRONG in the other direction — it under-carves, and voids stop
        // opening: channel_s0 at h=0.0075 m went to mesh=4.990e-06 against
        // cad=4.534e-06, rel_err 0.1006 with the material still filling the
        // slot, which is the hole-disappearance failure this project already
        // fixed once at the coarse level.
        std::vector<char> kill(out.mesh.tets.size(), 0);
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            const auto& tet = out.mesh.tets[ti];
            const Eigen::Vector3d centroid =
                0.25 * (out.mesh.nodes[tet[0]] + out.mesh.nodes[tet[1]] +
                        out.mesh.nodes[tet[2]] + out.mesh.nodes[tet[3]]);
            const Eigen::Vector3d lattice = (centroid - grid.origin).cwiseQuotient(grid.cell);
            const int i = std::clamp(static_cast<int>(std::floor(lattice.x())), 0, nx - 1);
            const int j = std::clamp(static_cast<int>(std::floor(lattice.y())), 0, ny - 1);
            const int k = std::clamp(static_cast<int>(std::floor(lattice.z())), 0, nz - 1);
            const std::uint8_t mask = classification.child_inside_mask[idx(i, j, k)];
            if (mask == 0 || mask == std::uint8_t{0xff}) {
                continue;
            }
            const int a = lattice.x() - static_cast<double>(i) >= 0.5 ? 1 : 0;
            const int b = lattice.y() - static_cast<double>(j) >= 0.5 ? 1 : 0;
            const int c = lattice.z() - static_cast<double>(k) >= 0.5 ? 1 : 0;
            if ((mask & static_cast<std::uint8_t>(1U << (a + 2 * b + 4 * c))) != 0) {
                continue;
            }
            if (!outside_solid(centroid)) {
                continue; // centroid is inside the solid: keep the tet
            }
            kill[ti] = 1;
        }
        // A centroid-in-void test decides one tet at a time and cannot see that
        // dropping this one strands its neighbour with two exposed faces. That
        // is where the graded mesher's torn skins came from: measured on
        // sphere_box_s0 at h=0.0072 m, the LEB lattice reaching this point is
        // perfectly conforming (0 torn edges over 26008 tets) and this carve
        // alone introduced 30. The later repair rounds only ever whittled that
        // down (30 -> 17 -> 6); they were never the source.
        restrict_kill_to_shell(out.mesh.tets, kill);
        std::size_t write = 0;
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            if (!kill[ti]) {
                out.mesh.tets[write++] = out.mesh.tets[ti];
            }
        }
        out.mesh.tets.resize(write);
        if (out.mesh.tets.empty()) {
            throw ValidityError("graded_tet_fill_surface: local child carve removed all tets");
        }
    }

    // CRITICAL: recollect free-surface nodes *after* LEB so mid-edge nodes on
    // the hole rim actually get snapped. Only unpaired-face nodes (S3) — do not
    // merge stale pre-LEB lattice quads (those can include interior/non-skin
    // corners after refine). Run as a reusable round: the S5 void carve below
    // exposes fresh (never-snapped) lattice faces that need a second round.
    const auto snap_round = [&]() {
        std::vector<std::uint32_t> snap_nodes =
            tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
        if (snap_nodes.empty()) {
            return;
        }
        const double vol_eps = 1e-14 * hc * hc * hc;
        std::vector<char> on_boundary(out.mesh.nodes.size(), 0);
        for (const auto ni : snap_nodes) {
            on_boundary[ni] = 1;
        }
        // The bad-cell test: inverted is always bad; below the shared shape
        // floor is bad ONLY when every corner is on the boundary. A thin cell
        // with an interior corner is repairable — the downstream collapse and
        // relaxation rounds move the interior corner and lift it (the sphere
        // snap depends on this: its stair cells dip under the floor mid-snap
        // and repair_round restores them; gating the snap on the floor alone
        // pinned 28 boundary nodes 1.45 mm off the sphere, star quality
        // exactly 0.0200). A thin ALL-BOUNDARY cap has no interior corner to
        // give, so no later pass can lift it — it must block the move here
        // (measured on icecream_cone at h = 8 mm: one mid-wall orbit sat
        // 0.64 hc off the CAD behind four such caps).
        const auto tet_is_bad = [&](const std::array<std::uint32_t, 4>& n) {
            const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
            const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
            const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
            const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
            if (!(tet_signed_volume(a, b, c, d) > vol_eps)) {
                return true;
            }
            if (validity::tet_shape_quality(a, b, c, d) >= validity::kCellShapeFloor) {
                return false;
            }
            return on_boundary[n[0]] != 0 && on_boundary[n[1]] != 0 &&
                   on_boundary[n[2]] != 0 && on_boundary[n[3]] != 0;
        };
        // Node -> incident tets, so the snap line-searches ONE node against its
        // own star instead of rescanning the mesh (same wiring as tet_fill).
        std::vector<std::vector<std::size_t>> incident(out.mesh.nodes.size());
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            for (const auto ni : out.mesh.tets[ti]) {
                incident[ni].push_back(ti);
            }
        }
        std::vector<std::vector<std::uint32_t>> nbrs(out.mesh.nodes.size());
        {
            std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
            for (const auto& n : out.mesh.tets) {
                for (int a = 0; a < 4; ++a) {
                    for (int b = a + 1; b < 4; ++b) {
                        const auto u = n[static_cast<std::size_t>(a)];
                        const auto v = n[static_cast<std::size_t>(b)];
                        const auto key = std::minmax(u, v);
                        if (u == v || !seen.insert({key.first, key.second}).second) {
                            continue;
                        }
                        nbrs[u].push_back(v);
                        nbrs[v].push_back(u);
                    }
                }
            }
        }
        const auto node_offends = [&](std::uint32_t ni) {
            for (const auto ti : incident[ni]) {
                if (tet_is_bad(out.mesh.tets[ti])) {
                    return true;
                }
            }
            return false;
        };
        // Reflection orbit over the node set, so a nudge applies to a node and
        // its mirror images together or not at all: the nudges are a
        // Gauss-Seidel sweep on shared state, and accepting one on one side of
        // a symmetry plane changes what the other side's gate reads (measured
        // on cylinder/feature at h = 12 mm: the per-node variant shipped a
        // 0.930 mirrored-tet fraction; the gate is == 1.0).
        const mesh::MirrorNodeOrbit orbit(
            mirror != nullptr ? *mirror : mesh::MirrorFrame{}, out.mesh.nodes, [&] {
                const mesh::MirrorKeyFrame frame = mesh::mirror_key_frame(out.mesh.nodes);
                return frame.inv_quantum > 0.0 ? 1.0 / frame.inv_quantum : 0.0;
            }());
        const auto orbit_of = [&](std::uint32_t node) {
            std::vector<std::uint32_t> group{node};
            if (!orbit.active()) {
                return group;
            }
            for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
                const std::uint32_t other = orbit.reflected(node, mask);
                if (other == mesh::MirrorNodeOrbit::npos) {
                    group.clear();
                    return group;
                }
                if (std::find(group.begin(), group.end(), other) == group.end()) {
                    group.push_back(other);
                }
            }
            return group;
        };
        // Interior room for a blocked node, mirroring tet_fill: the star's
        // interior neighbours are free, and its other wall nodes may slide
        // tangentially (re-projected through the same oracle, so their
        // placement never changes — only their spacing).
        const auto reproject = [&](std::uint32_t ni, const Eigen::Vector3d& p) {
            if (projection == nullptr) {
                return mirror_unfold(
                    mirror, closest_on_surface(surface, mirror_fold(mirror, p)).point, p);
            }
            const auto target = boundary_projection_target(surface, p, ni, projection, mirror);
            return target ? target->point : p;
        };
        const auto relax_neighborhood = [&](std::uint32_t seed) {
            if (incident[seed].empty()) {
                return false;
            }
            std::vector<std::uint32_t> ring;
            std::vector<std::uint32_t> wall;
            for (const auto ti : incident[seed]) {
                for (const auto ni : out.mesh.tets[ti]) {
                    if (ni == seed || nbrs[ni].empty()) {
                        continue;
                    }
                    (on_boundary[ni] == 0 ? ring : wall).push_back(ni);
                }
            }
            auto dedup = [&](std::vector<std::uint32_t>& v) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
                // Mirror-canonical: each nudge is accepted against the shared
                // node array, so the sweep order decides the result (ADR-0036).
                sort_mirror_canonical(out.mesh.nodes, v);
            };
            dedup(ring);
            dedup(wall);
            bool moved_any = false;
            const double cap = 0.25 * hc;
            std::vector<char> done(out.mesh.nodes.size(), 0);
            const auto nudge = [&](std::uint32_t ni, bool tangential) {
                if (done[ni] != 0) {
                    return;
                }
                const auto group = orbit_of(ni);
                if (group.empty()) {
                    return; // incomplete orbit: no move is symmetric
                }
                for (const auto node : group) {
                    done[node] = 1;
                }
                std::vector<Eigen::Vector3d> saved;
                std::vector<Eigen::Vector3d> moved;
                saved.reserve(group.size());
                moved.reserve(group.size());
                for (const auto node : group) {
                    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                    for (const auto other : nbrs[node]) {
                        centroid += out.mesh.nodes[other];
                    }
                    centroid /= static_cast<double>(nbrs[node].size());
                    saved.push_back(out.mesh.nodes[node]);
                    const Eigen::Vector3d step = 0.5 * (centroid - saved.back());
                    const double len = step.norm();
                    Eigen::Vector3d trial =
                        saved.back() + (len > cap ? step * (cap / len) : step);
                    if (tangential) {
                        trial = reproject(node, trial);
                        if ((trial - saved.back()).norm() > cap) {
                            return; // projection ran away; leave the wall alone
                        }
                    }
                    moved.push_back(trial);
                }
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    out.mesh.nodes[group[gi]] = moved[gi];
                }
                for (const auto node : group) {
                    if (node_offends(node)) {
                        for (std::size_t gi = 0; gi < group.size(); ++gi) {
                            out.mesh.nodes[group[gi]] = saved[gi];
                        }
                        return;
                    }
                }
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    if ((out.mesh.nodes[group[gi]] - saved[gi]).squaredNorm() > 0.0) {
                        moved_any = true;
                    }
                }
            };
            for (const auto ni : ring) {
                nudge(ni, /*tangential=*/false);
            }
            if (!moved_any) {
                for (const auto ni : wall) {
                    nudge(ni, /*tangential=*/true);
                }
            }
            return moved_any;
        };
        // Strong budget so LEB mid-edges on holes leave the Cartesian stair.
        // Use max(hc, ~cell diagonal) scale via frac>1; soft-unsnap keeps quality.
        //
        // Two stages. The bulk runs the compatibility (inversion-only) path:
        // its all-or-nothing semantics are what the downstream collapse and
        // relaxation rounds are built on — a stair cell may dip thin mid-snap
        // because repair_round lifts it afterwards. Measured on sphere.step at
        // h = 8 mm: gating THIS stage on the shape floor pinned 28 boundary
        // nodes 1.45 mm off the sphere behind transient stair caps.
        std::vector<std::size_t> skin_tets;
        skin_tets.reserve(snap_nodes.size() * 4);
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            const auto& n = out.mesh.tets[ti];
            if (on_boundary[n[0]] || on_boundary[n[1]] || on_boundary[n[2]] ||
                on_boundary[n[3]]) {
                skin_tets.push_back(ti);
            }
        }
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
        snap_boundary_nodes(surface, out.mesh.nodes, snap_nodes, hc, collect_invert,
                            /*max_move_frac=*/1.15, /*passes=*/7, features, {}, {},
                            /*defer_coupled=*/false, projection, {}, mirror);
        // Straggler rescue. The compatibility ladder's last resort is a full
        // retreat to the raw lattice site, and a node can stay there: measured
        // on icecream_cone at h = 8 mm, one mid-wall orbit sat 0.64 hc off the
        // CAD behind four all-boundary cap tets pinned at the shape floor, and
        // neither the exterior gate nor any smoother could move it afterwards.
        // Re-attempt exactly those nodes with the culprit-aware path: bisected
        // partial keeps, floor-gated (an all-boundary cap has no interior
        // corner, so no later pass can lift it — it must block here), and
        // neighbourhood relaxation to open the room first. CAD-backed only:
        // against a tessellation the exact oracle does not exist and a node
        // 0.01 hc off the facets is within facet error, not stranded — the
        // rescue there moved nodes onto facet noise (measured on
        // test.stl/hole plate: composite score 0.48 -> 0.25).
        std::vector<std::uint32_t> stragglers;
        if (projection != nullptr) {
            for (const auto ni : snap_nodes) {
                if (ni >= out.mesh.nodes.size()) {
                    continue;
                }
                const auto target = boundary_projection_target(surface, out.mesh.nodes[ni], ni,
                                                               projection, mirror);
                if (target && target->distance > 0.01 * hc && target->distance <= 2.5 * hc) {
                    stragglers.push_back(ni);
                }
            }
        }
        if (!stragglers.empty()) {
            // Orbit-locked rescue: one fraction ladder for a whole reflection
            // orbit, accepted only when EVERY member's star accepts it. The
            // shared culprit-aware snap path cannot do this — its per-node
            // quality gates read values that tie across a mirror pair in exact
            // arithmetic and diverge in floating point, and a ladder that
            // stops one member at 0.5 and its image at 0.75 ships the
            // asymmetry (measured on cylinder/feature at h = 12 mm: 0.930
            // mirrored-tet fraction against the == 1.0 gate, ADR-0036 §9.2).
            mesh::sort_mirror_canonical(out.mesh.nodes, stragglers);
            std::vector<char> rescued(out.mesh.nodes.size(), 0);
            for (const auto seed : stragglers) {
                if (rescued[seed] != 0) {
                    continue;
                }
                const auto group = orbit_of(seed);
                if (group.empty()) {
                    continue; // incomplete orbit: no move is symmetric
                }
                for (const auto node : group) {
                    rescued[node] = 1;
                }
                std::vector<Eigen::Vector3d> saved;
                std::vector<Eigen::Vector3d> target_pt;
                saved.reserve(group.size());
                target_pt.reserve(group.size());
                bool have_all = true;
                for (const auto node : group) {
                    const auto target = boundary_projection_target(
                        surface, out.mesh.nodes[node], node, projection, mirror);
                    if (!target) {
                        have_all = false;
                        break;
                    }
                    saved.push_back(out.mesh.nodes[node]);
                    target_pt.push_back(target->point);
                }
                if (!have_all) {
                    continue;
                }
                const auto star_clean = [&] {
                    for (const auto node : group) {
                        if (node_offends(node)) {
                            return false;
                        }
                    }
                    return true;
                };
                // One fraction for the whole orbit, bisected; the ladder rungs
                // are shared with the snap's retreat ladder.
                double good = -1.0;
                double bad = -1.0;
                // Exact placement first, gated on inversion only — the
                // pipeline's standing contract is "inversion blocks, thinness
                // is repaired downstream" (repair_round / ship gate). The
                // floor-gated bisection would otherwise park a node a
                // bracket-width off the exact BRep — measured 1.35e-9 m on
                // sphere/graded, where the fidelity tests demand 1e-12·h.
                {
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] = target_pt[gi];
                    }
                    bool invert_free = true;
                    for (const auto node : group) {
                        for (const auto ti : incident[node]) {
                            const auto& n = out.mesh.tets[ti];
                            const double v =
                                tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                                  out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                            if (!(v > vol_eps)) {
                                invert_free = false;
                                break;
                            }
                        }
                        if (!invert_free) {
                            break;
                        }
                    }
                    if (invert_free) {
                        good = 1.0;
                    }
                }
                if (good < 0.0) {
                    for (const double frac : {0.75, 0.5, 0.25}) {
                        for (std::size_t gi = 0; gi < group.size(); ++gi) {
                            out.mesh.nodes[group[gi]] =
                                saved[gi] + frac * (target_pt[gi] - saved[gi]);
                        }
                        if (star_clean()) {
                            good = frac;
                            break;
                        }
                        bad = frac;
                    }
                }
                if (good < 0.0) {
                    // No ladder rung is clean: open room (orbit-locked) and
                    // retry once, exactly as the snap's relax rounds do.
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] = saved[gi];
                    }
                    if (relax_neighborhood(seed)) {
                        for (const double frac : {1.0, 0.75, 0.5, 0.25}) {
                            for (std::size_t gi = 0; gi < group.size(); ++gi) {
                                out.mesh.nodes[group[gi]] =
                                    saved[gi] + frac * (target_pt[gi] - saved[gi]);
                            }
                            if (star_clean()) {
                                good = frac;
                                break;
                            }
                            bad = frac;
                        }
                    }
                }
                if (good < 0.0) {
                    for (std::size_t gi = 0; gi < group.size(); ++gi) {
                        out.mesh.nodes[group[gi]] = saved[gi];
                    }
                    continue;
                }
                if (bad > good) {
                    for (int step = 0; step < 6; ++step) {
                        const double mid = 0.5 * (good + bad);
                        for (std::size_t gi = 0; gi < group.size(); ++gi) {
                            out.mesh.nodes[group[gi]] =
                                saved[gi] + mid * (target_pt[gi] - saved[gi]);
                        }
                        if (star_clean()) {
                            good = mid;
                        } else {
                            bad = mid;
                        }
                    }
                }
                for (std::size_t gi = 0; gi < group.size(); ++gi) {
                    out.mesh.nodes[group[gi]] = saved[gi] + good * (target_pt[gi] - saved[gi]);
                }
            }
        }
        // Per-node accept/reject re-project for residual outliers (hole kinks).
        // Full projection; keep only if no skin tet inverts.
        {
            const double thr = 0.08 * hc;
            for (auto ni : snap_nodes) {
                if (ni >= out.mesh.nodes.size()) {
                    continue;
                }
                const auto target = boundary_projection_target(surface, out.mesh.nodes[ni], ni,
                                                               projection, mirror);
                if (!target || !(target->distance > thr) || target->distance > 2.5 * hc) {
                    continue;
                }
                const Eigen::Vector3d saved = out.mesh.nodes[ni];
                // Full projection first, then partial fractions — a partial move
                // still shrinks the residual when the full one inverts a tet.
                static constexpr double kFracs[] = {1.0, 0.6, 0.35};
                for (const double frac : kFracs) {
                    out.mesh.nodes[ni] = saved + frac * (target->point - saved);
                    bool ok = true;
                    for (const auto ti : incident[ni]) {
                        const auto& n = out.mesh.tets[ti];
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

    // Capture projection-resistant boundary nodes exactly once. Repair may
    // expose interior lattice nodes; treating those newly exposed nodes as
    // fresh juts on every round peels successive healthy layers from the
    // solid. The carve contract is deliberately limited to this initial set.
    //
    // Distance alone is the WRONG test, and it is the sphere crater. A jut is
    // meant to be a stair chord left hanging in a CAD void: the snap could not
    // pull it onto the wall, so it pokes into a hole and its tets must go. But
    // a boundary node that stayed put because projecting it OUTWARD would
    // invert a skin tet is the exact mirror image — it sits inside the solid,
    // short of the surface, and peeling its tets does not remove a spike, it
    // digs a pit one element deep and calls the pit floor the boundary.
    //
    // A sphere has no holes, so every one of its juts is that mirror case, and
    // the carve gouged it: measured on sphere.step at h = 8 mm feature-graded,
    // 12 boundary nodes ended up as far as 9.46 mm inside the fitted 49.90 mm
    // sphere (1.2 h, one carved layer), and the showcase solve of the same part
    // had one 12.18 mm deep. Every crater passed the shell census -- open=0,
    // nonmanifold=0 -- because a pit is watertight. `hybrid` on the same part
    // and the same h keeps its worst boundary node 0.65 mm off the sphere.
    //
    // So the distance proposes and the surface confirms, exactly as the child
    // carve above: a jut is a node that is far from the surface AND outside it.
    const double initial_jut_threshold = 0.15 * hc;
    std::unordered_set<std::uint32_t> initial_juts;
    for (const auto ni : tet_boundary_nodes(out.mesh.tets, out.mesh.nodes)) {
        const Eigen::Vector3d& p = out.mesh.nodes[ni];
        if (surface_distance(p) > initial_jut_threshold && outside_solid(p)) {
            initial_juts.insert(ni);
        }
    }

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
            // `try_collapse` checks that no incident tet inverts or degrades,
            // which is necessary and not sufficient: an edge collapse also has
            // to satisfy the link condition, or it welds the complex to itself
            // and slits the skin. Rather than evaluate that condition -- which
            // is subtle in 3D and easy to get subtly wrong -- the pass is run
            // and then checked, and reverted whole if it tore anything. A
            // sliver that survives is a quality problem; a torn skin is a
            // correctness one, and the two are not tradeable.
            const auto tets_before = out.mesh.tets;
            const auto remap_before = node_remap;
            const std::size_t torn_before = tet_shell_topology(out.mesh.tets).n_torn_edges;
            // Slivers are, by definition, almost no material: a pass that
            // cleans them up loses a rounding error of volume. A pass that
            // loses real volume is not cleaning up, it is eating the part.
            // Measured after the link condition let S4 collapse freely for the
            // first time: channel_s0 at h=0.0075 m went from rel_err 0.0045 to
            // 0.1006 and was refused outright, and sphere_box and
            // perforated_plate lost an order of magnitude of accuracy each.
            const auto total_volume = [&]() {
                double v = 0.0;
                for (const auto& t : out.mesh.tets) {
                    v += std::abs(tet_signed_volume(out.mesh.nodes[t[0]], out.mesh.nodes[t[1]],
                                                    out.mesh.nodes[t[2]],
                                                    out.mesh.nodes[t[3]]));
                }
                return v;
            };
            const double volume_before = total_volume();
            const auto bvec = tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
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
            // Viability/quality of merging `dead` into `surv`, as the worst
            // post-collapse aspect over the incident star, or -infinity when the
            // merge is not legal. Pure: reads the mesh, never mutates it, so a
            // collapse direction can be *chosen* on quality before it is applied.
            const auto collapse_score = [&](std::uint32_t dead, std::uint32_t surv) {
                // Never pull the surface inward: a boundary node may only merge
                // into another boundary node.
                if (bset.count(dead) && !bset.count(surv)) {
                    return -std::numeric_limits<double>::infinity();
                }
                const auto it = incident.find(dead);
                if (it == incident.end()) {
                    return -std::numeric_limits<double>::infinity();
                }
                double worst = std::numeric_limits<double>::infinity();
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
                        return -std::numeric_limits<double>::infinity();
                    }
                    worst = std::min(worst, aspect_after);
                }
                return worst;
            };
            const auto try_collapse = [&](std::uint32_t dead, std::uint32_t surv) {
                if (!std::isfinite(collapse_score(dead, surv))) {
                    return false;
                }
                const auto it = incident.find(dead);
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
                return true;
            };

            // Collapse decisions are sequential and their inputs tie constantly on
            // a symmetric mesh (equal edge lengths, equal aspects), so every
            // ordering below runs through this frame instead of node/tet indices,
            // which do not mirror (ADR-0036).
            const MirrorKeyFrame mkey = mirror_key_frame(out.mesh.nodes);

            // Collapse the whole reflection orbit or none of it.
            //
            // Ordering the decisions equivariantly is not enough here, and the
            // measurement says so: with every geometry query folded and every tie
            // broken on a mirror-invariant key, cylinder.step at h = 8 mm still
            // entered this round at 100/100/100% mirrored tets and left it at
            // 99.7/98.8/99.4%, with 170 of 1880 collapses lacking a mirror image —
            // all on the curved wall, none within four cells of a mid-plane. The
            // mechanism is coupling along a ring of caps: a greedy sweep that
            // merges adjacent wall nodes commits a matching, and a matching chosen
            // one cap at a time need not be mirror-symmetric even when every
            // individual choice is.
            //
            // So a collapse is applied to every reflected copy of itself at once,
            // and refused unless
            //   * every reflected copy of both endpoints exists and is still live,
            //   * every copy is legal on its own (`collapse_score` finite), and
            //   * the copies' incident stars are pairwise DISJOINT.
            // The disjointness requirement is what makes applying them in sequence
            // equivalent to applying them simultaneously: with disjoint stars no
            // copy can change another's legality. It refuses collapses within a
            // cell of a mid-plane, and a node ON a plane — whose two candidate
            // survivors are reflections of each other — can never collapse, which
            // is correct: either choice would break the symmetry it sits on.
            const double orbit_tol = mkey.inv_quantum > 0.0 ? 1.0 / mkey.inv_quantum : 0.0;
            const MirrorNodeOrbit orbit =
                mirror != nullptr ? MirrorNodeOrbit(*mirror, out.mesh.nodes, orbit_tol)
                                  : MirrorNodeOrbit(MirrorFrame{}, out.mesh.nodes, 0.0);
            const auto collapse_orbit = [&](std::uint32_t dead, std::uint32_t surv) {
                if (!orbit.active()) {
                    return try_collapse(dead, surv);
                }
                std::vector<std::pair<std::uint32_t, std::uint32_t>> copies;
                copies.reserve(8);
                copies.emplace_back(dead, surv);
                for (unsigned mask = 1; mask <= orbit.reflection_count(); ++mask) {
                    const std::uint32_t d2 = orbit.reflected(dead, mask);
                    const std::uint32_t s2 = orbit.reflected(surv, mask);
                    if (d2 == MirrorNodeOrbit::npos || s2 == MirrorNodeOrbit::npos) {
                        return false;
                    }
                    if (d2 == dead && s2 == surv) {
                        continue; // this reflection fixes the edge
                    }
                    if (d2 == dead || s2 == surv || d2 == surv || s2 == dead) {
                        return false; // edge meets its own reflection
                    }
                    if (std::find(copies.begin(), copies.end(),
                                  std::pair<std::uint32_t, std::uint32_t>{d2, s2}) !=
                        copies.end()) {
                        continue;
                    }
                    copies.emplace_back(d2, s2);
                }
                // Disjointness is required BETWEEN copies, never within one: a
                // copy's own two endpoints necessarily share the tets on the edge
                // being collapsed.
                std::unordered_set<std::size_t> other_stars;
                for (const auto& [d2, s2] : copies) {
                    if (node_remap[d2] != d2 || node_remap[s2] != s2) {
                        return false;
                    }
                    if (!std::isfinite(collapse_score(d2, s2))) {
                        return false;
                    }
                    std::vector<std::size_t> star;
                    for (const auto node : {d2, s2}) {
                        const auto it = incident.find(node);
                        if (it == incident.end()) {
                            return false;
                        }
                        for (const auto tj : it->second) {
                            if (!removed[tj]) {
                                star.push_back(tj);
                            }
                        }
                    }
                    for (const auto tj : star) {
                        if (other_stars.count(tj) != 0) {
                            return false; // copies interact: not independent
                        }
                    }
                    other_stars.insert(star.begin(), star.end());
                }
                bool applied = false;
                for (const auto& [d2, s2] : copies) {
                    applied = try_collapse(d2, s2) || applied;
                }
                return applied;
            };
            // Phase A — void juts: boundary nodes whose projection the snap had
            // to reject (hole-rim stair chords poking into the void) merge into
            // an adjacent on-surface boundary node instead of leaving a spike.
            for (const auto ni : bvec) {
                if (node_remap[ni] != ni || !initial_juts.contains(ni)) {
                    continue;
                }
                const double resid = surface_distance(out.mesh.nodes[ni]);
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
                // Distance ties break on the mirror key, not the node id, so a
                // jut and its mirror image merge toward mirrored neighbours. The
                // length itself is compared through `tie_key`: mirrored lengths
                // agree only to the last ulp, and ordering on that noise is what
                // sent mirrored juts to unmirrored survivors (ADR-0036).
                std::sort(cand.begin(), cand.end(), [&](const auto& x, const auto& y) {
                    const auto lx = tie_key(x.first, hc);
                    const auto ly = tie_key(y.first, hc);
                    if (lx != ly) {
                        return lx < ly;
                    }
                    const auto kx = mkey.key(out.mesh.nodes[x.second]);
                    const auto ky = mkey.key(out.mesh.nodes[y.second]);
                    return kx != ky ? kx < ky : x.second < y.second;
                });
                cand.erase(std::unique(cand.begin(), cand.end(),
                                       [](const auto& x, const auto& y) {
                                           return x.second == y.second;
                                       }),
                           cand.end());
                for (const auto& [len, surv] : cand) {
                    if (surface_distance(out.mesh.nodes[surv]) > 0.05 * hc) {
                        continue;
                    }
                    if (collapse_orbit(ni, surv)) {
                        break;
                    }
                }
            }

            // Phase B — sliver caps: collapse the shortest viable edge of the
            // worst cap first. Visit order measured across three candidates on
            // the STL scorecards (graded column): index order fails the sphere
            // residual (M1max 0.035 h), centre-out radial order passes the sphere
            // (0.0095 h) but destroys the hole plate (M1max 4.76 — centre-out
            // starts at the hole rim, the one boundary that must not move first),
            // and worst-aspect-first passes both (sphere 0.0095 h, hole plate
            // composite 0.530 at parity with the old index order's 0.5304).
            // Quality, not geometry, is the only order that respects both.
            const auto& mkey_b = mkey;
            const auto tet_center_b = [&](std::size_t ti) {
                const auto& n = out.mesh.tets[ti];
                return 0.25 * (out.mesh.nodes[n[0]] + out.mesh.nodes[n[1]] +
                               out.mesh.nodes[n[2]] + out.mesh.nodes[n[3]]);
            };
            std::vector<std::size_t> cap_order;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                if (!removed[ti] && aspect_of(out.mesh.tets[ti]) < kCapAspect) {
                    cap_order.push_back(ti);
                }
            }
            std::sort(cap_order.begin(), cap_order.end(), [&](std::size_t a, std::size_t b) {
                const auto qa = tie_key(aspect_of(out.mesh.tets[a]), 1.0);
                const auto qb = tie_key(aspect_of(out.mesh.tets[b]), 1.0);
                if (qa != qb) {
                    return qa < qb;
                }
                const auto ka = mkey_b.key(tet_center_b(a));
                const auto kb = mkey_b.key(tet_center_b(b));
                return ka != kb ? ka < kb : a < b;
            });
            for (const auto ti : cap_order) {
                if (removed[ti] || aspect_of(out.mesh.tets[ti]) >= kCapAspect) {
                    continue;
                }
                const auto tet = out.mesh.tets[ti];
                // Candidate edges by ascending length; the six edges of a lattice
                // cap tie on length constantly, and the tie-break decides which
                // edge collapses — keys, not ids, so a mirrored cap tries its
                // edges in mirrored order (ADR-0036).
                std::array<std::pair<double, std::array<std::uint32_t, 2>>, 6> edges_len{};
                int ne = 0;
                for (int p = 0; p < 4; ++p) {
                    for (int q2 = p + 1; q2 < 4; ++q2) {
                        const std::uint32_t a = tet[static_cast<std::size_t>(p)];
                        const std::uint32_t b = tet[static_cast<std::size_t>(q2)];
                        edges_len[static_cast<std::size_t>(ne++)] = {
                            (out.mesh.nodes[a] - out.mesh.nodes[b]).norm(), {a, b}};
                    }
                }
                // A cap's six edges must be tried in mirrored order, so the
                // tie-break has to identify an EDGE mirror-invariantly, not just
                // its lower endpoint: two edges sharing that endpoint tie, and the
                // tie then fell to the node id, which does not mirror. Measured on
                // cylinder.step at h = 8 mm with every geometry query folded, that
                // single tie decided 164 of 2032 collapses differently on the two
                // sides of the y mid-plane. The sorted pair of endpoint mirror keys
                // is unique per edge here: no lattice edge joins two nodes of the
                // same reflection orbit, because even cell counts keep every cell —
                // and therefore every tet and every edge — off the mid-planes.
                using EdgeMirrorKey = std::array<std::array<long long, 3>, 2>;
                const auto edge_mirror_key = [&](std::array<std::uint32_t, 2> e) {
                    const auto k0 = mkey.key(out.mesh.nodes[e[0]]);
                    const auto k1 = mkey.key(out.mesh.nodes[e[1]]);
                    return k1 < k0 ? EdgeMirrorKey{{k1, k0}} : EdgeMirrorKey{{k0, k1}};
                };
                std::sort(edges_len.begin(), edges_len.end(),
                          [&](const auto& x, const auto& y) {
                              const auto lx = tie_key(x.first, hc);
                              const auto ly = tie_key(y.first, hc);
                              if (lx != ly) {
                                  return lx < ly;
                              }
                              const auto kx = edge_mirror_key(x.second);
                              const auto ky = edge_mirror_key(y.second);
                              return kx != ky ? kx < ky
                                              : std::min(x.second[0], x.second[1]) <
                                                    std::min(y.second[0], y.second[1]);
                          });
                bool done = false;
                for (int e = 0; e < ne && !done; ++e) {
                    const std::uint32_t a = edges_len[static_cast<std::size_t>(e)].second[0];
                    const std::uint32_t b = edges_len[static_cast<std::size_t>(e)].second[1];
                    if (node_remap[a] != a || node_remap[b] != b) {
                        continue;
                    }
                    // When both directions are legal, keep the one whose incident
                    // star survives in better shape. Aspect is mirror-invariant,
                    // so a cap and its mirror image collapse in mirrored
                    // directions — but only when the two scores are compared
                    // through `tie_key`. On a symmetric lattice the scores tie
                    // exactly in exact arithmetic and differ in the last ulp in
                    // floating point, so a raw comparison picked the direction
                    // from that noise and mirrored caps collapsed opposite ways.
                    // A genuine tie falls back to farther-from-centre.
                    const double sa = collapse_score(a, b); // a dies
                    const double sb = collapse_score(b, a); // b dies
                    if (!std::isfinite(sa) && !std::isfinite(sb)) {
                        continue;
                    }
                    const auto qa = tie_key(sa, 1.0);
                    const auto qb = tie_key(sb, 1.0);
                    std::uint32_t dead;
                    std::uint32_t surv;
                    if (qa > qb) {
                        dead = a;
                        surv = b;
                    } else if (qb > qa) {
                        dead = b;
                        surv = a;
                    } else {
                        const auto ka = mkey.key(out.mesh.nodes[a]);
                        const auto kb = mkey.key(out.mesh.nodes[b]);
                        const bool a_dies = ka < kb || (ka == kb && a < b);
                        dead = a_dies ? a : b;
                        surv = a_dies ? b : a;
                    }
                    done = collapse_orbit(dead, surv);
                }
            }

            if (!any) {
                break;
            }
            std::size_t w = 0;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                if (!removed[ti]) {
                    out.mesh.tets[w++] = out.mesh.tets[ti];
                }
            }
            out.mesh.tets.resize(w);
            // 0.5% of the part per pass: three orders of magnitude more than a
            // sliver sweep needs, and far below the fill guard's 10% limit, so
            // a pass has to be visibly destructive to trip it.
            constexpr double kMaxPassVolumeLoss = 0.005;
            const double volume_after = total_volume();
            if (tet_shell_topology(out.mesh.tets).n_torn_edges > torn_before ||
                volume_after < (1.0 - kMaxPassVolumeLoss) * volume_before) {
                out.mesh.tets = tets_before;
                node_remap = remap_before;
                break;
            }
            collapsed_any = true;
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
            const auto& jut = initial_juts;
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
                // Deleting a tet that has a free face exposes its other three,
                // which can strand a neighbour with two exposed faces of its
                // own. Same class as the child carve above, same remedy.
                if (n_kill > 0 && n_kill < out.mesh.tets.size()) {
                    restrict_kill_to_shell(out.mesh.tets, kill);
                    n_kill = static_cast<std::size_t>(
                        std::count(kill.begin(), kill.end(), static_cast<char>(1)));
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
        // Deterministic order, and the reason is not tidiness: `smooth_boundary_nodes`
        // relaxes and re-projects node positions in the order it is handed the
        // faces, reverting moves that invert a tet, so the surviving coordinates
        // depend on that order. Iterating `once` directly makes the mesh a
        // function of libstdc++'s bucket layout: measured 2026-08-14, the same
        // commit built with MSVC and with gcc disagreed on 5 of 24 corpus pairs
        // (stepped_shaft_s2_c0 hybrid_zoo 264 vs 200 elements), which no
        // floating-point flag reproduced (-ffp-contract=off changed nothing).
        std::vector<std::array<std::uint32_t, 4>> free_faces;
        free_faces.reserve(once.size() / 2);
        for (const auto& [key, tri] : once) {
            if (fcount[key] == 1) {
                free_faces.push_back({tri[0], tri[1], tri[2], tri[2]});
            }
        }
        std::sort(free_faces.begin(), free_faces.end(), [](const auto& l, const auto& r) {
            return std::tie(l[0], l[1], l[2]) < std::tie(r[0], r[1], r[2]);
        });
        const double vol_eps = 1e-14 * hc * hc * hc;
        smooth_boundary_nodes(
            surface, out.mesh.nodes, free_faces, hc,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto& n : out.mesh.tets) {
                    const double v =
                        tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                          out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                    if (v <= vol_eps) {
                        offenders.insert(n.begin(), n.end());
                    }
                }
            },
            /*passes=*/3, /*relax=*/0.5, features, projection, mirror);
        for (auto& n : out.mesh.tets) {
            const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                               out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
            if (v < 0.0) {
                std::swap(n[1], n[2]);
            }
        }
        // Hard-pin CAD vertices and sharp edge curves. Smoothing has just
        // evened the wall spacing, so this is where a crease becomes exact
        // rather than "as close as the nearest face point happens to be" —
        // the difference between a 90° edge and the chamfer it used to render
        // as (ADR-0035).
        if (fit != nullptr && fit->can_pin()) {
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> star;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                for (const auto ni : out.mesh.tets[ti]) {
                    star[ni].push_back(ti);
                }
            }
            const auto node_offends = [&](std::uint32_t ni) {
                const auto it = star.find(ni);
                if (it == star.end()) {
                    return false;
                }
                for (const auto ti : it->second) {
                    const auto& n = out.mesh.tets[ti];
                    const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
                    const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
                    const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
                    const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
                    // Shape floor, not just a positive volume: a sign-only
                    // gate let the pin flatten a skin tet to quality 1e-4
                    // (measured on cylinder graded, ADR-0033's failure mode).
                    if (!(tet_signed_volume(a, b, c, d) > vol_eps) ||
                        validity::tet_shape_quality(a, b, c, d) < validity::kCellShapeFloor) {
                        return true;
                    }
                }
                return false;
            };
            std::vector<std::uint32_t> bnodes;
            for (const auto& f : free_faces) {
                bnodes.insert(bnodes.end(), f.begin(), f.end());
            }
            std::sort(bnodes.begin(), bnodes.end());
            bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
            out.mesh.pin = pin_feature_nodes(
                *fit->cad, *fit->topo, out.mesh.nodes, bnodes, hc, node_offends,
                projection != nullptr ? projection->provenance : nullptr, mirror);
            for (auto& n : out.mesh.tets) {
                const double v = tet_signed_volume(out.mesh.nodes[n[0]], out.mesh.nodes[n[1]],
                                                   out.mesh.nodes[n[2]], out.mesh.nodes[n[3]]);
                if (v < 0.0) {
                    std::swap(n[1], n[2]);
                }
            }
        }
    }
    // Smoothing can thin an already-marginal cap — one more collapse round.
    repair_round();
    // The final repair can expose lattice nodes after smoothing. Projection is
    // inversion-safe and does not remove cells, so finish on a snapped boundary
    // rather than leaving those new faces at raw lattice coordinates.
    snap_round();

    // S7 overlapped-sheet carve. Snap gives every node its exact CAD owner, so
    // at a concave crease two sheets project onto their own face patches and
    // can legally interpenetrate — every tet positive, every edge manifold,
    // and free faces buried strictly inside other cells (icecream_cone at
    // h = 10 mm shipped 9, sphere_box_s0 at h = 3.6 mm shipped ~500; rendered
    // as holes). Born in the FIRST snap round (596 buried immediately on
    // sphere_box), so no downstream smoothing can prevent it. A buried face's
    // owner tet is doubly-counted volume — the same material is inside another
    // cell too — so the remedy is deletion under the shell guard, then re-snap
    // for the newly exposed layer; node-pulling is the rejected variant
    // (star-centroid pulls strand at 299 on sphere_box; crease pulls pile both
    // sheets onto the crease and grow it to 706).
    {
        constexpr int kOverlapPasses = 48;
        const auto carve_to_clean = [&]() {
            for (int pass = 0; pass < kOverlapPasses; ++pass) {
                const auto owners =
                    buried_free_tet_face_owners(out.mesh.nodes, out.mesh.tets, hc);
                if (owners.empty()) {
                    return;
                }
                std::vector<char> kill(out.mesh.tets.size(), 0);
                for (const auto ti : owners) {
                    kill[ti] = 1;
                }
                restrict_kill_to_shell(out.mesh.tets, kill);
                std::size_t n_kill = static_cast<std::size_t>(
                    std::count(kill.begin(), kill.end(), static_cast<char>(1)));
                if (n_kill == 0) {
                    // Every single-tet kill was vetoed: at a one-cell-thick
                    // overlap band each deletion alone would pinch the shell.
                    // Escalate to the whole node-neighbourhood of every stuck
                    // owner so the guard judges the band, not a pinch.
                    std::unordered_set<std::uint32_t> owner_nodes;
                    for (const auto ti : owners) {
                        owner_nodes.insert(out.mesh.tets[ti].begin(), out.mesh.tets[ti].end());
                    }
                    std::fill(kill.begin(), kill.end(), static_cast<char>(0));
                    for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                        for (const auto ni : out.mesh.tets[ti]) {
                            if (owner_nodes.count(ni)) {
                                kill[ti] = 1;
                                break;
                            }
                        }
                    }
                    restrict_kill_to_shell(out.mesh.tets, kill);
                    n_kill = static_cast<std::size_t>(
                        std::count(kill.begin(), kill.end(), static_cast<char>(1)));
                }
                if (n_kill == 0 || n_kill >= out.mesh.tets.size()) {
                    return;
                }
                std::size_t w = 0;
                for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                    if (!kill[ti]) {
                        out.mesh.tets[w++] = out.mesh.tets[ti];
                    }
                }
                out.mesh.tets.resize(w);
                // No snap inside the carve: re-projecting each freshly exposed
                // layer regenerates the tangle mid-carve (measured 33 -> 58 on
                // sphere_box_s0 at h = 3.6 mm).
            }
        };
        // Alternate carve -> snap toward the JOINT fixed point: no burial AND
        // a snapped boundary. Carving alone leaves the exposed layer at raw
        // positions, which costs real fidelity (plate_hole p99 off-surface
        // 0.025 h -> 0.035 h); snapping alone re-creates the tangle. Most
        // parts close the loop in one round; a near-tangent wedge that will
        // not close ships the divot instead of the self-intersection.
        constexpr int kAlternations = 3;
        for (int round = 0; round < kAlternations; ++round) {
            carve_to_clean();
            if (count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc).n_buried != 0) {
                break; // carve stranded: do not snap on top of a tangle
            }
            snap_round();
            if (count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc).n_buried == 0) {
                break; // snapped AND clean
            }
        }
        // The carve can expose sliver caps (cylinder_prism's min boundary
        // aspect fell to 0.00077, floor 0.0025) — one more collapse round
        // cleans them; it may itself re-expose or re-tangle, so it runs
        // BEFORE the final carve + pull and the census gate stays last.
        repair_round();
        carve_to_clean();
        pull_buried_free_faces(out.mesh.nodes, out.mesh.tets, hc, /*max_iters=*/8, mirror);
        if (const auto st = count_buried_free_tet_faces(out.mesh.nodes, out.mesh.tets, hc);
            st.n_buried != 0) {
            throw ValidityError(
                std::format("graded_tet_fill_surface: {} boundary faces remain buried inside "
                            "other cells after the overlap carve — self-intersecting boundary",
                            st.n_buried));
        }
    }

    // S6 interior sliver relaxation.
    //
    // S4 collapses sliver caps and S5 peels the flakes that gain a free face, so
    // both are boundary-facing by construction. A sliver wedged in the INTERIOR
    // survives them, and it is not merely a quality complaint: measured
    // 2026-08-15 on tests/fixtures/parts/cylinder.step at h=0.005 with the
    // graded mesher, the mesh shipped 194,098 valid tets whose worst aspect was
    // 4.17e-05, and the elastostatic solve then failed outright — CG broke down
    // under both preconditioners with a true relative residual of 1.9e6. A cell
    // three decades below the shape floor conditions the stiffness matrix out of
    // the solver's reach, so the mesher owns this, not the solver.
    //
    // The cure is room, exactly as in `hex_fill_surface`: a sliver's non-boundary
    // nodes relax toward the centroid of their edge neighbours, and a move is kept
    // only when the worst aspect over the node's whole incident star strictly
    // improves. Boundary nodes are frozen, so this cannot cost one micron of
    // boundary fidelity, and monotone acceptance means it cannot make any cell
    // worse than it found it.
    {
        constexpr double kSliverFloor = 0.01; // ~half kCellShapeFloor: cure, not polish
        constexpr int kRelaxPasses = 6;
        const auto aspect = [&](const std::array<std::uint32_t, 4>& n) {
            const Eigen::Vector3d& a = out.mesh.nodes[n[0]];
            const Eigen::Vector3d& b = out.mesh.nodes[n[1]];
            const Eigen::Vector3d& c = out.mesh.nodes[n[2]];
            const Eigen::Vector3d& d = out.mesh.nodes[n[3]];
            return validity::tet_shape_quality(a, b, c, d);
        };
        std::vector<std::vector<std::uint32_t>> incident(out.mesh.nodes.size());
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            for (const auto ni : out.mesh.tets[ti]) {
                incident[ni].push_back(static_cast<std::uint32_t>(ti));
            }
        }
        // Boundary nodes are every node on a face used by exactly one tet.
        std::vector<char> frozen(out.mesh.nodes.size(), 0);
        {
            static constexpr int kTris[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
            std::map<std::array<std::uint32_t, 3>, int> face_use;
            for (const auto& t : out.mesh.tets) {
                for (const auto& f : kTris) {
                    std::array<std::uint32_t, 3> key{{t[static_cast<std::size_t>(f[0])],
                                                      t[static_cast<std::size_t>(f[1])],
                                                      t[static_cast<std::size_t>(f[2])]}};
                    std::sort(key.begin(), key.end());
                    ++face_use[key];
                }
            }
            for (const auto& [key, uses] : face_use) {
                if (uses == 1) {
                    for (const auto ni : key) {
                        frozen[ni] = 1;
                    }
                }
            }
        }
        // Neighbour lists, ascending node id: the acceptance test reads the shared
        // node array, so visit order is mesh-level mutation state (ADR-0032).
        std::vector<std::vector<std::uint32_t>> nbrs(out.mesh.nodes.size());
        {
            std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
            for (const auto& t : out.mesh.tets) {
                for (int i = 0; i < 4; ++i) {
                    for (int j = i + 1; j < 4; ++j) {
                        const auto a = t[static_cast<std::size_t>(i)];
                        const auto b = t[static_cast<std::size_t>(j)];
                        if (a == b) {
                            continue;
                        }
                        const auto key = std::minmax(a, b);
                        if (!seen.insert({key.first, key.second}).second) {
                            continue;
                        }
                        nbrs[a].push_back(b);
                        nbrs[b].push_back(a);
                    }
                }
            }
        }
        const auto worst_incident = [&](std::uint32_t ni) {
            double lo = 1.0;
            for (const auto ti : incident[ni]) {
                lo = std::min(lo, aspect(out.mesh.tets[ti]));
            }
            return lo;
        };
        for (int pass = 0; pass < kRelaxPasses; ++pass) {
            // Nodes of every sliver tet, deduplicated, visited in mirror-canonical
            // order. This is a Gauss-Seidel sweep on the shared node array — an
            // accepted move changes whether the next node's move improves its own
            // star — so ascending node id gave a node and its mirror image
            // different predecessors here too (ADR-0036).
            std::set<std::uint32_t> target_set;
            for (const auto& t : out.mesh.tets) {
                if (aspect(t) >= kSliverFloor) {
                    continue;
                }
                for (const auto ni : t) {
                    if (frozen[ni] == 0 && !nbrs[ni].empty()) {
                        target_set.insert(ni);
                    }
                }
            }
            if (target_set.empty()) {
                break;
            }
            std::vector<std::uint32_t> targets(target_set.begin(), target_set.end());
            sort_mirror_canonical(out.mesh.nodes, targets);
            std::size_t n_moved = 0;
            for (const auto ni : targets) {
                Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                for (const auto other : nbrs[ni]) {
                    centroid += out.mesh.nodes[other];
                }
                centroid /= static_cast<double>(nbrs[ni].size());
                const Eigen::Vector3d saved = out.mesh.nodes[ni];
                const double before = worst_incident(ni);
                bool kept = false;
                for (const double omega : {0.7, 0.4, 0.2}) {
                    out.mesh.nodes[ni] = saved + omega * (centroid - saved);
                    if (worst_incident(ni) > before) {
                        kept = true;
                        break;
                    }
                }
                if (kept) {
                    ++n_moved;
                } else {
                    out.mesh.nodes[ni] = saved;
                }
            }
            if (n_moved == 0) {
                break;
            }
        }
    }

    // Rebuild boundary quads as exterior tris padded for pipeline display
    // (quad[3]=quad[2] for pure tris is OK — pipeline may re-extract).
    // Keep original lattice quads when present; append nothing if already set.
    check_tet_fill_geometry(out.mesh);
    return out;
}

} // namespace polymesh::mesh
