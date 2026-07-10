// SPDX-License-Identifier: BSD-3-Clause

// D6 Tier-3 instrument: uniform tet10 vs geometrically graded tet10 on the
// L-domain energy problem (ADR-0005 baseline). Same assembly/solver; only the
// mesh density strategy changes. Emits JSON (full suite + summary) to -o/stdout.
//
// Graded path uses Babuška-style geometric layers toward the re-entrant corner
// so h_min near the singularity can match a fine uniform mesh while far-field
// cells stay coarse → fewer DOFs at comparable energy.
//
// Not a ctest multi-minute gate — invoked by bench/d6/run_tier3.py.

#include "bench/reference_case.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace polymesh::fea;
using json = nlohmann::json;

namespace {

void usage() {
    std::fputs("usage: polymesh-d6-tier3 [options]\n"
               "\n"
               "D6 L-domain: uniform tet10 vs geometric graded tet10 (same solver).\n"
               "\n"
               "options:\n"
               "  -o PATH          write JSON to PATH (default: stdout)\n"
               "  --quick          smaller grids (CI-friendly, ~seconds)\n"
               "  --label LABEL    scoreboard label (default: d6-tier3)\n"
               "  --help           this help\n",
               stderr);
}

std::vector<double> uniform_line(int n_cells, double a, double b) {
    std::vector<double> xs(static_cast<std::size_t>(n_cells) + 1u);
    for (int i = 0; i <= n_cells; ++i) {
        xs[static_cast<std::size_t>(i)] =
            a + (b - a) * static_cast<double>(i) / static_cast<double>(n_cells);
    }
    return xs;
}

/// Geometric layers from `focus` toward `end`: first spacing h0, then ×rho each layer.
std::vector<double> geometric_layers(double focus, double end, double h0, double rho,
                                     int max_layers) {
    std::vector<double> pts;
    pts.push_back(focus);
    if (h0 <= 0.0 || max_layers <= 0) {
        pts.push_back(end);
        return pts;
    }
    const double sign = (end >= focus) ? 1.0 : -1.0;
    const double len = std::abs(end - focus);
    double pos = focus;
    double h = h0;
    for (int i = 0; i < max_layers; ++i) {
        if (std::abs(pos - focus) + h >= len - 1e-14) {
            pts.push_back(end);
            return pts;
        }
        pos += sign * h;
        pts.push_back(pos);
        h *= rho;
    }
    if (std::abs(pts.back() - end) > 1e-12) {
        pts.push_back(end);
    } else {
        pts.back() = end;
    }
    return pts;
}

std::vector<double> merge_unique(std::vector<double> a, const std::vector<double>& b) {
    a.insert(a.end(), b.begin(), b.end());
    std::sort(a.begin(), a.end());
    constexpr double tol = 1e-12;
    std::vector<double> out;
    out.reserve(a.size());
    for (double x : a) {
        if (out.empty() || std::abs(x - out.back()) > tol) {
            out.push_back(x);
        }
    }
    return out;
}

/// Graded 1D line: geometric layers from focus covering [a,b], optionally merged
/// with a coarse uniform background so mid-domain is never empty.
std::vector<double> graded_line(double a, double b, double focus, double h0, double rho,
                                int max_layers, int n_coarse_bg) {
    std::vector<double> pts;
    if (focus > a + 1e-15) {
        auto left = geometric_layers(focus, a, h0, rho, max_layers);
        std::reverse(left.begin(), left.end());
        pts = merge_unique(pts, left);
    } else {
        pts.push_back(a);
    }
    if (focus < b - 1e-15) {
        auto right = geometric_layers(focus, b, h0, rho, max_layers);
        pts = merge_unique(pts, right);
    } else if (pts.empty() || std::abs(pts.back() - b) > 1e-12) {
        pts.push_back(b);
    }
    if (n_coarse_bg > 0) {
        pts = merge_unique(pts, uniform_line(n_coarse_bg, a, b));
    }
    // Ensure endpoints exact.
    if (!pts.empty()) {
        pts.front() = a;
        pts.back() = b;
    }
    return pts;
}

std::uint32_t add_node(NodalMesh& mesh,
                       std::map<std::array<std::int64_t, 3>, std::uint32_t>& lookup, double x,
                       double y, double z) {
    const auto key =
        std::array<std::int64_t, 3>{static_cast<std::int64_t>(std::llround(x * 1e9)),
                                    static_cast<std::int64_t>(std::llround(y * 1e9)),
                                    static_cast<std::int64_t>(std::llround(z * 1e9))};
    const auto [it, inserted] =
        lookup.try_emplace(key, static_cast<std::uint32_t>(mesh.nodes.size()));
    if (inserted) {
        mesh.nodes.emplace_back(x, y, z);
    }
    return it->second;
}

void push_hex_as_tets(NodalMesh& mesh, const std::array<std::uint32_t, 8>& c) {
    constexpr std::array<std::array<int, 4>, 6> kTets{
        {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}}};
    for (const auto& t : kTets) {
        mesh.elements.push_back(NodalElement{
            ElementType::kTet4,
            {c[static_cast<std::size_t>(t[0])], c[static_cast<std::size_t>(t[1])],
             c[static_cast<std::size_t>(t[2])], c[static_cast<std::size_t>(t[3])]}});
    }
}

void fill_block_tets(NodalMesh& mesh,
                     std::map<std::array<std::int64_t, 3>, std::uint32_t>& lookup,
                     const std::vector<double>& xs, const std::vector<double>& ys,
                     const std::vector<double>& zs) {
    const int nx = static_cast<int>(xs.size()) - 1;
    const int ny = static_cast<int>(ys.size()) - 1;
    const int nz = static_cast<int>(zs.size()) - 1;
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const std::array<std::uint32_t, 8> c = {
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i)],
                             ys[static_cast<std::size_t>(j)], zs[static_cast<std::size_t>(k)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i + 1)],
                             ys[static_cast<std::size_t>(j)], zs[static_cast<std::size_t>(k)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i + 1)],
                             ys[static_cast<std::size_t>(j + 1)],
                             zs[static_cast<std::size_t>(k)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i)],
                             ys[static_cast<std::size_t>(j + 1)],
                             zs[static_cast<std::size_t>(k)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i)],
                             ys[static_cast<std::size_t>(j)],
                             zs[static_cast<std::size_t>(k + 1)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i + 1)],
                             ys[static_cast<std::size_t>(j)],
                             zs[static_cast<std::size_t>(k + 1)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i + 1)],
                             ys[static_cast<std::size_t>(j + 1)],
                             zs[static_cast<std::size_t>(k + 1)]),
                    add_node(mesh, lookup, xs[static_cast<std::size_t>(i)],
                             ys[static_cast<std::size_t>(j + 1)],
                             zs[static_cast<std::size_t>(k + 1)])};
                push_hex_as_tets(mesh, c);
            }
        }
    }
}

/// Uniform L tet mesh: structured n×(2n) + n×n blocks (same topology as Tier-1 L test).
NodalMesh make_l_uniform(int n, double L, double w, double thickness) {
    const int nz = std::max(1, n / 2);
    auto xs_a = uniform_line(n, 0.0, w);
    auto ys_a = uniform_line(2 * n, 0.0, L);
    auto zs = uniform_line(nz, 0.0, thickness);
    auto xs_b = uniform_line(n, w, L);
    auto ys_b = uniform_line(n, w, L);
    NodalMesh mesh;
    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    fill_block_tets(mesh, lookup, xs_a, ys_a, zs);
    fill_block_tets(mesh, lookup, xs_b, ys_b, zs);
    return mesh;
}

/// Geometric graded L mesh: h0 near re-entrant corner (w,w), growth `rho`.
NodalMesh make_l_graded(double h0, double rho, int max_layers, int n_coarse_bg, int nz,
                        double L, double w, double thickness) {
    auto xs_a = graded_line(0.0, w, w, h0, rho, max_layers, n_coarse_bg);
    auto ys_a = graded_line(0.0, L, w, h0, rho, max_layers, std::max(1, 2 * n_coarse_bg));
    auto zs = uniform_line(std::max(1, nz), 0.0, thickness);
    auto xs_b = graded_line(w, L, w, h0, rho, max_layers, n_coarse_bg);
    auto ys_b = graded_line(w, L, w, h0, rho, max_layers, n_coarse_bg);
    NodalMesh mesh;
    std::map<std::array<std::int64_t, 3>, std::uint32_t> lookup;
    fill_block_tets(mesh, lookup, xs_a, ys_a, zs);
    fill_block_tets(mesh, lookup, xs_b, ys_b, zs);
    return mesh;
}

NodalMesh promote_tet4_to_tet10(const NodalMesh& mesh) {
    constexpr std::array<std::array<int, 2>, 6> kTetEdges{
        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
    NodalMesh out;
    out.nodes = mesh.nodes;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
    const auto midpoint = [&](std::uint32_t a, std::uint32_t b) {
        const auto key = std::minmax(a, b);
        const auto [it, inserted] =
            midpoints.try_emplace(key, static_cast<std::uint32_t>(out.nodes.size()));
        if (inserted) {
            out.nodes.push_back(0.5 * (out.nodes[a] + out.nodes[b]));
        }
        return it->second;
    };
    for (const auto& element : mesh.elements) {
        if (element.type != ElementType::kTet4) {
            throw FeaError("d6: expected tet4 before promote");
        }
        NodalElement promoted;
        promoted.type = ElementType::kTet10;
        promoted.nodes = element.nodes;
        for (const auto& e : kTetEdges) {
            promoted.nodes.push_back(midpoint(element.nodes[static_cast<std::size_t>(e[0])],
                                              element.nodes[static_cast<std::size_t>(e[1])]));
        }
        out.elements.push_back(std::move(promoted));
    }
    return out;
}

struct SolveOut {
    double energy = 0.0;
    int free_dofs = 0;
    int nnodes = 0;
    int nelems = 0;
    double mesh_s = 0.0;
    double solve_s = 0.0;
    double total_s = 0.0;
};

SolveOut solve_l_mesh(NodalMesh linear, const polymesh::bench::ReferenceCase& ref) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();

    const double L = ref.values.at("arm_length_m");
    const double traction = ref.values.at("traction_pa");
    const Material material{.youngs_modulus = ref.values.at("youngs_modulus_pa"),
                            .poissons_ratio = ref.values.at("poissons_ratio")};

    NodalMesh mesh = promote_tet4_to_tet10(linear);
    mesh.check_validity();
    const auto t_mesh = clock::now();

    std::vector<SurfaceFace> load_faces;
    for (const auto& el : mesh.elements) {
        const auto& nodes = el.nodes;
        const auto on_end = [&](std::uint32_t id) {
            return std::abs(mesh.nodes[id][0] - L) < 1e-9;
        };
        struct Face {
            std::array<int, 3> c;
            std::array<int, 3> m;
        };
        // Tet10 mids: edges (0,1),(1,2),(0,2),(0,3),(1,3),(2,3) → 4..9
        const std::array<Face, 4> faces{{
            {{0, 1, 2}, {4, 5, 6}},
            {{0, 1, 3}, {4, 8, 7}},
            {{0, 2, 3}, {6, 9, 7}},
            {{1, 2, 3}, {5, 9, 8}},
        }};
        for (const auto& f : faces) {
            if (on_end(nodes[static_cast<std::size_t>(f.c[0])]) &&
                on_end(nodes[static_cast<std::size_t>(f.c[1])]) &&
                on_end(nodes[static_cast<std::size_t>(f.c[2])])) {
                load_faces.push_back({FaceType::kTri6,
                                      {nodes[static_cast<std::size_t>(f.c[0])],
                                       nodes[static_cast<std::size_t>(f.c[1])],
                                       nodes[static_cast<std::size_t>(f.c[2])],
                                       nodes[static_cast<std::size_t>(f.m[0])],
                                       nodes[static_cast<std::size_t>(f.m[1])],
                                       nodes[static_cast<std::size_t>(f.m[2])]}});
            }
        }
    }
    if (load_faces.empty()) {
        throw FeaError("d6: no traction faces on x=L");
    }

    Dirichlet bc;
    for (std::size_t i = 0; i < mesh.nodes.size(); ++i) {
        const auto& q = mesh.nodes[i];
        const auto ni = static_cast<Eigen::Index>(i);
        if (std::abs(q[0]) < 1e-9) {
            bc.fix_node(static_cast<std::uint32_t>(i));
        }
        bc.dof_values[3 * ni + 2] = 0.0;
    }

    const int free_dofs =
        3 * static_cast<int>(mesh.nodes.size()) - static_cast<int>(bc.dof_values.size());

    const auto loads = assemble_traction_load(mesh, load_faces, [&](const Eigen::Vector3d&) {
        return Eigen::Vector3d(traction, 0.0, 0.0);
    });
    const auto u = solve_elastostatics(mesh, material, bc, loads);
    const double energy = strain_energy(mesh, material, u);
    const auto t1 = clock::now();

    SolveOut out;
    out.energy = energy;
    out.free_dofs = free_dofs;
    out.nnodes = static_cast<int>(mesh.nodes.size());
    out.nelems = static_cast<int>(mesh.elements.size());
    out.mesh_s = std::chrono::duration<double>(t_mesh - t0).count();
    out.solve_s = std::chrono::duration<double>(t1 - t_mesh).count();
    out.total_s = std::chrono::duration<double>(t1 - t0).count();
    return out;
}

json make_row(const std::string& case_id, const std::string& label,
              const std::string& path_name, const SolveOut& s, double energy_ref,
              const std::string& notes) {
    const double deficit =
        energy_ref > 0.0 ? std::max(0.0, (energy_ref - s.energy) / energy_ref * 100.0) : 0.0;
    return json{
        {"schema_version", 1},
        {"solver", "PolyMesh"},
        {"version", "0.1.0"},
        {"case_id", case_id},
        {"dofs", s.free_dofs},
        {"wall_time_s", {{"mesh", s.mesh_s}, {"solve", s.solve_s}, {"total", s.total_s}}},
        {"accuracy",
         {{"name", "energy_deficit_pct"}, {"value", deficit}, {"unit", "percent"}}},
        {"label", label},
        {"path", path_name},
        {"nnodes", s.nnodes},
        {"nelems", s.nelems},
        {"strain_energy_j", s.energy},
        {"notes", notes},
    };
}

std::string utc_now() {
    using clock = std::chrono::system_clock;
    const auto t = clock::now();
    const std::time_t tt = clock::to_time_t(t);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

struct Spec {
    const char* path; // "uniform_tet10" | "graded_tet10"
    const char* tag;
    // uniform:
    int n = 0;
    // graded:
    double h0 = 0.0;
    double rho = 0.0;
    int layers = 0;
    int n_bg = 0;
    int nz = 1;
};

} // namespace

int main(int argc, char** argv) {
    std::string out_path;
    std::string label = "d6-tier3";
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        }
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (std::strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    const auto ref = polymesh::bench::load_reference("bench/reference/l-domain.json");
    const double L = ref.values.at("arm_length_m");
    const double w = ref.values.at("arm_width_m");
    const double thickness = ref.values.at("thickness_m");
    const std::string ts = utc_now();

    // Uniform n: h ≈ w/n on the arm width. Graded uses same h0 = w/n_ref near
    // the corner with geometric growth so far-field is coarse.
    std::vector<Spec> specs;
    if (quick) {
        specs = {
            {"uniform_tet10", "n2", 2, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n4", 4, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n6", 6, 0, 0, 0, 0, 0},
            // h0 = w/6 ≈ uniform n6 near corner; rho=2, few layers
            {"graded_tet10", "h0=w/6_rho2", 0, w / 6.0, 2.0, 8, 0, 1},
            {"graded_tet10", "h0=w/8_rho2", 0, w / 8.0, 2.0, 10, 0, 1},
            {"graded_tet10", "h0=w/8_rho1.8_bg2", 0, w / 8.0, 1.8, 10, 2, 1},
        };
    } else {
        specs = {
            {"uniform_tet10", "n2", 2, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n3", 3, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n4", 4, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n6", 6, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n8", 8, 0, 0, 0, 0, 0},
            {"uniform_tet10", "n10", 10, 0, 0, 0, 0, 0},
            {"graded_tet10", "h0=w/6_rho2", 0, w / 6.0, 2.0, 10, 0, 1},
            {"graded_tet10", "h0=w/8_rho2", 0, w / 8.0, 2.0, 12, 0, 1},
            {"graded_tet10", "h0=w/10_rho2", 0, w / 10.0, 2.0, 14, 0, 1},
            {"graded_tet10", "h0=w/12_rho2", 0, w / 12.0, 2.0, 14, 0, 1},
            {"graded_tet10", "h0=w/8_rho1.7", 0, w / 8.0, 1.7, 14, 0, 1},
            {"graded_tet10", "h0=w/10_rho1.7_bg2", 0, w / 10.0, 1.7, 14, 2, 2},
            {"graded_tet10", "h0=w/12_rho1.6_bg2", 0, w / 12.0, 1.6, 16, 2, 2},
            {"graded_tet10", "h0=w/16_rho1.8", 0, w / 16.0, 1.8, 16, 0, 1},
        };
    }

    struct Run {
        Spec spec;
        SolveOut s;
    };
    std::vector<Run> runs;
    runs.reserve(specs.size());

    for (const auto& sp : specs) {
        std::fprintf(stderr, "d6: solving %s %s ...\n", sp.path, sp.tag);
        std::fflush(stderr);
        NodalMesh linear;
        if (std::strcmp(sp.path, "uniform_tet10") == 0) {
            linear = make_l_uniform(sp.n, L, w, thickness);
        } else {
            linear = make_l_graded(sp.h0, sp.rho, sp.layers, sp.n_bg, sp.nz, L, w, thickness);
        }
        if (linear.elements.empty()) {
            std::fprintf(stderr, "d6: skip empty mesh %s\n", sp.tag);
            continue;
        }
        runs.push_back({sp, solve_l_mesh(std::move(linear), ref)});
        std::fprintf(stderr, "d6:   E=%.6e  dofs=%d  nelems=%d  total=%.3fs\n",
                     runs.back().s.energy, runs.back().s.free_dofs, runs.back().s.nelems,
                     runs.back().s.total_s);
    }

    double e_ref = 0.0;
    for (const auto& r : runs) {
        e_ref = std::max(e_ref, r.s.energy);
    }

    // Finest uniform (largest n) as primary baseline.
    const Run* baseline = nullptr;
    for (const auto& r : runs) {
        if (std::strcmp(r.spec.path, "uniform_tet10") == 0) {
            if (baseline == nullptr || r.spec.n > baseline->spec.n) {
                baseline = &r;
            }
        }
    }

    // Equal-accuracy match (Galerkin energy from below): graded matches a
    // uniform level when E_graded >= E_uniform * (1 - tol). Energy on this
    // singular problem only spans ~0.7% from n2→n10, so tol must be tight
    // (0.01%) or coarse graded meshes spuriously "match" fine uniforms.
    constexpr double kTol = 1e-4; // 0.01% relative energy
    const Run* best_g = nullptr;
    const Run* best_u = nullptr;
    double best_ratio = 0.0;

    for (const auto& g : runs) {
        if (std::strcmp(g.spec.path, "graded_tet10") != 0) {
            continue;
        }
        for (const auto& u : runs) {
            if (std::strcmp(u.spec.path, "uniform_tet10") != 0) {
                continue;
            }
            if (g.s.energy + 1e-30 < u.s.energy * (1.0 - kTol)) {
                continue; // graded energy worse than this uniform
            }
            if (g.s.free_dofs >= u.s.free_dofs) {
                continue; // no DOF win
            }
            const double ratio =
                static_cast<double>(u.s.free_dofs) / static_cast<double>(g.s.free_dofs);
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best_g = &g;
                best_u = &u;
            }
        }
    }

    // Fallback: highest-energy graded vs finest uniform (even if energy short).
    if (best_g == nullptr) {
        for (const auto& g : runs) {
            if (std::strcmp(g.spec.path, "graded_tet10") != 0) {
                continue;
            }
            if (best_g == nullptr || g.s.energy > best_g->s.energy) {
                best_g = &g;
            }
        }
        best_u = baseline;
        if (best_g != nullptr && best_u != nullptr) {
            best_ratio = static_cast<double>(best_u->s.free_dofs) /
                         static_cast<double>(std::max(1, best_g->s.free_dofs));
        }
    }

    json summary = json::object();
    std::string claim = "no_pair";
    double time_ratio = 0.0;
    if (best_g != nullptr && best_u != nullptr) {
        time_ratio = best_u->s.total_s / std::max(1e-12, best_g->s.total_s);
        if (best_g->s.energy + 1e-30 >= best_u->s.energy * (1.0 - kTol) &&
            best_g->s.free_dofs < best_u->s.free_dofs) {
            claim = std::string("graded_energy_ge_uniform_") + best_u->spec.tag +
                    "_with_fewer_dofs";
        } else {
            claim = "fallback_finest_uniform_vs_best_graded";
        }
        summary = {
            {"baseline_path", best_u->spec.path},
            {"baseline_tag", best_u->spec.tag},
            {"baseline_dofs", best_u->s.free_dofs},
            {"baseline_energy_j", best_u->s.energy},
            {"baseline_total_s", best_u->s.total_s},
            {"graded_path", best_g->spec.path},
            {"graded_tag", best_g->spec.tag},
            {"graded_dofs", best_g->s.free_dofs},
            {"graded_energy_j", best_g->s.energy},
            {"graded_total_s", best_g->s.total_s},
            {"dof_ratio_uniform_over_graded", best_ratio},
            {"time_ratio_uniform_over_graded", time_ratio},
            {"tier3_dof_target", 5.0},
            {"tier3_time_target", 3.0},
            {"meets_dof_target", best_ratio >= 5.0},
            {"meets_time_target", time_ratio >= 3.0},
            {"claim", claim},
            {"energy_ref_j", e_ref},
        };
        std::fprintf(stderr,
                     "d6 summary: dof_ratio=%.3f time_ratio=%.3f claim=%s "
                     "meets_dof=%s meets_time=%s\n",
                     best_ratio, time_ratio, claim.c_str(), best_ratio >= 5.0 ? "yes" : "no",
                     time_ratio >= 3.0 ? "yes" : "no");
    }

    json rows = json::array();
    for (const auto& r : runs) {
        const std::string notes = std::string("L-domain tet10 ") + r.spec.path + " " +
                                  r.spec.tag + " nelems=" + std::to_string(r.s.nelems);
        auto row = make_row("l-domain", label, r.spec.path, r.s, e_ref, notes);
        row["timestamp"] = ts;
        row["mesh_tag"] = r.spec.tag;
        if (r.spec.n > 0) {
            row["mesh_n"] = r.spec.n;
        }
        if (r.spec.h0 > 0.0) {
            row["mesh_h0"] = r.spec.h0;
            row["mesh_rho"] = r.spec.rho;
        }
        rows.push_back(std::move(row));
    }

    if (best_u != nullptr && best_g != nullptr) {
        auto base_row =
            make_row("l-domain-d6-baseline", label, "uniform_tet10", best_u->s, e_ref,
                     std::string("D6 uniform tet10 baseline ") + best_u->spec.tag);
        base_row["timestamp"] = ts;
        base_row["mesh_tag"] = best_u->spec.tag;
        rows.push_back(std::move(base_row));

        auto grad_row = make_row("l-domain-d6-graded", label, "graded_tet10", best_g->s, e_ref,
                                 std::string("D6 graded tet10 ") + best_g->spec.tag +
                                     " geometric layers toward re-entrant corner");
        grad_row["timestamp"] = ts;
        grad_row["mesh_tag"] = best_g->spec.tag;
        rows.push_back(std::move(grad_row));
    }

    json root = {
        {"schema_version", 1}, {"label", label},     {"timestamp", ts},
        {"case", "l-domain"},  {"summary", summary}, {"results", rows},
    };

    const std::string text = root.dump(2);
    if (out_path.empty()) {
        std::fputs(text.c_str(), stdout);
        std::fputc('\n', stdout);
    } else {
        std::ofstream ofs(out_path);
        if (!ofs) {
            std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
            return 1;
        }
        ofs << text << '\n';
    }
    return 0;
}
