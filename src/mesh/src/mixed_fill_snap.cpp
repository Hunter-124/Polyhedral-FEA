// SPDX-License-Identifier: BSD-3-Clause
// mixed_fill_surface boundary phases: pre-snap shell apex placement, boundary
// snap, and private fan apex repair (also exposed as repair_mixed_fan_apices).
#include "mixed_fill_internal.hpp"

#include "geom/features.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace polymesh::mesh {
namespace {

// Boundary-snap rejection tests: a cell is rejected when INVERTED (signed
// measure <= vol_eps, a machine-degeneracy test ~1e-14·h³) or a SLIVER
// (normalized shape below `shape_floor`). `shape_floor <= 0` disables the
// shape test.
bool hex_bad(const std::array<std::uint32_t, 8>& hx, const std::vector<Eigen::Vector3d>& nodes,
             double shape_floor) {
    std::array<Eigen::Vector3d, 8> x{};
    for (int i = 0; i < 8; ++i) {
        x[static_cast<std::size_t>(i)] = nodes[hx[static_cast<std::size_t>(i)]];
    }
    if (validity::hex8_min_jacobian(x) <= 0.0) {
        return true;
    }
    return shape_floor > 0.0 && validity::hex8_shape_quality(x) < shape_floor;
}

bool tet_bad(const std::array<std::uint32_t, 4>& n, const std::vector<Eigen::Vector3d>& nodes,
             double vol_eps, double shape_floor) {
    const Eigen::Vector3d& a = nodes[n[0]];
    const Eigen::Vector3d& b = nodes[n[1]];
    const Eigen::Vector3d& c = nodes[n[2]];
    const Eigen::Vector3d& d = nodes[n[3]];
    if (validity::tet_signed_volume(a, b, c, d) <= vol_eps) {
        return true;
    }
    return shape_floor > 0.0 && validity::tet_shape_quality(a, b, c, d) < shape_floor;
}

bool pyramid_bad(const std::array<std::uint32_t, 5>& n,
                 const std::vector<Eigen::Vector3d>& nodes, double vol_eps,
                 double shape_floor) {
    const Eigen::Vector3d& p0 = nodes[n[0]];
    const Eigen::Vector3d& p1 = nodes[n[1]];
    const Eigen::Vector3d& p2 = nodes[n[2]];
    const Eigen::Vector3d& p3 = nodes[n[3]];
    const Eigen::Vector3d& p4 = nodes[n[4]];
    if (validity::pyramid_min_split_volume(p0, p1, p2, p3, p4) <= vol_eps) {
        return true;
    }
    return shape_floor > 0.0 &&
           validity::pyramid_split_shape_quality(p0, p1, p2, p3, p4) < shape_floor;
}

/// Search the interior natural-coordinate lattice for the least-displaced fan
/// apex that clears the requested cell-quality floor. If the floor cannot be
/// reached, retain the best improving candidate. Samples |ξ|,|η|,|ζ| <= 0.75
/// (step 0.25, chosen empirically), so the apex stays inside the lattice cell
/// and the emitted windings hold for the unsnapped mesh. Least displacement
/// beats best score because the snap prediction it is scored on is approximate.
template <typename WorstQualityFn>
bool search_fan_apex(const std::array<Eigen::Vector3d, 8>& lattice, const Eigen::Vector3d& ctr,
                     WorstQualityFn&& worst, double q_reference, double shape_floor,
                     Eigen::Vector3d& best) {
    best = ctr;
    double best_q = q_reference;
    double best_d2 = std::numeric_limits<double>::max();
    constexpr std::array<double, 7> kAxis{{-0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75}};
    for (const double zeta : kAxis) {
        for (const double eta : kAxis) {
            for (const double xi : kAxis) {
                Eigen::Vector3d a = Eigen::Vector3d::Zero();
                for (std::size_t t = 0; t < 8; ++t) {
                    const auto& s = validity::kHexCornerSigns[t];
                    a += 0.125 * (1.0 + s[0] * xi) * (1.0 + s[1] * eta) * (1.0 + s[2] * zeta) *
                         lattice[t];
                }
                const double q = worst(a);
                const double d2 = (a - ctr).squaredNorm();
                if (best_q >= shape_floor) {
                    if (q >= shape_floor && d2 < best_d2) {
                        best_q = q;
                        best_d2 = d2;
                        best = a;
                    }
                } else if (q >= shape_floor || q > best_q) {
                    best_q = q;
                    best_d2 = d2;
                    best = a;
                }
            }
        }
    }
    return best_q > q_reference;
}

/// Live pyramid/tet cells of each fan, keyed by apex (the last node of every
/// fan cell, see emit_pyramid / emit_tet).
template <typename Fan>
std::map<std::uint32_t, std::vector<const MixedCell*>>
live_fan_cells_by_apex(const std::vector<Fan>& fans, const std::vector<MixedCell>& cells) {
    std::map<std::uint32_t, std::vector<const MixedCell*>> live_fan_cells;
    for (const auto& fan : fans) {
        live_fan_cells.try_emplace(fan.apex);
    }
    for (const auto& cell : cells) {
        if (cell.kind != MixedCellKind::kPyramid5 && cell.kind != MixedCellKind::kTet4) {
            continue;
        }
        const auto apex = cell.nodes[static_cast<std::size_t>(cell.n_nodes - 1)];
        const auto it = live_fan_cells.find(apex);
        if (it != live_fan_cells.end()) {
            it->second.push_back(&cell);
        }
    }
    return live_fan_cells;
}

/// Re-place one fan's private apex against its current bases when its worst
/// cell is below `shape_floor`. Returns true when the apex moved.
bool reseat_fan_apex(std::vector<Eigen::Vector3d>& nodes,
                     const std::array<std::uint32_t, 8>& corners, std::uint32_t apex,
                     const std::vector<const MixedCell*>& fan_cells, double shape_floor) {
    std::array<Eigen::Vector3d, 8> lattice{};
    Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
    for (std::size_t t = 0; t < 8; ++t) {
        lattice[t] = nodes[corners[t]];
        ctr += lattice[t];
    }
    ctr /= 8.0;
    const auto worst_actual = [&](const Eigen::Vector3d& a) {
        double q = std::numeric_limits<double>::max();
        for (const auto* cell : fan_cells) {
            if (cell->kind == MixedCellKind::kPyramid5) {
                q = std::min(q, validity::pyramid_split_shape_quality(
                                    nodes[cell->nodes[0]], nodes[cell->nodes[1]],
                                    nodes[cell->nodes[2]], nodes[cell->nodes[3]], a));
            } else {
                q = std::min(q, validity::tet_shape_quality(nodes[cell->nodes[0]],
                                                            nodes[cell->nodes[1]],
                                                            nodes[cell->nodes[2]], a));
            }
        }
        return q;
    };
    const double q_current = worst_actual(nodes[apex]);
    if (q_current >= shape_floor) {
        return false;
    }
    Eigen::Vector3d best;
    if (search_fan_apex(lattice, ctr, worst_actual, q_current, shape_floor, best)) {
        nodes[apex] = best;
        return true;
    }
    return false;
}

} // namespace

namespace detail::mixed {

void place_shell_apexes(MixedFillOutput& out, const std::vector<FanSpan>& fan_spans,
                        const geom::TriSurface& surface,
                        std::span<const geom::SharpEdge> features,
                        bool native_poly_transitions, double h_cell,
                        const std::function<void()>& cancel_check) {
    // Shell-cell apex placement (product-FE path, ADR-0013). The caller expands
    // hexes into centroid-apex pyramids and snaps afterwards; on a curved wall the
    // snap can carry a shell cell's corners level with or past that apex, and its
    // line-search then retreats the wall to keep the cell valid. Predict each
    // boundary node's snap site and, only where the centroid apex would fall
    // below the shape floor, place the apex where the predicted cell is healthy:
    // expand the hex here, or move an existing fan apex. Faces, conformity and
    // element count are unchanged. Healthy cells keep the centroid (an off-centre
    // apex only thins pyramids on flat walls). Gated on an already mixed lattice:
    // a pure-hex lattice stays hex8, and one pyramid would expand it 6x.
    const bool product_expand_follows = !native_poly_transitions && out.n_hex > 0 &&
                                        (out.n_pyramid > 0 || out.n_tet > 0 || out.n_poly > 0);
    if (product_expand_follows && !out.boundary_quads.empty()) {
        std::set<std::uint32_t> bnode_set;
        for (const auto& q : out.boundary_quads) {
            bnode_set.insert(q.begin(), q.end());
        }
        // Predicted post-snap site of every boundary node. Mirror
        // snap_boundary_nodes exactly: true crease/rim nodes prefer the
        // detected feature when it is as close as the surface target, then the
        // total travel is capped at 1.25h.
        const double travel_cap = 1.25 * h_cell;
        const double edge_prefer_r = 0.55 * h_cell;
        std::map<std::uint32_t, Eigen::Vector3d> predicted;
        for (const auto g : bnode_set) {
            const Eigen::Vector3d& p = out.nodes[g];
            const auto cp = closest_on_surface(surface, p);
            Eigen::Vector3d target = cp.point;
            if (!features.empty()) {
                const auto cf = geom::closest_on_features(p, surface, features);
                if (std::isfinite(cf.distance) && cf.distance > 1e-15 &&
                    cf.distance <= edge_prefer_r &&
                    cf.distance <= cp.distance + 0.08 * h_cell) {
                    target = cf.point;
                }
            }
            const Eigen::Vector3d d = target - p;
            const double len = d.norm();
            predicted.emplace(g, len > travel_cap ? Eigen::Vector3d(p + d * (travel_cap / len))
                                                  : target);
        }
        const double shape_floor = validity::kCellShapeFloor;
        // Post-snap site of a node: its prediction when the snap will move it,
        // its lattice site otherwise.
        const auto site = [&](std::uint32_t g) -> const Eigen::Vector3d& {
            const auto it = predicted.find(g);
            return it == predicted.end() ? out.nodes[g] : it->second;
        };
        // Best apex per cell/fan: search_fan_apex over the lattice cell.
        const std::size_t n_emitted = out.cells.size();
        std::vector<char> replaced(n_emitted, 0);
        std::size_t n_replaced = 0;
        for (std::size_t ci = 0; ci < n_emitted; ++ci) {
            if ((ci & 255U) == 0U) {
                poll_cancel(cancel_check);
            }
            if (out.cells[ci].kind != MixedCellKind::kHex8) {
                continue;
            }
            const std::array<std::uint32_t, 8> cn = out.cells[ci].nodes;
            std::array<Eigen::Vector3d, 8> lattice{};
            std::array<Eigen::Vector3d, 8> snapped{};
            Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
            int n_core = 0;
            for (std::size_t t = 0; t < 8; ++t) {
                lattice[t] = out.nodes[cn[t]];
                ctr += lattice[t];
                const auto it = predicted.find(cn[t]);
                if (it == predicted.end()) {
                    snapped[t] = lattice[t];
                    ++n_core;
                } else {
                    snapped[t] = it->second;
                }
            }
            if (n_core == 0 || n_core == 8) {
                continue; // untouched cell, or nothing left to anchor an apex to
            }
            ctr /= 8.0;
            // Worst pyramid of the predicted post-snap cell for a given apex.
            // Base winding follows emit_pyramid (apex on the positive side of
            // the lattice face) so the split-tet volumes keep their sign.
            const auto worst_split = [&](const Eigen::Vector3d& a) {
                double q = std::numeric_limits<double>::max();
                for (const auto& face : kHexFaces) {
                    const auto f0 = static_cast<std::size_t>(face[0]);
                    const auto f1 = static_cast<std::size_t>(face[1]);
                    const auto f2 = static_cast<std::size_t>(face[2]);
                    const auto f3 = static_cast<std::size_t>(face[3]);
                    const Eigen::Vector3d nrm =
                        (lattice[f1] - lattice[f0]).cross(lattice[f2] - lattice[f0]);
                    const bool ccw = nrm.dot(a - lattice[f0]) > 0.0;
                    q = std::min(q, validity::pyramid_split_shape_quality(
                                        snapped[f0], snapped[ccw ? f1 : f3], snapped[f2],
                                        snapped[ccw ? f3 : f1], a));
                }
                return q;
            };
            const double q_centroid = worst_split(ctr);
            if (q_centroid >= shape_floor) {
                continue; // the caller's centroid apex survives its own snap
            }
            Eigen::Vector3d best;
            if (!search_fan_apex(lattice, ctr, worst_split, q_centroid, shape_floor, best)) {
                continue; // no apex does better; leave the cell to the caller
            }
            const auto apex = static_cast<std::uint32_t>(out.nodes.size());
            out.nodes.push_back(best);
            emit_cell_pyramids(out, cn, apex);
            out.movable_fans.push_back({apex, cn});
            replaced[ci] = 1;
            ++n_replaced;
            --out.n_hex;
        }
        // Same treatment for the 2:1 closure and plain-skin fans: their apex is
        // the coarse cell centre and belongs to no other cell.
        for (const auto& fan : fan_spans) {
            poll_cancel(cancel_check);
            std::array<Eigen::Vector3d, 8> lattice{};
            Eigen::Vector3d ctr = Eigen::Vector3d::Zero();
            for (std::size_t t = 0; t < 8; ++t) {
                lattice[t] = out.nodes[fan.corners[t]];
                ctr += lattice[t];
            }
            ctr /= 8.0;
            bool touched = false;
            for (std::size_t ci = fan.first; ci < fan.end && !touched; ++ci) {
                const auto& cell = out.cells[ci];
                for (std::uint8_t m = 0; m + 1 < cell.n_nodes; ++m) {
                    if (predicted.count(cell.nodes[m]) != 0) {
                        touched = true;
                        break;
                    }
                }
            }
            if (!touched) {
                continue;
            }
            // The apex is the last node of every fan cell (emit_pyramid /
            // emit_tet), so only the bases come from the predicted sites.
            const auto worst_fan = [&](const Eigen::Vector3d& a) {
                double q = std::numeric_limits<double>::max();
                for (std::size_t ci = fan.first; ci < fan.end; ++ci) {
                    const auto& cell = out.cells[ci];
                    if (cell.kind == MixedCellKind::kPyramid5) {
                        q = std::min(q, validity::pyramid_split_shape_quality(
                                            site(cell.nodes[0]), site(cell.nodes[1]),
                                            site(cell.nodes[2]), site(cell.nodes[3]), a));
                    } else if (cell.kind == MixedCellKind::kTet4) {
                        q = std::min(q, validity::tet_shape_quality(site(cell.nodes[0]),
                                                                    site(cell.nodes[1]),
                                                                    site(cell.nodes[2]), a));
                    }
                }
                return q;
            };
            const double q_centroid = worst_fan(ctr);
            if (q_centroid >= shape_floor) {
                continue;
            }
            Eigen::Vector3d best;
            if (search_fan_apex(lattice, ctr, worst_fan, q_centroid, shape_floor, best)) {
                out.nodes[fan.apex] = best;
            }
        }
        if (n_replaced > 0) {
            std::vector<MixedCell> kept;
            kept.reserve(out.cells.size() - n_replaced);
            for (std::size_t ci = 0; ci < out.cells.size(); ++ci) {
                if (ci < n_emitted && replaced[ci]) {
                    continue;
                }
                kept.push_back(std::move(out.cells[ci]));
            }
            out.cells = std::move(kept);
        }
    }
}

void snap_mixed_boundary(MixedFillOutput& out, const std::vector<FanSpan>& fan_spans,
                         const geom::TriSurface& surface,
                         std::span<const geom::SharpEdge> features, double h_cell,
                         const std::function<void()>& cancel_check) {
    std::set<std::uint32_t> bnode_set;
    poll_cancel(cancel_check);
    for (const auto& q : out.boundary_quads) {
        bnode_set.insert(q.begin(), q.end());
    }
    std::vector<std::uint32_t> bnodes(bnode_set.begin(), bnode_set.end());
    const double vol_eps = 1e-14 * h_cell * h_cell * h_cell;
    // Sliver floor: vol_eps alone cannot reject a flattened cell. Nominal
    // lattice cells score ~0.8-1.0 here.
    const double shape_floor = validity::kCellShapeFloor;
    // Use bulk h as move/search budget (fine nodes still reproject within it).
    out.boundary_max_distance =
        snap_boundary_nodes(
            surface, out.nodes, bnodes, h_cell,
            [&](std::set<std::uint32_t>& offenders) {
                for (const auto& cell : out.cells) {
                    bool bad = false;
                    if (cell.kind == MixedCellKind::kTet4) {
                        bad = tet_bad(
                            {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]},
                            out.nodes, vol_eps, shape_floor);
                    } else if (cell.kind == MixedCellKind::kPyramid5) {
                        bad = pyramid_bad({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                           cell.nodes[3], cell.nodes[4]},
                                          out.nodes, vol_eps, shape_floor);
                    } else if (cell.kind == MixedCellKind::kPolyVem) {
                        std::vector<Eigen::Vector3d> coords;
                        coords.reserve(cell.poly_nodes.size());
                        for (const auto g : cell.poly_nodes) {
                            coords.push_back(out.nodes[g]);
                        }
                        bad = closed_poly_volume(coords, cell.poly_faces) <= vol_eps;
                    } else {
                        bad = hex_bad({cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                       cell.nodes[3], cell.nodes[4], cell.nodes[5],
                                       cell.nodes[6], cell.nodes[7]},
                                      out.nodes, shape_floor);
                    }
                    if (!bad) {
                        continue;
                    }
                    if (cell.kind == MixedCellKind::kPolyVem) {
                        offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                    } else {
                        for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                            offenders.insert(cell.nodes[m]);
                        }
                    }
                }
            },
            /*max_move_frac=*/1.25, /*passes=*/8, features)
            .max_residual;
    // The closest-point prediction above is intentionally conservative,
    // but the real multi-pass projection plus per-node rollback can finish
    // at a different site. Re-evaluate every private transition/skin fan
    // against the ACTUAL snapped bases and move only its interior apex.
    // This cannot change boundary conformity or the measured residual.
    if (!fan_spans.empty()) {
        const auto live_fan_cells = live_fan_cells_by_apex(fan_spans, out.cells);
        for (const auto& fan : fan_spans) {
            poll_cancel(cancel_check);
            const auto fit = live_fan_cells.find(fan.apex);
            if (fit == live_fan_cells.end() || fit->second.empty()) {
                continue;
            }
            reseat_fan_apex(out.nodes, fan.corners, fan.apex, fit->second, shape_floor);
        }
    }
}

} // namespace detail::mixed

std::size_t repair_mixed_fan_apices(MixedFillOutput& fill, double shape_floor) {
    if (fill.movable_fans.empty()) {
        return 0;
    }
    const auto live_fan_cells = live_fan_cells_by_apex(fill.movable_fans, fill.cells);

    std::size_t n_moved = 0;
    for (const auto& fan : fill.movable_fans) {
        const auto fit = live_fan_cells.find(fan.apex);
        if (fit == live_fan_cells.end() || fit->second.empty()) {
            continue;
        }
        if (reseat_fan_apex(fill.nodes, fan.corners, fan.apex, fit->second, shape_floor)) {
            ++n_moved;
        }
    }
    for (auto& cell : fill.cells) {
        detail::mixed::normalize_pyramid_diagonal(cell, fill.nodes);
    }
    return n_moved;
}

} // namespace polymesh::mesh
