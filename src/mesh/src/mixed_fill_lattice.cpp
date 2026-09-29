// SPDX-License-Identifier: BSD-3-Clause
// mixed_fill_surface lattice phases: classification, fine marking, and the
// conforming 2:1 transition closure.
#include "mixed_fill_internal.hpp"

#include "mesh/cell_stamp.hpp"
#include "mesh/grid_classify.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <span>
#include <vector>

namespace polymesh::mesh::detail::mixed {

MixedLattice classify_mixed_lattice(const geom::TriSurface& surface,
                                    const Eigen::Vector3d& bbox_min,
                                    const Eigen::Vector3d& bbox_max, double h,
                                    const SizeFieldFn& size_field,
                                    bool local_surface_classification,
                                    const std::function<void()>& cancel_check) {
    MixedLattice lat;
    // Budget for 2:1 fine subcells (up to 8× in refined bands).
    constexpr long kHybridMaxCoarse = static_cast<long>(kHybridMaxElems);
    const double h_budget =
        min_h_for_cell_budget(bbox_min, bbox_max, kHybridMaxCoarse, /*subdivision=*/1);
    lat.h_budget = h_budget;
    const double h_use = (h_budget > 0.0) ? std::max(h, h_budget) : h;
    // The optional h/2 classifier is sampling-only. Mixed parents are folded
    // into the existing one-level local refinement below; the coarse lattice
    // itself never advances globally.
    lat.classification = classify_cells_feature_aware(
        surface, bbox_min, bbox_max, h_use, kHybridMaxCoarse,
        /*relative_volume_tolerance=*/0.01, local_surface_classification ? 1 : 0, size_field);
    const CartesianGrid& grid = lat.classification.grid;
    const auto& inside = lat.classification.inside;
    poll_cancel(cancel_check);
    lat.nx = grid.nx;
    lat.ny = grid.ny;
    lat.nz = grid.nz;
    lat.h_cell = grid.max_edge();
    const int nx = lat.nx, ny = lat.ny, nz = lat.nz;

    lat.dist.assign(inside.size(), -1);
    auto& dist = lat.dist;
    std::queue<std::array<int, 3>> q;
    int max_dist = 0;
    for (int k = 0; k < nz; ++k) {
        poll_cancel(cancel_check);
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[lat.idx(i, j, k)]) {
                    continue;
                }
                bool boundary = false;
                for (const auto& o : kFaceNbr) {
                    if (!lat.inb(i + o[0], j + o[1], k + o[2])) {
                        boundary = true;
                        break;
                    }
                }
                if (boundary) {
                    dist[lat.idx(i, j, k)] = 0;
                    q.push({i, j, k});
                }
            }
        }
    }
    while (!q.empty()) {
        if ((q.size() & 1023U) == 0U) {
            poll_cancel(cancel_check);
        }
        const auto c = q.front();
        q.pop();
        const int d0 = dist[lat.idx(c[0], c[1], c[2])];
        max_dist = std::max(max_dist, d0);
        for (const auto& o : kFaceNbr) {
            const int ni = c[0] + o[0], nj = c[1] + o[1], nk = c[2] + o[2];
            if (!lat.inb(ni, nj, nk)) {
                continue;
            }
            auto& dn = dist[lat.idx(ni, nj, nk)];
            if (dn < 0 || dn > d0 + 1) {
                dn = d0 + 1;
                q.push({ni, nj, nk});
            }
        }
    }
    lat.max_dist = max_dist;
    return lat;
}

void mark_fine_cells(MixedLattice& lat, MixedFillOutput& out, const geom::TriSurface& surface,
                     int skin_layers, std::span<const geom::SharpEdge> features,
                     double feature_band, std::span<const Eigen::Vector3d> curvature_seeds,
                     double seed_band, double curvature_turn_deg,
                     const SizeFieldFn& size_field,
                     const std::function<void()>& cancel_check) {
    const CartesianGrid& grid = lat.classification.grid;
    const auto& inside = lat.classification.inside;
    const auto& classification = lat.classification;
    const int nx = lat.nx, ny = lat.ny, nz = lat.nz;
    const double h_cell = lat.h_cell;
    const auto& dist = lat.dist;

    // Free-surface hop skin only when no geo drivers (unit boxes). With
    // feature/seed/curvature, refine those bands to h/2 instead of flooding
    // the exterior.
    const int skin_cap = std::max(1, (lat.max_dist + 1) / 2);
    const bool have_geo = (feature_band > 0.0) || (seed_band > 0.0) ||
                          (curvature_turn_deg > 0.0) || static_cast<bool>(size_field);
    const int skin_use = have_geo ? 0 : std::min(skin_layers, skin_cap);

    lat.is_fine.assign(inside.size(), 0);
    lat.is_feature_skin.assign(inside.size(), 0);
    lat.is_seed_skin.assign(inside.size(), 0);
    auto& is_fine = lat.is_fine;
    auto& is_feature_skin = lat.is_feature_skin;
    auto& is_seed_skin = lat.is_seed_skin;
    lat.size_adaptive = have_geo;

    for (int k = 0; k < nz; ++k) {
        poll_cancel(cancel_check);
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                if (!inside[lat.idx(i, j, k)]) {
                    continue;
                }
                const int d = dist[lat.idx(i, j, k)];
                if (skin_use > 0 && d >= 0 && d < skin_use) {
                    is_fine[lat.idx(i, j, k)] = 1; // plain mode: skin as fine pyramids at h
                }
            }
        }
    }
    double field_h_min = std::numeric_limits<double>::infinity();
    double field_h_max = 0.0;
    std::size_t n_field_budget_clamped = 0;
    if (size_field) {
        const double h_floor = (lat.h_budget > 0.0) ? lat.h_budget : h_cell;
        for (int k = 0; k < nz; ++k) {
            poll_cancel(cancel_check);
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const auto id = lat.idx(i, j, k);
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
                        static_cast<int>(std::lround(std::log2(h_cell / h_target))), 0, 1);
                    if (level >= 1) {
                        is_fine[id] = 1;
                    }
                }
            }
        }
    }
    // Feature/seed → fine (h/2 via 2×2×2). stamp writes into is_fine.
    stamp_feature_cells(is_fine, &is_feature_skin, nx, ny, nz, grid, surface, features,
                        feature_band);
    stamp_seed_cells(is_fine, &is_seed_skin, nx, ny, nz, grid, curvature_seeds, seed_band);
    // Per-cell turning-angle criterion (angle-adaptive; hybrid has one fine
    // level, so L2 output is unused here).
    if (curvature_turn_deg > 0.0) {
        stamp_curvature_cells(is_fine, nullptr, &is_seed_skin, nx, ny, nz, grid, surface,
                              curvature_turn_deg * 3.14159265358979323846 / 180.0);
    }

    // Fine child sampling detects sub-cell topology without a global lattice
    // advance. Mixed parents are already surface cells; one face-neighbour
    // solid shell is promoted so the 2:1 interface closes in the interior.
    if (!classification.child_inside_mask.empty()) {
        std::vector<char> promote(inside.size(), 0);
        for (int k = 0; k < nz; ++k) {
            for (int j = 0; j < ny; ++j) {
                for (int i = 0; i < nx; ++i) {
                    const auto id = lat.idx(i, j, k);
                    const auto mask = classification.child_inside_mask[id];
                    if (!inside[id] || mask == 0 || mask == std::uint8_t{0xff}) {
                        continue;
                    }
                    is_fine[id] = 1;
                    is_feature_skin[id] = 1;
                    for (const auto& o : kFaceNbr) {
                        const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                        if (lat.inb(ni, nj, nk)) {
                            promote[lat.idx(ni, nj, nk)] = 1;
                        }
                    }
                }
            }
        }
        for (std::size_t c = 0; c < promote.size(); ++c) {
            if (promote[c]) {
                is_fine[c] = 1;
            }
        }
    }
    // Outside → not fine.
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            is_fine[c] = 0;
            is_feature_skin[c] = 0;
            is_seed_skin[c] = 0;
            continue;
        }
        if (is_feature_skin[c] || is_seed_skin[c]) {
            ++out.n_feature_skin_cells;
        }
    }
    out.field_h_min = std::isfinite(field_h_min) ? field_h_min : 0.0;
    out.field_h_max = field_h_max;
    out.n_field_budget_clamped = n_field_budget_clamped;
}

void close_transitions(MixedLattice& lat, MixedFillOutput& out, bool native_poly_transitions,
                       const std::function<void()>& cancel_check) {
    const auto& inside = lat.classification.inside;
    const int nx = lat.nx, ny = lat.ny, nz = lat.nz;
    const double h_cell = lat.h_cell;
    auto& is_fine = lat.is_fine;
    auto& is_transition = lat.is_transition;
    is_transition.assign(inside.size(), 0);
    // A hanging mid-node exists on a coarse lattice edge iff ANY cell incident
    // to that edge is fine, so every non-fine cell touching such an edge must
    // emit facets that include those mids, else the mesh cracks along edges.
    const bool size_adaptive = lat.size_adaptive;
    if (size_adaptive) {
        auto is_free_surface = [&](int i, int j, int k) {
            for (const auto& o : kFaceNbr) {
                if (!lat.inb(i + o[0], j + o[1], k + o[2])) {
                    return true;
                }
            }
            return false;
        };
        long n_interior = 0;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            n_interior += (inside[c] != 0);
        }
        // Recompute the 2:1 interface against the current fine set. A hanging
        // mid-node exists on a coarse lattice edge iff ANY cell incident to that
        // edge is fine, so every non-fine cell touching such an edge — not just
        // the face-neighbors of fine cells — is a transition cell.
        const auto mark_transitions = [&] {
            std::fill(is_transition.begin(), is_transition.end(), 0);
            for (int k = 0; k < nz; ++k) {
                poll_cancel(cancel_check);
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = lat.idx(i, j, k);
                        if (!inside[id] || is_fine[id]) {
                            continue;
                        }
                        bool fan = false;
                        for (const auto& o : kFaceNbr) {
                            if (lat.cell_fine(i + o[0], j + o[1], k + o[2])) {
                                fan = true;
                                break;
                            }
                        }
                        // Edge-adjacent fine cells also hang mids on this cell's edges.
                        for (int eb = 0; !fan && eb < 2; ++eb) {
                            for (int ec = 0; !fan && ec < 2; ++ec) {
                                fan = lat.edge_split(i, j + eb, k + ec, 0) ||
                                      lat.edge_split(i + eb, j, k + ec, 1) ||
                                      lat.edge_split(i + eb, j + ec, k, 2);
                            }
                        }
                        is_transition[id] = fan ? 1 : 0;
                    }
                }
            }
        };
        // Free-surface gap-close only (2 hops). Spatial seeds already cover the
        // hole ring; a long free-surface BFS floods flat box faces and kills
        // bulk/fine contrast on the exterior.
        constexpr int kFsGapHops = 2;
        for (int pass = 0; pass < kFsGapHops; ++pass) {
            std::vector<char> promote(inside.size(), 0);
            for (int k = 0; k < nz; ++k) {
                poll_cancel(cancel_check);
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = lat.idx(i, j, k);
                        if (!inside[id] || is_fine[id] || !is_free_surface(i, j, k)) {
                            continue;
                        }
                        for (const auto& o : kFaceNbr) {
                            const int ni = i + o[0], nj = j + o[1], nk = k + o[2];
                            if (lat.inb(ni, nj, nk) && is_fine[lat.idx(ni, nj, nk)]) {
                                promote[id] = 1;
                                break;
                            }
                        }
                    }
                }
            }
            for (std::size_t c = 0; c < promote.size(); ++c) {
                if (promote[c]) {
                    is_fine[c] = 1;
                }
            }
        }
        // Push the 2:1 interface one cell inside the wall: the boundary snap would
        // squash a free-surface transition fan (and bend native-poly facets out of
        // plane, breaking the patch test). Monotone: is_fine only grows.
        // Every exit leaves `is_transition` consistent with `is_fine`.
        mark_transitions();
        // At least one promotion per round, so `n_interior` rounds bound the fixed point.
        for (long guard = 0; guard <= n_interior; ++guard) {
            poll_cancel(cancel_check);
            bool changed = false;
            for (int k = 0; k < nz; ++k) {
                poll_cancel(cancel_check);
                for (int j = 0; j < ny; ++j) {
                    for (int i = 0; i < nx; ++i) {
                        const auto id = lat.idx(i, j, k);
                        if (is_transition[id] && is_free_surface(i, j, k)) {
                            is_fine[id] = 1;
                            changed = true;
                        }
                    }
                }
            }
            if (!changed) {
                break;
            }
            mark_transitions();
        }
        out.h_fine = 0.5 * h_cell;

        // Fine-level affordability (ADR-0013, ADR-0015): a fine cell costs 48
        // product-FE pyramids against 6 for a bulk hex. When the graded estimate
        // busts the budget but a uniform h/2 lattice fits, the uniform lattice wins
        // (no coarse cell survives, so no 2:1 interface); otherwise the graded
        // lattice is kept. kFanElemsPerTransition: measured fans are 18-26 elements.
        constexpr long kFanElemsPerTransition = 24;
        const long max_hybrid_elems = static_cast<long>(kHybridMaxElems);
        long n_fine_cells = 0, n_trans_cells = 0;
        for (std::size_t c = 0; c < inside.size(); ++c) {
            n_fine_cells += (is_fine[c] != 0);
            n_trans_cells += (is_transition[c] != 0);
        }
        const long n_lattice_hex =
            8 * n_fine_cells + (n_interior - n_fine_cells - n_trans_cells);
        const long est_graded =
            native_poly_transitions
                ? n_lattice_hex + n_trans_cells
                : 6 * n_lattice_hex + kFanElemsPerTransition * n_trans_cells;
        if (est_graded > max_hybrid_elems && 8 * n_interior <= max_hybrid_elems) {
            std::fill(is_transition.begin(), is_transition.end(), 0);
            for (std::size_t c = 0; c < is_fine.size(); ++c) {
                is_fine[c] = (inside[c] != 0) ? 1 : 0;
            }
            out.n_feature_skin_cells = 0;
        }
    }
}

} // namespace polymesh::mesh::detail::mixed
