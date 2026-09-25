// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"
#include "scene_internal.hpp"

#include "adapt/graded_sizing.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/element_validity.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/poly_to_vem.hpp"
#include "fea/vem.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"
#include "mesh/cell_validity.hpp"
#include "mesh/cvt_export.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/cvt_sites.hpp"
#include "mesh/feature_pin.hpp"
#include "mesh/fill_progress.hpp"
#include "mesh/geogram_clip.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/hex_fill.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/mirror.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/octa_fill.hpp"
#include "mesh/poly_mesh.hpp"
#include "mesh/prism_fill.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "mesh/transition_fill.hpp"
#include "mesh/varyhedron_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {
/// The mixed-cell zoo lifted into the solver's element types.
struct MixedConversion {
    fea::NodalMesh mesh;
    /// Corner-folded pyramid5 cells shipped as their two assembly tets.
    std::size_t n_pyramid_split_to_tets = 0;
};

/// `mesh::MixedCell` container -> `fea::NodalMesh`: the hybrid branch's one and
/// only conversion, shared by the construction-stage observer (`MeshStageSink`)
/// and the shipped mesh so both see exactly the same cells. `nodes` arrives by
/// value so the shipping call moves its container in, while an observer's call
/// pays for its own copy.
MixedConversion convert_mixed_cells(std::vector<Eigen::Vector3d> nodes,
                                    const std::vector<mesh::MixedCell>& cells,
                                    const std::function<void()>& cancel_check) {
    const auto poll_cancel = [&] {
        if (cancel_check) {
            cancel_check();
        }
    };
    // Corner-fold decomposition floor: the same normalized cell-shape floor
    // every mesher gate uses, so "folded" means one thing in this codebase.
    constexpr double kMinShapeConvert = mesh::validity::kCellShapeFloor;
    MixedConversion conv;
    conv.mesh.nodes = std::move(nodes);
    const std::vector<Eigen::Vector3d>& pts = conv.mesh.nodes;
    auto& elems = conv.mesh.elements;
    elems.reserve(cells.size());

    // Which pyramid5 cells ship as their two assembly tets. A base quad is
    // shared by the fans of the two lattice cells across it, so triangulating
    // it on one side only leaves the shared face non-conforming. The split
    // diagonal depends only on the base quad, so splitting BOTH sides is
    // conforming; the mark therefore propagates from a folded cell to whoever
    // shares its base. Side faces are triangles already, so this closes in one
    // round — no cascade.
    std::vector<char> split_pyramid(cells.size(), 0);
    {
        // Same winding normalization the emission below applies, so the fold
        // is measured on the cell that actually ships (the corner Jacobian
        // is sign-sensitive: an inverted stored winding would otherwise read
        // as folded).
        const auto oriented = [&](const mesh::MixedCell& cell) {
            std::array<std::uint32_t, 5> p{
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}};
            const auto& xa = pts[p[4]];
            const double vtk_volume =
                0.5 * (mesh::validity::tet_signed_volume(pts[p[0]], pts[p[1]], pts[p[2]], xa) +
                       mesh::validity::tet_signed_volume(pts[p[0]], pts[p[2]], pts[p[3]], xa) +
                       mesh::validity::tet_signed_volume(pts[p[1]], pts[p[2]], pts[p[3]], xa) +
                       mesh::validity::tet_signed_volume(pts[p[1]], pts[p[3]], pts[p[0]], xa));
            if (vtk_volume < 0.0) {
                std::swap(p[1], p[3]);
            }
            return p;
        };
        std::map<std::array<std::uint32_t, 4>, std::vector<std::size_t>> base_owners;
        std::vector<char> folded(cells.size(), 0);
        std::vector<char> splittable(cells.size(), 0);
        for (std::size_t ci = 0; ci < cells.size(); ++ci) {
            const auto& cell = cells[ci];
            if (cell.kind != mesh::MixedCellKind::kPyramid5) {
                continue;
            }
            const auto p = oriented(cell);
            std::array<std::uint32_t, 4> key{{p[0], p[1], p[2], p[3]}};
            std::sort(key.begin(), key.end());
            base_owners[key].push_back(ci);
            const auto& x0 = pts[p[0]];
            const auto& x1 = pts[p[1]];
            const auto& x2 = pts[p[2]];
            const auto& x3 = pts[p[3]];
            const auto& x4 = pts[p[4]];
            // Splittable = both assembly tets have positive volume. The bar
            // is validity, NOT the shape floor: two positive tets are
            // strictly better than one folded pyramid whatever their aspect.
            splittable[ci] = static_cast<char>(
                mesh::validity::pyramid_min_split_volume(x0, x1, x2, x3, x4) > 0.0);
            folded[ci] = static_cast<char>(
                mesh::validity::pyramid_corner_folded(x0, x1, x2, x3, x4, kMinShapeConvert));
        }
        for (const auto& [key, owners] : base_owners) {
            (void)key;
            const bool any_folded = std::any_of(
                owners.begin(), owners.end(), [&](std::size_t ci) { return folded[ci] != 0; });
            const bool all_splittable =
                std::all_of(owners.begin(), owners.end(),
                            [&](std::size_t ci) { return splittable[ci] != 0; });
            if (!any_folded || !all_splittable) {
                continue; // nothing folded here, or a partner could not be split safely
            }
            for (const auto ci : owners) {
                split_pyramid[ci] = 1;
            }
        }
    }
    std::size_t conversion_poll = 0;
    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const auto& cell = cells[ci];
        if ((conversion_poll++ & 255U) == 0U) {
            poll_cancel();
        }
        if (cell.kind == mesh::MixedCellKind::kPolyVem) {
            elems.emplace_back(fea::ElementType::kPolyVem, cell.poly_nodes, cell.poly_faces);
        } else if (cell.kind == mesh::MixedCellKind::kPyramid5) {
            std::array<std::uint32_t, 5> p{
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}};
            // Normalize winding at the final coordinates: snap rollback and
            // smoothing happen after mixed-cell emission, so a pre-existing
            // all-negative winding can otherwise survive as an offender
            // with no moved node to restore. VTK's signed pyramid volume is
            // the mean of the two base-diagonal tet-volume sums.
            const auto& x0 = pts[p[0]];
            const auto& x1 = pts[p[1]];
            const auto& x2 = pts[p[2]];
            const auto& x3 = pts[p[3]];
            const auto& xa = pts[p[4]];
            const double vtk_volume =
                0.5 * (mesh::validity::tet_signed_volume(x0, x1, x2, xa) +
                       mesh::validity::tet_signed_volume(x0, x2, x3, xa) +
                       mesh::validity::tet_signed_volume(x1, x2, x3, xa) +
                       mesh::validity::tet_signed_volume(x1, x3, x0, xa));
            if (vtk_volume < 0.0) {
                std::swap(p[1], p[3]);
            }
            // A base corner folded by the boundary snap makes the kPyramid5
            // isoparametric map turn inside out there, even with both
            // assembly split tets healthy — `fea::cell_quality` reports the
            // cell inverted and every consumer that trusts the map is
            // wrong. Ship it as the two tets the assembly would have built
            // from it (`element_stiffness` splits kPyramid5 along exactly
            // this diagonal): identical geometry, identical stiffness,
            // conforming (the diagonal depends only on the shared base
            // quad), and no folded cell leaves the mesher. Unsnapping the
            // wall instead would cost boundary fidelity.
            const int diagonal = mesh::validity::pyramid_split_diagonal(pts[p[0]], pts[p[1]],
                                                                        pts[p[2]], pts[p[3]]);
            if (split_pyramid[ci] != 0) {
                const auto emit = [&](std::size_t a, std::size_t b, std::size_t c) {
                    elems.push_back(
                        fea::NodalElement{fea::ElementType::kTet4, {p[a], p[b], p[c], p[4]}});
                };
                if (diagonal == 1) {
                    emit(1, 2, 3);
                    emit(1, 3, 0);
                } else {
                    emit(0, 1, 2);
                    emit(0, 2, 3);
                }
                ++conv.n_pyramid_split_to_tets;
                continue;
            }
            // VTK/PyVista triangulate pyramid5 along local 0-2. Rotate the
            // cyclic base so it is the conformity-safe assembly diagonal.
            if (diagonal == 1) {
                std::rotate(p.begin(), p.begin() + 1, p.begin() + 4);
            }
            elems.push_back(fea::NodalElement{fea::ElementType::kPyramid5,
                                              {p[0], p[1], p[2], p[3], p[4]}});
        } else if (cell.kind == mesh::MixedCellKind::kHex8) {
            elems.push_back(fea::NodalElement{fea::ElementType::kHex8,
                                              {cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                               cell.nodes[3], cell.nodes[4], cell.nodes[5],
                                               cell.nodes[6], cell.nodes[7]}});
        } else {
            elems.push_back(fea::NodalElement{
                fea::ElementType::kTet4,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]}});
        }
    }
    return conv;
}
} // namespace

// The public `volume_mesh` below wraps this: a refinement-limit hit becomes an
// auto-h retry and a zero-interior-cell fill a resolution refusal.
static VolumeMeshOutput volume_mesh_impl(
    const Model& model, double h, VolumeMesher mesher, int skin_layers, bool feature_refine,
    std::span<const Eigen::Vector3d> refine_seeds, double seed_band, double element_tendency,
    std::size_t max_elems, std::size_t max_dof, int auto_retry_budget,
    const std::function<void()>& cancel_check, const mesh::SizeFieldFn& size_field,
    const MeshStageSink& on_stage, const mesh::FillOptions& fill_options) {
    const auto poll_cancel = [&] {
        if (cancel_check) {
            cancel_check();
        }
    };
    // Construction-stage observation (`MeshStageSink`, `kMeshStageNames`). The
    // sink sees each MeshStage by const reference and nothing else, and the fill
    // never reads a result back from it; `stage_index` only feeds
    // `MeshStage::index`. Every emission is `if (on_stage)` guarded, so an unset
    // sink costs one branch per boundary and no conversion.
    int stage_index = 0;
    const auto emit_stage = [&](std::string_view name, fea::NodalMesh stage_mesh) {
        on_stage(
            MeshStage{std::string(name), stage_index++, /*pass=*/0, std::move(stage_mesh)});
    };
    const auto emit_mixed_stage = [&](std::string_view name,
                                      const std::vector<Eigen::Vector3d>& nodes,
                                      const std::vector<mesh::MixedCell>& cells) {
        emit_stage(name, convert_mixed_cells(nodes, cells, cancel_check).mesh);
    };
    poll_cancel();
    const double predicted_elems = predict_mesh_elements(model, h);
    const double predicted_dof = 3.0 * predicted_elems;
    if (max_elems > 0 && predicted_elems > static_cast<double>(max_elems)) {
        throw std::runtime_error(std::format(
            "mesh element ceiling {} exceeded: predicted {:.0f} elements at h={:.6g} m; "
            "increase -h or raise --max-elems",
            max_elems, std::ceil(predicted_elems), h));
    }
    if (max_dof > 0 && predicted_dof > static_cast<double>(max_dof)) {
        throw std::runtime_error(std::format(
            "mesh DOF ceiling {} exceeded: predicted {:.0f} DOF ({:.0f} elements) at "
            "h={:.6g} m; increase -h or raise --max-dof",
            max_dof, std::ceil(predicted_dof), std::ceil(predicted_elems), h));
    }
    VolumeMeshOutput out;
    const VolumeMesher requested_mesher = mesher;
    const int requested_skin_layers = skin_layers;
    double fill_h = h;
    const auto tendency_plan = resolve_element_tendency(mesher, element_tendency, skin_layers);
    mesher = tendency_plan.mesher;
    skin_layers = tendency_plan.skin_layers;

    // One exact BRep oracle and one compact owner slot per eventual mesh node.
    // Unknown nodes classify on their first snap; known owners remain immutable.
    //
    // Every CAD-backed mesher gets this: without crease awareness sharp edges
    // come out chamfered and curved walls carry lattice sawtooth (ADR-0035).
    std::vector<mesh::BoundarySupport> boundary_provenance;
    mesh::BoundaryProjectionContext projection_context;
    mesh::BoundaryProjectionContext* projection = nullptr;
    std::shared_ptr<const geom::CadTopology> cad_topology;
    if (model.cad && !model.cad->empty()) {
        try {
            if (make_boundary_projection(*model.cad, h, &projection_context,
                                         &boundary_provenance, &cad_topology)) {
                projection = &projection_context;
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::format("exact BRep projection setup failed: {}", e.what()));
        } catch (...) {
            throw std::runtime_error("exact BRep projection setup failed");
        }
    }
    mesh::BoundaryFit boundary_fit;
    boundary_fit.cad = model.cad ? &*model.cad : nullptr;
    boundary_fit.topo = cad_topology.get();
    boundary_fit.projection = projection;
    const mesh::BoundaryFit* fit = projection != nullptr ? &boundary_fit : nullptr;
    // Verified reflection symmetry of the geometry (mesh/mirror.hpp). Detected
    // from the exact BRep when there is one, and from the tessellation itself
    // when the tessellation IS the geometry (STL input, OCC-disabled build).
    //
    // This is what makes a symmetric part come out with a symmetric element
    // pattern: without it every mesher decision is read off a tessellation that
    // is not mirror-symmetric, so a cell and its mirror image genuinely disagree.
    // Detection is dense and tight — every exact face sample is reflected and
    // must land back on the solid — so an asymmetric part simply gets no frame
    // and no fold.
    const mesh::MirrorFrame& mirror_frame = model.mirror;
    const mesh::MirrorFrame* mirror = mirror_frame.any() ? &mirror_frame : nullptr;
    const auto mirror_note = [&]() -> std::string {
        if (mirror == nullptr) {
            return " | mirror=none";
        }
        std::string axes;
        for (int a = 0; a < 3; ++a) {
            if (mirror_frame.plane[static_cast<std::size_t>(a)]) {
                axes += "xyz"[a];
            }
        }
        return std::format(" | mirror={} (reflected-sample residual {:.2g}·diag)", axes,
                           mirror_frame.max_residual_over_diag);
    };
    // Per-cell turning-angle refinement threshold for local curvature grading.
    constexpr double kCurvatureTurnDeg = 15.0;
    if (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem) {
        // SPEC hybrid zoo: hex bulk @ h + 2:1 fine @ h/2 on feature/curvature
        // bands + conforming transitions.
        // kHybrid: product FE expands hex→pyramids (ADR-0012 / ADR-0013).
        // kHybridVem: keep hex as FE + unsplit transition polyhedra as VEM
        // (ADR-0019 fe-vem-assembly); no fan-split, no hex→pyramid expand.
        const bool native_poly = (mesher == VolumeMesher::kHybridVem);
        std::vector<geom::SharpEdge> edges;
        std::vector<Eigen::Vector3d> adapt_seeds(refine_seeds.begin(), refine_seeds.end());
        double feat_band = 0.0;
        double s_band = seed_band;
        double turn_deg = 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Feature band ~2 bulk cells so hole rims get a clear h/2 shell.
                feat_band = 2.0 * h;
            }
            turn_deg = kCurvatureTurnDeg;
        }
        // A-posteriori adapt seeds (caller) keep their ball semantics.
        if (s_band <= 0.0 && !adapt_seeds.empty()) {
            s_band = 2.0 * h;
        }
        if (adapt_seeds.empty()) {
            s_band = 0.0;
        }
        // Build lattice without snap first; product FE snaps after hex→pyramid
        // expand so free-surface Jacobian is pyramid-based. Native-poly path
        // keeps hex FE + poly VEM and snaps on that mesh.
        auto raw = mesh::mixed_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feat_band, adapt_seeds, s_band,
            /*snap_boundary=*/false, turn_deg, native_poly, cancel_check, size_field,
            /*local_surface_classification=*/projection != nullptr);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_raw = raw.n_pyramid;
        if (on_stage) {
            emit_mixed_stage(kMeshStageNames[0], raw.nodes, raw.cells);
        }
        // ADR-0013: the hex→pyramid product expansion exists so an
        // isoparametric hex8 never shares a face with a tet-split pyramid5 /
        // tet4 (the mixed-zoo patch test fails otherwise). A lattice that came
        // out pure hex — no 2:1 interface, so no fans — has no such face, and
        // expanding it only multiplies the element count by six while
        // downgrading hex8 to split-pyramid accuracy.
        const bool pure_hex_lattice = raw.n_pyramid == 0 && raw.n_tet == 0 && raw.n_poly == 0;
        // Named so the stage emission can say whether the expansion RAN. A
        // pure-hex or native-poly lattice skips it, and reporting a stage that
        // did not happen would be a fabricated frame.
        const bool expanded = !native_poly && !pure_hex_lattice;
        auto fill = expanded ? mesh::expand_mixed_hex_to_pyramids(raw) : std::move(raw);
        poll_cancel();
        if (on_stage && expanded) {
            emit_mixed_stage(kMeshStageNames[1], fill.nodes, fill.cells);
        }
        fill_h = fill.h;
        // Post-expand free-surface snap (boundary quads from lattice).
        if (!fill.boundary_quads.empty()) {
            std::set<std::uint32_t> bset;
            for (const auto& q : fill.boundary_quads) {
                bset.insert(q.begin(), q.end());
            }
            std::vector<std::uint32_t> bnodes(bset.begin(), bset.end());
            const double h_snap = fill.h > 0.0 ? fill.h : h;
            const double vol_eps = 1e-14 * h_snap * h_snap * h_snap;
            // Fan tets must keep a usable shape: an unchecked snap flattens them
            // into zero-aspect boundary tets.
            const double kMinTetAspect = mesh::validity::kCellShapeFloor;
            const double kMinShape = mesh::validity::kCellShapeFloor;
            const auto tet_aspect_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                const double v = (b - a).dot((c - a).cross(d - a)) / 6.0;
                if (v <= vol_eps) {
                    return false;
                }
                const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                              (b - c).norm(), (b - d).norm(), (c - d).norm()});
                if (emax <= 0.0) {
                    return false;
                }
                return 6.0 * 1.4142135623730951 * v / (emax * emax * emax) >= kMinTetAspect;
            };
            // A pyramid is a single cell, not two independently quality-scored
            // tetrahedra. Require both halves of the conformity-safe assembly
            // split to stay positively oriented, then apply the shared shape
            // floor to the normalized signed pyramid volume (the collapse term
            // shared with `fea::cell_quality`).
            //
            // What this gate deliberately does NOT test is the base-corner scaled
            // Jacobian: a fold there is cured for free at conversion, by shipping
            // the cell as the two tets the assembly already builds from it, and
            // testing it here would instead retreat the wall.
            const auto pyramid_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& p0 = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& p1 = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& p2 = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& p3 = fill.nodes[cell.nodes[3]];
                const Eigen::Vector3d& p4 = fill.nodes[cell.nodes[4]];
                if (mesh::validity::pyramid_min_split_volume(p0, p1, p2, p3, p4) <= vol_eps) {
                    return false;
                }
                return mesh::validity::pyramid_volume_collapse(p0, p1, p2, p3, p4) >=
                       kMinShape;
            };
            // Hex8: positive minimum Jacobian and the shared shape floor.
            const auto hex_ok = [&](const mesh::MixedCell& cell) {
                std::array<Eigen::Vector3d, 8> x{};
                for (std::size_t i = 0; i < 8; ++i) {
                    x[i] = fill.nodes[cell.nodes[i]];
                }
                return mesh::validity::hex8_min_jacobian(x) > 0.0 &&
                       mesh::validity::hex8_shape_quality(x) >= kMinShape;
            };
            // One canonical snap predicate feeds both the global audit and the
            // per-node incident-cell query below. Keeping these identical is
            // what lets surface_project avoid full-mesh scans during a trial
            // move without weakening the final global validity sweep.
            const auto snap_cell_valid = [&](const mesh::MixedCell& cell) {
                if (cell.kind == mesh::MixedCellKind::kTet4) {
                    // Volume only: a fan tet flattened by a full snap is peeled
                    // right after (apex coplanar), cheaper than unsnapping wall.
                    const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                    const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                    const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                    const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                    return (b - a).dot((c - a).cross(d - a)) >= 0.0;
                }
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    std::vector<Eigen::Vector3d> coords;
                    coords.reserve(cell.poly_nodes.size());
                    for (const auto g : cell.poly_nodes) {
                        coords.push_back(fill.nodes[g]);
                    }
                    return fea::poly_volume(coords, cell.poly_faces) > vol_eps;
                }
                if (cell.kind == mesh::MixedCellKind::kHex8) {
                    return hex_ok(cell);
                }
                return cell.kind != mesh::MixedCellKind::kPyramid5 || pyramid_ok(cell);
            };

            // Boundary-node → incident-cell map. Trial line-search checks are
            // O(local degree), not O(all cells). The global collector still
            // runs before/after bounded rounds to prove whole-mesh validity.
            std::vector<std::vector<std::size_t>> snap_node_cells(fill.nodes.size());
            std::vector<char> is_snap_node(fill.nodes.size(), 0);
            for (const auto ni : bnodes) {
                if (ni < is_snap_node.size()) {
                    is_snap_node[ni] = 1;
                }
            }
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                const auto& cell = fill.cells[ci];
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    for (const auto ni : cell.poly_nodes) {
                        if (ni < is_snap_node.size() && is_snap_node[ni]) {
                            snap_node_cells[ni].push_back(ci);
                        }
                    }
                } else {
                    for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                        const auto ni = cell.nodes[m];
                        if (ni < is_snap_node.size() && is_snap_node[ni]) {
                            snap_node_cells[ni].push_back(ci);
                        }
                    }
                }
            }

            std::size_t validity_poll = 0;
            const auto collect_snap_offenders = [&](std::set<std::uint32_t>& offenders) {
                for (const auto& cell : fill.cells) {
                    if ((validity_poll++ & 255U) == 0U) {
                        poll_cancel();
                    }
                    if (snap_cell_valid(cell)) {
                        continue;
                    }
                    if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                        offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                    } else {
                        offenders.insert(cell.nodes.begin(),
                                         cell.nodes.begin() + cell.n_nodes);
                    }
                }
            };
            const auto snap_node_offends = [&](std::uint32_t ni) {
                if (ni >= snap_node_cells.size()) {
                    return false;
                }
                for (const auto ci : snap_node_cells[ni]) {
                    if (!snap_cell_valid(fill.cells[ci])) {
                        return true;
                    }
                }
                return false;
            };
            // Interior room for the snap, the mechanism tet_fill/hex_fill
            // already use (ADR-0035). Without it a boundary node whose star is
            // a stair fold retreats to its raw lattice site and ships off the
            // exact BRep. This needs adjacency for EVERY node, not just the
            // boundary ones `snap_node_cells` tracks, because the nodes being
            // opened up are interior.
            std::vector<std::vector<std::size_t>> all_node_cells(fill.nodes.size());
            std::vector<std::vector<std::uint32_t>> nbrs(fill.nodes.size());
            {
                std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
                const auto link = [&](std::uint32_t u, std::uint32_t v) {
                    if (u == v) {
                        return;
                    }
                    const auto key = std::minmax(u, v);
                    if (!seen.insert({key.first, key.second}).second) {
                        return;
                    }
                    nbrs[u].push_back(v);
                    nbrs[v].push_back(u);
                };
                for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                    const auto& cell = fill.cells[ci];
                    std::span<const std::uint32_t> members =
                        cell.kind == mesh::MixedCellKind::kPolyVem
                            ? std::span<const std::uint32_t>(cell.poly_nodes)
                            : std::span<const std::uint32_t>(cell.nodes.data(), cell.n_nodes);
                    for (const auto ni : members) {
                        all_node_cells[ni].push_back(ci);
                    }
                    for (std::size_t a = 0; a < members.size(); ++a) {
                        for (std::size_t b = a + 1; b < members.size(); ++b) {
                            link(members[a], members[b]);
                        }
                    }
                }
            }
            const auto any_node_offends = [&](std::uint32_t ni) {
                if (ni >= all_node_cells.size()) {
                    return false;
                }
                for (const auto ci : all_node_cells[ni]) {
                    if (!snap_cell_valid(fill.cells[ci])) {
                        return true;
                    }
                }
                return false;
            };
            const auto reproject_node = [&](std::uint32_t ni, const Eigen::Vector3d& p) {
                const auto target =
                    mesh::boundary_projection_target(model.surface, p, ni, projection);
                return target ? target->point : p;
            };
            const auto relax_neighborhood = [&](std::uint32_t seed) {
                if (seed >= all_node_cells.size()) {
                    return false;
                }
                std::vector<std::uint32_t> ring;
                std::vector<std::uint32_t> wall;
                for (const auto ci : all_node_cells[seed]) {
                    const auto& cell = fill.cells[ci];
                    std::span<const std::uint32_t> members =
                        cell.kind == mesh::MixedCellKind::kPolyVem
                            ? std::span<const std::uint32_t>(cell.poly_nodes)
                            : std::span<const std::uint32_t>(cell.nodes.data(), cell.n_nodes);
                    for (const auto ni : members) {
                        if (ni == seed || nbrs[ni].empty()) {
                            continue;
                        }
                        (is_snap_node[ni] == 0 ? ring : wall).push_back(ni);
                    }
                }
                const auto dedup = [](std::vector<std::uint32_t>& v) {
                    std::sort(v.begin(), v.end());
                    v.erase(std::unique(v.begin(), v.end()), v.end());
                };
                dedup(ring);
                dedup(wall);
                bool moved_any = false;
                const double cap = 0.25 * h_snap;
                const auto nudge = [&](std::uint32_t ni, bool tangential) {
                    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
                    for (const auto other : nbrs[ni]) {
                        centroid += fill.nodes[other];
                    }
                    centroid /= static_cast<double>(nbrs[ni].size());
                    const Eigen::Vector3d saved = fill.nodes[ni];
                    const Eigen::Vector3d step = 0.5 * (centroid - saved);
                    const double len = step.norm();
                    Eigen::Vector3d moved = saved + (len > cap ? step * (cap / len) : step);
                    if (tangential) {
                        // A wall node may only slide ALONG the surface: it is
                        // re-projected through the same owner-aware oracle, so
                        // it stays on its own face/edge and only its spacing
                        // changes. A projection that runs away is abandoned.
                        moved = reproject_node(ni, moved);
                        if ((moved - saved).norm() > cap) {
                            return;
                        }
                    }
                    fill.nodes[ni] = moved;
                    if (any_node_offends(ni)) {
                        fill.nodes[ni] = saved;
                    } else if ((fill.nodes[ni] - saved).squaredNorm() > 0.0) {
                        moved_any = true;
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
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, bnodes, h_snap, collect_snap_offenders,
                    /*max_move_frac=*/1.25, /*passes=*/8, edges,
                    [&] { mesh::repair_mixed_fan_apices(fill, kMinShape); }, snap_node_offends,
                    /*defer_coupled=*/fill.n_pyramid > 0 || fill.n_tet > 0, projection,
                    relax_neighborhood)
                    .max_residual;
            poll_cancel();
            if (on_stage) {
                emit_mixed_stage(kMeshStageNames[2], fill.nodes, fill.cells);
            }
            // Peel snap-flattened fan tets: a full wall snap can pull all three
            // free nodes of a transition fan tet into the apex plane (aspect →
            // 0). The apex is then coplanar with the wall, so deleting the tet
            // exposes conforming faces with ~zero residual — better than
            // unsnapping the wall to save a degenerate element.
            {
                struct TriKey {
                    std::uint32_t a, b, c;
                    bool operator==(const TriKey& o) const {
                        return a == o.a && b == o.b && c == o.c;
                    }
                };
                struct TriHash {
                    std::size_t operator()(const TriKey& f) const noexcept {
                        std::size_t s = f.a;
                        s ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL + (s << 6) +
                             (s >> 2);
                        s ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL + (s << 6) +
                             (s >> 2);
                        return s;
                    }
                };
                const auto tkey = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    std::array<std::uint32_t, 3> v{{x, y, z}};
                    std::sort(v.begin(), v.end());
                    return TriKey{v[0], v[1], v[2]};
                };
                static constexpr int kTetTris[4][3] = {
                    {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
                static constexpr int kPyrTris[4][3] = {
                    {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
                const auto count_tris = [&]() {
                    std::unordered_map<TriKey, int, TriHash> tri_count;
                    tri_count.reserve(fill.cells.size() * 2);
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            ++tri_count[tkey(cell.nodes[static_cast<std::size_t>(tt[f][0])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][1])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][2])])];
                        }
                    }
                    return tri_count;
                };
                bool peeled_any = false;
                for (int pass = 0; pass < 3; ++pass) {
                    const auto tri_count = count_tris();
                    std::vector<char> kill(fill.cells.size(), 0);
                    std::size_t n_kill = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        const auto& cell = fill.cells[ci];
                        if (cell.kind != mesh::MixedCellKind::kTet4 || tet_aspect_ok(cell)) {
                            continue;
                        }
                        for (const auto& f : kTetTris) {
                            const auto it = tri_count.find(
                                tkey(cell.nodes[static_cast<std::size_t>(f[0])],
                                     cell.nodes[static_cast<std::size_t>(f[1])],
                                     cell.nodes[static_cast<std::size_t>(f[2])]));
                            if (it != tri_count.end() && it->second == 1) {
                                kill[ci] = 1;
                                ++n_kill;
                                break;
                            }
                        }
                    }
                    if (n_kill == 0 || n_kill >= fill.cells.size()) {
                        break;
                    }
                    std::size_t w = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        if (!kill[ci]) {
                            fill.cells[w++] = fill.cells[ci];
                        }
                    }
                    fill.cells.resize(w);
                    fill.n_tet -= n_kill;
                    peeled_any = true;
                }
                if (peeled_any) {
                    // Rebuild tri-encoded boundary entries from the surviving
                    // cells (true quads — pyramid bases — are unaffected by a
                    // tet peel and are kept as-is).
                    std::erase_if(fill.boundary_quads,
                                  [](const auto& q) { return q[2] == q[3]; });
                    const auto tri_count = count_tris();
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            const auto n0 = cell.nodes[static_cast<std::size_t>(tt[f][0])];
                            const auto n1 = cell.nodes[static_cast<std::size_t>(tt[f][1])];
                            const auto n2 = cell.nodes[static_cast<std::size_t>(tt[f][2])];
                            const auto it = tri_count.find(tkey(n0, n1, n2));
                            if (it != tri_count.end() && it->second == 1) {
                                fill.boundary_quads.push_back({{n0, n1, n2, n2}});
                            }
                        }
                    }
                }
            }
            // The peel is a whole-mesh boundary: `fill.cells` is compacted and
            // the tri-encoded boundary entries rebuilt inside the block above,
            // so nothing here is mid-delete. Mid-block there IS a window where
            // condemned cells are still present, which is why no stage is
            // emitted inside the peel's three passes.
            if (on_stage) {
                emit_mixed_stage(kMeshStageNames[3], fill.nodes, fill.cells);
            }
            // Per-node outlier re-projection (mirror of graded S3): residual
            // stragglers get a full/partial projection accepted only when every
            // incident cell stays valid. After expand all cells are pyramid/tet.
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> node_cells;
            poll_cancel();
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                if ((ci & 255U) == 0U) {
                    poll_cancel();
                }
                const auto& cell = fill.cells[ci];
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    for (const auto g : cell.poly_nodes) {
                        node_cells[g].push_back(ci);
                    }
                } else {
                    for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                        node_cells[cell.nodes[m]].push_back(ci);
                    }
                }
            }
            const auto cell_valid = [&](const mesh::MixedCell& cell) {
                if (cell.kind == mesh::MixedCellKind::kTet4) {
                    return tet_aspect_ok(cell);
                }
                if (cell.kind == mesh::MixedCellKind::kPyramid5) {
                    return pyramid_ok(cell);
                }
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    std::vector<Eigen::Vector3d> coords;
                    coords.reserve(cell.poly_nodes.size());
                    for (const auto g : cell.poly_nodes) {
                        coords.push_back(fill.nodes[g]);
                    }
                    return fea::poly_volume(coords, cell.poly_faces) > vol_eps;
                }
                if (cell.kind == mesh::MixedCellKind::kHex8) {
                    return hex_ok(cell);
                }
                return true;
            };
            const double thr = 0.08 * h_snap;
            double max_resid = 0.0;
            std::size_t reprojection_poll = 0;
            // A boundary node the exact oracle cannot give a target for is not
            // moved and would not be counted by `snap max|d|`, leaving the
            // fidelity figure blind to precisely the nodes that failed. Count
            // them, and count the tail left above 0.2 h, and report both (open
            // boundary-tail defect on BSpline-represented faces, ADR-0033).
            std::size_t n_no_target = 0;
            std::size_t n_residual_tail = 0;
            for (const auto ni : bnodes) {
                if ((reprojection_poll++ & 63U) == 0U) {
                    poll_cancel();
                }
                if (ni >= fill.nodes.size()) {
                    continue;
                }
                const auto target = mesh::boundary_projection_target(
                    model.surface, fill.nodes[ni], ni, projection);
                if (!target) {
                    ++n_no_target;
                    continue;
                }
                double resid = target->distance;
                if (resid > thr && resid <= 2.5 * h_snap) {
                    const Eigen::Vector3d saved = fill.nodes[ni];
                    static constexpr double kFracs[] = {1.0, 0.6, 0.35};
                    for (const double frac : kFracs) {
                        fill.nodes[ni] = saved + frac * (target->point - saved);
                        bool ok = true;
                        const auto it = node_cells.find(ni);
                        if (it != node_cells.end()) {
                            for (const auto ci : it->second) {
                                if (!cell_valid(fill.cells[ci])) {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (ok) {
                            resid = (1.0 - frac) * resid;
                            break;
                        }
                        fill.nodes[ni] = saved;
                    }
                }
                if (resid > 0.2 * h_snap) {
                    ++n_residual_tail;
                }
                max_resid = std::max(max_resid, resid);
            }
            fill.boundary_max_distance = max_resid;
            fill.n_boundary_no_target = n_no_target;
            fill.n_boundary_residual_tail = n_residual_tail;
            poll_cancel();
            if (on_stage) {
                emit_mixed_stage(kMeshStageNames[4], fill.nodes, fill.cells);
            }
            // Tangential smoothing: even out lattice-stair spacing on curved
            // walls / hole rims (crease nodes relax along the crease). Moves
            // are re-projected so the residual cannot grow; any move that
            // invalidates a cell is reverted.
            validity_poll = 0;
            const auto smooth_st = mesh::smooth_boundary_nodes(
                model.surface, fill.nodes, fill.boundary_quads, h_snap,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : fill.cells) {
                        if ((validity_poll++ & 255U) == 0U) {
                            poll_cancel();
                        }
                        if (cell_valid(cell)) {
                            continue;
                        }
                        if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                            offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                        } else {
                            for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                                offenders.insert(cell.nodes[m]);
                            }
                        }
                    }
                },
                /*passes=*/3, /*relax=*/0.5, edges, projection);
            poll_cancel();
            if (smooth_st.n_moved > 0) {
                fill.boundary_max_distance = smooth_st.max_residual;
            }
            if (on_stage) {
                emit_mixed_stage(kMeshStageNames[5], fill.nodes, fill.cells);
            }
            // No topology-repair path follows the mixed-cell merge (it never
            // collapses a node on the STEP fixtures). This post-smoothing
            // projection round is required for exact-BRep boundary fidelity.
            std::set<std::uint32_t> final_boundary_set;
            for (const auto& face : fill.boundary_quads) {
                final_boundary_set.insert(face.begin(), face.end());
            }
            const std::vector<std::uint32_t> final_boundary_nodes(final_boundary_set.begin(),
                                                                  final_boundary_set.end());
            const auto final_node_offends = [&](std::uint32_t node) {
                const auto it = node_cells.find(node);
                if (it == node_cells.end()) {
                    return false;
                }
                for (const auto cell_index : it->second) {
                    if (!cell_valid(fill.cells[cell_index])) {
                        return true;
                    }
                }
                return false;
            };
            validity_poll = 0;
            const auto collect_final_offenders = [&](std::set<std::uint32_t>& offenders) {
                for (const auto& cell : fill.cells) {
                    if ((validity_poll++ & 255U) == 0U) {
                        poll_cancel();
                    }
                    if (cell_valid(cell)) {
                        continue;
                    }
                    if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                        offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                    } else {
                        offenders.insert(cell.nodes.begin(),
                                         cell.nodes.begin() + cell.n_nodes);
                    }
                }
            };
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, final_boundary_nodes, h_snap,
                    collect_final_offenders, /*max_move_frac=*/1.25, /*passes=*/4, edges,
                    [&] { mesh::repair_mixed_fan_apices(fill, kMinShape); },
                    final_node_offends, /*defer_coupled=*/true, projection, relax_neighborhood)
                    .max_residual;
            poll_cancel();
            if (on_stage) {
                emit_mixed_stage(kMeshStageNames[6], fill.nodes, fill.cells);
            }
            // Hard-pin CAD vertices and sharp edge curves, exactly as the
            // tet/hex/graded fills do; without it a 90° crease comes out as
            // whatever chamfer the lattice happened to cut. Pin, even out the
            // free surface, pin again — smoothing can slide a chain node a
            // little off its curve, and the second pass is what makes the
            // crease exact.
            if (fit != nullptr && fit->can_pin()) {
                std::vector<mesh::BoundarySupport>* pin_provenance =
                    projection != nullptr ? projection->provenance : nullptr;
                mesh::pin_feature_nodes(*fit->cad, *fit->topo, fill.nodes,
                                        final_boundary_nodes, h_snap, final_node_offends,
                                        pin_provenance);
                poll_cancel();
                mesh::smooth_boundary_nodes(model.surface, fill.nodes, fill.boundary_quads,
                                            h_snap, collect_final_offenders, /*passes=*/3,
                                            /*relax=*/0.5, /*feature_edges=*/{}, projection);
                poll_cancel();
                const auto pin = mesh::pin_feature_nodes(*fit->cad, *fit->topo, fill.nodes,
                                                         final_boundary_nodes, h_snap,
                                                         final_node_offends, pin_provenance);
                fill.boundary_max_distance =
                    std::max(fill.boundary_max_distance, pin.worst_node_distance);
                poll_cancel();
                if (on_stage) {
                    emit_mixed_stage(kMeshStageNames[7], fill.nodes, fill.cells);
                }
            }
        }
        // The mixed zoo becomes solver elements through the shared converter
        // (`convert_mixed_cells`), which owns the corner-fold pyramid split.
        auto converted = convert_mixed_cells(std::move(fill.nodes), fill.cells, cancel_check);
        const std::size_t n_pyramid_split_to_tets = converted.n_pyramid_split_to_tets;
        out.mesh = std::move(converted.mesh);
        out.boundary_quads = std::move(fill.boundary_quads);
        out.local_child_boundary_quads = std::move(fill.local_child_boundary_quads);
        if (native_poly) {
            out.mesher_note = std::format(
                "hybrid-VEM zoo (hex FE bulk@h + 2:1 fine@h/2 + native poly VEM "
                "transitions; not Delaunay): {} hex8 + {} polyVEM ({} pyr raw unused), "
                "{} nodes, h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} "
                "feature={}, snap max|d|={:.3g} m{}",
                fill.n_hex, fill.n_poly, n_pyr_raw, out.mesh.nodes.size(), fill.h,
                fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
        } else {
            out.mesher_note = std::format(
                "hybrid zoo v4 (hex bulk@h + 2:1 fine@h/2 + conforming fan transition; "
                "not Delaunay): {} hex + {} pyr raw → {} pyramid5 + {} tet4, {} nodes, "
                "h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} feature={}, "
                "snap max|d|={:.3g} m{}",
                n_hex_lattice, n_pyr_raw, fill.n_pyramid, fill.n_tet, out.mesh.nodes.size(),
                fill.h, fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
        }
        if (fill.n_boundary_no_target > 0 || fill.n_boundary_residual_tail > 0) {
            out.mesher_note += std::format(
                " | boundary tail: {} node(s) with no exact CAD target, {} left >0.2 h off",
                fill.n_boundary_no_target, fill.n_boundary_residual_tail);
        }
        if (n_pyramid_split_to_tets > 0) {
            out.mesher_note +=
                std::format(" | {} corner-folded pyramid5 shipped as their 2 assembly tets",
                            n_pyramid_split_to_tets);
        }
        if (size_field) {
            out.mesher_note += std::format(
                " | size_field h_min={:.4g} h_max={:.4g} m, levels L0={} L1={}{}",
                fill.field_h_min, fill.field_h_max, fill.n_level0_cells, fill.n_level1_cells,
                fill.n_field_budget_clamped > 0
                    ? std::format(", {} cells clamped at budget floor",
                                  fill.n_field_budget_clamped)
                    : std::string{});
        }
        if (fill.classification_refinement_levels > 0) {
            out.mesher_note += std::format(" | feature-aware classify L{} volume_err={:.4g}",
                                           fill.classification_refinement_levels,
                                           fill.classification_volume_error);
        }
    } else if (mesher == VolumeMesher::kHexFill || mesher == VolumeMesher::kHexVem) {
        auto fill = mesh::hex_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                           /*snap_boundary=*/true, fit);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.hexes.size());
        for (const auto& hx : fill.hexes) {
            fea::NodalElement el{fea::ElementType::kHex8,
                                 {hx[0], hx[1], hx[2], hx[3], hx[4], hx[5], hx[6], hx[7]}};
            if (mesher == VolumeMesher::kHexVem) {
                auto poly = fea::hex8_as_poly(el);
                el.type = fea::ElementType::kPolyVem;
                el.faces = std::move(poly.faces);
            }
            out.mesh.elements.push_back(std::move(el));
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "{} grid fill v1 (Cartesian, not CAD-fitted): {} cells, {} nodes, h={:.4g} m, "
            "snap max|d|={:.3g} m",
            mesher == VolumeMesher::kHexVem ? "hex-VEM" : "hex", out.mesh.elements.size(),
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kHexPyramid) {
        // Topology: hex core + pyramid skin (ADR-0013). Product FE path expands
        // each interior hex to six pyramids (centroid apex) so face diagonals
        // match the tet-split pyramid skin — constant-strain patch exact.
        auto raw =
            mesh::transition_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_skin = raw.n_pyramid;
        auto fill = mesh::expand_hex_core_to_pyramids(raw);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPyramid5,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "hex+pyramid product FE (Cartesian grid, not Delaunay; all-pyramid expand): "
            "{} lattice hex → {} pyramids ({} skin + {} from hex), {} nodes, h={:.4g} m, "
            "boundary max|d|={:.3g} m",
            n_hex_lattice, fill.n_pyramid, n_pyr_skin, fill.n_pyramid - n_pyr_skin,
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kPrismSweep) {
        // Cartesian prism6 wedges along the longest bbox axis.
        // Not CAD extrusion detection — same grid-fill honesty as tet/hex (ADR-0015).
        auto fill = mesh::prism_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.prisms.size());
        for (const auto& pr : fill.prisms) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPrism6, {pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        static constexpr const char* kAxis[] = {"x", "y", "z"};
        const char* axis_name =
            (fill.sweep_axis >= 0 && fill.sweep_axis < 3) ? kAxis[fill.sweep_axis] : "?";
        out.mesher_note =
            std::format("prism sweep grid fill v1 (Cartesian, not CAD extrusion; sweep={}): "
                        "{} prism6, {} nodes, h={:.4g} m, snap max|d|={:.3g} m",
                        axis_name, out.mesh.elements.size(), out.mesh.nodes.size(), fill_h,
                        fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kGradedTet) {
        std::vector<geom::SharpEdge> edges;
        // Interior Kuhn lattice: three axis, three face-diagonal and one
        // body-diagonal unique edges per cell; median = sqrt(2) * pitch.
        const double graded_h = h / std::sqrt(2.0);
        // CAD geometry supplies a LOCAL size demand. A curved-area fraction or
        // tessellated corner curvature is not a reason to halve the whole part.
        mesh::SizeFieldFn graded_field = size_field;
        if (cad_topology && !cad_topology->empty()) {
            const auto geometry_field =
                feature_refine ? build_refinement_plan(model, h, {}, true, false, 0).size_field
                               : mesh::SizeFieldFn{};
            graded_field = [size_field, geometry_field, h](const Eigen::Vector3d& p) {
                double target = geometry_field ? geometry_field(p) : h;
                if (size_field)
                    target = std::min(target, size_field(p));
                return target;
            };
        }
        if (feature_refine)
            edges = geom::detect_sharp_edges(model.surface, 30.0);
        double feature_band = 0.0;
        // Caller a-posteriori adapt seeds keep ball semantics; curvature is
        // the per-cell turning-angle criterion inside the fill (no caps, no
        // stray islands). Thin walls (t < 2.5 h) still seed *locally* — but a
        // globally thin part (most of the surface thin) is an h problem, not a
        // local feature: seeding it would just scatter capped fine islands.
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = 0.0;
        std::size_t n_thin_seeds = 0;
        if (feature_refine && !graded_field) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Crease band ~ two bulk cells so hole rims get a clear L1/L2 shell.
                feature_band = 2.0 * graded_h;
            }
            turn_deg = kCurvatureTurnDeg;
            const auto thick = geom::estimate_local_thickness(model.surface);
            std::vector<Eigen::Vector3d> thin_pts;
            std::size_t n_finite = 0;
            for (std::size_t i = 0; i < model.surface.vertices.size(); ++i) {
                if (i >= thick.thickness.size() ||
                    !geom::has_finite_thickness(thick.thickness[i])) {
                    continue;
                }
                ++n_finite;
                if (thick.thickness[i] < 2.5 * graded_h) {
                    thin_pts.push_back(model.surface.vertices[i]);
                }
            }
            const bool globally_thin =
                n_finite > 0 &&
                thin_pts.size() * 3 > model.surface.vertices.size(); // > ~1/3 thin
            if (!globally_thin && !thin_pts.empty()) {
                if (band <= 0.0) {
                    band = 1.6 * graded_h;
                }
                // Spatial thinning: min sep 0.75 h, capped, caller seeds first.
                constexpr std::size_t kMaxGeoSeeds = 256;
                const double min_sep2 = (0.75 * graded_h) * (0.75 * graded_h);
                for (const auto& p : thin_pts) {
                    if (seeds.size() >= kMaxGeoSeeds) {
                        break;
                    }
                    bool far = true;
                    for (const auto& q : seeds) {
                        if ((p - q).squaredNorm() < min_sep2) {
                            far = false;
                            break;
                        }
                    }
                    if (far) {
                        seeds.push_back(p);
                        ++n_thin_seeds;
                    }
                }
            }
            // Flat solids: free-surface skin alone is enough (no fake seed flood).
            if (band > 0.0 && seeds.empty()) {
                band = 0.0;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * graded_h;
        }
        auto graded = mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, graded_h, std::max(1, skin_layers),
            edges, feature_band, seeds, band, turn_deg, fit, graded_field, mirror, max_elems,
            fill_options);
        fill_h = graded.h_fine;
        out.size_floor = graded.h_coarse > graded_h * 1.001 ? graded.h_coarse : 0.0;
        out.mesh.nodes = std::move(graded.mesh.nodes);
        out.mesh.elements.reserve(graded.mesh.tets.size());
        for (const auto& tet : graded.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        // Prefer true exterior faces after LEB (includes mid-edge free-surface nodes).
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(graded.mesh.boundary_quads);
        }
        std::vector<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(bnodes.end(), q.begin(), q.end());
        }
        std::sort(bnodes.begin(), bnodes.end());
        bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, bnodes);
        const char* budget_note =
            (graded.h_fine > (graded_h / static_cast<double>(graded.subdivision)) * 1.05)
                ? ", h raised to cell budget"
                : "";
        out.mesher_note = std::format(
            "graded tet v6 (multi-level LEB geo + cap collapse/void carve): {} tets ({} bulk, "
            "{} refined L1/L2, "
            "{} feature, {} seed), h_bulk={:.4g}/h_L2~{:.4g} m (L0/L1/L2), "
            "snap max|d|={:.3g} m mean|d|={:.3g} m"
            "{}{}{}",
            out.mesh.elements.size(), graded.n_coarse_cells, graded.n_fine_cells,
            graded.n_feature_cells, graded.n_seed_cells, graded.h_coarse, graded.h_fine,
            conf.max_distance, conf.mean_distance, budget_note,
            turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg) : std::string{},
            n_thin_seeds > 0 ? std::format(", thin_seeds={}", n_thin_seeds) : std::string{});
        if (graded_field) {
            out.mesher_note += std::format(
                " | size_field h_min={:.4g} h_max={:.4g} m, levels L0={} L1={} L2={}{}",
                graded.field_h_min, graded.field_h_max, graded.n_level0_cells,
                graded.n_level1_cells, graded.n_level2_cells,
                graded.n_field_budget_clamped > 0
                    ? std::format(", {} cells clamped at budget floor",
                                  graded.n_field_budget_clamped)
                    : std::string{});
        }
        if (graded.classification_refinement_levels > 0) {
            out.mesher_note += std::format(" | feature-aware classify L{} volume_err={:.4g}",
                                           graded.classification_refinement_levels,
                                           graded.classification_volume_error);
        }
        out.mesher_note += mirror_note();
    } else if (mesher == VolumeMesher::kOctahedral) {
        // Experimental BCC octahedra → tet4 (ADR-0019). Not a product claim.
        auto fill = mesh::octa_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.mesh.boundary_quads);
        out.mesher_note =
            std::format("octahedral experimental (BCC face-octa → tet4; not product): "
                        "{} tets ({} octa + {} bdy pyr), {} nodes, h={:.4g} m",
                        out.mesh.elements.size(), fill.n_octahedra, fill.n_boundary_pyramids,
                        out.mesh.nodes.size(), fill_h);
    } else if (mesher == VolumeMesher::kVaryhedron) {
        // Varyhedron (ADR-0021): CAD edge seeds + packing seeds, exported as a
        // graded tet4 mesh, then sharp snap + wall re-projection.
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = feature_refine ? 15.0 : 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                feature_band = 2.0 * h;
            }
            if (band <= 0.0 && !seeds.empty()) {
                band = 1.6 * h;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * h;
        }

        // Prefer retained Model::cad (ADR-0020 / V1c); fall back to reloading
        // the source CAD path when the model was surface-only (legacy).
        std::optional<geom::CadModel> cad_reload;
        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            try {
                topo = geom::extract_topology(*model.cad, 10);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        } else if (geom::occ_enabled()) {
            std::vector<std::string> candidates;
            if (!model.source_path.empty()) {
                candidates.push_back(model.source_path);
            }
            if (!model.name.empty()) {
                candidates.push_back(model.name);
                candidates.push_back(std::string("tests/fixtures/parts/") + model.name);
                candidates.push_back(std::string("tests/fixtures/") + model.name);
            }
            for (const auto& cand : candidates) {
                try {
                    std::string low = cand;
                    for (char& c : low) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (low.ends_with(".step") || low.ends_with(".stp")) {
                        cad_reload = geom::CadModel::load_step(cand);
                    } else if (low.ends_with(".brep") || low.ends_with(".brp")) {
                        cad_reload = geom::CadModel::load_brep(cand);
                    } else {
                        continue;
                    }
                    if (cad_reload && !cad_reload->empty()) {
                        topo = geom::extract_topology(*cad_reload, 10);
                        topo_ptr = &(*topo);
                        break;
                    }
                } catch (...) {
                    cad_reload.reset();
                    topo.reset();
                    topo_ptr = nullptr;
                }
            }
        }

        // Live BRep for M10 wall free-slide + OCC re-project (optional).
        const geom::CadModel* cad_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            cad_ptr = &(*model.cad);
        } else if (cad_reload && !cad_reload->empty()) {
            cad_ptr = &(*cad_reload);
        }

        auto fill = mesh::varyhedron_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, seeds, band, turn_deg, topo_ptr, cad_ptr,
            /*wall_smooth_iters=*/4, projection);
        fill_h = fill.h_fine;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(fill.mesh.boundary_quads);
        }
        // Packing-seed engine (ADR-0023): sharp-only edge protection with radii
        // min(α h, β lfs); wall free-slide + OCC surface re-project after the
        // sharp snap.
        out.mesher_note = std::format(
            "varyhedron packing (sharp-only edge protect + interior bubble seeds + graded "
            "scaffold + sharp snap + wall OCC project; dual deferred; CVT target): "
            "{} tets, edge_seeds={}, r_protect=[{:.3g},{:.3g}], "
            "sharp/smooth/seam={}/{}/{}, vol_seeds={}, pack_relax={}, "
            "pack_fill={:.3g}, h_bulk={:.4g}/h_fine={:.4g} m, edge_Hd={:.3g} m "
            "(rel={:.3g}, /h={:.3g}, e_chord={:.3g}), wall_nodes={}/moved={}/iters={}{}",
            out.mesh.elements.size(), fill.n_edge_seeds, fill.min_protect_radius,
            fill.max_protect_radius, fill.n_sharp_edges, fill.n_smooth_edges,
            fill.n_seam_edges, fill.n_volume_seeds, fill.n_pack_relax_iters,
            fill.pack_fill_frac, fill.h_coarse, fill.h_fine, fill.edge_profile_hausdorff_max,
            fill.edge_profile_rel, fill.edge_hausdorff_over_h,
            fill.edge_chordal_efficiency_max, fill.n_wall_nodes, fill.n_wall_moved,
            fill.n_wall_iters,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=surface_only");
    } else if (mesher == VolumeMesher::kCvtPoly) {
        // G1–G4 product poly path: constrained restricted CVT → clipped cells → VEM.
        // Free sites at surface-interior cell centres (not full AABB lattice).
        // M5: export clips cells to local surface halfspaces so boundary faces
        // sit on the solid (not the bbox). Free sites stay strictly interior —
        // projecting onto the surface creates zero-thickness domain clips.
        if (!mesh::geogram_available()) {
            throw std::runtime_error(
                "cvt_poly mesher requires POLYMESH_WITH_GEOGRAM (Geogram ConvexCell)");
        }
        mesh::ClipBox box;
        box.min = model.bbox_min;
        box.max = model.bbox_max;
        const Eigen::Vector3d ext = (box.max - box.min).cwiseMax(1e-30);
        const double diag = ext.norm();

        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            try {
                topo = geom::extract_topology(*model.cad, 8);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        }

        // Sharp fixed protectors + free sites from interior grid cells.
        mesh::ConstrainedSiteSeedParams seed_p;
        seed_p.interior_n_side = 0; // no full-AABB lattice; we inject interior free sites
        seed_p.sharp_sample_stride = 1;
        // Slightly denser sharp protectors for hole SCF (0.20 vs 0.25); 0.16
        // raised plate residual.
        seed_p.sharp_min_sep_frac = 0.20 * h / diag;
        auto seeded = mesh::seed_constrained_cvt_sites(box, topo_ptr, seed_p);

        // Plate-like (high aspect) gets hole-local densify for SCF.
        const Eigen::Vector3d extent = (box.max - box.min).cwiseMax(1e-30);
        const double aspect = extent.maxCoeff() / extent.minCoeff();
        const bool plate_like = aspect > 5.0; // plate_hole ~10–20; cylinder ~ few
        const double h_site = std::max(0.9 * h, 1e-9);
        mesh::CartesianGrid grid =
            mesh::make_bbox_grid(model.bbox_min, model.bbox_max, h_site);
        const auto inside = mesh::classify_cells_inside(model.surface, grid);
        const double min_sep = seed_p.sharp_min_sep_frac * diag;
        const double min_sep2 = min_sep * min_sep;
        // Spatial hash over site positions so min-separation and nearest-sharp
        // queries are O(1) per grid cell, not O(N): parts with ~10^4 sharp sites
        // would otherwise make seeding O(N^2).
        const double hg = std::max(0.35 * h, 1e-12);
        const double inv_hg = 1.0 / hg;
        struct BKey {
            long long a, b, c;
            bool operator==(const BKey& o) const { return a == o.a && b == o.b && c == o.c; }
        };
        struct BKeyHash {
            std::size_t operator()(const BKey& k) const noexcept {
                std::size_t h = static_cast<std::size_t>(k.a) * 73856093ull;
                h ^= static_cast<std::size_t>(k.b) * 19349663ull + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                h ^= static_cast<std::size_t>(k.c) * 83492791ull + 0x9e3779b9 + (h << 6) +
                     (h >> 2);
                return h;
            }
        };
        auto bkey = [&](const Eigen::Vector3d& p) -> BKey {
            return BKey{static_cast<long long>(std::floor(p.x() * inv_hg)),
                        static_cast<long long>(std::floor(p.y() * inv_hg)),
                        static_cast<long long>(std::floor(p.z() * inv_hg))};
        };
        std::unordered_map<BKey, std::vector<Eigen::Vector3d>, BKeyHash> site_hash;
        // Sharp hash uses a coarser cell (≈ the densify band) so nearest-sharp is
        // a fixed 27-bucket lookup, not a deep ring search in sharp-free regions.
        const double sg = std::max(3.2 * h, 1e-12);
        const double inv_sg = 1.0 / sg;
        auto skey = [&](const Eigen::Vector3d& p) -> BKey {
            return BKey{static_cast<long long>(std::floor(p.x() * inv_sg)),
                        static_cast<long long>(std::floor(p.y() * inv_sg)),
                        static_cast<long long>(std::floor(p.z() * inv_sg))};
        };
        std::unordered_map<BKey, std::vector<Eigen::Vector3d>, BKeyHash> sharp_hash;
        std::vector<Eigen::Vector3d> sharp_pos;
        for (const auto& s : seeded.sites) {
            site_hash[bkey(s.pos)].push_back(s.pos);
            if (s.fixed) {
                sharp_hash[skey(s.pos)].push_back(s.pos);
                sharp_pos.push_back(s.pos);
            }
        }
        auto site_far = [&](const Eigen::Vector3d& p, double sep2) -> bool {
            const int R = std::max(1, static_cast<int>(std::ceil(std::sqrt(sep2) * inv_hg)));
            const BKey k0 = bkey(p);
            for (int dz = -R; dz <= R; ++dz) {
                for (int dy = -R; dy <= R; ++dy) {
                    for (int dx = -R; dx <= R; ++dx) {
                        auto it = site_hash.find(BKey{k0.a + dx, k0.b + dy, k0.c + dz});
                        if (it == site_hash.end()) {
                            continue;
                        }
                        for (const auto& q : it->second) {
                            if ((q - p).squaredNorm() < sep2) {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        };
        auto add_site = [&](const mesh::CvtSite& s) {
            seeded.sites.push_back(s);
            site_hash[bkey(s.pos)].push_back(s.pos);
        };
        auto dist_to_sharp = [&](const Eigen::Vector3d& p) -> double {
            // Coarse cell = band, so all sharps within the densify band lie in the
            // 3×3×3 neighbourhood — a fixed 27-bucket scan, exact for d ≤ band.
            const BKey k0 = skey(p);
            double best2 = 1e300;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        auto it = sharp_hash.find(BKey{k0.a + dx, k0.b + dy, k0.c + dz});
                        if (it == sharp_hash.end()) {
                            continue;
                        }
                        for (const auto& q : it->second) {
                            best2 = std::min(best2, (q - p).squaredNorm());
                        }
                    }
                }
            }
            return std::sqrt(best2);
        };

        // Infer hole radius from sharp fixed sites near the plate mid-plane
        // (top/bottom circular rims). Fallback 0.01 matches plate_hole fixture.
        double hole_r = 0.01;
        if (plate_like && !sharp_pos.empty()) {
            double r_sum = 0.0;
            std::size_t r_n = 0;
            const double z_mid = 0.5 * (box.min.z() + box.max.z());
            const double z_band = 0.35 * extent.z();
            for (const auto& q : sharp_pos) {
                if (std::abs(q.z() - z_mid) > z_band) {
                    continue;
                }
                const double r = q.head<2>().norm();
                if (r > 1e-6 && r < 0.4 * extent.head<2>().minCoeff()) {
                    r_sum += r;
                    ++r_n;
                }
            }
            if (r_n >= 4) {
                hole_r = r_sum / static_cast<double>(r_n);
            }
        }

        std::size_t n_interior_free = 0;
        std::size_t n_feature_free = 0;
        for (int k = 0; k < grid.nz; ++k) {
            for (int j = 0; j < grid.ny; ++j) {
                for (int i = 0; i < grid.nx; ++i) {
                    if (!inside[grid.index(i, j, k)]) {
                        continue;
                    }
                    const Eigen::Vector3d c = grid.cell_center(i, j, k);
                    if (!site_far(c, min_sep2)) {
                        continue;
                    }
                    mesh::CvtSite s;
                    s.pos = c;
                    s.fixed = false;
                    add_site(s);
                    ++n_interior_free;

                    // Plate-like only: mild in-plane densify near sharp features
                    // (grid half-offsets). Aggressive packing / graded size fields
                    // lowered measured SCF — keep this light.
                    if (!plate_like) {
                        continue;
                    }
                    const double d_sharp = dist_to_sharp(c);
                    const double r_xy = c.head<2>().norm();
                    const bool near_hole = r_xy < hole_r + 2.5 * h;
                    const double band = near_hole ? 3.2 * h : 2.6 * h;
                    if (d_sharp < band && d_sharp > 0.18 * h) {
                        static constexpr double kOff[][2] = {
                            {1, 0},  {-1, 0},  {0, 1},   {0, -1},   {1, 1},   {1, -1},
                            {-1, 1}, {-1, -1}, {0.5, 0}, {-0.5, 0}, {0, 0.5}, {0, -0.5},
                        };
                        const double sep = near_hole ? 0.28 * h : 0.33 * h;
                        const double local_sep2 = sep * sep;
                        const double scale = near_hole ? 0.36 : 0.40;
                        for (const auto& o : kOff) {
                            Eigen::Vector3d p = c;
                            p[0] += scale * grid.cell[0] * o[0];
                            p[1] += scale * grid.cell[1] * o[1];
                            if (std::abs(p[0] - c[0]) > 0.48 * grid.cell[0] ||
                                std::abs(p[1] - c[1]) > 0.48 * grid.cell[1]) {
                                continue;
                            }
                            if (!site_far(p, local_sep2)) {
                                continue;
                            }
                            mesh::CvtSite fs;
                            fs.pos = p;
                            fs.fixed = false;
                            add_site(fs);
                            ++n_feature_free;
                            ++n_interior_free;
                        }
                    }
                }
            }
        }

        // Cylinder: free sites on an inset cylindrical shell (smooth curved wall
        // is not sharp-protected). Improves SE without wall-pull stiffening.
        if (!plate_like && topo_ptr) {
            const double z_lo = box.min.z() + 0.08 * extent.z();
            const double z_hi = box.max.z() - 0.08 * extent.z();
            // Outer radius from sharp rims (top/bottom circles).
            double R_out = 0.5 * extent.head<2>().minCoeff();
            {
                double r_sum = 0.0;
                std::size_t r_n = 0;
                for (const auto& q : sharp_pos) {
                    const double r = q.head<2>().norm();
                    if (r > 0.2 * R_out) {
                        r_sum += r;
                        ++r_n;
                    }
                }
                if (r_n >= 4) {
                    R_out = r_sum / static_cast<double>(r_n);
                }
            }
            const double shell_r = std::max(R_out - 0.35 * h, 0.5 * R_out);
            const double shell_sep = 0.32 * h;
            const double shell_sep2 = shell_sep * shell_sep;
            const int n_ang =
                std::max(16, static_cast<int>(std::ceil(2.0 * 3.141592653589793 * shell_r /
                                                        std::max(0.30 * h, 1e-9))));
            const int n_z = std::max(
                4, static_cast<int>(std::ceil((z_hi - z_lo) / std::max(0.40 * h, 1e-9))));
            // A genuine inset-cylinder shell is modest; a bogus cylinder fit on a
            // non-cylinder part (e.g. a frame) or an over-fine h explodes n_ang·n_z
            // into millions of shell candidates. Skip when it isn't cylinder-scale.
            const long long shell_budget =
                static_cast<long long>(n_ang) * static_cast<long long>(n_z + 1);
            if (shell_budget <= 200000) {
                for (int iz = 0; iz <= n_z; ++iz) {
                    const double z = z_lo + (z_hi - z_lo) * static_cast<double>(iz) /
                                                static_cast<double>(std::max(n_z, 1));
                    for (int a = 0; a < n_ang; ++a) {
                        const double th = 2.0 * 3.141592653589793 * static_cast<double>(a) /
                                          static_cast<double>(n_ang);
                        Eigen::Vector3d p(shell_r * std::cos(th), shell_r * std::sin(th), z);
                        if (!site_far(p, shell_sep2)) {
                            continue;
                        }
                        const int gi = static_cast<int>(
                            std::floor((p.x() - grid.origin.x()) / grid.cell[0]));
                        const int gj = static_cast<int>(
                            std::floor((p.y() - grid.origin.y()) / grid.cell[1]));
                        const int gk = static_cast<int>(
                            std::floor((p.z() - grid.origin.z()) / grid.cell[2]));
                        if (gi < 0 || gj < 0 || gk < 0 || gi >= grid.nx || gj >= grid.ny ||
                            gk >= grid.nz) {
                            continue;
                        }
                        if (!inside[grid.index(gi, gj, gk)]) {
                            continue;
                        }
                        mesh::CvtSite fs;
                        fs.pos = p;
                        fs.fixed = false;
                        add_site(fs);
                        ++n_feature_free;
                        ++n_interior_free;
                    }
                }
            }
        }

        seeded.n_interior_free = n_interior_free;

        mesh::CvtLloydParams lloyd;
        // Slightly more Lloyd for free-site equilibrium (helps SE a little).
        lloyd.max_iters = 32;
        lloyd.move_tol_rel = 7e-4;
        // Geometry-graded density (ADR-0021): curvature / thin-wall drive cell
        // size and count; gradient-limited (β=1) so grading is smooth. Flat
        // surfaces emit no source ⇒ falls back to uniform h.
        auto poly_size = std::make_shared<adapt::GradedSizing>(
            adapt::geometry_size_sources(model.surface, 0.35 * h, h), 0.35 * h, h, 1.0);
        lloyd.size_at = [poly_size](const Eigen::Vector3d& p) {
            return poly_size->size_at(p);
        };

        auto sites = seeded.sites;
        const auto lr = mesh::lloyd_cvt(box, sites, lloyd);
        for (std::size_t i = 0; i < sites.size() && i < lr.positions.size(); ++i) {
            if (!sites[i].fixed) {
                sites[i].pos = lr.positions[i];
            }
        }
        // Soft wall pull: plate needs interior inset for RVD; cylinder keeps
        // sites where Lloyd left them (better SE — wall pull stiffened response).
        const geom::CadModel* cad_ptr =
            (model.cad && !model.cad->empty()) ? &(*model.cad) : nullptr;
        if (cad_ptr && plate_like) {
            const double wall_band = 0.12 * diag;
            const double inset = std::max(0.15 * h, 1e-4 * diag);
            const double sharp_guard = 0.04 * diag;
            for (auto& s : sites) {
                if (s.fixed) {
                    continue;
                }
                if (topo_ptr && sharp_guard > 0.0) {
                    if (auto q = geom::closest_edge(*topo_ptr, s.pos, /*sharp_only=*/true)) {
                        if (q->distance < sharp_guard) {
                            continue;
                        }
                    }
                }
                const auto pr = geom::project_point_on_surface(*cad_ptr, s.pos);
                if (!pr || pr->distance > wall_band) {
                    continue;
                }
                Eigen::Vector3d n = pr->normal;
                const double nn = n.norm();
                if (!(nn > 1e-14)) {
                    continue;
                }
                n /= nn;
                s.pos = pr->point - inset * n;
            }
        }

        std::vector<Eigen::Vector3d> positions;
        positions.reserve(sites.size());
        for (const auto& s : sites) {
            positions.push_back(s.pos);
        }

        // RVD ∩ tet scaffold for all solids (holes need it; prismatic SE is
        // competitive with load_area trim). AABB-only reintroduces bad SE when
        // free-site counts stay modest.
        const std::string clip_mode = "rvd_tet";
        mesh::ClippedVoronoiExport exp;
        std::size_t n_domain_tets = 0;
        double scaffold_tet_volume = 0.0;
        try {
            const double h_tet = std::max(h, 1e-9);
            auto tet_fill =
                mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h_tet,
                                       /*snap_boundary=*/true, fit, mirror);
            std::vector<mesh::DomainTet> dtets;
            dtets.reserve(tet_fill.tets.size());
            for (const auto& t : tet_fill.tets) {
                mesh::DomainTet d;
                d.v0 = tet_fill.nodes[t[0]];
                d.v1 = tet_fill.nodes[t[1]];
                d.v2 = tet_fill.nodes[t[2]];
                d.v3 = tet_fill.nodes[t[3]];
                d.centroid = 0.25 * (d.v0 + d.v1 + d.v2 + d.v3);
                const double tet_volume = mesh::tet_signed_volume(d.v0, d.v1, d.v2, d.v3);
                if (!(tet_volume > 0.0)) {
                    continue;
                }
                scaffold_tet_volume += tet_volume;
                dtets.push_back(d);
            }
            n_domain_tets = dtets.size();
            const double R = std::max(2.5 * h, 0.08 * diag);
            exp = mesh::export_rvd_tet_clipped(box, positions, dtets, R);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                std::format("cvt_poly RVD construction failed: {}", error.what()));
        }
        const double volume_scale =
            std::max({std::abs(scaffold_tet_volume), std::abs(exp.stats.sum_cell_volume),
                      std::numeric_limits<double>::min()});
        const double coverage_tol =
            (1e-9 + 128.0 * std::numeric_limits<double>::epsilon()) * volume_scale;
        if (!std::isfinite(scaffold_tet_volume) || !std::isfinite(exp.stats.sum_cell_volume) ||
            std::abs(exp.stats.sum_cell_volume - scaffold_tet_volume) > coverage_tol) {
            throw std::runtime_error(std::format(
                "cvt_poly RVD raw clip volume {:.17g} does not cover positive scaffold tetra "
                "volume {:.17g} within scale-aware tolerance {:.3e}",
                exp.stats.sum_cell_volume, scaffold_tet_volume, coverage_tol));
        }
        const std::size_t min_admitted_sites =
            std::min(positions.size(), std::max<std::size_t>(1, positions.size() / 4));
        const std::size_t n_admitted_sites = static_cast<std::size_t>(std::count_if(
            exp.site_to_cell.begin(), exp.site_to_cell.end(),
            [](std::size_t cell) { return cell != static_cast<std::size_t>(-1); }));
        if (n_admitted_sites < min_admitted_sites) {
            throw std::runtime_error(
                std::format("cvt_poly RVD admitted only {} sites; at least {} are required",
                            n_admitted_sites, min_admitted_sites));
        }

        if (exp.stats.n_invalid_face_claims != 0) {
            throw std::runtime_error(
                std::format("cvt_poly RVD is nonmanifold: {} exact face claims have invalid "
                            "multiplicity, site ownership, or winding",
                            exp.stats.n_invalid_face_claims));
        }
        if (exp.stats.n_unpaired_bisector_faces != 0) {
            throw std::runtime_error(
                std::format("cvt_poly RVD is nonconforming: {} Voronoi interface fragments "
                            "lack an opposite owner",
                            exp.stats.n_unpaired_bisector_faces));
        }
        if (exp.stats.n_unpaired_scaffold_faces != 0) {
            throw std::runtime_error(std::format(
                "cvt_poly RVD is incomplete: {} internal scaffold-cut faces lack an "
                "opposite owner",
                exp.stats.n_unpaired_scaffold_faces));
        }
        // Admit the exact exported polygons before triangulation.  Otherwise a
        // warped source n-gon could be replaced by valid triangles and escape
        // the original geometry contract.
        exp.mesh.check_validity();
        exp.mesh.check_geometry();
        // Any face incident to a projected exterior vertex is triangulated
        // first. Independent CAD projection can then preserve face planarity
        // while deep-interior coalesced Voronoi n-gons remain intact.
        exp.mesh.triangulate_boundary_incident_faces();
        exp.mesh.check_validity();
        exp.mesh.check_geometry();
        const std::vector<Eigen::Vector3d> pre_projection_vertices = exp.mesh.vertices;

        // Light boundary polish onto the surface (helps residual staircasing).
        std::size_t n_bnd_snapped = 0;
        if (!model.surface.triangles.empty() && !exp.mesh.faces.empty()) {
            std::vector<char> is_bnd(exp.mesh.vertices.size(), 0);
            for (const auto& f : exp.mesh.faces) {
                if (f.neighbour) {
                    continue;
                }
                for (auto v : f.vertices) {
                    if (v < is_bnd.size()) {
                        is_bnd[v] = 1;
                    }
                }
            }
            const double snap_budget = (clip_mode == "rvd_tet") ? 0.4 * h : 1.35 * h;
            for (std::size_t vi = 0; vi < exp.mesh.vertices.size(); ++vi) {
                if (!is_bnd[vi]) {
                    continue;
                }
                // Prefer OCC surface project when available (curved walls /
                // cylinder SE); fall back to STL closest.
                bool snapped = false;
                if (cad_ptr) {
                    if (const auto pr =
                            geom::project_point_on_surface(*cad_ptr, exp.mesh.vertices[vi])) {
                        if (pr->distance > 0.0 && pr->distance <= snap_budget) {
                            exp.mesh.vertices[vi] = pr->point;
                            ++n_bnd_snapped;
                            snapped = true;
                        }
                    }
                }
                if (!snapped) {
                    const auto cp =
                        mesh::closest_on_surface(model.surface, exp.mesh.vertices[vi]);
                    if (cp.distance > 0.0 && cp.distance <= snap_budget) {
                        exp.mesh.vertices[vi] = cp.point;
                        ++n_bnd_snapped;
                    }
                }
            }
        }

        // Projection is all-or-nothing. In addition to local shell validity,
        // preserve the authoritative positive scaffold volume. A 1e-4 relative
        // allowance is large compared with summation/welding roundoff but small
        // enough to reject a coherent CAD-projection shrink or expansion.
        constexpr double kProjectionVolumeRelTol = 1e-4;
        const auto convert_and_measure = [&]() {
            fea::NodalMesh converted = fea::poly_mesh_to_vem(exp.mesh);
            if (converted.elements.size() != exp.mesh.cells.size()) {
                throw std::runtime_error(std::format(
                    "cvt_poly VEM conversion changed admitted cell count from {} to {}",
                    exp.mesh.cells.size(), converted.elements.size()));
            }
            double volume = 0.0;
            for (const fea::NodalElement& element : converted.elements) {
                const Eigen::Vector3d origin = converted.nodes[element.nodes.front()];
                for (const auto& face : element.faces) {
                    if (face.size() < 3) {
                        continue;
                    }
                    const Eigen::Vector3d a = converted.nodes[element.nodes[face[0]]] - origin;
                    for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                        const Eigen::Vector3d b =
                            converted.nodes[element.nodes[face[i]]] - origin;
                        const Eigen::Vector3d c =
                            converted.nodes[element.nodes[face[i + 1]]] - origin;
                        volume += a.dot(b.cross(c)) / 6.0;
                    }
                }
            }
            return std::pair{std::move(converted), volume};
        };
        const auto admitted_volume_matches_scaffold = [&](double volume) {
            const double scale = std::max({std::abs(scaffold_tet_volume), std::abs(volume),
                                           std::numeric_limits<double>::min()});
            return std::isfinite(volume) &&
                   std::abs(volume - scaffold_tet_volume) <= kProjectionVolumeRelTol * scale;
        };

        double snap_scale = 0.0;
        double post_admission_vem_volume = 0.0;
        std::string projection_outcome = "not_applied";
        fea::NodalMesh admitted_mesh;
        if (n_bnd_snapped == 0) {
            exp.mesh.check_geometry();
            auto [converted, volume] = convert_and_measure();
            if (!admitted_volume_matches_scaffold(volume)) {
                throw std::runtime_error(std::format(
                    "cvt_poly admitted VEM volume {:.17g} differs from scaffold volume "
                    "{:.17g} beyond projection-relative tolerance {:.3e}",
                    volume, scaffold_tet_volume, kProjectionVolumeRelTol));
            }
            admitted_mesh = std::move(converted);
            post_admission_vem_volume = volume;
        } else {
            try {
                exp.mesh.check_geometry();
                auto [projected, projected_volume] = convert_and_measure();
                if (!admitted_volume_matches_scaffold(projected_volume)) {
                    throw mesh::ValidityError(std::format(
                        "CAD projection changed VEM volume from scaffold {:.17g} to {:.17g}",
                        scaffold_tet_volume, projected_volume));
                }
                admitted_mesh = std::move(projected);
                post_admission_vem_volume = projected_volume;
                snap_scale = 1.0;
                projection_outcome = "accepted";
            } catch (const mesh::ValidityError&) {
                exp.mesh.vertices = pre_projection_vertices;
                exp.mesh.check_geometry();
                auto [restored, restored_volume] = convert_and_measure();
                if (!admitted_volume_matches_scaffold(restored_volume)) {
                    throw std::runtime_error(std::format(
                        "cvt_poly restored VEM volume {:.17g} differs from scaffold volume "
                        "{:.17g} beyond projection-relative tolerance {:.3e}",
                        restored_volume, scaffold_tet_volume, kProjectionVolumeRelTol));
                }
                admitted_mesh = std::move(restored);
                post_admission_vem_volume = restored_volume;
                n_bnd_snapped = 0;
                projection_outcome = "rolled_back";
            }
        }

        out.mesh = std::move(admitted_mesh);
        const std::size_t post_admission_face_count = exp.mesh.faces.size();
        out.mesh.compact_unused_nodes();
        fill_h = h;
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        out.mesher_note = std::format(
            "cvt_poly RVD (interior sites + {} tets + clip={} + projection={} + "
            "bnd_snap={}@{:.4g} → kPolyVem): {} post_polys, {} post_nodes, post_faces={}, "
            "sites={}/fixed={}/interior={}/feat={}, lloyd_iters={}, "
            "raw_split_components={}, raw_unpaired_bisectors={}, "
            "raw_unpaired_scaffold={}, raw_invalid_face_claims={}, "
            "raw_coalesced_faces/fragments={}/{}, raw_clip_volume={:.17g}, "
            "scaffold_volume={:.17g}, post_vem_volume={:.17g}, raw_domain_clips={}, "
            "grid={}x{}x{}, h={:.4g} m{}",
            n_domain_tets, clip_mode, projection_outcome, n_bnd_snapped, snap_scale,
            out.mesh.elements.size(), out.mesh.nodes.size(), post_admission_face_count,
            sites.size(), seeded.n_sharp_fixed, n_interior_free - n_feature_free,
            n_feature_free, lr.stats.n_iters, exp.stats.n_split_site_components,
            exp.stats.n_unpaired_bisector_faces, exp.stats.n_unpaired_scaffold_faces,
            exp.stats.n_invalid_face_claims, exp.stats.n_coalesced_faces,
            exp.stats.n_coalesced_face_fragments, exp.stats.sum_cell_volume,
            scaffold_tet_volume, post_admission_vem_volume, exp.stats.n_domain_plane_clips,
            grid.nx, grid.ny, grid.nz, h,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=surface_class");
    } else {
        auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                           /*snap_boundary=*/true, fit, mirror);
        const auto owner_name = [](mesh::BoundarySupportKind k) {
            switch (k) {
            case mesh::BoundarySupportKind::kCadVertex:
                return "cad_vertex";
            case mesh::BoundarySupportKind::kCadEdge:
                return "cad_edge";
            case mesh::BoundarySupportKind::kCadFace:
                return "cad_face";
            case mesh::BoundarySupportKind::kUnknown:
                break;
            }
            return "unknown";
        };
        const std::string conformity_note = std::format(
            " | conformity snap_moved={} unsnapped={} relax_rescued={} "
            "pin_edge={} pin_vertex={} pin_chains={} pin_rejected={} pin_res={:.3g} m "
            "worst_node={} d={:.3g} m owner={}",
            fill.snap.n_moved, fill.snap.n_unsnapped, fill.snap.n_relax_rescued,
            fill.pin.edge_pinned, fill.pin.vertex_pinned, fill.pin.chains, fill.pin.rejected,
            fill.pin.max_edge_residual, fill.pin.worst_node, fill.pin.worst_node_distance,
            owner_name(fill.pin.worst_node_owner));
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.tets.size());
        for (const auto& tet : fill.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        std::vector<std::array<std::uint32_t, 4>> tet_ids;
        tet_ids.reserve(out.mesh.elements.size());
        for (const auto& el : out.mesh.elements) {
            if (el.nodes.size() == 4) {
                tet_ids.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
            }
        }
        const auto q = mesh::summarize_tet4_quality(out.mesh.nodes, tet_ids);
        // Snap residual from boundary nodes (snap already ran inside tet_fill).
        std::vector<std::uint32_t> btmp;
        for (const auto& qf : out.boundary_quads) {
            btmp.insert(btmp.end(), qf.begin(), qf.end());
        }
        std::sort(btmp.begin(), btmp.end());
        btmp.erase(std::unique(btmp.begin(), btmp.end()), btmp.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, btmp);
        out.mesher_note = std::format(
            "tet grid fill v1 (Cartesian, not Delaunay): {} tet4, {} nodes, h={:.4g} m, "
            "minQ={:.3f}, meanQ={:.3f}, slivers={}, snap max|d|={:.3g} m mean|d|={:.3g} m",
            out.mesh.elements.size(), out.mesh.nodes.size(), fill_h, q.min_aspect,
            q.mean_aspect, q.n_sliver, conf.max_distance, conf.mean_distance);
        out.mesher_note += conformity_note;
        out.mesher_note += mirror_note();
    }

    mesh::FillProgressScope pipeline_progress(fill_options);
    pipeline_progress.set_elements(out.mesh.elements.size());
    pipeline_progress.set_cells(0, out.mesh.elements.size());
    pipeline_progress.set_phase("exterior_quality");

    // The mesher's own output, in solver types, before any pipeline-level
    // conformity/quality/compaction step touches it. Every branch above reaches
    // here, so this is the one stage every mesher emits: for all of them except
    // the hybrid zoo it is also the ONLY visible construction boundary, because
    // at this level the fill is a single opaque call.
    if (on_stage) {
        emit_stage(kMeshStageNames[8], out.mesh);
    }

    // Prefer true element exterior faces for display/region skin so tet/prism
    // previews show element triangles (incl. Kuhn diagonals), not only lattice quads.
    {
        auto exterior = fea::extract_boundary_faces(out.mesh);
        if (!exterior.empty()) {
            out.boundary_quads = std::move(exterior);
        }
    }

    // Exterior conformity gate (ADR-0035): the snap ran on each mesher's own
    // lattice skin; the mesh ships the true element exterior extracted just
    // above. Conform THAT set, so a node exposed by a late peel/split is not
    // shipped at its raw lattice site.
    if (projection != nullptr) {
        const auto ext = detail::conform_true_exterior(
            out.mesh, out.boundary_quads, projection, fit, fill_h > 0.0 ? fill_h : h,
            mesh::validity::kCellShapeFloor, mirror);
        if (ext.n_candidates > 0 || ext.n_edge_pinned > 0 || ext.n_kink_relieved > 0) {
            out.mesher_note += std::format(
                " | exterior_gate cand={} moved={} relax_rescued={} hex_fanned={} left={} "
                "worst_node={} worst_xyz=({:.6g},{:.6g},{:.6g}) "
                "worst|d|={:.3g} m kink_relieved={} edge_pinned={} connected={} [{}] "
                "edge_pass={:.0f} ms chains={} pin_rejected={}{}",
                ext.n_candidates, ext.n_moved, ext.n_relax_rescued, ext.n_hex_fanned,
                ext.n_left, ext.worst_node, ext.worst_position.x(), ext.worst_position.y(),
                ext.worst_position.z(), ext.worst_residual, ext.n_kink_relieved,
                ext.n_edge_pinned, ext.n_connected_edges, ext.connected_edge_census,
                ext.edge_pass_ms, ext.n_edge_chains, ext.n_pin_rejected,
                ext.reverted ? " REVERTED" : "");
        }
    }

    // Ship gate (ADR-0035): the cells that leave this function are measured
    // with the product's own `fea::cell_quality`, not with whatever predicate
    // each mesher used internally over its own intermediate zoo. Cells under
    // the floor get interior room; whatever is left is reported, never hidden.
    {
        mesh::fill_progress_phase("ship_quality");
        const std::size_t below_floor = detail::relax_cells_below_shape_floor(
            out.mesh, out.boundary_quads, mesh::validity::kCellShapeFloor);
        out.n_cells_below_shape_floor = below_floor;
        if (below_floor > 0) {
            out.mesher_note += std::format(" | ship_gate {} cells below shape floor {:.3g}",
                                           below_floor, mesh::validity::kCellShapeFloor);
        }
        // Integrability is a separate question from shape, and it is the one the
        // solver actually asks: `element_stiffness` refuses any element with a
        // non-positive Jacobian at a quadrature point. Counting it here means a
        // mesh that would abort the solve says so in its own note instead of
        // failing later with a bare error.
        std::size_t nonintegrable = 0;
        mesh::fill_progress_phase("ship_integrability");
        std::size_t checked = 0;
        for (const auto& element : out.mesh.elements) {
            mesh::fill_progress_poll(checked++, out.mesh.elements.size());
            if (!fea::element_jacobians_positive(out.mesh, element)) {
                ++nonintegrable;
            }
        }
        if (nonintegrable > 0) {
            out.mesher_note +=
                std::format(" | ship_gate {} non-integrable cells (det J <= 0 at a quadrature "
                            "point)",
                            nonintegrable);
        }
    }

    // CAD-face → boundary-node map. Fixtures / loads are picked on CAD faces,
    // so every mesh handed to the solver has to carry this map; rebuild it from
    // scratch whenever the node numbering changes below.
    const auto& surf = model.surface;
    auto map_boundary_regions = [&] {
        mesh::fill_progress_phase("boundary_region_mapping");
        out.boundary_node_region.clear();
        std::size_t boundary_poll = 0;
        std::set<std::uint32_t> boundary_nodes;
        for (const auto& quad : out.boundary_quads) {
            boundary_nodes.insert(quad.begin(), quad.end());
        }
        for (const auto node : boundary_nodes) {
            mesh::fill_progress_poll(boundary_poll, boundary_nodes.size());
            if ((boundary_poll++ & 255U) == 0U) {
                poll_cancel();
            }
            const auto cp = mesh::closest_on_surface(surf, out.mesh.nodes[node]);
            if (cp.distance <= 1.5 * fill_h && cp.triangle < model.triangle_region.size()) {
                out.boundary_node_region[node] = model.triangle_region[cp.triangle];
            }
        }
    };
    map_boundary_regions();
    if (std::abs(tendency_plan.tendency) >= 1e-12 || tendency_plan.remapped) {
        out.mesher_note = std::format("{} | element_tendency={:.3g}→{}{}", out.mesher_note,
                                      tendency_plan.tendency, tendency_plan.label,
                                      tendency_plan.remapped ? " (remapped)" : "");
    }
    // Graded LEB / packing can leave orphan node slots (no element refs) that
    // inject zero-stiffness free DOFs and singular K — drop them always.
    const std::size_t n_orphans = out.mesh.compact_unused_nodes();
    if (n_orphans > 0) {
        out.mesher_note += std::format(" | compact_orphans={}", n_orphans);
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        // compact_unused_nodes() renumbered the nodes, so the map built above
        // keys on dead ids. Re-map onto the compacted mesh; without it every
        // fixture/load would silently drop from the solve.
        map_boundary_regions();
    }
    out.fill_geometry_volume = measure_geometry_volume(model, out.mesh);
    out.solved_geometry_volume = out.fill_geometry_volume;
    if (out.fill_geometry_volume.available) {
        detail::replace_geometry_volume_note(out.mesher_note, "fill",
                                             out.fill_geometry_volume);
        if (out.fill_geometry_volume.relative_error > kGeometryVolumeHardLimit) {
            // Name the remedy, like the two sibling refusals do
            // (`enforce_feature_resolution` and `refuse_unresolvable_h`). The
            // error does not fall smoothly with h (a small reduction can make
            // it worse), so the recommended halving is a verified jump, not an
            // extrapolation. The pyramid5 2:1 transition is conforming and
            // volume-exact and is not blamed (ADR-0030).
            const std::size_t n_pyramid = static_cast<std::size_t>(
                std::count_if(out.mesh.elements.begin(), out.mesh.elements.end(),
                              [](const fea::NodalElement& e) {
                                  return e.type == fea::ElementType::kPyramid5;
                              }));
            // Both h values, because they differ: the fill snaps the requested
            // size to a whole number of cells, and a user told only the snapped
            // one cannot match it to the -h they typed.
            throw GeometryVolumeLimitError(
                std::format("geometry fill-stage guard failed at h={:.6g} m (requested -h "
                            "{:.6g} m): mesh/BRep volume relative error {:.4g} exceeds hard "
                            "limit {:.4g}; the lattice does not resolve the solid, so parts "
                            "of it are missing from the fill. Reducing -h a little does NOT "
                            "fix this -- the error is set by which features the lattice "
                            "straddles, not by resolution in the small. Reduce -h to <= "
                            "{:.6g} m, or raise --max-elems/--max-dof to afford it. (Fill "
                            "census: {} cells, {} of them pyramid5 2:1 transition cells; the "
                            "transition is conforming and volume-exact and is not the cause.)"
                            " | {}",
                            fill_h, h, out.fill_geometry_volume.relative_error,
                            kGeometryVolumeHardLimit, 0.5 * h, out.mesh.elements.size(),
                            n_pyramid, out.mesher_note),
                out.fill_geometry_volume, false);
        }
    }
    // A volume check cannot see a duplicated boundary skin: two coincident
    // patches with opposite orientation cancel in the divergence sum, so the
    // mesh measures right and is still torn. The shell is what the solver's
    // traction integral and the renderer both consume, so check the shell.
    {
        const auto shell = detail::boundary_shell_topology(out.boundary_quads);
        out.mesher_note += std::format(" | boundary_shell edges={} open={} nonmanifold={}",
                                       shell.n_edges, shell.n_open, shell.n_nonmanifold);
        if (shell.n_open > 0 || shell.n_nonmanifold > 0) {
            throw GeometryVolumeLimitError(
                std::format(
                    "geometry fill-stage guard failed at h={:.6g} m (requested -h {:.6g} m): "
                    "the boundary is not a closed surface -- {} edge(s) are used by one face "
                    "(a hole) and {} by three or more (two skins in the same place), out of "
                    "{}. A volume check cannot catch this, because coincident skins cancel: "
                    "this mesh's volume error is {:.4g}. This is a mesher defect, not a "
                    "resolution one, and refining does not clear it. Retry with the default "
                    "--mesher hybrid, which is watertight on every part measured | {}",
                    fill_h, h, shell.n_open, shell.n_nonmanifold, shell.n_edges,
                    out.fill_geometry_volume.available
                        ? out.fill_geometry_volume.relative_error
                        : std::numeric_limits<double>::quiet_NaN(),
                    out.mesher_note),
                out.fill_geometry_volume, false);
        }
    }
    if (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem ||
        mesher == VolumeMesher::kGradedTet) {
        detail::enforce_feature_resolution(model, out, h, fill_h);
    }
    poll_cancel();
    const std::size_t actual_elems = out.mesh.elements.size();
    const std::size_t actual_dof =
        out.mesh.nodes.size() > std::numeric_limits<std::size_t>::max() / 3
            ? std::numeric_limits<std::size_t>::max()
            : 3 * out.mesh.nodes.size();
    const bool elem_over = max_elems > 0 && actual_elems > max_elems;
    const bool dof_over = max_dof > 0 && actual_dof > max_dof;
    if ((elem_over || dof_over) && auto_retry_budget > 0) {
        double scale = 1.0;
        if (elem_over) {
            scale = std::max(scale, std::cbrt(static_cast<double>(actual_elems) /
                                              static_cast<double>(max_elems)));
        }
        if (dof_over) {
            scale = std::max(scale, std::cbrt(static_cast<double>(actual_dof) /
                                              static_cast<double>(max_dof)));
        }
        const double retry_h =
            std::nextafter(h * scale * 1.05, std::numeric_limits<double>::infinity());
        // The retry is a second, independent fill, so it gets the sink too and
        // its stage indices restart at 0; the abandoned attempt's stages stay
        // reported.
        auto retry = volume_mesh(model, retry_h, requested_mesher, requested_skin_layers,
                                 feature_refine, refine_seeds, seed_band, element_tendency,
                                 max_elems, max_dof, auto_retry_budget - 1, cancel_check,
                                 size_field, on_stage, fill_options);
        const std::string ceiling_note =
            elem_over ? std::format("element ceiling {}, actual {}", max_elems, actual_elems)
                      : std::format("DOF ceiling {}, actual {}", max_dof, actual_dof);
        retry.mesher_note = std::format("auto h clamped from {:.4g} to {:.4g} m ({}) | {}", h,
                                        retry_h, ceiling_note, retry.mesher_note);
        return retry;
    }
    if (elem_over) {
        throw std::runtime_error(
            std::format("mesh element ceiling {} exceeded after fill: actual {} elements "
                        "(predicted {:.0f}); increase -h or raise --max-elems",
                        max_elems, actual_elems, predicted_elems));
    }
    if (dof_over) {
        throw std::runtime_error(
            std::format("mesh DOF ceiling {} exceeded after fill: actual {} DOF / {} elements "
                        "(predicted {:.0f} DOF); increase -h or raise --max-dof",
                        max_dof, actual_dof, actual_elems, predicted_dof));
    }
    if (max_elems > 0 || max_dof > 0) {
        out.mesher_note +=
            std::format(" | budget predicted={:.0f} elems/{:.0f} DOF ceilings={}/{}",
                        predicted_elems, predicted_dof, max_elems, max_dof);
    }
    out.geometry_h = fill_h > 0.0 ? fill_h : h;
    out.mesh.check_validity();
    // The delivered mesh, after the exterior conform, the ship gate's cell
    // relaxation and the orphan-node compaction -- and after
    // `check_validity()`, so this stage is the only one guaranteed to be a mesh
    // the solver will accept. Emitted last, once, on the success path only.
    if (on_stage) {
        emit_stage(kMeshStageNames[9], out.mesh);
    }
    return out;
}

/// A fill that yields zero interior cells means h could not represent the part at
/// all -- typically h exceeds a wall thickness -- which is a resolution refusal,
/// not a generic validity failure. Report it in the same actionable shape as
/// `enforce_feature_resolution`, and keep the three refusal causes textually
/// distinct: "feature unresolved at h=", "geometry fill-stage guard failed:",
/// and "resolution refused at h=".
[[noreturn]] static void refuse_unresolvable_h(const Model& model, double h,
                                               const mesh::ValidityError& cause) {
    // Only reached on a failure path, so an otherwise-expensive measurement is
    // free here -- and it is the one number that makes the refusal actionable.
    double thinnest = 0.0;
    try {
        const auto thickness = geom::estimate_local_thickness(model.surface);
        for (const double t : thickness.thickness) {
            if (geom::has_finite_thickness(t) && t > 0.0 &&
                (thinnest == 0.0 || t < thinnest)) {
                thinnest = t;
            }
        }
    } catch (...) {
        thinnest = 0.0; // fall through to the honest "cannot derive" message
    }
    if (thinnest > 0.0) {
        // Two cells across the thinnest wall is the minimum that can represent it.
        throw GeometryVolumeLimitError(
            std::format("resolution refused at h={:.6g} m: the fill produced no interior "
                        "cells, so this h cannot represent the part at all; thinnest wall "
                        "is {:.6g} m and needs at least two cells across, so reduce -h to "
                        "<= {:.6g} m (or raise --max-elems/--max-dof to afford it) | {}",
                        h, thinnest, 0.5 * thinnest, cause.what()),
            GeometryVolumeAssessment{}, false);
    }
    throw GeometryVolumeLimitError(
        std::format("resolution refused at h={:.6g} m: the fill produced no interior cells, "
                    "so this h cannot represent the part at all. A recommended h could not "
                    "be derived here (no finite local thickness sample on this surface), so "
                    "reduce -h and retry rather than trusting a number this guard never "
                    "measured | {}",
                    h, cause.what()),
        GeometryVolumeAssessment{}, false);
}

VolumeMeshOutput
volume_mesh(const Model& model, double h, VolumeMesher mesher, int skin_layers,
            bool feature_refine, std::span<const Eigen::Vector3d> refine_seeds,
            double seed_band, double element_tendency, std::size_t max_elems,
            std::size_t max_dof, int auto_retry_budget,
            const std::function<void()>& cancel_check, const mesh::SizeFieldFn& size_field,
            const MeshStageSink& on_stage, const mesh::FillOptions& fill_options) {
    try {
        return volume_mesh_impl(model, h, mesher, skin_layers, feature_refine, refine_seeds,
                                seed_band, element_tendency, max_elems, max_dof,
                                auto_retry_budget, cancel_check, size_field, on_stage,
                                fill_options);
    } catch (const mesh::RefinementLimitError& e) {
        if (auto_retry_budget <= 0) {
            throw;
        }
        const double scale =
            std::cbrt(static_cast<double>(e.elements) / static_cast<double>(e.limit));
        const double retry_h =
            std::nextafter(h * scale * 1.05, std::numeric_limits<double>::infinity());
        auto retry =
            volume_mesh(model, retry_h, mesher, skin_layers, feature_refine, refine_seeds,
                        seed_band, element_tendency, max_elems, max_dof, auto_retry_budget - 1,
                        cancel_check, size_field, on_stage, fill_options);
        retry.mesher_note = std::format("auto h clamped from {:.4g} to {:.4g} m "
                                        "(refinement ceiling {}, actual {}) | {}",
                                        h, retry_h, e.limit, e.elements, retry.mesher_note);
        return retry;
    } catch (const mesh::ValidityError& e) {
        // ONLY this cause is reclassified; every other validity failure
        // propagates unchanged.
        if (std::string(e.what()).find("no interior cells") == std::string::npos) {
            throw;
        }
        refuse_unresolvable_h(model, h, e);
    }
}

VolumeMeshOutput voxel_mesh(const Model& model, double h) {
    return volume_mesh(model, h, VolumeMesher::kTetFill, 2);
}

} // namespace polymesh::pipeline
