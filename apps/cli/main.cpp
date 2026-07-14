// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.

#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/loop.hpp"
#include "fea/backend.hpp"
#include "fea/material.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/step.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <span>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::fputs("usage: polymesh <command> [args]\n"
               "\n"
               "commands:\n"
               "  check <part.step|.brep>    validate CAD geometry\n"
               "  mesh  <part> [-h m] [-o out.vtu] [--mesher name] [--skin n]\n"
               "              [--no-feature] [--element-tendency t]\n"
               "              [--fix-box x0 y0 z0 x1 y1 z1] [--load-box x0 y0 z0 x1 y1 z1]\n"
               "                             geometry+BC-aware volume mesh; optional VTU\n"
               "  solve <part> -o out.vtu [-h m] [-E Pa] [-nu r]\n"
               "              [--mesher name] [--skin n] [--no-feature] [--adapt n]\n"
               "              [--eta-target η] [--p-elevate] [--element-tendency t]\n"
               "              [--fix-box ...6] [--load-box ...6] [--bc-grade]\n"
               "                             mesh + BCs + VTU. Default BCs: fix min-x,\n"
               "                             load +Fy on max-x. Boxes override selection.\n"
               "  diag  <part> [-h m] [--mesher name] [--json out.json] [--no-solve]\n"
               "                             JSON diagnostics: fidelity, quality, timings\n"
               "  backend                    print compute backend + OpenMP/opt summary\n"
               "\n"
               "inputs: CAD only (.step .stp .brep .brp). STL is no longer supported.\n"
               "mesh size: omit -h (or -h 0) for auto h0 from bbox + feature density\n"
               "mesher names: hybrid|zoo (default), varyhedron|vary (CAD packing),\n"
               "              hybridvem, tet, hex, hexvem|vem, graded, hexpyr|transition,\n"
               "              prism|sweep, octa|octahedral (experimental)\n"
               "--skin n: graded fine skin layers (default 2)\n"
               "--no-feature: disable geometry (curvature/thin-wall) grading (default on)\n"
               "--element-tendency t: shape dial in [-1,+1] (hex↔fan hybrid↔poly VEM↔tet)\n"
               "--fix-box / --load-box: BC/load selection AABBs; the mesh grades finer\n"
               "              toward them (loads finest) — geometry + simulation setup\n"
               "--adapt n: ZZ→Dörfler remesh passes (local seeds on graded path)\n"
               "--eta-target η: stop adapt when global ZZ η ≤ η (0=off; needs --adapt)\n"
               "--p-elevate: promote smooth tet4/hex8 → tet10/hex20 (auto-on --adapt>0)\n"
               "--bc-grade: force a-priori BC grading from the default cantilever faces\n",
               stderr);
    return 2;
}

polymesh::pipeline::VolumeMesher parse_mesher(const std::string& m) {
    if (m == "hybrid" || m == "zoo" || m == "mixed") {
        return polymesh::pipeline::VolumeMesher::kHybrid;
    }
    if (m == "hybridvem" || m == "hybrid-vem" || m == "hybrid_vem") {
        return polymesh::pipeline::VolumeMesher::kHybridVem;
    }
    if (m == "tet") {
        return polymesh::pipeline::VolumeMesher::kTetFill;
    }
    if (m == "hex") {
        return polymesh::pipeline::VolumeMesher::kHexFill;
    }
    if (m == "hexvem" || m == "vem") {
        return polymesh::pipeline::VolumeMesher::kHexVem;
    }
    if (m == "graded") {
        return polymesh::pipeline::VolumeMesher::kGradedTet;
    }
    if (m == "varyhedron" || m == "vary") {
        return polymesh::pipeline::VolumeMesher::kVaryhedron;
    }
    if (m == "cvt_poly" || m == "cvt" || m == "restricted_cvt") {
        return polymesh::pipeline::VolumeMesher::kCvtPoly;
    }
    if (m == "hexpyr" || m == "transition") {
        return polymesh::pipeline::VolumeMesher::kHexPyramid;
    }
    if (m == "prism" || m == "sweep") {
        return polymesh::pipeline::VolumeMesher::kPrismSweep;
    }
    if (m == "octa" || m == "octahedral") {
        return polymesh::pipeline::VolumeMesher::kOctahedral;
    }
    return polymesh::pipeline::VolumeMesher::kHybrid;
}

struct BoxSel {
    bool set = false;
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
};

// Parse the 6 numbers following a --fix-box / --load-box flag at args[i].
// On success advances i past the 6 values and sets `b`; returns false on error.
bool parse_box6(std::span<char*> args, std::size_t& i, BoxSel& b) {
    if (i + 6 >= args.size()) {
        return false;
    }
    double v[6];
    for (int k = 0; k < 6; ++k) {
        v[static_cast<std::size_t>(k)] =
            std::atof(args[i + 1 + static_cast<std::size_t>(k)]);
    }
    b.lo = Eigen::Vector3d(std::min(v[0], v[3]), std::min(v[1], v[4]), std::min(v[2], v[5]));
    b.hi = Eigen::Vector3d(std::max(v[0], v[3]), std::max(v[1], v[4]), std::max(v[2], v[5]));
    b.set = true;
    i += 6;
    return true;
}

// Geometry+BC refine regions from optional fix/load boxes (loads finest).
std::vector<polymesh::pipeline::RefineRegion> make_regions(const BoxSel& fix, const BoxSel& load) {
    std::vector<polymesh::pipeline::RefineRegion> regions;
    if (load.set) {
        regions.push_back({load.lo, load.hi, 0.25});
    }
    if (fix.set) {
        regions.push_back({fix.lo, fix.hi, 0.5});
    }
    return regions;
}

int cmd_check(std::string_view input) {
    const auto model = polymesh::pipeline::Model::load(std::string(input));
    const auto& surface = model.surface;
    surface.validate();
    std::printf("%.*s: OK — %zu vertices, %zu triangles%s\n", static_cast<int>(input.size()),
                input.data(), surface.vertices.size(), surface.triangles.size(),
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
    auto mesher = polymesh::pipeline::VolumeMesher::kHybrid;
    int skin = 2;
    bool feature = true; // geometry (curvature/thin-wall) grading on by default
    double element_tendency = 0.0;
    BoxSel fix_box, load_box;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            mesher = parse_mesher(args[++i]);
        } else if (std::strcmp(args[i], "--skin") == 0 && i + 1 < args.size()) {
            skin = std::atoi(args[++i]);
            if (skin < 1) {
                skin = 1;
            }
        } else if (std::strcmp(args[i], "--feature") == 0) {
            feature = true; // accepted for back-compat (now the default)
        } else if (std::strcmp(args[i], "--no-feature") == 0) {
            feature = false;
        } else if (std::strcmp(args[i], "--element-tendency") == 0 && i + 1 < args.size()) {
            element_tendency = std::atof(args[++i]);
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
    const auto model = polymesh::pipeline::Model::load(path);
    const auto resolved = polymesh::pipeline::resolve_mesh_size(model, h);
    h = resolved.h;

    // Geometry + simulation-setup (BC/load box) aware refinement plan → seeds.
    const auto regions = make_regions(fix_box, load_box);
    const auto plan = polymesh::pipeline::build_refinement_plan(model, h, regions, feature);
    auto vol = polymesh::pipeline::volume_mesh(model, h, mesher, skin, feature,
                                               plan.refine_seeds, plan.seed_band,
                                               element_tendency);
    vol.mesh.check_validity();
    std::printf("mesh: %zu nodes, %zu elems, h=%.6g m\n"
                "refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m\n"
                "%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), h, plan.n_geometry_seeds,
                plan.n_bc_seeds, plan.refine_seeds.size(), plan.seed_band, plan.h_fine,
                resolved.note.c_str(), vol.mesher_note.c_str());
    if (!out_path.empty()) {
        const auto quality = polymesh::fea::tet4_cell_quality(vol.mesh);
        std::vector<polymesh::fea::VtuCellData> cdata;
        cdata.push_back({.name = "quality", .scalars = quality});
        polymesh::fea::write_vtu(out_path, vol.mesh, {}, cdata);
        std::printf("wrote %s\n", out_path.c_str());
    }
    return 0;
}

int cmd_solve(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    double E = 200e9;
    double nu = 0.3;
    std::string out_path;
    auto mesher = polymesh::pipeline::VolumeMesher::kHybrid;
    int skin = 2;
    bool feature = true; // geometry grading on by default (CAD)
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = false;
    double element_tendency = 0.0;
    bool bc_grade = false;
    BoxSel fix_box, load_box;
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
            mesher = parse_mesher(args[++i]);
        } else if (std::strcmp(args[i], "--skin") == 0 && i + 1 < args.size()) {
            skin = std::atoi(args[++i]);
            if (skin < 1) {
                skin = 1;
            }
        } else if (std::strcmp(args[i], "--feature") == 0) {
            feature = true; // accepted for back-compat (now the default)
        } else if (std::strcmp(args[i], "--no-feature") == 0) {
            feature = false;
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
        } else if (std::strcmp(args[i], "--bc-grade") == 0) {
            bc_grade = true;
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

    const auto model = polymesh::pipeline::Model::load(path);
    const auto resolved = polymesh::pipeline::resolve_mesh_size(model, h);
    h = resolved.h;

    double h_use = h;
    std::vector<Eigen::Vector3d> seeds;
    double seed_band = 0.0;
    // Geometry + simulation-setup refinement. Explicit --fix-box/--load-box
    // define the grading (and BC) regions; otherwise --bc-grade derives the
    // default cantilever slabs (fix min-x, load max-x). Geometry grading
    // (curvature / thin-wall) applies whenever --feature is on (default).
    std::vector<polymesh::pipeline::RefineRegion> regions;
    {
        const double xmin = model.bbox_min[0];
        const double xmax = model.bbox_max[0];
        const double slab = 0.51 * h_use;
        if (load_box.set) {
            regions.push_back({load_box.lo, load_box.hi, 0.25});
        } else if (bc_grade) {
            Eigen::Vector3d lo = model.bbox_min, hi = model.bbox_max;
            lo[0] = xmax - slab;
            regions.push_back({lo, hi, 0.25});
        }
        if (fix_box.set) {
            regions.push_back({fix_box.lo, fix_box.hi, 0.5});
        } else if (bc_grade) {
            Eigen::Vector3d lo = model.bbox_min, hi = model.bbox_max;
            hi[0] = xmin + slab;
            regions.push_back({lo, hi, 0.5});
        }
    }
    {
        const auto plan = polymesh::pipeline::build_refinement_plan(model, h_use, regions, feature);
        seeds = plan.refine_seeds;
        seed_band = plan.seed_band;
        std::printf("refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m\n",
                    plan.n_geometry_seeds, plan.n_bc_seeds, seeds.size(), seed_band, plan.h_fine);
    }
    auto mesh_now = [&](polymesh::pipeline::VolumeMesher m) {
        return polymesh::pipeline::volume_mesh(model, h_use, m, skin, feature, seeds,
                                               seed_band, element_tendency);
    };
    auto vol = mesh_now(mesher);
    vol.mesh.check_validity();

    const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
    auto in_box = [](const BoxSel& b, const Eigen::Vector3d& p) {
        return p.x() >= b.lo.x() && p.x() <= b.hi.x() && p.y() >= b.lo.y() &&
               p.y() <= b.hi.y() && p.z() >= b.lo.z() && p.z() <= b.hi.z();
    };
    auto make_bc_loads = [&](const polymesh::pipeline::VolumeMeshOutput& v) {
        const double xmin = model.bbox_min[0];
        const double xmax = model.bbox_max[0];
        const double tol = 0.51 * h_use;
        polymesh::fea::Dirichlet bc;
        std::vector<std::uint32_t> load_nodes;
        for (std::uint32_t i = 0; i < v.mesh.nodes.size(); ++i) {
            const Eigen::Vector3d& p = v.mesh.nodes[i];
            const bool fixed = fix_box.set ? in_box(fix_box, p) : (p.x() <= xmin + tol);
            const bool loaded = load_box.set ? in_box(load_box, p) : (p.x() >= xmax - tol);
            if (fixed) {
                bc.fix_node(i);
            }
            if (loaded) {
                load_nodes.push_back(i);
            }
        }
        Eigen::VectorXd loads =
            Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(v.mesh.nodes.size()));
        if (!load_nodes.empty()) {
            const Eigen::Vector3d f(0.0, 1000.0 / static_cast<double>(load_nodes.size()), 0.0);
            for (auto n : load_nodes) {
                loads.segment<3>(3 * static_cast<Eigen::Index>(n)) += f;
            }
        }
        return std::pair{std::move(bc), std::move(loads)};
    };

    Eigen::VectorXd u;
    polymesh::fea::ZzRecovery zz;
    for (int pass = 0; pass <= adapt_passes; ++pass) {
        if (pass > 0) {
            auto m = mesher;
            if (!seeds.empty() && mesher == polymesh::pipeline::VolumeMesher::kTetFill) {
                m = polymesh::pipeline::VolumeMesher::kGradedTet;
            }
            vol = mesh_now(m);
            vol.mesh.check_validity();
        }
        auto [bc, loads] = make_bc_loads(vol);
        if (bc.dof_values.empty()) {
            std::fputs("solve: no fixture nodes found\n", stderr);
            return 1;
        }
        u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads);
        zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
        const bool last_pass =
            (pass == adapt_passes) || (eta_target > 0.0 && zz.global_eta <= eta_target);
        if (last_pass) {
            if (eta_target > 0.0 && zz.global_eta <= eta_target) {
                std::printf("eta-target stop: η=%.4g ≤ %.4g at pass %d/%d\n", zz.global_eta,
                            eta_target, pass, adapt_passes);
            }
            if (p_elevate) {
                const auto smooth = polymesh::adapt::mark_smooth(zz.element_eta, 0.3);
                if (!smooth.empty()) {
                    const auto n0 = vol.mesh.nodes.size();
                    vol.mesh = polymesh::fea::p_elevate(vol.mesh, smooth);
                    vol.mesh.check_validity();
                    auto [bc2, loads2] = make_bc_loads(vol);
                    if (bc2.dof_values.empty()) {
                        std::fputs("solve: no fixture nodes after p-elevate\n", stderr);
                        return 1;
                    }
                    u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc2, loads2);
                    zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
                    const auto counts = polymesh::fea::count_element_types(vol.mesh);
                    std::printf("p-elevate: %zu smooth, nodes %zu→%zu (tet10=%zu hex20=%zu)\n",
                                smooth.size(), n0, vol.mesh.nodes.size(), counts.tet10,
                                counts.hex20);
                }
            }
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
                if (p_elevate) {
                    const auto smooth = polymesh::adapt::mark_smooth(zz.element_eta, 0.3);
                    if (!smooth.empty()) {
                        vol.mesh = polymesh::fea::p_elevate(vol.mesh, smooth);
                        vol.mesh.check_validity();
                        auto [bc2, loads2] = make_bc_loads(vol);
                        u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc2, loads2);
                        zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
                    }
                }
                break;
            }
            h_use = sug.h_next;
            seeds = sug.refine_seeds;
            seed_band = sug.seed_band;
        }
    }

    std::vector<double> vm(zz.nodal_stress.size());
    double max_vm = 0.0, max_u = 0.0;
    for (std::size_t i = 0; i < vm.size(); ++i) {
        vm[i] = polymesh::fea::von_mises(zz.nodal_stress[i]);
        max_vm = std::max(max_vm, vm[i]);
        max_u = std::max(max_u, u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm());
    }

    std::vector<polymesh::fea::VtuPointData> pdata;
    pdata.push_back({.name = "von_Mises", .scalars = vm, .vectors = {}});
    pdata.push_back({.name = "displacement", .scalars = {}, .vectors = u});
    const auto quality = polymesh::fea::tet4_cell_quality(vol.mesh);
    std::vector<polymesh::fea::VtuCellData> cdata;
    cdata.push_back({.name = "quality", .scalars = quality});
    polymesh::fea::write_vtu(out_path, vol.mesh, pdata, cdata);

    std::printf("solve: %zu nodes, %zu elems | max von Mises %.4g Pa | max |u| %.4g m | "
                "ZZ η %.4g | h=%.4g | seeds=%zu\n%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), max_vm, max_u, zz.global_eta,
                h_use, seeds.size(), resolved.note.c_str(), vol.mesher_note.c_str());
    std::printf("wrote %s\n", out_path.c_str());
    return 0;
}

// Structured diagnostics: import fidelity, mesh quality, and phase timings as
// JSON — the profiler output and the measurement feed for the self-improve loop
// (scripts/self_improve.sh). Optionally runs a default cantilever solve too.
int cmd_diag(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    auto mesher = polymesh::pipeline::VolumeMesher::kVaryhedron;
    bool do_solve = true;
    std::string json_path;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            mesher = parse_mesher(args[++i]);
        } else if (std::strcmp(args[i], "--json") == 0 && i + 1 < args.size()) {
            json_path = args[++i];
        } else if (std::strcmp(args[i], "--no-solve") == 0) {
            do_solve = false;
        } else {
            return usage();
        }
    }
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };

    auto t0 = clock::now();
    const auto model = polymesh::pipeline::Model::load(path);
    const double import_ms = ms(clock::now() - t0);
    const double bbox_diag = (model.bbox_max - model.bbox_min).norm();

    const auto resolved = polymesh::pipeline::resolve_mesh_size(model, h);
    h = resolved.h;
    // Diagnostics run at a coarse, representative resolution: cap auto-h so a
    // curvature-fine auto size doesn't explode the quick battery. A user -h is
    // always respected.
    if (resolved.auto_chosen && bbox_diag > 0.0 && h < bbox_diag / 12.0) {
        h = bbox_diag / 12.0;
    }
    const auto plan = polymesh::pipeline::build_refinement_plan(model, h, {}, /*use_geometry=*/true);

    t0 = clock::now();
    auto vol = polymesh::pipeline::volume_mesh(model, h, mesher, 2, true, plan.refine_seeds,
                                               plan.seed_band);
    const double mesh_ms = ms(clock::now() - t0);
    vol.mesh.check_validity();

    const auto quality = polymesh::fea::tet4_cell_quality(vol.mesh);
    double q_min = 1.0, q_sum = 0.0;
    std::size_t q_n = 0;
    for (const double q : quality) {
        if (q > 0.0) {
            q_min = std::min(q_min, q);
            q_sum += q;
            ++q_n;
        }
    }
    const double q_mean = q_n > 0 ? q_sum / static_cast<double>(q_n) : 0.0;

    double max_vm = 0.0, max_u = 0.0, global_eta = 0.0, solve_ms = 0.0;
    std::size_t dof = 0;
    bool solved = false;
    if (do_solve) {
        const double xmin = model.bbox_min[0], xmax = model.bbox_max[0];
        const double tol = 0.51 * h;
        polymesh::fea::Dirichlet bc;
        std::vector<std::uint32_t> load_nodes;
        for (std::uint32_t i = 0; i < vol.mesh.nodes.size(); ++i) {
            const double x = vol.mesh.nodes[i][0];
            if (x <= xmin + tol) {
                bc.fix_node(i);
            }
            if (x >= xmax - tol) {
                load_nodes.push_back(i);
            }
        }
        if (!bc.dof_values.empty() && !load_nodes.empty()) {
            Eigen::VectorXd loads =
                Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
            const Eigen::Vector3d f(0.0, 1000.0 / static_cast<double>(load_nodes.size()), 0.0);
            for (auto n : load_nodes) {
                loads.segment<3>(3 * static_cast<Eigen::Index>(n)) += f;
            }
            const polymesh::fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
            t0 = clock::now();
            const Eigen::VectorXd uu = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads);
            const auto zz = polymesh::fea::recover_zz(vol.mesh, mat, uu);
            solve_ms = ms(clock::now() - t0);
            global_eta = zz.global_eta;
            dof = 3 * vol.mesh.nodes.size();
            for (std::size_t i = 0; i < zz.nodal_stress.size(); ++i) {
                max_vm = std::max(max_vm, polymesh::fea::von_mises(zz.nodal_stress[i]));
                max_u = std::max(max_u,
                                 uu.segment<3>(3 * static_cast<Eigen::Index>(i)).norm());
            }
            solved = true;
        }
    }

    const std::string mesher_name = [&] {
        switch (mesher) {
        case polymesh::pipeline::VolumeMesher::kVaryhedron: return "varyhedron";
        case polymesh::pipeline::VolumeMesher::kGradedTet: return "graded";
        case polymesh::pipeline::VolumeMesher::kTetFill: return "tet";
        case polymesh::pipeline::VolumeMesher::kHybrid: return "hybrid";
        default: return "other";
        }
    }();
    const double mesh_throughput =
        mesh_ms > 0.0 ? static_cast<double>(vol.mesh.elements.size()) / (mesh_ms / 1000.0) : 0.0;

    const std::string json = std::format(
        "{{\n"
        "  \"part\": \"{}\",\n"
        "  \"mesher\": \"{}\",\n"
        "  \"import\": {{ \"vertices\": {}, \"triangles\": {}, \"bbox_diag\": {:.6g}, "
        "\"cad_brep\": {} }},\n"
        "  \"mesh\": {{ \"h\": {:.6g}, \"nodes\": {}, \"elements\": {}, "
        "\"quality_min\": {:.4g}, \"quality_mean\": {:.4g}, "
        "\"geometry_seeds\": {}, \"bc_seeds\": {} }},\n"
        "  \"timing_ms\": {{ \"import\": {:.3f}, \"mesh\": {:.3f}, \"solve\": {:.3f} }},\n"
        "  \"mesh_throughput_elem_per_s\": {:.1f},\n"
        "  \"solve\": {{ \"ran\": {}, \"dof\": {}, \"max_von_mises\": {:.6g}, "
        "\"max_disp\": {:.6g}, \"global_eta\": {:.6g} }},\n"
        "  \"mesher_note\": \"{}\"\n"
        "}}\n",
        model.name, mesher_name, model.surface.vertices.size(), model.surface.triangles.size(),
        bbox_diag, model.cad ? "true" : "false", h, vol.mesh.nodes.size(),
        vol.mesh.elements.size(), q_min, q_mean, plan.n_geometry_seeds, plan.n_bc_seeds,
        import_ms, mesh_ms, solve_ms, mesh_throughput, solved ? "true" : "false", dof, max_vm,
        max_u, global_eta,
        resolved.note);

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

} // namespace

int main(int argc, char** argv) {
    polymesh::fea::init_runtime_performance();
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2) {
        return usage();
    }
    const std::string_view command = args[1];
    try {
        if (command == "check" && args.size() == 3) {
            return cmd_check(args[2]);
        }
        if (command == "mesh") {
            return cmd_mesh(args);
        }
        if (command == "solve") {
            return cmd_solve(args);
        }
        if (command == "diag") {
            return cmd_diag(args);
        }
        if (command == "backend" && args.size() == 2) {
            polymesh::fea::init_runtime_performance();
            std::printf("%s\n", polymesh::fea::performance_description().c_str());
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
