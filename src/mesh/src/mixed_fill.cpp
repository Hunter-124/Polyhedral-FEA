// SPDX-License-Identifier: BSD-3-Clause
#include "mesh/mixed_fill.hpp"

#include "mesh/poly_mesh.hpp"
#include "mixed_fill_internal.hpp"

#include <Eigen/Core>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::mesh {

using detail::mixed::FanSpan;
using detail::mixed::kHexFaces;
using detail::mixed::MixedLattice;
using detail::mixed::normalize_pyramid_diagonal;
using detail::mixed::orient_pyramid_winding;
using detail::mixed::poll_cancel;

MixedFillOutput
mixed_fill_surface(const geom::TriSurface& surface, const Eigen::Vector3d& bbox_min,
                   const Eigen::Vector3d& bbox_max, double h, int skin_layers,
                   std::span<const geom::SharpEdge> features, double feature_band,
                   std::span<const Eigen::Vector3d> curvature_seeds, double seed_band,
                   bool snap_boundary, double curvature_turn_deg, bool native_poly_transitions,
                   const std::function<void()>& cancel_check, const SizeFieldFn& size_field,
                   bool local_surface_classification) {
    if (!(h > 0.0) || !std::isfinite(h)) {
        throw ValidityError("mixed_fill_surface: h must be positive");
    }
    if (skin_layers < 1) {
        skin_layers = 1;
    }
    if (!(feature_band > 0.0) || features.empty()) {
        feature_band = 0.0;
    }
    if (!(seed_band > 0.0) || curvature_seeds.empty()) {
        seed_band = 0.0;
    }
    if (!(curvature_turn_deg > 0.0)) {
        curvature_turn_deg = 0.0;
    }
    poll_cancel(cancel_check);

    // Phases run in this order; each is deterministic in lattice (k, j, i) order.
    MixedLattice lat =
        detail::mixed::classify_mixed_lattice(surface, bbox_min, bbox_max, h, size_field,
                                              local_surface_classification, cancel_check);
    const double h_cell = lat.h_cell;

    MixedFillOutput out;
    out.h = h_cell;
    out.h_fine = h_cell;
    out.skin_layers = skin_layers;
    out.native_poly_transitions = native_poly_transitions;
    out.classification_refinement_levels = lat.classification.refinement_levels;
    out.classification_volume_error = lat.classification.relative_volume_error;

    detail::mixed::mark_fine_cells(lat, out, surface, skin_layers, features, feature_band,
                                   curvature_seeds, seed_band, curvature_turn_deg, size_field,
                                   cancel_check);
    detail::mixed::close_transitions(lat, out, native_poly_transitions, cancel_check);
    const auto& inside = lat.classification.inside;
    const auto& is_fine = lat.is_fine;
    for (std::size_t c = 0; c < inside.size(); ++c) {
        if (!inside[c]) {
            continue;
        }
        if (is_fine[c]) {
            ++out.n_level1_cells;
        } else {
            ++out.n_level0_cells;
        }
    }

    const std::vector<FanSpan> fan_spans =
        detail::mixed::emit_mixed_cells(lat, out, native_poly_transitions, cancel_check);

    if (out.cells.empty()) {
        throw ValidityError("mixed_fill_surface: no interior cells");
    }

    detail::mixed::place_shell_apexes(out, fan_spans, surface, features,
                                      native_poly_transitions, h_cell, cancel_check);
    if (snap_boundary && !out.boundary_quads.empty()) {
        detail::mixed::snap_mixed_boundary(out, fan_spans, surface, features, h_cell,
                                           cancel_check);
    }
    // Snapping can change which geometric base diagonal is preferred; rotate
    // again so any direct MixedFillOutput consumer sees the chosen split in
    // VTK/PyVista's fixed local 0-2 slot.
    for (auto& cell : out.cells) {
        normalize_pyramid_diagonal(cell, out.nodes);
    }
    return out;
}

MixedFillOutput expand_mixed_hex_to_pyramids(const MixedFillOutput& fill) {
    MixedFillOutput out;
    out.h = fill.h;
    out.h_fine = fill.h_fine;
    out.boundary_quads = fill.boundary_quads;
    out.local_child_boundary_quads = fill.local_child_boundary_quads;
    out.movable_fans = fill.movable_fans;
    out.boundary_max_distance = fill.boundary_max_distance;
    out.skin_layers = fill.skin_layers;
    out.n_feature_skin_cells = fill.n_feature_skin_cells;
    out.n_fine_cells = fill.n_fine_cells;
    out.n_transition_cells = fill.n_transition_cells;
    out.n_level0_cells = fill.n_level0_cells;
    out.n_level1_cells = fill.n_level1_cells;
    out.classification_refinement_levels = fill.classification_refinement_levels;
    out.classification_volume_error = fill.classification_volume_error;
    out.field_h_min = fill.field_h_min;
    out.field_h_max = fill.field_h_max;
    out.n_field_budget_clamped = fill.n_field_budget_clamped;
    out.native_poly_transitions = fill.native_poly_transitions;
    out.nodes = fill.nodes;
    out.n_hex = 0;
    out.n_pyramid = 0;
    out.n_tet = 0;
    out.n_poly = 0;
    out.cells.reserve(fill.cells.size() + 5 * fill.n_hex);

    for (const auto& cell : fill.cells) {
        if (cell.kind == MixedCellKind::kTet4) {
            out.cells.push_back(cell);
            ++out.n_tet;
            continue;
        }
        if (cell.kind == MixedCellKind::kPyramid5) {
            MixedCell pyr = cell;
            normalize_pyramid_diagonal(pyr, out.nodes);
            out.cells.push_back(std::move(pyr));
            ++out.n_pyramid;
            continue;
        }
        if (cell.kind == MixedCellKind::kPolyVem) {
            out.cells.push_back(cell);
            ++out.n_poly;
            continue;
        }
        Eigen::Vector3d center = Eigen::Vector3d::Zero();
        for (int i = 0; i < 8; ++i) {
            center += out.nodes[cell.nodes[static_cast<std::size_t>(i)]];
        }
        center /= 8.0;
        const auto apex = static_cast<std::uint32_t>(out.nodes.size());
        out.nodes.push_back(center);
        out.movable_fans.push_back({apex, cell.nodes});
        for (const auto& face : kHexFaces) {
            MixedCell pyr;
            pyr.kind = MixedCellKind::kPyramid5;
            pyr.n_nodes = 5;
            pyr.nodes[0] = cell.nodes[static_cast<std::size_t>(face[0])];
            pyr.nodes[1] = cell.nodes[static_cast<std::size_t>(face[1])];
            pyr.nodes[2] = cell.nodes[static_cast<std::size_t>(face[2])];
            pyr.nodes[3] = cell.nodes[static_cast<std::size_t>(face[3])];
            pyr.nodes[4] = apex;
            orient_pyramid_winding(pyr, out.nodes);
            normalize_pyramid_diagonal(pyr, out.nodes);
            out.cells.push_back(pyr);
            ++out.n_pyramid;
        }
    }
    return out;
}

} // namespace polymesh::mesh
