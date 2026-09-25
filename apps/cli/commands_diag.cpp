// SPDX-License-Identifier: BSD-3-Clause

// `polymesh diag`: structured diagnostics — import fidelity, mesh quality, and
// phase timings as JSON. Optionally runs a default cantilever solve too.

#include "cli_common.hpp"

#include "fea/bc_selection.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/constraints.hpp"
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "fea/zz.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "mesh/brep_fidelity.hpp"
#include "pipeline/scene.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace polymesh::cli {

int cmd_diag(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    double E = 200e9;
    double nu = 0.3;
    auto mesher = polymesh::pipeline::VolumeMesher::kVaryhedron;
    bool do_solve = true;
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    bool curved = true;   // exact curved CAD solve/export geometry (ADR-0035)
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    double max_mem_gb = 0.0;
    std::string json_path;
    BoxSel fix_box, load_box;
    polymesh::fea::SurfaceLoadSpec load_spec;
    double scale = 1.0;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-E") == 0 && i + 1 < args.size()) {
            E = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-nu") == 0 && i + 1 < args.size()) {
            nu = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            if (!parse_mesher_arg(args[++i], mesher)) {
                std::fprintf(stderr, "unknown --mesher '%s'\n", args[i]);
                return 2;
            }
        } else if (std::strcmp(args[i], "--json") == 0 && i + 1 < args.size()) {
            json_path = args[++i];
        } else if (std::strcmp(args[i], "--no-solve") == 0) {
            do_solve = false;
        } else if (std::strcmp(args[i], "--scale") == 0) {
            if (!parse_scale(args, i, scale)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--no-curved") == 0) {
            curved = false;
        } else if (std::strcmp(args[i], "--spectral") == 0) {
            spectral = true; // accepted for symmetry (now the default)
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
        } else if (std::strcmp(args[i], "--max-elems") == 0) {
            if (!parse_ceiling(args, i, max_elems)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--max-dof") == 0) {
            if (!parse_ceiling(args, i, max_dof)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--max-mem") == 0 && i + 1 < args.size()) {
            max_mem_gb = std::max(0.0, std::atof(args[++i]));
        } else if (std::strcmp(args[i], "--fix-box") == 0) {
            if (!parse_box6(args, i, fix_box)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--load-box") == 0) {
            if (!parse_box6(args, i, load_box)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--load-dir") == 0 ||
                   std::strcmp(args[i], "--force") == 0 ||
                   std::strcmp(args[i], "--traction") == 0) {
            if (!parse_load_flag(args, i, load_spec)) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    auto t0 = clock::now();
    const auto model = polymesh::pipeline::Model::load(path, 30.0, scale);
    const auto exact_pressure_area =
        load_spec.traction_mode && load_box.set
            ? polymesh::pipeline::cad_pressure_area(model, *load_box.region(), load_spec.dir)
            : std::nullopt;
    const double import_ms = ms(clock::now() - t0);
    report_scale(scale);
    const double bbox_diag = (model.bbox_max - model.bbox_min).norm();

    const auto resolved =
        polymesh::pipeline::resolve_mesh_size(model, h, 30.0, max_elems, max_dof);
    h = resolved.h;
    std::string mesh_size_note = resolved.note;
    // Diagnostics run at a coarse, representative resolution: cap auto-h so a
    // curvature-fine auto size doesn't explode the quick battery. A user -h is
    // always respected, and the note records the effective cap explicitly.
    if (resolved.auto_chosen && bbox_diag > 0.0 && h < bbox_diag / 12.0) {
        h = bbox_diag / 12.0;
        mesh_size_note = std::format("h={:.6g} m (diagnostic auto cap from {:.6g} m; {})", h,
                                     resolved.h, resolved.note);
    }
    // BC/load boxes feed the refinement plan too, so bc_seeds is a real
    // measurement instead of a structural zero.
    const auto plan = polymesh::pipeline::build_refinement_plan(
        model, h, make_regions(fix_box, load_box), /*use_geometry=*/true, spectral,
        /*spectral_budget=*/0);

    t0 = clock::now();
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, mesher, 2, true, plan.refine_seeds, plan.seed_band, 0.0,
        resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
        plan.size_field);
    const double mesh_ms = ms(clock::now() - t0);
    polymesh::fea::LinearConstraints curved_constraints;
    if (curved) {
        auto shaped = polymesh::pipeline::curve_volume_geometry(model, vol.mesh, h);
        vol.mesh = std::move(shaped.mesh);
        curved_constraints = std::move(shaped.constraints);
        vol.boundary_quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        vol.mesher_note += std::format(
            " | curved_volume promoted={} pyramid_split={} projected={} partial={} "
            "reverted={} h_refined={}",
            shaped.n_promoted, shaped.n_pyramids_split, shaped.n_projected, shaped.n_partial,
            shaped.n_reverted, shaped.n_h_refined);
    }
    vol.mesh.check_validity();

    // Measured per-cell quality for every element type (fea::cell_quality);
    // min/mean cover measured cells only, so an unmeasured cell never scores.
    const auto q = polymesh::fea::summarize_cell_quality(vol.mesh);
    const double q_min = q.min;
    const double q_mean = q.mean;
    // Which element type owns the worst cell, and how many cells are inverted.
    // `quality_min` on its own cannot tell a fill defect from a snap defect, and
    // a single number hides whether one cell or a thousand are folded.
    std::string q_min_type = "n/a";
    std::size_t n_inverted = 0;
    {
        const auto per_cell = polymesh::fea::cell_quality(vol.mesh);
        double lo = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < per_cell.size(); ++i) {
            if (!std::isfinite(per_cell[i])) {
                continue;
            }
            if (per_cell[i] < 0.0) {
                ++n_inverted;
            }
            if (per_cell[i] < lo) {
                lo = per_cell[i];
                q_min_type = polymesh::fea::element_type_name(vol.mesh.elements[i].type);
            }
        }
    }

    const auto fidelity_surface = polymesh::fea::tessellate_boundary_surface(vol.mesh, 8);
    std::vector<Eigen::Vector3d> fidelity_nodes;
    fidelity_nodes.reserve(fidelity_surface.samples.size());
    for (const auto& sample : fidelity_surface.samples) {
        fidelity_nodes.push_back(sample.position);
    }
    std::vector<polymesh::mesh::FreeFace> fidelity_faces;
    fidelity_faces.reserve(fidelity_surface.triangles.size());
    for (const auto& tri : fidelity_surface.triangles) {
        fidelity_faces.push_back({tri[0], tri[1], tri[2], tri[2]});
    }
    constexpr std::size_t kBRepSurfaceSampleCeiling = 10'000;
    polymesh::mesh::BRepGeometryFidelity fidelity;
    if (model.cad && !model.cad->empty()) {
        const auto topology = polymesh::geom::extract_topology(*model.cad);
        std::vector<polymesh::geom::MeshEdgeSegment> feature_segments;
        std::set<std::array<std::uint32_t, 2>> visited_edges;
        for (const auto& face : polymesh::fea::boundary_surface_faces(vol.mesh)) {
            const int corners = (face.type == polymesh::fea::FaceType::kTri3 ||
                                 face.type == polymesh::fea::FaceType::kTri6)
                                    ? 3
                                    : 4;
            const bool quadratic = face.type == polymesh::fea::FaceType::kTri6 ||
                                   face.type == polymesh::fea::FaceType::kQuad8;
            for (int edge_index = 0; edge_index < corners; ++edge_index) {
                const std::uint32_t a = face.nodes[static_cast<std::size_t>(edge_index)];
                const std::uint32_t b =
                    face.nodes[static_cast<std::size_t>((edge_index + 1) % corners)];
                auto key = std::array{a, b};
                std::sort(key.begin(), key.end());
                if (!visited_edges.insert(key).second) {
                    continue;
                }
                const std::uint32_t mid =
                    quadratic ? face.nodes[static_cast<std::size_t>(corners + edge_index)] : a;
                const auto point = [&](double t) -> Eigen::Vector3d {
                    if (!quadratic) {
                        return ((1.0 - t) * vol.mesh.nodes[a] + t * vol.mesh.nodes[b]).eval();
                    }
                    const double wa = (1.0 - t) * (1.0 - 2.0 * t);
                    const double wb = t * (2.0 * t - 1.0);
                    const double wm = 4.0 * t * (1.0 - t);
                    return (wa * vol.mesh.nodes[a] + wb * vol.mesh.nodes[b] +
                            wm * vol.mesh.nodes[mid])
                        .eval();
                };
                const auto edge_a = polymesh::geom::closest_edge(topology, point(0.0), true);
                const auto edge_b = polymesh::geom::closest_edge(topology, point(1.0), true);
                if (!edge_a || !edge_b || edge_a->edge_id != edge_b->edge_id) {
                    continue;
                }
                const auto exact_a = polymesh::geom::project_point_on_edge(
                    *model.cad, edge_a->edge_id, point(0.0));
                const auto exact_b = polymesh::geom::project_point_on_edge(
                    *model.cad, edge_a->edge_id, point(1.0));
                const double edge_tol = 1e-6 * h;
                if (!exact_a || !exact_b || exact_a->distance > edge_tol ||
                    exact_b->distance > edge_tol) {
                    continue;
                }
                Eigen::Vector3d previous = point(0.0);
                for (int sample = 1; sample <= 8; ++sample) {
                    const Eigen::Vector3d current = point(static_cast<double>(sample) / 8.0);
                    feature_segments.push_back({previous, current});
                    previous = current;
                }
            }
        }
        const double mesh_volume =
            polymesh::mesh::boundary_surface_volume(fidelity_nodes, fidelity_faces);
        fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
            *model.cad, fidelity_nodes, fidelity_faces, feature_segments, h, mesh_volume,
            kBRepSurfaceSampleCeiling);
    }

    double max_vm = 0.0, max_u = 0.0, global_eta = 0.0, solve_ms = 0.0;
    std::size_t dof = 0;
    bool solved = false;
    if (do_solve) {
        const double xmin = model.bbox_min[0], xmax = model.bbox_max[0];
        const double tol = 0.51 * h;
        const auto all_faces = polymesh::fea::boundary_surface_faces(vol.mesh);
        const std::size_t n_bnd = polymesh::fea::count_boundary_nodes(all_faces);
        const auto fix_sel = polymesh::fea::select_cantilever_end(
            vol.mesh, all_faces, n_bnd, fix_box.region(), xmin, xmax, tol, -1);
        const auto load_sel = polymesh::fea::select_cantilever_end(
            vol.mesh, all_faces, n_bnd, load_box.region(), xmin, xmax, tol, +1);
        const auto load_faces =
            load_spec.traction_mode
                ? polymesh::fea::pressure_aligned_faces(vol.mesh, load_sel.faces, load_spec.dir)
                : load_sel.faces;
        polymesh::fea::Dirichlet bc;
        for (const auto n : fix_sel.nodes) {
            bc.fix_node(n);
        }
        if (const auto defect = polymesh::fea::constraint_defect(vol.mesh, fix_sel.nodes);
            !defect.empty()) {
            throw std::runtime_error(std::format(
                "diag: the fixture selection is geometrically degenerate — {}. Select a "
                "real face with --fix-box x0 y0 z0 x1 y1 z1.",
                defect));
        }
        if (!bc.dof_values.empty() && !load_sel.nodes.empty()) {
            Eigen::VectorXd loads = polymesh::fea::assemble_selection_load(
                vol.mesh, load_faces, load_sel.nodes, load_spec, "diag", stderr,
                load_sel.region, exact_pressure_area);
            const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
            t0 = clock::now();
            polymesh::fea::SolveOptions solve_options;
            solve_options.max_mem_gb = max_mem_gb;
            solve_options.on_note = [](std::string_view note) {
                std::fprintf(stderr, "diag: %.*s\n", static_cast<int>(note.size()),
                             note.data());
            };
            const Eigen::VectorXd uu =
                polymesh::fea::solve_elastostatics(
                    vol.mesh, mat, bc, loads, solve_options,
                    curved_constraints.empty() ? nullptr : &curved_constraints)
                    .u;
            const auto zz = polymesh::fea::recover_zz(vol.mesh, mat, uu);
            solve_ms = ms(clock::now() - t0);
            global_eta = zz.global_eta;
            dof = 3 * vol.mesh.nodes.size();
            for (std::size_t i = 0; i < zz.nodal_stress.size(); ++i) {
                max_vm = std::max(max_vm, polymesh::fea::von_mises(zz.nodal_stress[i]));
                max_u =
                    std::max(max_u, uu.segment<3>(3 * static_cast<Eigen::Index>(i)).norm());
            }
            solved = true;
        }
    }

    const double mesh_throughput =
        mesh_ms > 0.0 ? static_cast<double>(vol.mesh.elements.size()) / (mesh_ms / 1000.0)
                      : 0.0;

    const auto distance_json = [](const polymesh::mesh::DistanceDistribution& d) {
        return std::format("{{\"count\":{},\"rms_m\":{:.9g},\"p95_m\":{:.9g},"
                           "\"p99_m\":{:.9g},\"max_m\":{:.9g},\"p95_over_h\":{:.9g},"
                           "\"p99_over_h\":{:.9g},\"max_over_h\":{:.9g},"
                           "\"p99_over_bbox\":{:.9g}}}",
                           d.metres.count, d.metres.rms, d.metres.p95, d.metres.p99,
                           d.metres.max, d.over_h.p95, d.over_h.p99, d.over_h.max,
                           d.over_bbox_diagonal.p99);
    };
    constexpr double kRadiansToDegrees = 57.2957795130823208768;
    const auto& normal = fidelity.mesh_boundary_normal_angle_to_brep_normal;
    const std::string normal_json = std::format(
        "{{\"count\":{},\"rms_deg\":{:.9g},\"p95_deg\":{:.9g},"
        "\"p99_deg\":{:.9g},\"max_deg\":{:.9g}}}",
        normal.count, normal.rms * kRadiansToDegrees, normal.p95 * kRadiansToDegrees,
        normal.p99 * kRadiansToDegrees, normal.max * kRadiansToDegrees);
    const std::string relative_volume =
        fidelity.has_relative_volume_error
            ? std::format("{:.9g}", fidelity.mesh_vs_brep_relative_volume_error)
            : "null";
    const std::string fidelity_json = std::format(
        "{{\"available\":{},\"brep_valid\":{},\"brep_closed\":{},"
        "\"brep_volume_m3\":{:.9g},\"brep_surface_area_m2\":{:.9g},"
        "\"mesh_boundary_to_brep\":{},"
        "\"mesh_boundary_nodes_to_brep\":{},"
        "\"brep_surface_samples_to_mesh_boundary\":{},"
        "\"brep_surface_sampler\":\"exact_trimmed_face_uv_grid\","
        "\"brep_surface_sample_ceiling\":{},\"brep_surface_sample_faces\":{},"
        "\"brep_surface_uv_attempts\":{},\"brep_surface_fallback_vertices\":{},"
        "\"normal_angle\":{},"
        "\"mesh_feature_classifier\":\"cad_owned_quadratic_boundary_edges\","
        "\"mesh_feature_to_sharp_brep_edge\":{},"
        "\"sharp_brep_edge_to_mesh_feature\":{},\"brep_vertex_to_mesh_node\":{},"
        "\"mesh_feature_segments\":{},\"max_chordal_efficiency\":{:.9g},"
        "\"relative_volume_error\":{}}}",
        fidelity.available ? "true" : "false", fidelity.brep.valid ? "true" : "false",
        fidelity.brep.closed ? "true" : "false", fidelity.brep.volume,
        fidelity.brep.surface_area,
        distance_json(fidelity.mesh_boundary_samples_to_brep_surface),
        distance_json(fidelity.mesh_boundary_nodes_to_brep_surface),
        distance_json(fidelity.brep_surface_samples_to_mesh_boundary),
        kBRepSurfaceSampleCeiling, fidelity.brep_surface_sample_face_count,
        fidelity.brep_surface_uv_attempt_count, fidelity.brep_surface_fallback_vertex_count,
        normal_json, distance_json(fidelity.mesh_feature_segment_samples_to_sharp_brep_edges),
        distance_json(fidelity.sharp_brep_edge_samples_to_mesh_feature_segments),
        distance_json(fidelity.brep_vertices_to_mesh_boundary_nodes),
        fidelity.mesh_feature_segment_count, fidelity.max_sharp_edge_chordal_efficiency,
        relative_volume);

    const std::string spectral_json = std::format(
        "{{ \"applied\": {}, \"modes_kept\": {}, \"modes_total\": {}, "
        "\"energy_kept\": {:.6g}, \"edge_curve_seeds\": {}, "
        "\"n_pred_before\": {:.6g}, \"n_pred_after\": {:.6g} }}",
        plan.spectral.applied ? "true" : "false", plan.spectral.modes_kept,
        plan.spectral.modes_total, plan.spectral.energy_kept, plan.spectral.n_edge_curve_seeds,
        plan.spectral.predicted_before, plan.spectral.predicted_after);

    const std::string json = std::format(
        "{{\n"
        "  \"part\": \"{}\",\n"
        "  \"mesher\": \"{}\",\n"
        "  \"scale\": {:.6g},\n"
        "  \"import\": {{ \"vertices\": {}, \"triangles\": {}, \"bbox_diag\": {:.6g}, "
        "\"cad_brep\": {} }},\n"
        "  \"mesh\": {{ \"h\": {:.6g}, \"nodes\": {}, \"elements\": {}, "
        "\"quality_min\": {:.4g}, \"quality_min_type\": \"{}\", "
        "\"n_inverted_cells\": {}, \"n_below_shape_floor\": {}, \"quality_mean\": {:.4g}, "
        "\"geometry_seeds\": {}, \"bc_seeds\": {}, \"geo_curv\": \"{}\" }},\n"
        "  \"spectral\": {},\n"
        "  \"timing_ms\": {{ \"import\": {:.3f}, \"mesh\": {:.3f}, \"solve\": {:.3f} }},\n"
        "  \"mesh_throughput_elem_per_s\": {:.1f},\n"
        "  \"fidelity\": {},\n"
        "  \"solve\": {{ \"ran\": {}, \"dof\": {}, \"youngs_modulus_pa\": {:.6g}, "
        "\"poissons_ratio\": {:.6g}, \"max_von_mises\": {:.6g}, "
        "\"max_disp\": {:.6g}, \"global_eta\": {:.6g} }},\n"
        "  \"mesh_size_note\": \"{}\",\n"
        "  \"mesher_note\": \"{}\"\n"
        "}}\n",
        model.name, polymesh::pipeline::mesher_name(mesher), scale,
        model.surface.vertices.size(), model.surface.triangles.size(), bbox_diag,
        model.cad ? "true" : "false", h,
        vol.mesh.nodes.size(), vol.mesh.elements.size(), q_min, q_min_type, n_inverted,
        vol.n_cells_below_shape_floor, q_mean, plan.n_geometry_seeds, plan.n_bc_seeds,
        plan.geometry_curvature_from_brep ? "brep" : "tessellation", spectral_json, import_ms,
        mesh_ms, solve_ms, mesh_throughput, fidelity_json, solved ? "true" : "false", dof, E,
        nu, max_vm, max_u, global_eta, mesh_size_note, vol.mesher_note);

    if (!json_path.empty()) {
        std::FILE* f = std::fopen(json_path.c_str(), "w");
        if (f == nullptr) {
            std::fprintf(stderr, "diag: cannot write %s\n", json_path.c_str());
            return 1;
        }
        std::fputs(json.c_str(), f);
        std::fclose(f);
        std::printf("wrote %s\n", json_path.c_str());
    }
    std::fputs(json.c_str(), stdout);
    return 0;
}

} // namespace polymesh::cli
