// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.

#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/loop.hpp"
#include "fea/backend.hpp"
#include "fea/cell_quality.hpp"
#include "fea/material.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/step.hpp"
#include "mesh/tet_fill.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
               "              [--load-dir x y z] [--force N] [--traction Pa]\n"
               "                             mesh + BCs + VTU. Default BCs: fix min-x,\n"
               "                             load max-x. Boxes override selection.\n"
               "  diag  <part> [-h m] [--mesher name] [--json out.json] [--no-solve]\n"
               "              [--fix-box ...6] [--load-box ...6]\n"
               "              [--load-dir x y z] [--force N] [--traction Pa]\n"
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
               "--load-dir x y z: load direction (normalized; default 0 1 0)\n"
               "--force N: total resultant force in newtons over the loaded faces\n"
               "              (default 1000); applied as a consistent traction ∫Nᵗt dS\n"
               "--traction Pa: pressure magnitude instead of a total force — resultant\n"
               "              is Pa × loaded-face area. Last of --force/--traction wins\n"
               "--adapt n: ZZ→Dörfler remesh passes (local seeds on graded path)\n"
               "--eta-target η: stop adapt when global ZZ η ≤ η (0=off; needs --adapt)\n"
               "--p-elevate: promote smooth tet4/hex8 → tet10/hex20 (auto-on --adapt>0)\n"
               "--bc-grade: force a-priori BC grading from the default cantilever faces\n"
               "\n"
               "default BC selection: nodes in a 0.51·h slab at min-x (fixed) / max-x\n"
               "              (loaded). If a slab captures too few nodes to act as a\n"
               "              face (curved parts), selection falls back to boundary\n"
               "              faces whose outward normal aligns with ∓x/±x.\n",
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

// How much of the requested load the CLI applies, and where it points.
struct LoadSpec {
    Eigen::Vector3d dir{0.0, 1.0, 0.0}; // unit, default +y (historical CLI load)
    double force = 1000.0;              // total resultant, N (historical default)
    double traction_pa = 0.0;           // > 0: pressure mode, resultant = Pa * area
};

// Parse --load-dir / --force / --traction at args[i]; advances i past values.
// Returns false when the flag is unknown or its values are missing/invalid.
bool parse_load_flag(std::span<char*> args, std::size_t& i, LoadSpec& spec) {
    if (std::strcmp(args[i], "--load-dir") == 0) {
        if (i + 3 >= args.size()) {
            return false;
        }
        const Eigen::Vector3d d(std::atof(args[i + 1]), std::atof(args[i + 2]),
                                std::atof(args[i + 3]));
        const double n = d.norm();
        if (!(n > 0.0) || !std::isfinite(n)) {
            std::fputs("--load-dir: direction must be a nonzero finite vector\n", stderr);
            return false;
        }
        spec.dir = d / n;
        i += 3;
        return true;
    }
    if (std::strcmp(args[i], "--force") == 0 && i + 1 < args.size()) {
        spec.force = std::atof(args[++i]);
        spec.traction_pa = 0.0; // last of --force/--traction wins
        return true;
    }
    if (std::strcmp(args[i], "--traction") == 0 && i + 1 < args.size()) {
        spec.traction_pa = std::atof(args[++i]);
        return true;
    }
    return false;
}

// A default BC slab must be broad enough to behave like a face. 12 nodes is
// ~4 boundary faces — the smallest patch that carries a traction instead of a
// point force — and 0.5% of the boundary nodes keeps that true on fine meshes,
// where 12 nodes is still a pinpoint. Below either bound the 0.51·h x-slab has
// degenerated (icecream_cone: 3 of 1890 nodes) and we switch to face selection.
constexpr std::size_t kMinSelNodes = 12;
constexpr double kMinSelFrac = 0.005;
// cos(~45°): a face counts as end-facing when its outward normal is within
// 45° of ±x. Matches the normal-aligned region machinery in scene.cpp.
constexpr double kNormalMinDot = 0.7;

struct BcSelection {
    std::vector<std::uint32_t> nodes;
    std::vector<polymesh::fea::SurfaceFace> faces;
    std::size_t slab_nodes = 0;    // what the plain 0.51·h slab captured
    bool face_fallback = false;    // slab was degenerate → normal-aligned faces
    double fallback_band = 0.0;    // end band as a fraction of the x extent
    bool from_box = false;
};

// Node count a default selection must reach to count as a face rather than a
// point/edge clamp.
std::size_t sane_selection_minimum(std::size_t n_boundary_nodes) {
    return std::max<std::size_t>(
        kMinSelNodes, static_cast<std::size_t>(
                          std::ceil(kMinSelFrac * static_cast<double>(n_boundary_nodes))));
}

// Provenance of a selection for the `bc:` report line.
std::string selection_note(const BcSelection& sel, bool is_fix) {
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

// Select one cantilever end. `end` is -1 for the min-x end (default fixture)
// and +1 for the max-x end (default load). An explicit box wins; otherwise the
// x-slab is used, with the normal-aligned boundary-face fallback (RANK 12).
BcSelection select_end(const polymesh::fea::NodalMesh& mesh,
                       const std::vector<polymesh::fea::SurfaceFace>& all_faces,
                       std::size_t n_boundary_nodes, const BoxSel& box, double xmin,
                       double xmax, double tol, int end) {
    BcSelection sel;
    if (box.set) {
        sel.from_box = true;
        for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
            const Eigen::Vector3d& p = mesh.nodes[i];
            if (p.x() >= box.lo.x() && p.x() <= box.hi.x() && p.y() >= box.lo.y() &&
                p.y() <= box.hi.y() && p.z() >= box.lo.z() && p.z() <= box.hi.z()) {
                sel.nodes.push_back(i);
            }
        }
        sel.faces = polymesh::fea::faces_within(all_faces, sel.nodes);
        return sel;
    }
    for (std::uint32_t i = 0; i < mesh.nodes.size(); ++i) {
        const double x = mesh.nodes[i].x();
        if (end < 0 ? (x <= xmin + tol) : (x >= xmax - tol)) {
            sel.nodes.push_back(i);
        }
    }
    sel.slab_nodes = sel.nodes.size();
    const auto need = sane_selection_minimum(n_boundary_nodes);
    if (sel.nodes.size() >= need) {
        sel.faces = polymesh::fea::faces_within(all_faces, sel.nodes);
        return sel;
    }
    // Degenerate slab: take the ±x-facing boundary faces near this end instead.
    // |n·x̂| (not the signed dot) because mixed hex/pyramid skins do not
    // guarantee outward winding; the end band keeps the patch at the end rather
    // than over the whole half. Start tight and widen only while the patch is
    // still too small to act as a face.
    sel.face_fallback = true;
    const double extent = xmax - xmin;
    for (const double frac : {0.10, 0.25, 0.50}) {
        sel.faces.clear();
        sel.nodes.clear();
        sel.fallback_band = frac;
        const double cut = end < 0 ? xmin + frac * extent : xmax - frac * extent;
        for (const auto& f : all_faces) {
            const Eigen::Vector3d n = polymesh::fea::surface_face_normal(mesh, f);
            if (n.squaredNorm() <= 0.0 || std::abs(n.x()) < kNormalMinDot) {
                continue;
            }
            Eigen::Vector3d c = Eigen::Vector3d::Zero();
            for (const auto id : f.nodes) {
                c += mesh.nodes[id];
            }
            c /= static_cast<double>(f.nodes.size());
            if (end < 0 ? (c.x() > cut) : (c.x() < cut)) {
                continue;
            }
            sel.faces.push_back(f);
            sel.nodes.insert(sel.nodes.end(), f.nodes.begin(), f.nodes.end());
        }
        std::sort(sel.nodes.begin(), sel.nodes.end());
        sel.nodes.erase(std::unique(sel.nodes.begin(), sel.nodes.end()), sel.nodes.end());
        if (sel.nodes.size() >= need) {
            break;
        }
    }
    return sel;
}

std::size_t count_boundary_nodes(const std::vector<polymesh::fea::SurfaceFace>& faces) {
    std::vector<std::uint32_t> ids;
    for (const auto& f : faces) {
        ids.insert(ids.end(), f.nodes.begin(), f.nodes.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids.size();
}

// Fully fixing 3 non-collinear nodes gives 9 constraints and removes all six
// rigid-body modes; 2 nodes (or any collinear set) leaves the rotation about
// their axis free and the stiffness matrix singular. Returns an empty string
// when the set is admissible, else the reason it is not.
std::string constraint_defect(const polymesh::fea::NodalMesh& mesh,
                              const std::vector<std::uint32_t>& fixed_nodes) {
    if (fixed_nodes.size() < 3) {
        return std::format("only {} fixed node(s) ({} constraints): fewer than the 3 "
                           "non-collinear nodes needed to remove all 6 rigid-body modes",
                           fixed_nodes.size(), 3 * fixed_nodes.size());
    }
    Eigen::Vector3d mean = Eigen::Vector3d::Zero();
    for (const auto n : fixed_nodes) {
        mean += mesh.nodes[n];
    }
    mean /= static_cast<double>(fixed_nodes.size());
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const auto n : fixed_nodes) {
        const Eigen::Vector3d d = mesh.nodes[n] - mean;
        cov += d * d.transpose();
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    const Eigen::Vector3d ev = es.eigenvalues(); // ascending
    if (!(ev[2] > 0.0)) {
        return "all fixed nodes are coincident";
    }
    // Second spread axis relative to the first: below 1e-6 the cloud is a line.
    if (std::sqrt(ev[1] / ev[2]) < 1e-6) {
        return std::format("the {} fixed nodes are collinear (spread ratio {:.3g}), leaving "
                           "rotation about that axis unconstrained",
                           fixed_nodes.size(), std::sqrt(ev[1] / ev[2]));
    }
    return {};
}

// Consistent (energy-conjugate) nodal loads for `spec` over `faces`, with the
// total-force conservation check reported to `report` (diag keeps stdout pure
// JSON). Throws when the selection cannot carry a surface load at all.
Eigen::VectorXd build_loads(const polymesh::fea::NodalMesh& mesh,
                            const std::vector<polymesh::fea::SurfaceFace>& faces,
                            const LoadSpec& spec, const char* what, std::FILE* report) {
    const double area = polymesh::fea::integrated_face_area(mesh, faces);
    if (!(area > 0.0)) {
        throw std::runtime_error(std::format(
            "{}: the load selection contains no complete boundary face (area 0) — a "
            "consistent traction cannot be applied. Widen --load-box or refine with -h.",
            what));
    }
    const double magnitude = spec.traction_pa > 0.0 ? spec.traction_pa * area : spec.force;
    const Eigen::Vector3d total = magnitude * spec.dir;
    auto load = polymesh::fea::consistent_face_load(mesh, faces, total);
    std::fprintf(report,
                 "load: %zu faces, area=%.9g m², %s → |F|=%.9g N along (%.4g %.4g %.4g) | "
                 "Σf=(%.9g %.9g %.9g) N, conservation err=%.3g N\n",
                 faces.size(), area,
                 spec.traction_pa > 0.0 ? std::format("t={:.6g} Pa", spec.traction_pa).c_str()
                                        : "total force",
                 total.norm(), spec.dir.x(), spec.dir.y(), spec.dir.z(), load.resultant.x(),
                 load.resultant.y(), load.resultant.z(), load.conservation_error);
    if (load.conservation_error > 1e-9 * std::max(1.0, total.norm())) {
        throw std::runtime_error(
            std::format("{}: traction assembly lost {:.3g} N of the requested {:.6g} N "
                        "resultant (total-force conservation check failed)",
                        what, load.conservation_error, total.norm()));
    }
    return std::move(load.loads);
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
    LoadSpec load_spec;
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
    auto make_bc_loads = [&](const polymesh::pipeline::VolumeMeshOutput& v) {
        const double xmin = model.bbox_min[0];
        const double xmax = model.bbox_max[0];
        const double tol = 0.51 * h_use;
        const auto all_faces = polymesh::fea::boundary_surface_faces(v.mesh);
        const std::size_t n_bnd = count_boundary_nodes(all_faces);
        const auto fix_sel =
            select_end(v.mesh, all_faces, n_bnd, fix_box, xmin, xmax, tol, -1);
        const auto load_sel =
            select_end(v.mesh, all_faces, n_bnd, load_box, xmin, xmax, tol, +1);
        std::printf("bc: fix %zu nodes%s | load %zu nodes, %zu faces%s | %zu boundary nodes "
                    "(sane-selection minimum %zu)\n",
                    fix_sel.nodes.size(), selection_note(fix_sel, true).c_str(),
                    load_sel.nodes.size(), load_sel.faces.size(),
                    selection_note(load_sel, false).c_str(), n_bnd,
                    sane_selection_minimum(n_bnd));
        polymesh::fea::Dirichlet bc;
        for (const auto n : fix_sel.nodes) {
            bc.fix_node(n);
        }
        if (const auto defect = constraint_defect(v.mesh, fix_sel.nodes); !defect.empty()) {
            throw std::runtime_error(std::format(
                "solve: the fixture selection is geometrically degenerate — {}. Select a "
                "real face with --fix-box x0 y0 z0 x1 y1 z1.",
                defect));
        }
        auto loads = build_loads(v.mesh, load_sel.faces, load_spec, "solve", stdout);
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
    BoxSel fix_box, load_box;
    LoadSpec load_spec;
    for (std::size_t i = 3; i < args.size(); ++i) {
        if (std::strcmp(args[i], "-h") == 0 && i + 1 < args.size()) {
            h = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--mesher") == 0 && i + 1 < args.size()) {
            mesher = parse_mesher(args[++i]);
        } else if (std::strcmp(args[i], "--json") == 0 && i + 1 < args.size()) {
            json_path = args[++i];
        } else if (std::strcmp(args[i], "--no-solve") == 0) {
            do_solve = false;
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
    // BC/load boxes feed the refinement plan too, so bc_seeds is a real
    // measurement instead of a structural zero.
    const auto plan = polymesh::pipeline::build_refinement_plan(
        model, h, make_regions(fix_box, load_box), /*use_geometry=*/true);

    t0 = clock::now();
    auto vol = polymesh::pipeline::volume_mesh(model, h, mesher, 2, true, plan.refine_seeds,
                                               plan.seed_band);
    const double mesh_ms = ms(clock::now() - t0);
    vol.mesh.check_validity();

    // Measured per-cell quality for every element type (fea::cell_quality): the
    // old tet4-only pass left non-tet cells at 0, which this loop skipped, so
    // quality_min kept its 1.0 initializer and quality_mean came out 0 — a
    // perfect score on meshes where not one cell had been measured.
    const auto q = polymesh::fea::summarize_cell_quality(vol.mesh);
    const double q_min = q.min;
    const double q_mean = q.mean;

    double max_vm = 0.0, max_u = 0.0, global_eta = 0.0, solve_ms = 0.0;
    std::size_t dof = 0;
    bool solved = false;
    if (do_solve) {
        const double xmin = model.bbox_min[0], xmax = model.bbox_max[0];
        const double tol = 0.51 * h;
        const auto all_faces = polymesh::fea::boundary_surface_faces(vol.mesh);
        const std::size_t n_bnd = count_boundary_nodes(all_faces);
        const auto fix_sel =
            select_end(vol.mesh, all_faces, n_bnd, fix_box, xmin, xmax, tol, -1);
        const auto load_sel =
            select_end(vol.mesh, all_faces, n_bnd, load_box, xmin, xmax, tol, +1);
        polymesh::fea::Dirichlet bc;
        for (const auto n : fix_sel.nodes) {
            bc.fix_node(n);
        }
        if (const auto defect = constraint_defect(vol.mesh, fix_sel.nodes); !defect.empty()) {
            throw std::runtime_error(std::format(
                "diag: the fixture selection is geometrically degenerate — {}. Select a "
                "real face with --fix-box x0 y0 z0 x1 y1 z1.",
                defect));
        }
        if (!bc.dof_values.empty() && !load_sel.faces.empty()) {
            Eigen::VectorXd loads =
                build_loads(vol.mesh, load_sel.faces, load_spec, "diag", stderr);
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

    // Every enumerator, spelled exactly as `parse_mesher` accepts it, so the
    // reported name round-trips through `--mesher`. The old `default: "other"`
    // silently mislabelled five of the eleven meshers — `--mesher hex` (the
    // hex-fill baseline every scorecard compares against) reported "other", so
    // no campaign row could tell hex from hexvem, prism or octa. No `default`
    // here: a new mesher must fail the -Wswitch build, not report "other".
    const std::string mesher_name = [&]() -> const char* {
        switch (mesher) {
        case polymesh::pipeline::VolumeMesher::kTetFill: return "tet";
        case polymesh::pipeline::VolumeMesher::kHexFill: return "hex";
        case polymesh::pipeline::VolumeMesher::kHexVem: return "hexvem";
        case polymesh::pipeline::VolumeMesher::kGradedTet: return "graded";
        case polymesh::pipeline::VolumeMesher::kHexPyramid: return "hexpyr";
        case polymesh::pipeline::VolumeMesher::kPrismSweep: return "prism";
        case polymesh::pipeline::VolumeMesher::kHybrid: return "hybrid";
        case polymesh::pipeline::VolumeMesher::kOctahedral: return "octa";
        case polymesh::pipeline::VolumeMesher::kHybridVem: return "hybridvem";
        case polymesh::pipeline::VolumeMesher::kVaryhedron: return "varyhedron";
        case polymesh::pipeline::VolumeMesher::kCvtPoly: return "cvt_poly";
        }
        return "unknown"; // only reachable from an out-of-range int cast
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
