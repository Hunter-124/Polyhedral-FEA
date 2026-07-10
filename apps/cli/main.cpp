// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.

#include "adapt/loop.hpp"
#include "fea/backend.hpp"
#include "fea/material.hpp"
#include "fea/solve.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace {

int usage() {
    std::fputs("usage: polymesh <command> [args]\n"
               "\n"
               "commands:\n"
               "  check <file.stl|.step>     validate surface geometry\n"
               "  mesh  <file> [-h m] [-o out.vtu] [--mesher name] [--skin n] [--feature]\n"
               "                             volume mesh; optional VTU write\n"
               "  solve <file> -o out.vtu [-h m] [-E Pa] [-nu r]\n"
               "              [--mesher name] [--skin n] [--feature] [--adapt n]\n"
               "              [--eta-target η]\n"
               "                             mesh + cantilever-style BCs + VTU\n"
               "                             (fix min-x face nodes, load +Fy on max-x)\n"
               "  backend                    print compute backend\n"
               "\n"
               "mesher names: tet (default), hex, hexvem|vem, graded, hexpyr|transition\n"
               "--skin n: graded-tet fine skin layers (default 2)\n"
               "--feature: refine graded mesh near sharp edges (default off in CLI)\n"
               "--adapt n: ZZ→Dörfler remesh passes (local seeds on graded path)\n"
               "--eta-target η: stop adapt when global ZZ η ≤ η (0=off; needs --adapt)\n",
               stderr);
    return 2;
}

polymesh::pipeline::VolumeMesher parse_mesher(const std::string& m) {
    if (m == "hex") {
        return polymesh::pipeline::VolumeMesher::kHexFill;
    }
    if (m == "hexvem" || m == "vem") {
        return polymesh::pipeline::VolumeMesher::kHexVem;
    }
    if (m == "graded") {
        return polymesh::pipeline::VolumeMesher::kGradedTet;
    }
    if (m == "hexpyr" || m == "transition") {
        return polymesh::pipeline::VolumeMesher::kHexPyramid;
    }
    return polymesh::pipeline::VolumeMesher::kTetFill;
}

polymesh::geom::TriSurface load_surface(std::string_view path) {
    const std::string p(path);
    std::string lower = p;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower.ends_with(".step") || lower.ends_with(".stp")) {
        return polymesh::geom::load_step(p);
    }
    return polymesh::geom::load_stl(p);
}

int cmd_check(std::string_view input) {
    const auto surface = load_surface(input);
    surface.validate();
    std::printf("%.*s: OK — %zu vertices, %zu triangles\n", static_cast<int>(input.size()),
                input.data(), surface.vertices.size(), surface.triangles.size());
    return 0;
}

int cmd_mesh(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    std::string out_path;
    auto mesher = polymesh::pipeline::VolumeMesher::kTetFill;
    int skin = 2;
    bool feature = false;
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
            feature = true;
        } else {
            return usage();
        }
    }
    const auto model = polymesh::pipeline::Model::load(path);
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    if (h <= 0.0) {
        h = extent / 16.0;
    }
    auto vol = polymesh::pipeline::volume_mesh(model, h, mesher, skin, feature);
    vol.mesh.check_validity();
    std::printf("mesh: %zu nodes, %zu elems, h=%.6g m\n%s\n", vol.mesh.nodes.size(),
                vol.mesh.elements.size(), h, vol.mesher_note.c_str());
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
    auto mesher = polymesh::pipeline::VolumeMesher::kTetFill;
    int skin = 2;
    bool feature = false;
    int adapt_passes = 0;
    double eta_target = 0.0;
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
            feature = true;
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
        } else {
            return usage();
        }
    }
    if (out_path.empty()) {
        std::fputs("solve: -o out.vtu is required\n", stderr);
        return 2;
    }

    const auto model = polymesh::pipeline::Model::load(path);
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    if (h <= 0.0) {
        h = extent / 12.0;
    }

    double h_use = h;
    std::vector<Eigen::Vector3d> seeds;
    double seed_band = 0.0;
    auto mesh_now = [&](polymesh::pipeline::VolumeMesher m) {
        return polymesh::pipeline::volume_mesh(model, h_use, m, skin, feature, seeds,
                                               seed_band);
    };
    auto vol = mesh_now(mesher);
    vol.mesh.check_validity();

    const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
    auto make_bc_loads = [&](const polymesh::pipeline::VolumeMeshOutput& v) {
        const double xmin = model.bbox_min[0];
        const double xmax = model.bbox_max[0];
        const double tol = 0.51 * h_use;
        polymesh::fea::Dirichlet bc;
        std::vector<std::uint32_t> load_nodes;
        for (std::uint32_t i = 0; i < v.mesh.nodes.size(); ++i) {
            const double x = v.mesh.nodes[i][0];
            if (x <= xmin + tol) {
                bc.fix_node(i);
            }
            if (x >= xmax - tol) {
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
        if (eta_target > 0.0 && zz.global_eta <= eta_target) {
            std::printf("eta-target stop: η=%.4g ≤ %.4g at pass %d/%d\n", zz.global_eta,
                        eta_target, pass, adapt_passes);
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
                "ZZ η %.4g | h=%.4g | seeds=%zu\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), max_vm, max_u, zz.global_eta,
                h_use, seeds.size(), vol.mesher_note.c_str());
    std::printf("wrote %s\n", out_path.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
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
        if (command == "backend" && args.size() == 2) {
            std::printf("%s\n", polymesh::fea::backend_description().c_str());
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return usage();
}
