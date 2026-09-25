// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/hybrid_fill.hpp"
#include "mesh/fill_progress.hpp"

#include "hybrid_fill_internal.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/lattice_split.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/mirror.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace detail {
namespace {

/// Face-neighbour offsets, in `emit_face_quad` face order.
constexpr int kFaceNbr[6][3] = {{-1, 0, 0}, {1, 0, 0},  {0, -1, 0},
                                {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

/// Is `p` on the void side of the surface? Answered by the outward normal of
/// the nearest triangle, which is the only inside/outside oracle available
/// at a scale finer than the classifier's lattice samples. A point exactly
/// on the surface, and a point whose nearest triangle is missing, both count
/// as inside: every caller uses this to authorise DELETING material, so the
/// ambiguous answer has to be the one that keeps it.
///
/// The whole test runs on the folded point: reflection preserves the sign of
/// (p − cp)·n because it reflects both vectors, so a tet and its mirror image
/// are condemned or spared together.
bool outside_solid(const geom::TriSurface& surface, const MirrorFrame* mirror,
                   const Eigen::Vector3d& raw) {
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
}

/// Classify the bbox lattice, fuse the grading marks, emit the coarse lattice
/// into `out.mesh` and refine it by multi-level LEB (ADR-0018). Fills the
/// grading fields of `out`; returns the mirror-symmetrised classification.
FeatureAwareClassification
build_graded_lattice(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                     const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                     std::span<const geom::SharpEdge> features, double feature_band,
                     std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                     double curvature_turn_deg, const BoundaryProjectionContext* projection,
                     const SizeFieldFn& size_field, const MirrorFrame* mirror,
                     std::size_t max_refinement_tets, FillProgressScope& progress,
                     GradedTetFillOutput& out) {
    // Coarse-primary lattice at target h. Multi-level LEB (ADR-0018):
    //   L0 bulk ~ h, L1 feature/skin ~ h/2, L2 high-κ seeds ~ h/4.
    constexpr int subdiv = 2; // max LEB depth (L2)
    const std::size_t kGradedMaxCells =
        max_refinement_tets > 0 ? std::max<std::size_t>(1, max_refinement_tets / 6)
                                : 48 * 1024;
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kGradedMaxCells, /*subdivision=*/1);
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    // The h/2 classifier is sampling-only. The coarse lattice remains at the
    // requested h; mixed parents are marked for local LEB below. Cell counts are
    // even so the alternating Kuhn split mirrors about the bbox mid-planes.
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
    progress.set_cells(0, classification.inside.size());
    progress.set_phase("background_grading");
    const auto& inside = classification.inside;
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const double hc = grid.max_edge();
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
                fill_progress_poll(idx(i, j, k), inside.size());
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
        fill_progress_poll();
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
    // Level 3 adds bisection waves at protected sharp-feature cores.
    std::vector<std::uint8_t> refine_level(inside.size(), 0);
    std::vector<char> is_feature(inside.size(), 0);
    std::vector<char> is_seed(inside.size(), 0);
    std::vector<char> is_l1(inside.size(), 0);
    std::vector<char> is_l2(inside.size(), 0);
    std::vector<char> is_deep_feature(inside.size(), 0);

    for (std::size_t c = 0; c < inside.size(); ++c) {
        fill_progress_poll(c, inside.size());
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
        // The Cartesian allocation floor bounds the background, not a local
        // wall/curvature demand. Local LEB is governed by the caller's cap.
        const double h_floor = hc / 4.0;
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    fill_progress_poll(idx(i, j, k), inside.size());
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
    // deepening every seed ball costs an order of magnitude in elements
    // without improving sharp-edge fidelity.
    if (feature_band > 0.0 && !features.empty()) {
        stamp_feature_cells(is_deep_feature, nullptr, nx, ny, nz, grid, surface, features,
                            0.75 * feature_band);
    }

    for (std::size_t c = 0; c < inside.size(); ++c) {
        fill_progress_poll(c, inside.size());
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
    // symmetric. Take the low-side octant's answer for the whole orbit: on a
    // verified symmetry the true answer is symmetric, so the disagreement is
    // aliasing (ADR-0036).
    if (cell_orbit.active()) {
        const auto level_before = refine_level;
        const auto feature_before = is_feature;
        const auto seed_before = is_seed;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            fill_progress_poll(c, inside.size());
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
        fill_progress_poll();
        any_l1 = any_l1 || lv >= 1;
        any_l2 = any_l2 || lv >= 2;
        any_deep_feature = any_deep_feature || lv >= 3;
    }

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

    // Lattice cell -> six Kuhn tets, main diagonal alternated per cell so the
    // tiling is mirror-symmetric (mesh/lattice_split.hpp).
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

    progress.set_cells(0, inside.size());
    progress.set_phase("background_lattice");
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                fill_progress_poll(idx(i, j, k), inside.size());
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
    progress.set_cells(inside.size(), inside.size());
    progress.poll(true);

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
    int leb_wave = 0;
    const int leb_waves = out.n_fine_cells > 0 ? 1 + (any_l2 ? 1 : 0) + (any_deep_feature ? 2 : 0) : 0;
    auto run_leb_for_min_level = [&](std::uint8_t min_level) {
        progress.set_cells(0, out.mesh.tets.size());
        progress.set_phase("leb_wave", ++leb_wave, leb_waves);
        if (max_refinement_tets > 0 && out.mesh.tets.size() > max_refinement_tets)
            throw RefinementLimitError(out.mesh.tets.size(), max_refinement_tets);
        std::vector<std::size_t> marked;
        marked.reserve(out.mesh.tets.size() / 4 + 8);
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
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
        // Keep regular parents until the requested local levels exist.
        auto refined = local_refine_tets(std::move(out.mesh.nodes), std::move(out.mesh.tets),
                                         marked, &st, nullptr, mirror);
        out.mesh.nodes = std::move(refined.nodes);
        out.mesh.tets = std::move(refined.tets);
        progress.poll(true);
    };

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
    return classification;
}

/// Record the regular lattice spacing, then snap the refined lattice's free
/// surface once at the field's resolution.
void project_refined_lattice(GradedFillState& s) {
    const geom::TriSurface& surface = s.surface;
    const std::span<const geom::SharpEdge> features = s.features;
    BoundaryProjectionContext* const projection = s.projection;
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    progress.set_cells(0, out.mesh.tets.size());
    progress.set_phase("projection");
    // The regular parent/LEB spacing, before any CAD projection. A corner
    // driven through 95% of this spacing toward its neighbour must be merged,
    // not left on the snap ladder's last near-coincident retreat.
    std::vector<double>& original_spacing = s.original_spacing;
    original_spacing.assign(out.mesh.nodes.size(), std::numeric_limits<double>::infinity());
    for (const auto& tet : out.mesh.tets) {
        fill_progress_poll();
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                const double length = (out.mesh.nodes[tet[a]] - out.mesh.nodes[tet[b]]).norm();
                original_spacing[tet[a]] = std::min(original_spacing[tet[a]], length);
                original_spacing[tet[b]] = std::min(original_spacing[tet[b]], length);
            }
        }
    }

    // Project at the field's resolution, not at the background resolution.
    // Warping coarse parents first leaves their locally refined children with
    // unrecoverable boundary slivers at tightly curved seating surfaces.
    {
        std::vector<std::uint32_t> pre_snap =
            tet_boundary_nodes(out.mesh.tets, out.mesh.nodes);
        if (!pre_snap.empty()) {
            std::unordered_set<std::uint32_t> bset(pre_snap.begin(), pre_snap.end());
            std::vector<std::size_t> skin_tets;
            for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
                fill_progress_poll(ti, out.mesh.tets.size());
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
                        fill_progress_poll();
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
}

void carve_void_children(GradedFillState& s,
                         const FeatureAwareClassification& classification) {
    const geom::TriSurface& surface = s.surface;
    const MirrorFrame* const mirror = s.mirror;
    FillProgressScope& progress = s.progress;
    GradedTetFillOutput& out = s.out;
    const CartesianGrid& grid = classification.grid;
    const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
    const auto idx = [&](int i, int j, int k) { return grid.index(i, j, k); };
    // The feature-aware classifier's h/2 samples are authoritative inside a
    // mixed coarse parent. LEB has already made those parents conforming; now
    // remove refined tets whose centroids fall in a void child. Without this
    // local carve the classifier can see a bore while the emitted tet mesh
    // remains the original solid coarse cube.
    if (!classification.child_inside_mask.empty()) {
        progress.set_phase("projection_child_carve");
        // The child mask is a cell-CENTRE parity sample on the h/2 lattice, but
        // LEB has already refined these tets to h/4 and finer. Condemning an
        // h/4 tet because one h/2 sample point landed in void is the hole
        // aliasing bug one level down: beside a curved wall the centre of a
        // child can sit outside the solid while most of the child is inside it,
        // and the carve then cuts a slot into the material.
        //
        // So the mask proposes and the surface confirms: a tet is carved only
        // when its own centroid is outside the solid, judged against the
        // surface at the tet's own scale rather than the sampler's. Requiring
        // instead that the whole tet lie in void children is wrong the other
        // way: it under-carves and voids stop opening.
        std::vector<char> kill(out.mesh.tets.size(), 0);
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
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
            if (!outside_solid(surface, mirror, centroid)) {
                continue; // centroid is inside the solid: keep the tet
            }
            kill[ti] = 1;
        }
        // A centroid-in-void test decides one tet at a time and cannot see that
        // dropping this one strands its neighbour with two exposed faces; the
        // shell guard vetoes deletions that would tear the skin.
        restrict_kill_to_shell(out.mesh.tets, kill);
        std::size_t write = 0;
        for (std::size_t ti = 0; ti < out.mesh.tets.size(); ++ti) {
            fill_progress_poll(ti, out.mesh.tets.size());
            if (!kill[ti]) {
                out.mesh.tets[write++] = out.mesh.tets[ti];
            }
        }
        out.mesh.tets.resize(write);
        if (out.mesh.tets.empty()) {
            throw ValidityError("graded_tet_fill_surface: local child carve removed all tets");
        }
    }
}

void capture_initial_juts(GradedFillState& s) {
    const geom::TriSurface& surface = s.surface;
    const MirrorFrame* const mirror = s.mirror;
    GradedTetFillOutput& out = s.out;
    const double hc = s.hc;
    // Capture projection-resistant boundary nodes exactly once. Repair may
    // expose interior lattice nodes; treating those newly exposed nodes as
    // fresh juts on every round peels successive healthy layers from the
    // solid. The carve contract is deliberately limited to this initial set.
    //
    // Distance alone is the WRONG test. A jut is meant to be a stair chord left
    // hanging in a CAD void: the snap could not pull it onto the wall, so it
    // pokes into a hole and its tets must go. But a boundary node that stayed
    // put because projecting it OUTWARD would invert a skin tet is the exact
    // mirror image — it sits inside the solid, short of the surface, and
    // peeling its tets does not remove a spike, it digs a watertight pit one
    // element deep that no shell census can see.
    //
    // So the distance proposes and the surface confirms, exactly as the child
    // carve: a jut is a node that is far from the surface AND outside it.
    const double initial_jut_threshold = 0.15 * hc;
    std::unordered_set<std::uint32_t>& initial_juts = s.initial_juts;
    for (const auto ni : tet_boundary_nodes(out.mesh.tets, out.mesh.nodes)) {
        fill_progress_poll();
        const Eigen::Vector3d& p = out.mesh.nodes[ni];
        if (surface_distance(surface, mirror, p) > initial_jut_threshold &&
            outside_solid(surface, mirror, p)) {
            initial_juts.insert(ni);
        }
    }
}

} // namespace

double surface_distance(const geom::TriSurface& surface, const MirrorFrame* mirror,
                        const Eigen::Vector3d& p) {
    return closest_on_surface(surface, mirror_fold(mirror, p)).distance;
}

} // namespace detail

GradedTetFillOutput
graded_tet_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                        const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                        std::span<const geom::SharpEdge> features, double feature_band,
                        std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                        double curvature_turn_deg, const BoundaryFit* fit,
                        const SizeFieldFn& size_field, const MirrorFrame* mirror,
                        std::size_t max_refinement_tets, const FillOptions& options) {
    BoundaryProjectionContext* projection = fit != nullptr ? fit->projection : nullptr;
    GradedTetFillOutput out;
    FillProgressScope progress(options, &out.mesh.tets);
    progress.set_phase("classification");
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

    const FeatureAwareClassification classification = detail::build_graded_lattice(
        surface, bbox_min, bbox_max, h, skin_layers, features, feature_band, refine_seeds,
        seed_band, curvature_turn_deg, projection, size_field, mirror, max_refinement_tets,
        progress, out);
    detail::GradedFillState s{.surface = surface,
                              .features = features,
                              .fit = fit,
                              .projection = projection,
                              .mirror = mirror,
                              .progress = progress,
                              .out = out,
                              .hc = classification.grid.max_edge(),
                              .original_spacing = {},
                              .initial_juts = {}};
    detail::project_refined_lattice(s);
    detail::carve_void_children(s, classification);
    detail::snap_round(s);
    detail::capture_initial_juts(s);
    detail::repair_round(s);
    // The carve exposes fresh lattice faces that were never snapped — run a
    // second snap + repair round so the new boundary reaches the surface too.
    detail::snap_round(s);
    detail::repair_round(s);
    detail::smooth_boundary_and_pin_features(s);
    // Smoothing can thin an already-marginal cap — one more collapse round.
    detail::repair_round(s);
    // The final repair can expose lattice nodes after smoothing. Projection is
    // inversion-safe and does not remove cells, so finish on a snapped boundary
    // rather than leaving those new faces at raw lattice coordinates.
    detail::snap_round(s);
    detail::carve_overlapped_sheets(s);
    detail::relax_interior_slivers(s);
    progress.set_phase("quality_geometry_check");
    check_tet_fill_geometry(out.mesh);
    progress.set_cells(out.mesh.tets.size(), out.mesh.tets.size());
    progress.set_phase("complete");
    return out;
}

} // namespace polymesh::mesh
