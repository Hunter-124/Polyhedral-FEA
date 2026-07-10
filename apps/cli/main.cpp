// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.

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
               "  mesh  <file> [-h m] [-o out.vtu]\n"
               "                             tet-fill mesh; optional VTU write\n"
               "  solve <file> -o out.vtu [-h m] [-E Pa] [-nu r]\n"
               "                             mesh + cantilever-style BCs + VTU\n"
               "                             (fix min-x face nodes, load +Fy on max-x)\n"
               "  backend                    print compute backend\n",
               stderr);
    return 2;
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
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            const std::string m = args[++i];
            if (m == "hex") {
                mesher = polymesh::pipeline::VolumeMesher::kHexFill;
            } else if (m == "hexvem" || m == "vem") {
                mesher = polymesh::pipeline::VolumeMesher::kHexVem;
            } else if (m == "graded") {
                mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
            } else {
                mesher = polymesh::pipeline::VolumeMesher::kTetFill;
            }
        } else {
            return usage();
        }
    }
    const auto model = polymesh::pipeline::Model::load(path);
    const double extent = (model.bbox_max - model.bbox_min).maxCoeff();
    if (h <= 0.0) {
        h = extent / 16.0;
    }
    auto vol = polymesh::pipeline::volume_mesh(model, h, mesher, 2);
    vol.mesh.check_validity();
    std::printf("mesh: %zu nodes, %zu elems, h=%.6g m\n%s\n", vol.mesh.nodes.size(),
                vol.mesh.elements.size(), h, vol.mesher_note.c_str());
    if (!out_path.empty()) {
        polymesh::fea::write_vtu(out_path, vol.mesh);
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
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-o") == 0 && i + 1 < args.size()) {
            out_path = args[++i];
        } else if (std::strcmp(args[i], "-E") == 0 && i + 1 < args.size()) {
            E = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "-nu") == 0 && i + 1 < args.size()) {
            nu = std::atof(args[++i]);
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
    auto vol = polymesh::pipeline::volume_mesh(model, h,
                                               polymesh::pipeline::VolumeMesher::kTetFill, 2);
    vol.mesh.check_validity();

    // Auto BCs: fix nodes near min-x plane; load +Y on max-x plane nodes.
    const double xmin = model.bbox_min[0];
    const double xmax = model.bbox_max[0];
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
    if (bc.dof_values.empty()) {
        std::fputs("solve: no fixture nodes found\n", stderr);
        return 1;
    }
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
    if (!load_nodes.empty()) {
        const Eigen::Vector3d f(0.0, 1000.0 / static_cast<double>(load_nodes.size()), 0.0);
        for (auto n : load_nodes) {
            loads.segment<3>(3 * static_cast<Eigen::Index>(n)) += f;
        }
    }

    const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
    const auto u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads);
    const auto zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
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
    polymesh::fea::write_vtu(out_path, vol.mesh, pdata);

    std::printf("solve: %zu nodes, %zu elems | max von Mises %.4g Pa | max |u| %.4g m | "
                "ZZ η %.4g\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), max_vm, max_u, zz.global_eta);
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
