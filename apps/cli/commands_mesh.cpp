// SPDX-License-Identifier: BSD-3-Clause

// `polymesh check` and `polymesh mesh`.

#include "cli_common.hpp"

#include "fea/boundary_faces.hpp"
#include "fea/vtu.hpp"
#include "pipeline/scene.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::cli {

int cmd_check(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double scale = 1.0;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "--scale") == 0) {
            if (!parse_scale(args, i, scale)) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    const auto model = polymesh::pipeline::Model::load(path, 30.0, scale);
    const auto& surface = model.surface;
    surface.validate();
    report_scale(scale);
    std::printf("%s: OK — %zu vertices, %zu triangles%s\n", path.c_str(),
                surface.vertices.size(), surface.triangles.size(),
                model.cad ? " (CAD BRep retained)" : "");
    return 0;
}

int cmd_mesh(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    std::string out_path;
    auto mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    int skin = 2;
    bool feature = true;  // geometry (curvature/thin-wall) grading on by default
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    double element_tendency = 0.0;
    bool curved = true; // exact curved CAD solve/export geometry (ADR-0035)
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    BoxSel fix_box, load_box;
    double scale = 1.0;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
            out_path = args[++i];
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
        } else if (std::strcmp(args[i], "--no-curved") == 0) {
            curved = false;
        } else if (std::strcmp(args[i], "--no-feature") == 0) {
            feature = false;
        } else if (std::strcmp(args[i], "--spectral") == 0) {
            spectral = true; // accepted for symmetry (now the default)
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
        } else if (std::strcmp(args[i], "--scale") == 0) {
            if (!parse_scale(args, i, scale)) {
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
        } else if (std::strcmp(args[i], "--fix-box") == 0) {
            if (!parse_box6(args, i, fix_box)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--load-box") == 0) {
            if (!parse_box6(args, i, load_box)) {
                return usage();
            }
        } else {
            return usage();
        }
    }
    const auto model = polymesh::pipeline::Model::load(path, 30.0, scale);
    report_scale(scale);
    const auto resolved =
        polymesh::pipeline::resolve_mesh_size(model, h, 30.0, max_elems, max_dof);
    h = resolved.h;

    // Geometry + simulation-setup (BC/load box) aware refinement plan → seeds.
    const auto regions = make_regions(fix_box, load_box);
    const auto plan =
        polymesh::pipeline::build_refinement_plan(model, h, regions, feature, spectral, 0);
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, mesher, skin, feature, plan.refine_seeds, plan.seed_band, element_tendency,
        resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
        plan.size_field);
    if (curved) {
        auto shaped = polymesh::pipeline::curve_volume_geometry(model, vol.mesh, h);
        vol.mesh = std::move(shaped.mesh);
        vol.boundary_quads = polymesh::fea::extract_boundary_faces(vol.mesh);
        vol.mesher_note += std::format(
            " | curved_volume promoted={} pyramid_split={} projected={} partial={} "
            "reverted={} h_refined={}",
            shaped.n_promoted, shaped.n_pyramids_split, shaped.n_projected, shaped.n_partial,
            shaped.n_reverted, shaped.n_h_refined);
    }
    vol.mesh.check_validity();
    std::printf("mesh: %zu nodes, %zu elems, h=%.6g m\n"
                "refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m, "
                "geo_curv=%s\n"
                "%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), h, plan.n_geometry_seeds,
                plan.n_bc_seeds, plan.refine_seeds.size(), plan.seed_band, plan.h_fine,
                plan.geometry_curvature_from_brep ? "brep" : "tessellation",
                resolved.note.c_str(), vol.mesher_note.c_str());
    if (plan.spectral.applied) {
        std::printf("spectral: %zu/%zu modes kept (%.2f%% energy), %zu denoised edge-curve "
                    "seeds, N_pred %.4g → %.4g%s\n",
                    plan.spectral.modes_kept, plan.spectral.modes_total,
                    100.0 * plan.spectral.energy_kept, plan.spectral.n_edge_curve_seeds,
                    plan.spectral.predicted_before, plan.spectral.predicted_after,
                    plan.spectral.budget_met ? "" : " (budget not met — geometry floor)");
    }
    if (!out_path.empty()) {
        const auto quality = polymesh::fea::tet4_cell_quality(vol.mesh);
        std::vector<polymesh::fea::VtuCellData> cdata;
        cdata.push_back({.name = "quality", .scalars = quality});
        polymesh::fea::write_vtu(out_path, vol.mesh, {}, cdata);
        std::printf("wrote %s\n", out_path.c_str());
    }
    return 0;
}

} // namespace polymesh::cli
