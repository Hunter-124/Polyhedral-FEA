// SPDX-License-Identifier: BSD-3-Clause

// `polymesh solve`: mesh (or import .msh) + BCs + elastostatic solve + VTU.

#include "cli_common.hpp"

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif
#include "adapt/error.hpp"
#include "adapt/loop.hpp"
#include "fea/backend.hpp"
#include "fea/bc_selection.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/constraints.hpp"
#include "fea/material.hpp"
#include "fea/msh.hpp"
#include "fea/p_elevate.hpp"
#include "fea/resource_budget.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "fea/traction.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "mesh/surface_project.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Eigenvalues>

#include <algorithm>
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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace polymesh::cli {
namespace {

bool is_msh_path(std::string_view path) {
    if (path.size() < 4 || path[path.size() - 4] != '.') {
        return false;
    }
    const auto ascii_lower = [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return ascii_lower(path[path.size() - 3]) == 'm' &&
           ascii_lower(path[path.size() - 2]) == 's' &&
           ascii_lower(path[path.size() - 1]) == 'h';
}

// Provenance of a selection for the `bc:` report line.
std::string selection_note(const polymesh::fea::BcSelection& sel, bool is_fix) {
    if (sel.from_box) {
        return is_fix ? " (--fix-box)" : " (--load-box)";
    }
    if (!sel.face_fallback) {
        return is_fix ? " (min-x slab)" : " (max-x slab)";
    }
    return std::format(" (slab captured only {} → ±x-aligned faces within the outer {:.0f}% "
                       "of the x extent)",
                       sel.slab_nodes, 100.0 * sel.fallback_band);
}

// Wall-clock account of one `solve` invocation. Every number printed on the
// `phases:` line is measured here or inside `solve_elastostatics`; nothing is
// derived by subtraction, so the phases sum to slightly less than the total and
// the remainder is honest un-instrumented overhead rather than a fudge.
struct SolvePhaseLog {
    using Clock = std::chrono::steady_clock;

    double import_s = 0.0;
    double refine_s = 0.0;
    double mesh_s = 0.0;
    double bc_s = 0.0;
    double preflight_s = 0.0;
    double assemble_s = 0.0;
    double reduce_s = 0.0;
    double order_s = 0.0;
    double factor_s = 0.0;
    double backsolve_s = 0.0;
    double stress_s = 0.0;
    double export_s = 0.0;
    int n_solves = 0;
    Clock::time_point start = Clock::now();

    static double since(Clock::time_point t) {
        return std::chrono::duration<double>(Clock::now() - t).count();
    }

    void add(const polymesh::fea::SolvePhaseTimings& p) {
        constexpr double kMsToS = 1e-3;
        preflight_s += p.preflight_ms * kMsToS;
        assemble_s += p.assemble_ms * kMsToS;
        reduce_s += p.reduce_ms * kMsToS;
        order_s += p.analyze_ms * kMsToS;
        factor_s += p.factorize_ms * kMsToS;
        backsolve_s += p.backsolve_ms * kMsToS;
        ++n_solves;
    }

    void report() const {
        const auto peak = polymesh::fea::peak_resident_bytes();
        std::printf(
            "phases: import=%.2f refine=%.2f mesh=%.2f bc=%.2f preflight=%.2f "
            "assemble=%.2f reduce=%.2f order=%.2f factor=%.2f backsolve=%.2f "
            "stress=%.2f export=%.2f s | solves=%d | total=%.2f s | peak RSS=%s | %s\n",
            import_s, refine_s, mesh_s, bc_s, preflight_s, assemble_s, reduce_s, order_s,
            factor_s, backsolve_s, stress_s, export_s, n_solves, since(start),
            peak > 0 ? polymesh::fea::format_memory_bytes(peak).c_str()
                     : "not reported by this OS",
            polymesh::fea::performance_description().c_str());
    }
};

} // namespace

int cmd_solve(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    double E = 200e9;
    double nu = 0.3;
    std::string out_path;
    auto mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    int skin = 2;
    bool feature = true;  // geometry grading on by default (CAD)
    bool curved = true;   // exact curved CAD solve/export geometry (ADR-0035)
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = true;
    bool p_elevate_uniform = true;
    double element_tendency = 0.0;
    bool bc_grade = false;
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    double max_mem_gb = 0.0;
    BoxSel fix_box, load_box;
    polymesh::fea::SurfaceLoadSpec load_spec;
    std::string advisor_dir;
    std::size_t advisor_max_dof = 0; // 0 = no advisor budget (ADR-0034)
    bool advisor_efficiency = false;
    double scale = 1.0;
    auto solve_method = polymesh::fea::SolveMethod::kAuto;
    int threads = 0; // 0 = process default
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (std::strcmp(args[i], "-E") == 0 && i + 1 < args.size()) {
            E = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-nu") == 0 && i + 1 < args.size()) {
            nu = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            if (!parse_mesher_arg(args[++i], mesher)) {
                std::fprintf(stderr, "unknown --mesher '%s'\n", args[i]);
                return 2;
            }
        } else if (std::strcmp(args[i], "--skin") == 0 && i + 1 < args.size()) {
            skin = std::atoi(args[++i]);
            if (skin < 1) {
                skin = 1;
            }
        } else if (std::strcmp(args[i], "--feature") == 0) {
            feature = true; // accepted for back-compat (now the default)
        } else if (std::strcmp(args[i], "--no-feature") == 0) {
            feature = false;
        } else if (std::strcmp(args[i], "--no-curved") == 0) {
            curved = false;
        } else if (std::strcmp(args[i], "--spectral") == 0) {
            spectral = true; // accepted for symmetry (now the default)
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
        } else if (std::strcmp(args[i], "--scale") == 0) {
            if (!parse_scale(args, i, scale)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--fix-box") == 0) {
            if (!parse_box6(args, i, fix_box)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--load-box") == 0) {
            if (!parse_box6(args, i, load_box)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--element-tendency") == 0 && i + 1 < args.size()) {
            element_tendency = std::atof(args[++i]);
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
        } else if (std::strcmp(args[i], "--solver") == 0 && i + 1 < args.size()) {
            const std::string_view name = args[++i];
            if (name == "auto") {
                solve_method = polymesh::fea::SolveMethod::kAuto;
            } else if (name == "direct") {
                solve_method = polymesh::fea::SolveMethod::kDirect;
            } else if (name == "cg") {
                solve_method = polymesh::fea::SolveMethod::kCG;
            } else {
                std::fputs("solve: --solver must be auto, direct or cg\n", stderr);
                return 2;
            }
        } else if (std::strcmp(args[i], "--threads") == 0 && i + 1 < args.size()) {
            threads = std::atoi(args[++i]);
            if (threads < 0) {
                threads = 0;
            }
        } else if (std::strcmp(args[i], "--adapt") == 0 && i + 1 < args.size()) {
            adapt_passes = std::atoi(args[++i]);
            if (adapt_passes < 0) {
                adapt_passes = 0;
            }
        } else if (std::strcmp(args[i], "--eta-target") == 0 && i + 1 < args.size()) {
            eta_target = std::atof(args[++i]);
            if (eta_target < 0.0) {
                eta_target = 0.0;
            }
        } else if (std::strcmp(args[i], "--p-elevate") == 0) {
            p_elevate = true;
        } else if (std::strcmp(args[i], "--p-elevate-uniform") == 0) {
            // Implies --p-elevate, so this flag never depends on a second one.
            p_elevate_uniform = true;
            p_elevate = true;
        } else if (std::strcmp(args[i], "--bc-grade") == 0) {
            bc_grade = true;
        } else if (std::strcmp(args[i], "--advisor") == 0 && i + 1 < args.size()) {
            advisor_dir = args[++i];
        } else if (std::strcmp(args[i], "--advisor-objective") == 0 && i + 1 < args.size()) {
            const std::string_view objective = args[++i];
            if (objective == "accuracy") {
                advisor_efficiency = false;
            } else if (objective == "efficiency") {
                advisor_efficiency = true;
            } else {
                std::fputs("solve: --advisor-objective must be accuracy or efficiency\n",
                           stderr);
                return 2;
            }
        } else if (std::strcmp(args[i], "--advisor-max-dof") == 0) {
            if (!parse_ceiling(args, i, advisor_max_dof)) {
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
    // Auto when adapt_passes > 0 (hp product path), same as SimSetup.
    if (adapt_passes > 0) {
        p_elevate = true;
    }
    if (out_path.empty()) {
        std::fputs("solve: -o out.vtu is required\n", stderr);
        return 2;
    }
    // A thread cap is applied before any parallel stage runs, and it covers the
    // mesher, assembly, recovery and (where SuiteSparse is present) the
    // factorization's own BLAS, because they all read the same OpenMP limit.
    if (threads > 0) {
        polymesh::fea::set_openmp_threads(threads);
    }
    SolvePhaseLog phase_log;

    const bool msh_input = is_msh_path(path);
    if (msh_input && !advisor_dir.empty()) {
        std::fputs("solve: --advisor requires CAD input and cannot be used with .msh\n",
                   stderr);
        return 2;
    }
    if (msh_input && scale != 1.0) {
        // A Gmsh mesh is already discretised: rescaling it here would move the
        // nodes out from under the element sizes the mesh was built with, and
        // there is no exact geometry left to re-derive them from. Convert the
        // units in the mesher that wrote it.
        std::fputs("solve: --scale applies to CAD input only, not a .msh volume mesh\n",
                   stderr);
        return 2;
    }

    std::optional<polymesh::pipeline::Model> model;
    std::optional<polymesh::fea::MshModel> msh_model;
    Eigen::Vector3d bbox_min;
    Eigen::Vector3d bbox_max;
    const auto import_start = SolvePhaseLog::Clock::now();
    if (msh_input) {
        msh_model.emplace(polymesh::fea::load_msh(path));
        bbox_min = msh_model->mesh.nodes.front();
        bbox_max = bbox_min;
        for (const auto& node : msh_model->mesh.nodes) {
            bbox_min = bbox_min.cwiseMin(node);
            bbox_max = bbox_max.cwiseMax(node);
        }
    } else {
        model.emplace(polymesh::pipeline::Model::load(path, 30.0, scale));
        report_scale(scale);
        bbox_min = model->bbox_min;
        bbox_max = model->bbox_max;
    }
    phase_log.import_s = SolvePhaseLog::since(import_start);

    // Learned mesh advisor (ADR-0027). It runs before size resolution and
    // grading so its action is the one that actually meshes: h, mesher, adapt
    // schedule and p-order all come from the model, inside the clamp box.
    // The decision is printed in full — a mesh chosen by a network must be as
    // auditable as one chosen by a flag.
    if (!advisor_dir.empty()) {
#ifdef POLYMESH_WITH_ADVISOR
        std::vector<polymesh::pipeline::RefineRegion> advisor_fix;
        std::vector<polymesh::pipeline::RefineRegion> advisor_load;
        if (fix_box.set) {
            advisor_fix.push_back({fix_box.lo, fix_box.hi, 0.5});
        }
        if (load_box.set) {
            advisor_load.push_back({load_box.lo, load_box.hi, 0.25});
        }
        const auto features = polymesh::pipeline::extract_case_features(
            *model, advisor_fix, advisor_load, load_spec.dir, nu);
        const polymesh::advisor::Advisor advisor(
            advisor_dir, advisor_efficiency ? polymesh::advisor::AdvisorObjective::kEfficiency
                                            : polymesh::advisor::AdvisorObjective::kAccuracy);
        const auto decision =
            advisor.recommend(features, static_cast<double>(advisor_max_dof));
        std::printf("advisor: %s\n", polymesh::advisor::to_json(decision).c_str());
        const double diag = (model->bbox_max - model->bbox_min).norm();
        const auto resolved_mesher = polymesh::pipeline::mesher_from_name(decision.mesher);
        if (!resolved_mesher) {
            std::fprintf(stderr,
                         "solve: advisor recommended mesher '%s', which this build cannot "
                         "parse. Refusing to silently mesh something else — re-export the "
                         "model with a mesher_choices vocabulary the CLI accepts.\n",
                         decision.mesher.c_str());
            return 2;
        }
        mesher = *resolved_mesher;
        h = std::max(decision.h_rel * diag, 1e-9);
        // Feasibility probe. `h_rel` is a scale-free fraction of the bounding
        // diagonal and cannot know about a feature finer than that, so the
        // verdict comes from the engine rather than a geometric proxy: probe-mesh
        // at the chosen h and, when the fill refuses on feature resolution,
        // refine by the factor the guard itself recommends (0.6), at most three
        // times. Nothing is clamped that meshes, and every refinement is reported.
        double feature_clamped_from = 0.0;
        std::string feature_clamp_reason;
        for (int probe = 0; probe < 3; ++probe) {
            try {
                (void)polymesh::pipeline::volume_mesh(*model, h, mesher, skin, feature, {},
                                                      0.0, element_tendency, max_elems,
                                                      max_dof, 0);
                break;
            } catch (const polymesh::pipeline::GeometryVolumeLimitError& e) {
                if (feature_clamped_from == 0.0) {
                    feature_clamped_from = h;
                    feature_clamp_reason = e.what();
                }
                h *= 0.6;
            }
        }
        adapt_passes = decision.adapt_passes;
        eta_target = decision.eta_target;
        // The solve path has one p-elevation step (tet4/hex8 -> tet10/hex20),
        // so orders above 2 are executed as quadratic. Say so rather than let
        // the decision JSON claim an order the mesh never had.
        p_elevate = decision.p_elevate || decision.order >= 2;
        // `rel_err_rel` is the centred per-case score that actually drove the
        // choice. On a refusal it is not printed: the decision JSON nulls every
        // prediction on a veto, so the prose line states the reason instead.
        if (decision.vetoed) {
            std::printf("advisor: applied mesher=%s h=%.6g m (h_rel=%.4g) adapt=%d eta=%.4g "
                        "order=%d p_elevate=%d [VETOED -> defaults: %s]\n",
                        decision.mesher.c_str(), h, decision.h_rel, adapt_passes, eta_target,
                        decision.order, p_elevate ? 1 : 0,
                        decision.note.empty() ? "no reason recorded" : decision.note.c_str());
        } else {
            std::printf("advisor: applied mesher=%s h=%.6g m (h_rel=%.4g) adapt=%d eta=%.4g "
                        "order=%d p_elevate=%d rel_err_rel=%+.4g (per-case score, lower is "
                        "better)\n",
                        decision.mesher.c_str(), h, decision.h_rel, adapt_passes, eta_target,
                        decision.order, p_elevate ? 1 : 0, decision.predicted_rel_err_rel);
        }
        if (feature_clamped_from > 0.0) {
            std::printf("advisor: h refined %.6g -> %.6g m — the fill refused the coarser "
                        "mesh: %s\n",
                        feature_clamped_from, h, feature_clamp_reason.c_str());
        }
        if (decision.order > 2) {
            std::printf("advisor: order %d executed as quadratic — this solve path has a "
                        "single p-elevation step\n",
                        decision.order);
        }
#else
        std::fputs("solve: --advisor needs a build with POLYMESH_WITH_ADVISOR=ON\n", stderr);
        return 2;
#endif
    }

    const auto exact_pressure_area =
        !msh_input && load_spec.traction_mode && load_box.set
            ? polymesh::pipeline::cad_pressure_area(*model, *load_box.region(), load_spec.dir)
            : std::nullopt;
    polymesh::pipeline::ResolvedMeshSize resolved;
    if (msh_input) {
        const bool auto_h = !(h > 0.0);
        if (auto_h) {
            const Eigen::Vector3d extent = bbox_max - bbox_min;
            const double n = static_cast<double>(msh_model->mesh.elements.size());
            const double bbox_volume = extent.x() * extent.y() * extent.z();
            h = bbox_volume > 0.0 ? std::cbrt(bbox_volume / n)
                                  : extent.maxCoeff() / std::cbrt(n);
        }
        if (!(h > 0.0) || !std::isfinite(h)) {
            throw std::runtime_error(
                "solve: cannot infer a positive mesh scale from the imported .msh; pass -h");
        }
        resolved.h = h;
        resolved.auto_chosen = auto_h;
        if (max_elems > 0) {
            resolved.element_ceiling = max_elems;
        }
        if (max_dof > 0) {
            resolved.dof_ceiling = max_dof;
        }
        resolved.note = std::format("imported .msh (CAD fidelity unavailable, h{}={:.6g} m)",
                                    auto_h ? "_estimate" : "", h);
    } else {
        resolved = polymesh::pipeline::resolve_mesh_size(*model, h, 30.0, max_elems, max_dof);
        h = resolved.h;
    }

    double h_use = h;
    std::vector<Eigen::Vector3d> seeds;
    double seed_band = 0.0;
    polymesh::mesh::SizeFieldFn size_field;
    // Geometry + simulation-setup refinement. Explicit --fix-box/--load-box
    // define the grading (and BC) regions; otherwise --bc-grade derives the
    // default cantilever slabs (fix min-x, load max-x). Geometry grading
    // (curvature / thin-wall) applies whenever --feature is on (default).
    std::vector<polymesh::pipeline::RefineRegion> regions;
    if (!msh_input) {
        const double xmin = bbox_min[0];
        const double xmax = bbox_max[0];
        const double slab = 0.51 * h_use;
        if (load_box.set) {
            regions.push_back({load_box.lo, load_box.hi, 0.25});
        } else if (bc_grade) {
            Eigen::Vector3d lo = bbox_min, hi = bbox_max;
            lo[0] = xmax - slab;
            regions.push_back({lo, hi, 0.25});
        }
        if (fix_box.set) {
            regions.push_back({fix_box.lo, fix_box.hi, 0.5});
        } else if (bc_grade) {
            Eigen::Vector3d lo = bbox_min, hi = bbox_max;
            hi[0] = xmin + slab;
            regions.push_back({lo, hi, 0.5});
        }
        const auto refine_start = SolvePhaseLog::Clock::now();
        const auto plan = polymesh::pipeline::build_refinement_plan(*model, h_use, regions,
                                                                    feature, spectral, 0);
        phase_log.refine_s = SolvePhaseLog::since(refine_start);
        seeds = plan.refine_seeds;
        seed_band = plan.seed_band;
        size_field = plan.size_field;
        std::printf(
            "refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m, "
            "geo_curv=%s\n",
            plan.n_geometry_seeds, plan.n_bc_seeds, seeds.size(), seed_band, plan.h_fine,
            plan.geometry_curvature_from_brep ? "brep" : "tessellation");
        if (plan.spectral.applied) {
            std::printf(
                "spectral: %zu/%zu modes kept (%.2f%% energy), %zu denoised edge-curve "
                "seeds, N_pred %.4g → %.4g%s\n",
                plan.spectral.modes_kept, plan.spectral.modes_total,
                100.0 * plan.spectral.energy_kept, plan.spectral.n_edge_curve_seeds,
                plan.spectral.predicted_before, plan.spectral.predicted_after,
                plan.spectral.budget_met ? "" : " (budget not met — geometry floor)");
        }
    }
    auto mesh_now = [&](polymesh::pipeline::VolumeMesher m) {
        if (msh_input) {
            throw std::runtime_error("solve: --adapt requires CAD geometry for remeshing and "
                                     "is unavailable for .msh");
        }
        const auto mesh_start = SolvePhaseLog::Clock::now();
        auto out = polymesh::pipeline::volume_mesh(
            *model, h_use, m, skin, feature, seeds, seed_band, element_tendency,
            resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
            size_field);
        phase_log.mesh_s += SolvePhaseLog::since(mesh_start);
        return out;
    };
    polymesh::pipeline::VolumeMeshOutput vol;
    if (msh_input) {
        vol.mesh = std::move(msh_model->mesh);
        vol.boundary_quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        vol.mesher_note =
            std::format("Gmsh import: {} boundary faces, {} physical groups",
                        vol.boundary_quads.size(), msh_model->physical_faces.size());
    } else {
        vol = mesh_now(mesher);
    }
    vol.mesh.check_validity();
    std::vector<polymesh::mesh::BoundarySupport> solve_boundary_provenance;
    polymesh::mesh::BoundaryProjectionContext solve_projection_context;
    polymesh::mesh::BoundaryProjectionContext* solve_projection = nullptr;
    if (model && model->cad &&
        polymesh::pipeline::make_boundary_projection(
            *model->cad, h_use, &solve_projection_context, &solve_boundary_provenance)) {
        solve_projection = &solve_projection_context;
    }
    const auto project_quadratic_mids = [&]() {
        if (solve_projection == nullptr || !model || !model->cad) {
            return;
        }
        std::vector<std::uint32_t> reverted;
        std::vector<std::uint32_t> partial;
        const std::size_t projected = polymesh::pipeline::project_quadratic_boundary_mids(
            vol.mesh, *model->cad, solve_projection, h_use, &reverted, &partial);
        vol.mesher_note += std::format(" | mids projected={} partial={} reverted={}",
                                       projected, partial.size(), reverted.size());
    };

    const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
    auto make_bc_loads = [&](const polymesh::pipeline::VolumeMeshOutput& v) {
        const auto bc_start = SolvePhaseLog::Clock::now();
        const double xmin = bbox_min[0];
        const double xmax = bbox_max[0];
        const double tol = 0.51 * h_use;
        const auto all_faces = polymesh::fea::boundary_surface_faces(v.mesh);
        const std::size_t n_bnd = polymesh::fea::count_boundary_nodes(all_faces);
        const auto fix_sel = polymesh::fea::select_cantilever_end(
            v.mesh, all_faces, n_bnd, fix_box.region(), xmin, xmax, tol, -1);
        const auto load_sel = polymesh::fea::select_cantilever_end(
            v.mesh, all_faces, n_bnd, load_box.region(), xmin, xmax, tol, +1);
        const auto load_faces =
            load_spec.traction_mode
                ? polymesh::fea::pressure_aligned_faces(v.mesh, load_sel.faces, load_spec.dir)
                : load_sel.faces;
        std::printf("bc: fix %zu nodes%s | load %zu nodes, %zu faces%s | %zu boundary nodes "
                    "(sane-selection minimum %zu)\n",
                    fix_sel.nodes.size(), selection_note(fix_sel, true).c_str(),
                    load_sel.nodes.size(), load_faces.size(),
                    selection_note(load_sel, false).c_str(), n_bnd,
                    polymesh::fea::sane_selection_minimum(n_bnd));
        polymesh::fea::Dirichlet bc;
        for (const auto n : fix_sel.nodes) {
            bc.fix_node(n);
        }
        if (const auto defect = polymesh::fea::constraint_defect(v.mesh, fix_sel.nodes);
            !defect.empty()) {
            throw std::runtime_error(std::format(
                "solve: the fixture selection is geometrically degenerate — {}. Select a "
                "real face with --fix-box x0 y0 z0 x1 y1 z1.",
                defect));
        }
        auto loads = polymesh::fea::assemble_selection_load(
            v.mesh, load_faces, load_sel.nodes, load_spec, "solve", stdout, load_sel.region,
            exact_pressure_area);
        phase_log.bc_s += SolvePhaseLog::since(bc_start);
        return std::pair{std::move(bc), std::move(loads)};
    };

    polymesh::fea::SolveOptions solve_options;
    solve_options.method = solve_method;
    solve_options.max_mem_gb = max_mem_gb;
    solve_options.on_note = [](std::string_view note) {
        std::printf("solve: %.*s\n", static_cast<int>(note.size()), note.data());
    };

    Eigen::VectorXd u;
    polymesh::fea::ZzRecovery zz;
    polymesh::fea::LinearConstraints curved_constraints;

    // Which elements get promoted. Default (selective) promotes only the
    // ZZ-smooth-marked subset, so an "order 2" run is really mixed p=1/p=2 with
    // a quadratic fraction that varies per case and per h. Gmsh delivers a
    // uniformly quadratic mesh, so --p-elevate-uniform promotes every promotable
    // linear element and makes an order-2 peer comparison a true parity run.
    const auto elevate_targets = [&]() {
        if (!p_elevate_uniform) {
            return polymesh::adapt::mark_smooth(zz.element_eta, 0.3);
        }
        std::vector<std::size_t> eligible;
        eligible.reserve(vol.mesh.elements.size());
        for (std::size_t e = 0; e < vol.mesh.elements.size(); ++e) {
            const auto type = vol.mesh.elements[e].type;
            if (type == polymesh::fea::ElementType::kTet4 ||
                type == polymesh::fea::ElementType::kHex8) {
                eligible.push_back(e);
            }
        }
        return eligible;
    };
    // The discretisation that produced a row must be readable off the output and
    // off the row's note, never inferred from element counts.
    const char* const p_elevate_mode = p_elevate_uniform ? "uniform" : "selective";
    const auto report_p_elevate = [&](std::size_t n_promoted, std::size_t n0) {
        const auto counts = polymesh::fea::count_element_types(vol.mesh);
        // Wording pinned: the peer matrix parses this line for per-row counts.
        std::printf("p-elevate: %zu smooth, nodes %zu→%zu (tet10=%zu hex20=%zu)\n", n_promoted,
                    n0, vol.mesh.nodes.size(), counts.tet10, counts.hex20);
        std::printf("p-elevate-mode: %s\n", p_elevate_mode);
        vol.mesher_note += std::format(" | p-elevate={} promoted={} tet10={} hex20={}",
                                       p_elevate_mode, n_promoted, counts.tet10, counts.hex20);
    };
    const auto curve_final_geometry = [&]() {
        if (!curved || !model || !model->cad) {
            return false;
        }
        const std::size_t n0 = vol.mesh.nodes.size();
        auto shaped = polymesh::pipeline::curve_volume_geometry(*model, vol.mesh, h_use);
        curved_constraints = std::move(shaped.constraints);
        vol.mesh = std::move(shaped.mesh);
        vol.boundary_quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        vol.mesher_note += std::format(
            " | curved_volume promoted={} pyramid_split={} projected={} partial={} "
            "reverted={} h_refined={}",
            shaped.n_promoted, shaped.n_pyramids_split, shaped.n_projected, shaped.n_partial,
            shaped.n_reverted, shaped.n_h_refined);
        report_p_elevate(shaped.n_promoted, n0);
        return true;
    };
    // One place where a solve happens, so the phase account and the stress
    // recovery that always follows it cannot drift apart between the two
    // call sites (adaptive pass and final promoted geometry).
    const auto run_solve = [&](const polymesh::fea::Dirichlet& bc,
                               const Eigen::VectorXd& loads,
                               const polymesh::fea::LinearConstraints* mpc) {
        auto result =
            polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads, solve_options, mpc);
        phase_log.add(result.phases);
        u = std::move(result.u);
        const auto stress_start = SolvePhaseLog::Clock::now();
        zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
        phase_log.stress_s += SolvePhaseLog::since(stress_start);
    };
    const auto solve_with_final_geometry = [&]() {
        bool promoted = curve_final_geometry();
        if (!promoted && p_elevate) {
            const auto targets = elevate_targets();
            if (!targets.empty()) {
                const std::size_t n0 = vol.mesh.nodes.size();
                vol.mesh = polymesh::fea::p_elevate(vol.mesh, targets);
                project_quadratic_mids();
                report_p_elevate(targets.size(), n0);
                promoted = true;
            }
        }
        if (!promoted) {
            return;
        }
        vol.mesh.check_validity();
        auto [bc2, loads2] = make_bc_loads(vol);
        if (bc2.dof_values.empty()) {
            throw std::runtime_error("solve: no fixture nodes after curved promotion");
        }
        if (model) {
            polymesh::pipeline::update_solved_geometry_volume(*model, vol);
        }
        run_solve(bc2, loads2, curved_constraints.empty() ? nullptr : &curved_constraints);
    };
    // Only the promoted final geometry's answer is reported. The linear solve
    // and its ZZ recovery exist to choose p-elevation targets and to drive
    // `--adapt`; when promotion is unconditional (curved CAD, or uniform
    // p-elevation with promotable cells) and there is no adaptive pass or η
    // target to serve, nothing reads them, so the linear pre-solve is skipped.
    const bool promotion_is_unconditional = [&] {
        if (curved && model && model->cad) {
            return true;
        }
        if (!p_elevate || !p_elevate_uniform) {
            return false; // selective p-elevation picks targets from ZZ η
        }
        return std::any_of(vol.mesh.elements.begin(), vol.mesh.elements.end(),
                           [](const polymesh::fea::NodalElement& el) {
                               return el.type == polymesh::fea::ElementType::kTet4 ||
                                      el.type == polymesh::fea::ElementType::kHex8;
                           });
    }();
    const bool skip_linear_presolve =
        adapt_passes == 0 && eta_target == 0.0 && promotion_is_unconditional;
    for (int pass = 0; pass <= adapt_passes; ++pass) {
        if (pass > 0) {
            auto m = mesher;
            if (!seeds.empty() && mesher == polymesh::pipeline::VolumeMesher::kTetFill) {
                m = polymesh::pipeline::VolumeMesher::kGradedTet;
            }
            vol = mesh_now(m);
            vol.mesh.check_validity();
        }
        if (skip_linear_presolve) {
            solve_with_final_geometry();
            break;
        }
        auto [bc, loads] = make_bc_loads(vol);
        if (bc.dof_values.empty()) {
            std::fputs("solve: no fixture nodes found\n", stderr);
            return 1;
        }
        if (model) {
            polymesh::pipeline::update_solved_geometry_volume(*model, vol);
        }

        run_solve(bc, loads, nullptr);
        const bool last_pass =
            (pass == adapt_passes) || (eta_target > 0.0 && zz.global_eta <= eta_target);
        if (last_pass) {
            if (eta_target > 0.0 && zz.global_eta <= eta_target) {
                std::printf("eta-target stop: η=%.4g ≤ %.4g at pass %d/%d\n", zz.global_eta,
                            eta_target, pass, adapt_passes);
            }
            solve_with_final_geometry();
            break;
        }
        if (pass < adapt_passes) {
            std::vector<Eigen::Vector3d> cents;
            cents.reserve(vol.mesh.elements.size());
            for (const auto& el : vol.mesh.elements) {
                Eigen::Vector3d c = Eigen::Vector3d::Zero();
                for (auto n : el.nodes) {
                    c += vol.mesh.nodes[n];
                }
                cents.push_back(c / static_cast<double>(el.nodes.size()));
            }
            const auto sug = polymesh::adapt::suggest_refine(cents, zz.element_eta, h_use, 0.3,
                                                             0.75, h * 0.35);
            if (sug.n_marked == 0 && sug.h_next >= h_use * 0.98) {
                solve_with_final_geometry();
                break;
            }
            h_use = sug.h_next;
            seeds = sug.refine_seeds;
            seed_band = sug.seed_band;
        }
    }

    std::vector<double> vm(zz.nodal_stress.size());
    double max_vm = 0.0, max_u = 0.0;
    double max_principal = -std::numeric_limits<double>::infinity();
    double diag_energy = 0.0, zz_energy = 0.0;
    Eigen::Vector3d max_principal_dir = Eigen::Vector3d::Zero();
    for (std::size_t i = 0; i < vm.size(); ++i) {
        vm[i] = polymesh::fea::von_mises(zz.nodal_stress[i]);
        max_vm = std::max(max_vm, vm[i]);
        max_u = std::max(max_u, u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm());
        const auto& s = zz.nodal_stress[i];
        diag_energy += s[0] * s[0] + s[1] * s[1] + s[2] * s[2];
        zz_energy += s[2] * s[2];
        Eigen::Matrix3d sigma;
        sigma << s[0], s[5], s[4], s[5], s[1], s[3], s[4], s[3], s[2];
        const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(sigma);
        for (Eigen::Index k = 0; k < 3; ++k) {
            const double principal = es.eigenvalues()[k];
            if (principal > max_principal) {
                max_principal = principal;
                max_principal_dir = es.eigenvectors().col(k);
            }
        }
    }
    const double sigma_zz_share = diag_energy > 0.0 ? std::sqrt(zz_energy / diag_energy) : 0.0;

    std::vector<polymesh::fea::VtuPointData> pdata;
    pdata.push_back({.name = "von_Mises", .scalars = vm, .vectors = {}});
    pdata.push_back({.name = "displacement", .scalars = {}, .vectors = u});
    const auto quality = polymesh::fea::tet4_cell_quality(vol.mesh);
    std::vector<polymesh::fea::VtuCellData> cdata;
    cdata.push_back({.name = "quality", .scalars = quality});
    const auto export_start = SolvePhaseLog::Clock::now();
    polymesh::fea::write_vtu(out_path, vol.mesh, pdata, cdata);
    phase_log.export_s = SolvePhaseLog::since(export_start);

    std::printf("solve: %zu nodes, %zu elems | max von Mises %.4g Pa | max |u| %.4g m | "
                "ZZ η %.4g | h=%.4g | seeds=%zu\n%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), max_vm, max_u, zz.global_eta,
                h_use, seeds.size(), resolved.note.c_str(), vol.mesher_note.c_str());
    std::printf("stress direction: max principal %.6g Pa along (%.4f %.4f %.4f); "
                "σzz RMS share of normal stress = %.6f\n",
                max_principal, max_principal_dir.x(), max_principal_dir.y(),
                max_principal_dir.z(), sigma_zz_share);
    std::printf("wrote %s\n", out_path.c_str());
    phase_log.report();
    return 0;
}

} // namespace polymesh::cli
