// SPDX-License-Identifier: BSD-3-Clause

// PolyMesh CLI — geometry check, tet mesh, elastostatic solve + VTU export.

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif
#include "adapt/error.hpp"
#include "adapt/graded_sizing.hpp"
#include "adapt/loop.hpp"
#include "fea/backend.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/cell_quality.hpp"
#include "fea/msh.hpp"
#include "fea/material.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/cad_topology.hpp"
#include "geom/step.hpp"
#include "mesh/brep_fidelity.hpp"
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
#include <limits>
#include <optional>
#include <set>
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
               "              [--no-feature] [--element-tendency t] [--no-spectral]\n"
               "              [--max-elems N] [--max-dof N]\n"
               "              [--fix-box x0 y0 z0 x1 y1 z1] [--load-box x0 y0 z0 x1 y1 z1]\n"
               "                             geometry+BC-aware volume mesh; optional VTU\n"
               "  solve <part.step|.brep|.msh> -o out.vtu [-h m] [-E Pa] [-nu r]\n"
               "              [--mesher name] [--skin n] [--no-feature] [--adapt n]\n"
               "              [--eta-target η] [--p-elevate] [--p-elevate-uniform]\n"
               "              [--element-tendency t] [--no-spectral]\n"
               "              [--max-elems N] [--max-dof N] [--max-mem GB]\n"
               "              [--fix-box ...6] [--load-box ...6] [--bc-grade]\n"
               "              [--load-dir x y z] [--force N] [--traction Pa]\n"
               "              [--advisor <model_dir>] [--advisor-max-dof N] (CAD inputs only)\n"
               "                             CAD: mesh + BCs + VTU; Gmsh: solve the imported\n"
               "                             volume mesh directly. Default BCs fix min-x and\n"
               "                             load max-x; boxes override selection.\n"
               "  diag  <part> [-h m] [--mesher name] [--json out.json] [--no-solve]\n"
               "              [--max-elems N] [--max-dof N] [--max-mem GB]\n"
               "              [--fix-box ...6] [--load-box ...6]\n"
               "              [--load-dir x y z] [--force N] [--traction Pa]\n"
               "                             JSON diagnostics: fidelity, quality, timings\n"
               "  backend                    print compute backend + OpenMP/opt summary\n"
               "\n"
               "inputs: CAD (.step .stp .brep .brp); solve also accepts Gmsh 2.x ASCII .msh.\n"
               "mesh size: omit -h (or -h 0) for auto h0 from bbox + feature density\n"
               "mesher names: hybrid|zoo (default), varyhedron|vary (CAD packing),\n"
               "              hybridvem, cvt_poly|cvt (experimental packed-poly VEM),\n"
               "              tet, hex, hexvem|vem, graded, hexpyr|transition,\n"
               "              prism|sweep, octa|octahedral (experimental)\n"
               "--skin n: graded fine skin layers (default 2)\n"
               "--no-feature: disable geometry (curvature/thin-wall) grading (default on)\n"
               "--element-tendency t: shape dial in [-1,+1] (hex↔fan hybrid↔poly VEM↔tet)\n"
               "--fix-box / --load-box: BC/load selection AABBs; the mesh grades finer\n"
               "              toward them (loads finest) — geometry + simulation setup\n"
               "--load-dir x y z: load direction (normalized; default 0 1 0)\n"
               "--force N: total resultant force in newtons over the loaded faces\n"
               "              (default 1000); applied as a consistent traction ∫Nᵗt dS\n"
               "--traction Pa: pressure magnitude instead of a total force; faces in the\n"
               "              load selection are filtered by normal alignment with\n"
               "              --load-dir, and resultant is Pa × their area. Last of\n"
               "              --force/--traction wins\n"
               "--max-mem GB: enforced preflight solve cap; 0=auto (70% of currently\n"
               "              available system memory)\n"
               "--adapt n: ZZ→Dörfler remesh passes (local seeds on graded path)\n"
               "--eta-target η: stop adapt when global ZZ η ≤ η (0=off; needs --adapt)\n"
               "--p-elevate: promote ZZ-smooth tet4/hex8 → tet10/hex20 (auto-on --adapt>0);\n"
               "             the unmarked remainder stays linear, so the mesh is mixed p\n"
               "--p-elevate-uniform: promote EVERY tet4/hex8 instead of the smooth-marked\n"
               "             subset → uniformly quadratic, for true order-2 parity with\n"
               "             Gmsh peers; implies --p-elevate\n"
               "--bc-grade: force a-priori BC grading from the default cantilever faces\n"
               "--advisor DIR: pick mesher/h/adapt/p-order with the learned mesh advisor\n"
               "               (DIR holds model.onnx, normalization.json, clamps.json);\n"
               "               every value is clamped and the decision is logged as JSON\n"
               "--max-elems N: pre-flight element ceiling (0=589824 default); auto-h\n"
               "               clamps to fit and over-ceiling meshes coarsen-and-retry;\n"
               "               with spectral sizing on (default) the size field is\n"
               "               FFT-trimmed first (insignificant fine bands merge, ADR-0034)\n"
               "--no-spectral: disable FFT sizing-field trimming and CAD-edge curvature\n"
               "               denoise (campaign-baseline behavior)\n"
               "--advisor-max-dof N: with --advisor, drop candidate actions whose\n"
               "               predicted DOF exceeds N; refusal (defaults) if none fit\n"
               "--max-dof N: pre-flight/adapt DOF ceiling (0=1769472 default)\n"
               "\n"
               "default BC selection: nodes in a 0.51·h slab at min-x (fixed) / max-x\n"
               "              (loaded). If a slab captures too few nodes to act as a\n"
               "              face (curved parts), selection falls back to boundary\n"
               "              faces whose outward normal aligns with ∓x/±x.\n",
               stderr);
    return 2;
}

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
bool parse_ceiling(std::span<char*> args, std::size_t& i, std::size_t& value) {
    if (i + 1 >= args.size()) {
        return false;
    }
    const char* text = args[++i];
    if (text[0] == '-') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

/// Accepts both the CLI's historical short names and the canonical spellings
/// `testlab`'s `mesher_name()` emits, because the advisor's `mesher_choices`
/// vocabulary is the latter. `graded_tet`, `hex_vem` and `hybrid_zoo` used to
/// miss every branch and fall through to the hybrid default, so an advisor
/// recommending `graded_tet` silently got a hybrid mesh while the logged
/// decision still said `graded_tet`.
std::optional<polymesh::pipeline::VolumeMesher> try_parse_mesher(const std::string& m) {
    if (m == "hybrid" || m == "zoo" || m == "mixed" || m == "hybrid_zoo") {
        return polymesh::pipeline::VolumeMesher::kHybrid;
    }
    if (m == "hybridvem" || m == "hybrid-vem" || m == "hybrid_vem") {
        return polymesh::pipeline::VolumeMesher::kHybridVem;
    }
    if (m == "tet" || m == "tet_fill") {
        return polymesh::pipeline::VolumeMesher::kTetFill;
    }
    if (m == "hex") {
        return polymesh::pipeline::VolumeMesher::kHexFill;
    }
    if (m == "hexvem" || m == "vem" || m == "hex_vem") {
        return polymesh::pipeline::VolumeMesher::kHexVem;
    }
    if (m == "graded" || m == "graded_tet") {
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
    return std::nullopt;
}

/// Lenient form kept for the `--mesher` flag's historical behaviour.
polymesh::pipeline::VolumeMesher parse_mesher(const std::string& m) {
    return try_parse_mesher(m).value_or(polymesh::pipeline::VolumeMesher::kHybrid);
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
        v[static_cast<std::size_t>(k)] = std::atof(args[i + 1 + static_cast<std::size_t>(k)]);
    }
    b.lo = Eigen::Vector3d(std::min(v[0], v[3]), std::min(v[1], v[4]), std::min(v[2], v[5]));
    b.hi = Eigen::Vector3d(std::max(v[0], v[3]), std::max(v[1], v[4]), std::max(v[2], v[5]));
    b.set = true;
    i += 6;
    return true;
}

// Geometry+BC refine regions from optional fix/load boxes (loads finest).
std::vector<polymesh::pipeline::RefineRegion> make_regions(const BoxSel& fix,
                                                           const BoxSel& load) {
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
    double traction_pa = 0.0;           // pressure magnitude, Pa
    bool traction_mode = false;         // last of --force/--traction wins
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
        spec.traction_mode = false;
        return std::isfinite(spec.force);
    }
    if (std::strcmp(args[i], "--traction") == 0 && i + 1 < args.size()) {
        spec.traction_pa = std::atof(args[++i]);
        spec.traction_mode = true;
        return spec.traction_pa >= 0.0 && std::isfinite(spec.traction_pa);
    }
    return false;
}

// A default BC slab must be broad enough to behave like a face. 12 nodes is
// ~4 boundary faces — the smallest patch that carries a traction instead of a
// point force — and 2% of the boundary nodes keeps that true on fine meshes,
// where even a few dozen nodes can still be a pinpoint. The 2% threshold
// separates curved closed parts (cone/sphere end slabs: <=1.6%) from the broad
// planar cantilever/plate ends (>=7.7%) on the default meshes. Below either
// bound the 0.51*h x-slab has degenerated and we switch to face selection.
constexpr std::size_t kMinSelNodes = 12;
constexpr double kMinSelFrac = 0.02;
// cos(~45°): a face counts as end-facing when its outward normal is within
// 45° of ±x. Matches the normal-aligned region machinery in scene.cpp.
constexpr double kNormalMinDot = 0.7;

struct BcSelection {
    std::vector<std::uint32_t> nodes;
    std::vector<polymesh::fea::SurfaceFace> faces;
    std::size_t slab_nodes = 0; // what the plain 0.51·h slab captured
    bool face_fallback = false; // slab was degenerate → normal-aligned faces
    double fallback_band = 0.0; // end band as a fraction of the x extent
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

// Pressure is a normal surface load: when a box includes a strip of adjacent
// side wall (for example z>=0.195 on a cylinder ending at z=0.2), keep only
// faces whose normal aligns with the requested pressure direction. Use |dot|
// because mixed-element boundary windings are not uniformly outward.
std::vector<polymesh::fea::SurfaceFace>
pressure_faces(const polymesh::fea::NodalMesh& mesh,
               const std::vector<polymesh::fea::SurfaceFace>& box_faces,
               const Eigen::Vector3d& direction) {
    std::vector<polymesh::fea::SurfaceFace> out;
    out.reserve(box_faces.size());
    for (const auto& f : box_faces) {
        const Eigen::Vector3d n = polymesh::fea::surface_face_normal(mesh, f);
        if (std::abs(n.dot(direction)) >= kNormalMinDot) {
            out.push_back(f);
        }
    }
    return out;
}

// Exact CAD area of planar faces wholly selected by `box` and aligned with the
// pressure direction. The nodal integrator still supplies the energy-conjugate
// distribution, but its resultant is pressure times the BRep area rather than
// the inscribed polygon area of a coarse boundary mesh. If no unambiguous CAD
// face matches, callers use the integrated mesh area.
std::optional<double> cad_pressure_area(const polymesh::pipeline::Model& model,
                                        const BoxSel& box, const Eigen::Vector3d& direction) {
    if (!box.set || !model.cad.has_value()) {
        return std::nullopt;
    }
    const auto topo = polymesh::geom::extract_topology(*model.cad, 16);
    double area = 0.0;
    for (const auto& face : topo.faces) {
        if (face.kind != polymesh::geom::CadSurfaceKind::kPlane) {
            continue;
        }
        std::vector<Eigen::Vector3d> points;
        for (const auto edge_id : face.edge_ids) {
            if (edge_id < topo.edges.size()) {
                const auto& samples = topo.edges[edge_id].samples;
                points.insert(points.end(), samples.begin(), samples.end());
            }
        }
        if (points.size() < 3) {
            continue;
        }
        const bool in_box = std::all_of(points.begin(), points.end(), [&](const auto& p) {
            return p.x() >= box.lo.x() && p.x() <= box.hi.x() && p.y() >= box.lo.y() &&
                   p.y() <= box.hi.y() && p.z() >= box.lo.z() && p.z() <= box.hi.z();
        });
        if (!in_box) {
            continue;
        }
        Eigen::Vector3d n = Eigen::Vector3d::Zero();
        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
            n = (points[i] - points[0]).cross(points[i + 1] - points[0]);
            if (n.norm() > 1e-15) {
                n.normalize();
                break;
            }
        }
        if (std::abs(n.dot(direction)) >= kNormalMinDot) {
            area += face.area;
        }
    }
    return area > 0.0 ? std::optional<double>(area) : std::nullopt;
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

// Energy-conjugate nodal loads for `spec`: integrate complete boundary faces,
// or, when a legitimate coarse selection has nodes but no complete face,
// preserve the requested resultant with a documented per-node fallback.
Eigen::VectorXd build_loads(const polymesh::fea::NodalMesh& mesh,
                            const std::vector<polymesh::fea::SurfaceFace>& faces,
                            std::span<const std::uint32_t> fallback_nodes,
                            const LoadSpec& spec, const char* what, std::FILE* report,
                            std::optional<double> exact_pressure_area = std::nullopt) {
    const double mesh_area = polymesh::fea::integrated_face_area(mesh, faces);
    if (fallback_nodes.empty() && !(mesh_area > 0.0)) {
        throw std::runtime_error(std::format(
            "{}: the load selection is empty — widen --load-box or refine with -h.", what));
    }
    if (spec.traction_mode && !(mesh_area > 0.0) && !exact_pressure_area.has_value()) {
        throw std::runtime_error(std::format(
            "{}: pressure selection has nodes but no integrable face or CAD area; widen "
            "--load-box or use --force for a known resultant.",
            what));
    }
    const double pressure_area =
        exact_pressure_area.has_value() ? *exact_pressure_area : mesh_area;
    const double magnitude =
        spec.traction_mode ? spec.traction_pa * pressure_area : spec.force;
    const Eigen::Vector3d total = magnitude * spec.dir;
    Eigen::VectorXd loads =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(mesh.nodes.size()));
    Eigen::Vector3d resultant = Eigen::Vector3d::Zero();
    double conservation_error = total.norm();
    const bool node_fallback = !(mesh_area > 0.0);
    if (!node_fallback) {
        auto applied = polymesh::fea::consistent_face_load(mesh, faces, total);
        loads = std::move(applied.loads);
        resultant = applied.resultant;
        conservation_error = applied.conservation_error;
    } else {
        const Eigen::Vector3d per_node = total / static_cast<double>(fallback_nodes.size());
        for (const auto node : fallback_nodes) {
            loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
            resultant += per_node;
        }
        conservation_error = (resultant - total).norm();
    }
    const std::string area_note =
        node_fallback ? std::format("node fallback={}", fallback_nodes.size())
                      : (exact_pressure_area.has_value()
                             ? std::format("mesh area={:.9g} m², CAD area={:.9g} m²",
                                           mesh_area, pressure_area)
                             : std::format("area={:.9g} m²", mesh_area));
    std::fprintf(report,
                 "load: %zu faces, %s, %s → |F|=%.9g N along (%.4g %.4g %.4g) | "
                 "Σf=(%.9g %.9g %.9g) N, conservation err=%.3g N\n",
                 faces.size(), area_note.c_str(),
                 spec.traction_mode ? std::format("t={:.6g} Pa", spec.traction_pa).c_str()
                                    : "total force",
                 total.norm(), spec.dir.x(), spec.dir.y(), spec.dir.z(), resultant.x(),
                 resultant.y(), resultant.z(), conservation_error);
    if (conservation_error > 1e-9) {
        throw std::runtime_error(
            std::format("{}: load assembly lost {:.3g} N of the requested {:.6g} N resultant "
                        "(total-force conservation check failed)",
                        what, conservation_error, total.norm()));
    }
    return loads;
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
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    double element_tendency = 0.0;
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
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
        } else if (std::strcmp(args[i], "--spectral") == 0) {
            spectral = true; // accepted for symmetry (now the default)
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
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
    const auto model = polymesh::pipeline::Model::load(path);
    const auto resolved =
        polymesh::pipeline::resolve_mesh_size(model, h, 30.0, max_elems, max_dof);
    h = resolved.h;

    // Geometry + simulation-setup (BC/load box) aware refinement plan → seeds.
    const auto regions = make_regions(fix_box, load_box);
    const auto plan = polymesh::pipeline::build_refinement_plan(model, h, regions, feature,
                                                                spectral, 0);
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, mesher, skin, feature, plan.refine_seeds, plan.seed_band, element_tendency,
        resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
        plan.size_field);
    vol.mesh.check_validity();
    std::printf("mesh: %zu nodes, %zu elems, h=%.6g m\n"
                "refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m\n"
                "%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), h, plan.n_geometry_seeds,
                plan.n_bc_seeds, plan.refine_seeds.size(), plan.seed_band, plan.h_fine,
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
        const auto display = polymesh::pipeline::curved_display_mesh(model, vol.mesh, h);
        polymesh::fea::write_vtu(out_path, display.mesh, {}, cdata);
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
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = false;
    bool p_elevate_uniform = false;
    double element_tendency = 0.0;
    bool bc_grade = false;
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    double max_mem_gb = 0.0;
    BoxSel fix_box, load_box;
    LoadSpec load_spec;
    std::string advisor_dir;
    std::size_t advisor_max_dof = 0; // 0 = no advisor budget (ADR-0034)
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
        } else if (std::strcmp(args[i], "--spectral") == 0) {
            spectral = true; // accepted for symmetry (now the default)
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
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
            // Implies --p-elevate: a flag that silently does nothing unless a
            // second flag is also present is the failure mode we keep fixing.
            p_elevate_uniform = true;
            p_elevate = true;
        } else if (std::strcmp(args[i], "--bc-grade") == 0) {
            bc_grade = true;
        } else if (std::strcmp(args[i], "--advisor") == 0 && i + 1 < args.size()) {
            advisor_dir = args[++i];
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

    const bool msh_input = is_msh_path(path);
    if (msh_input && !advisor_dir.empty()) {
        std::fputs("solve: --advisor requires CAD input and cannot be used with .msh\n", stderr);
        return 2;
    }

    std::optional<polymesh::pipeline::Model> model;
    std::optional<polymesh::fea::MshModel> msh_model;
    Eigen::Vector3d bbox_min;
    Eigen::Vector3d bbox_max;
    if (msh_input) {
        msh_model.emplace(polymesh::fea::load_msh(path));
        bbox_min = msh_model->mesh.nodes.front();
        bbox_max = bbox_min;
        for (const auto& node : msh_model->mesh.nodes) {
            bbox_min = bbox_min.cwiseMin(node);
            bbox_max = bbox_max.cwiseMax(node);
        }
    } else {
        model.emplace(polymesh::pipeline::Model::load(path));
        bbox_min = model->bbox_min;
        bbox_max = model->bbox_max;
    }

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
        const polymesh::advisor::Advisor advisor(advisor_dir);
        const auto decision =
            advisor.recommend(features, static_cast<double>(advisor_max_dof));
        std::printf("advisor: %s\n", polymesh::advisor::to_json(decision).c_str());
        const double diag = (model->bbox_max - model->bbox_min).norm();
        const auto resolved_mesher = try_parse_mesher(decision.mesher);
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
        // diagonal, and the model has no way to know that this part carries a
        // feature finer than that fraction: the v6 advisor picks h_rel = 0.2 on
        // plate_hole, and the Cartesian fill refuses the mesh outright
        // ("feature unresolved ... a hole/void smaller than that level can
        // disappear"), so the deployed path exited 1 on the flagship fixture.
        //
        // The verdict belongs to the engine, not to a proxy. Two scalar proxies
        // were measured and both over-clamp: 2x the shortest CAD feature length
        // takes the cylinder from h = 24.5 mm to 9.8 mm although it meshes
        // cleanly at 24.5, and the smallest CAD face size takes the cone from
        // 17.7 mm to 10.6 mm for the same reason. Clamping to the auto h0 is
        // worse still (sphere 17.3 mm -> 3.4 mm, plate_hole past a 300 s
        // timeout) since that default is far finer than any advisor action.
        //
        // So: probe-mesh at the chosen h, and when the fill refuses on feature
        // resolution, refine by the factor the guard itself recommends (0.6)
        // and probe again, at most three times. Nothing is clamped that meshes,
        // and every refinement is reported.
        double feature_clamped_from = 0.0;
        std::string feature_clamp_reason;
        for (int probe = 0; probe < 3; ++probe) {
            try {
                (void)polymesh::pipeline::volume_mesh(*model, h, mesher, skin, feature, {}, 0.0,
                                                      element_tendency, max_elems, max_dof, 0);
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
        // `rel_err_rel` is the score that actually drove the choice: the
        // absolute `rel_err` level does not generalize across parts, so the
        // ranking the policy learned is the centred one. Printing it lets the
        // operator see what the model was optimizing, not just its output.
        //
        // On a refusal it is NOT printed. The decision JSON nulls every
        // prediction when the advisor vetoes, and printing "+nan" in the prose
        // line would reintroduce the same defect one layer down: suppression that
        // covers the machine-readable path but not the line a human actually
        // reads. A refused row states the reason instead.
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
        !msh_input && load_spec.traction_mode
            ? cad_pressure_area(*model, load_box, load_spec.dir)
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
        resolved.note =
            std::format("imported .msh (CAD fidelity unavailable, h{}={:.6g} m)",
                        auto_h ? "_estimate" : "", h);
    } else {
        resolved =
            polymesh::pipeline::resolve_mesh_size(*model, h, 30.0, max_elems, max_dof);
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
        const auto plan =
            polymesh::pipeline::build_refinement_plan(*model, h_use, regions, feature,
                                                      spectral, 0);
        seeds = plan.refine_seeds;
        seed_band = plan.seed_band;
        size_field = plan.size_field;
        std::printf(
            "refine: %zu geometry + %zu BC seeds → %zu seeds, band=%.4g m, h_fine=%.4g m\n",
            plan.n_geometry_seeds, plan.n_bc_seeds, seeds.size(), seed_band, plan.h_fine);
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
            throw std::runtime_error(
                "solve: --adapt requires CAD geometry for remeshing and is unavailable for .msh");
        }
        return polymesh::pipeline::volume_mesh(
            *model, h_use, m, skin, feature, seeds, seed_band, element_tendency,
            resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
            size_field);
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
        vol.mesher_note +=
            std::format(" | mids projected={} partial={} reverted={}", projected,
                        partial.size(), reverted.size());
    };

    const polymesh::fea::Material mat{.youngs_modulus = E, .poissons_ratio = nu};
    auto make_bc_loads = [&](const polymesh::pipeline::VolumeMeshOutput& v) {
        const double xmin = bbox_min[0];
        const double xmax = bbox_max[0];
        const double tol = 0.51 * h_use;
        const auto all_faces = polymesh::fea::boundary_surface_faces(v.mesh);
        const std::size_t n_bnd = count_boundary_nodes(all_faces);
        const auto fix_sel =
            select_end(v.mesh, all_faces, n_bnd, fix_box, xmin, xmax, tol, -1);
        const auto load_sel =
            select_end(v.mesh, all_faces, n_bnd, load_box, xmin, xmax, tol, +1);
        const auto load_faces = load_spec.traction_mode
                                    ? pressure_faces(v.mesh, load_sel.faces, load_spec.dir)
                                    : load_sel.faces;
        std::printf("bc: fix %zu nodes%s | load %zu nodes, %zu faces%s | %zu boundary nodes "
                    "(sane-selection minimum %zu)\n",
                    fix_sel.nodes.size(), selection_note(fix_sel, true).c_str(),
                    load_sel.nodes.size(), load_faces.size(),
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
        auto loads = build_loads(v.mesh, load_faces, load_sel.nodes, load_spec, "solve",
                                 stdout, exact_pressure_area);
        return std::pair{std::move(bc), std::move(loads)};
    };

    polymesh::fea::SolveOptions solve_options;
    solve_options.max_mem_gb = max_mem_gb;
    solve_options.on_note = [](std::string_view note) {
        std::printf("solve: %.*s\n", static_cast<int>(note.size()), note.data());
    };

    Eigen::VectorXd u;
    polymesh::fea::ZzRecovery zz;

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
        std::printf("p-elevate: %zu smooth, nodes %zu→%zu (tet10=%zu hex20=%zu)\n",
                    n_promoted, n0, vol.mesh.nodes.size(), counts.tet10, counts.hex20);
        std::printf("p-elevate-mode: %s\n", p_elevate_mode);
        vol.mesher_note += std::format(" | p-elevate={} promoted={} tet10={} hex20={}",
                                       p_elevate_mode, n_promoted, counts.tet10,
                                       counts.hex20);
    };
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
        if (model) {
            polymesh::pipeline::update_solved_geometry_volume(*model, vol);
        }

        u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads, solve_options);
        zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
        const bool last_pass =
            (pass == adapt_passes) || (eta_target > 0.0 && zz.global_eta <= eta_target);
        if (last_pass) {
            if (eta_target > 0.0 && zz.global_eta <= eta_target) {
                std::printf("eta-target stop: η=%.4g ≤ %.4g at pass %d/%d\n", zz.global_eta,
                            eta_target, pass, adapt_passes);
            }
            if (p_elevate) {
                const auto promote = elevate_targets();
                if (!promote.empty()) {
                    const auto n0 = vol.mesh.nodes.size();
                    vol.mesh = polymesh::fea::p_elevate(vol.mesh, promote);
                    project_quadratic_mids();
                    vol.mesh.check_validity();
                    auto [bc2, loads2] = make_bc_loads(vol);
                    if (bc2.dof_values.empty()) {
                        std::fputs("solve: no fixture nodes after p-elevate\n", stderr);
                        return 1;
                    }
                    if (model) {
                        polymesh::pipeline::update_solved_geometry_volume(*model, vol);
                    }

                    u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc2, loads2,
                                                           solve_options);
                    zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
                    report_p_elevate(promote.size(), n0);
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
                    const auto promote = elevate_targets();
                    if (!promote.empty()) {
                        const auto n0 = vol.mesh.nodes.size();
                        vol.mesh = polymesh::fea::p_elevate(vol.mesh, promote);
                        project_quadratic_mids();
                        vol.mesh.check_validity();
                        auto [bc2, loads2] = make_bc_loads(vol);
                        if (model) {
                            polymesh::pipeline::update_solved_geometry_volume(*model, vol);
                        }

                        u = polymesh::fea::solve_elastostatics(vol.mesh, mat, bc2, loads2,
                                                               solve_options);
                        zz = polymesh::fea::recover_zz(vol.mesh, mat, u);
                        // Previously silent: a converged-early run promoted with
                        // no promotion line at all, so neither a reader nor the
                        // peer parser could tell what was actually solved.
                        report_p_elevate(promote.size(), n0);
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
    polymesh::fea::write_vtu(out_path, vol.mesh, pdata, cdata);

    std::printf("solve: %zu nodes, %zu elems | max von Mises %.4g Pa | max |u| %.4g m | "
                "ZZ η %.4g | h=%.4g | seeds=%zu\n%s\n%s\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), max_vm, max_u, zz.global_eta,
                h_use, seeds.size(), resolved.note.c_str(), vol.mesher_note.c_str());
    std::printf("stress direction: max principal %.6g Pa along (%.4f %.4f %.4f); "
                "σzz RMS share of normal stress = %.6f\n",
                max_principal, max_principal_dir.x(), max_principal_dir.y(),
                max_principal_dir.z(), sigma_zz_share);
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
    bool spectral = true; // spectral sizing on by default (ADR-0034)
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    double max_mem_gb = 0.0;
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
    const auto model = polymesh::pipeline::Model::load(path);
    const auto exact_pressure_area = load_spec.traction_mode
                                         ? cad_pressure_area(model, load_box, load_spec.dir)
                                         : std::nullopt;
    const double import_ms = ms(clock::now() - t0);
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
    vol.mesh.check_validity();

    // Measured per-cell quality for every element type (fea::cell_quality): the
    // old tet4-only pass left non-tet cells at 0, which this loop skipped, so
    // quality_min kept its 1.0 initializer and quality_mean came out 0 — a
    // perfect score on meshes where not one cell had been measured.
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

    const auto fidelity_faces = polymesh::fea::extract_boundary_faces(vol.mesh);
    constexpr std::size_t kBRepSurfaceSampleCeiling = 10'000;
    polymesh::mesh::BRepGeometryFidelity fidelity;
    if (model.cad && !model.cad->empty()) {
        const auto feature_segments =
            polymesh::mesh::mesh_dihedral_feature_segments(vol.mesh.nodes, fidelity_faces);
        const double mesh_volume =
            polymesh::mesh::boundary_surface_volume(vol.mesh.nodes, fidelity_faces);
        fidelity = polymesh::mesh::evaluate_brep_geometry_fidelity(
            *model.cad, vol.mesh.nodes, fidelity_faces, feature_segments, h, mesh_volume,
            kBRepSurfaceSampleCeiling);
    }

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
        const auto load_faces = load_spec.traction_mode
                                    ? pressure_faces(vol.mesh, load_sel.faces, load_spec.dir)
                                    : load_sel.faces;
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
        if (!bc.dof_values.empty() && !load_sel.nodes.empty()) {
            Eigen::VectorXd loads =
                build_loads(vol.mesh, load_faces, load_sel.nodes, load_spec, "diag", stderr,
                            exact_pressure_area);
            const polymesh::fea::Material mat{.youngs_modulus = 200e9, .poissons_ratio = 0.3};
            t0 = clock::now();
            polymesh::fea::SolveOptions solve_options;
            solve_options.max_mem_gb = max_mem_gb;
            solve_options.on_note = [](std::string_view note) {
                std::fprintf(stderr, "diag: %.*s\n", static_cast<int>(note.size()),
                             note.data());
            };
            const Eigen::VectorXd uu =
                polymesh::fea::solve_elastostatics(vol.mesh, mat, bc, loads, solve_options);
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

    // Every enumerator, spelled exactly as `parse_mesher` accepts it, so the
    // reported name round-trips through `--mesher`. The old `default: "other"`
    // silently mislabelled five of the eleven meshers — `--mesher hex` (the
    // hex-fill baseline every scorecard compares against) reported "other", so
    // no campaign row could tell hex from hexvem, prism or octa. No `default`
    // here: a new mesher must fail the -Wswitch build, not report "other".
    const std::string mesher_name = [&]() -> const char* {
        switch (mesher) {
        case polymesh::pipeline::VolumeMesher::kTetFill:
            return "tet";
        case polymesh::pipeline::VolumeMesher::kHexFill:
            return "hex";
        case polymesh::pipeline::VolumeMesher::kHexVem:
            return "hexvem";
        case polymesh::pipeline::VolumeMesher::kGradedTet:
            return "graded";
        case polymesh::pipeline::VolumeMesher::kHexPyramid:
            return "hexpyr";
        case polymesh::pipeline::VolumeMesher::kPrismSweep:
            return "prism";
        case polymesh::pipeline::VolumeMesher::kHybrid:
            return "hybrid";
        case polymesh::pipeline::VolumeMesher::kOctahedral:
            return "octa";
        case polymesh::pipeline::VolumeMesher::kHybridVem:
            return "hybridvem";
        case polymesh::pipeline::VolumeMesher::kVaryhedron:
            return "varyhedron";
        case polymesh::pipeline::VolumeMesher::kCvtPoly:
            return "cvt_poly";
        }
        return "unknown"; // only reachable from an out-of-range int cast
    }();
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
        "\"mesh_feature_classifier\":\"boundary_dihedral_ge_30_deg\","
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
        plan.spectral.modes_total, plan.spectral.energy_kept,
        plan.spectral.n_edge_curve_seeds, plan.spectral.predicted_before,
        plan.spectral.predicted_after);

    const std::string json = std::format(
        "{{\n"
        "  \"part\": \"{}\",\n"
        "  \"mesher\": \"{}\",\n"
        "  \"import\": {{ \"vertices\": {}, \"triangles\": {}, \"bbox_diag\": {:.6g}, "
        "\"cad_brep\": {} }},\n"
        "  \"mesh\": {{ \"h\": {:.6g}, \"nodes\": {}, \"elements\": {}, "
        "\"quality_min\": {:.4g}, \"quality_min_type\": \"{}\", "
        "\"n_inverted_cells\": {}, \"n_below_shape_floor\": {}, \"quality_mean\": {:.4g}, "
        "\"geometry_seeds\": {}, \"bc_seeds\": {} }},\n"
        "  \"spectral\": {},\n"
        "  \"timing_ms\": {{ \"import\": {:.3f}, \"mesh\": {:.3f}, \"solve\": {:.3f} }},\n"
        "  \"mesh_throughput_elem_per_s\": {:.1f},\n"
        "  \"fidelity\": {},\n"
        "  \"solve\": {{ \"ran\": {}, \"dof\": {}, \"max_von_mises\": {:.6g}, "
        "\"max_disp\": {:.6g}, \"global_eta\": {:.6g} }},\n"
        "  \"mesh_size_note\": \"{}\",\n"
        "  \"mesher_note\": \"{}\"\n"
        "}}\n",
        model.name, mesher_name, model.surface.vertices.size(), model.surface.triangles.size(),
        bbox_diag, model.cad ? "true" : "false", h, vol.mesh.nodes.size(),
        vol.mesh.elements.size(), q_min, q_min_type, n_inverted,
        vol.n_cells_below_shape_floor, q_mean,
        plan.n_geometry_seeds, plan.n_bc_seeds, spectral_json,
        import_ms, mesh_ms, solve_ms, mesh_throughput, fidelity_json,
        solved ? "true" : "false", dof, max_vm, max_u, global_eta, mesh_size_note,
        vol.mesher_note);

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
