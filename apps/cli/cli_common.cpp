// SPDX-License-Identifier: BSD-3-Clause

#include "cli_common.hpp"

#include "fea/bc_selection.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace polymesh::cli {

int usage() {
    std::fputs(
        "usage: polymesh <command> [args]\n"
        "\n"
        "commands:\n"
        "  check <part.step|.brep> [--scale f]\n"
        "                             validate CAD geometry\n"
        "  mesh  <part> [-h m] [-o out.vtu] [--mesher name] [--skin n] [--scale f]\n"
        "              [--no-feature] [--element-tendency t] [--no-spectral]\n"
        "              [--max-elems N] [--max-dof N]\n"
        "              [--fix-box x0 y0 z0 x1 y1 z1] [--load-box x0 y0 z0 x1 y1 z1]\n"
        "                             geometry+BC-aware volume mesh; optional VTU\n"
        "  solve <part.step|.brep|.msh> -o out.vtu [-h m] [-E Pa] [-nu r] [--scale f]\n"
        "              [--mesher name] [--skin n] [--no-feature] [--adapt n]\n"
        "              [--eta-target η] [--no-curved] [--p-elevate-uniform]\n"
        "              [--element-tendency t] [--no-spectral]\n"
        "              [--max-elems N] [--max-dof N] [--max-mem GB]\n"
        "              [--fix-box ...6] [--load-box ...6] [--bc-grade]\n"
        "              [--load-dir x y z] [--force N] [--traction Pa]\n"
        "              [--solver auto|direct|cg] [--threads N]\n"
        "              [--advisor <model_dir>] [--advisor-objective accuracy|efficiency]\n"
        "                             CAD: mesh + BCs + VTU; Gmsh: solve the imported\n"
        "                             volume mesh directly. Default BCs fix min-x and\n"
        "                             load max-x; boxes override selection.\n"
        "  diag  <part> [-h m] [-E Pa] [-nu r] [--mesher name] [--json out.json] [--no-solve]\n"
        "              [--scale f] [--max-elems N] [--max-dof N] [--max-mem GB]\n"
        "              [--fix-box ...6] [--load-box ...6]\n"
        "              [--load-dir x y z] [--force N] [--traction Pa]\n"
        "                             JSON diagnostics: fidelity, quality, timings\n"
        "  render <part> -o out.png [-h m] [--mesher name] [--no-curved] [--scale f]\n"
        "              [--subdiv N] [--size WxH] [--azimuth DEG] [--elevation DEG]\n"
        "              [--wireframe] [--stats out.json]\n"
        "                             headless PNG of the same boundary surface the\n"
        "                             Studio viewport paints — no GL, no window\n"
        "  calibrate --out host.json   benchmark portable FLOP/byte rates and reference mesh\n"
        "  backend                    print compute backend + OpenMP/opt summary\n"
        "\n"
        "inputs: CAD (.step .stp .brep .brp); solve also accepts Gmsh 2.x ASCII .msh.\n"
        "mesh size: omit -h (or -h 0) for auto h0 from bbox + feature density\n"
        "--scale f: uniform factor applied to the loaded geometry immediately after\n"
        "              the STEP/BREP read, before check/mesh/solve/diag/render do\n"
        "              anything with it. The solver treats coordinates as metres, so\n"
        "              a millimetre STEP needs --scale 0.001; every other value —\n"
        "              -h, --fix-box/--load-box, VTU/PNG geometry, diag bbox — is\n"
        "              then in scaled units. Not accepted for a Gmsh .msh input\n"
        "mesher names: hybrid|zoo (default), varyhedron|vary (CAD packing),\n"
        "              hybridvem, cvt_poly|cvt (experimental packed-poly VEM),\n"
        "              tet, hex, hexvem|vem, graded, hexpyr|transition,\n"
        "              prism|sweep, octa|octahedral (experimental)\n"
        "--skin n: graded fine skin layers (default 2)\n"
        "--no-feature: disable geometry (curvature/thin-wall) grading (default on)\n"
        "--element-tendency t: shape dial in [-1,+1] (hex↔fan hybrid↔poly VEM↔tet)\n"
        "--fix-box / --load-box: BC/load selection AABBs. They select the *boundary*\n"
        "              nodes and faces inside the box, never interior nodes, and the\n"
        "              mesh grades finer toward them (loads finest)\n"
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
        "--no-curved: ship the straight-edged linear mesh instead of the exact\n"
        "             curved CAD geometry. Default for a CAD part is curved: the\n"
        "             solve/export mesh is tet10/hex20 with boundary mids on the\n"
        "             BRep (ADR-0035), so this flag is the opt-out, not the opt-in\n"
        "--p-elevate-uniform: promote every tet4/hex8 on a non-CAD (STL) mesh too,\n"
        "             for order-2 parity with Gmsh peers on tessellated input\n"
        "--bc-grade: force a-priori BC grading from the default cantilever faces\n"
        "--advisor DIR: pick mesher/h/adapt/p-order with the learned mesh advisor\n"
        "               (DIR holds model.onnx, normalization.json, clamps.json);\n"
        "               every value is clamped and the decision is logged as JSON\n"
        "--advisor-objective: accuracy (default) or calibrated efficiency; efficiency\n"
        "               minimises predicted mesh+solve time inside a 5% accuracy envelope\n"
        "--max-elems N: pre-flight element ceiling (0=589824 default); auto-h\n"
        "               clamps to fit and over-ceiling meshes coarsen-and-retry;\n"
        "               with spectral sizing on (default) the size field is\n"
        "               FFT-trimmed first (insignificant fine bands merge, ADR-0034)\n"
        "--no-spectral: disable FFT sizing-field trimming and CAD-edge curvature\n"
        "               denoise (campaign-baseline behavior)\n"
        "--advisor-max-dof N: with --advisor, drop candidate actions whose\n"
        "               predicted DOF exceeds N; refusal (defaults) if none fit\n"
        "--max-dof N: pre-flight/adapt DOF ceiling (0=1769472 default)\n"
        "--subdiv N: render/tessellation subdivisions per quadratic boundary face\n"
        "               (default 8, the value the Studio viewport uses); linear\n"
        "               faces have no interior to subdivide and ignore it\n"
        "--size WxH: render pixel size (default 1200x900)\n"
        "--azimuth DEG / --elevation DEG: orbit camera angles for `render`,\n"
        "               degrees (defaults 35 / 25); the projection is orthographic\n"
        "               so a view is reproducible from these two numbers\n"
        "--wireframe: overlay the tessellation triangle edges on the render\n"
        "--stats out.json: numeric render report — node/element counts, element-type\n"
        "               census, triangle count, covered/silhouette pixels, and the\n"
        "               facet-normal deviation against the exact BRep normal\n"
        "               (omitted for non-CAD input, which has no exact normal)\n"
        "\n"
        "default BC selection: nodes in a 0.51·h slab at min-x (fixed) / max-x\n"
        "              (loaded). If a slab captures too few nodes to act as a\n"
        "              face (curved parts), selection falls back to boundary\n"
        "              faces whose outward normal aligns with ∓x/±x.\n",
        stderr);
    return 2;
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

bool parse_scale(std::span<char*> args, std::size_t& i, double& scale) {
    if (i + 1 >= args.size()) {
        return false;
    }
    const double parsed = std::atof(args[++i]);
    if (!(parsed > 0.0) || !std::isfinite(parsed)) {
        std::fputs("--scale: factor must be finite and positive\n", stderr);
        return false;
    }
    scale = parsed;
    return true;
}

void report_scale(double scale) {
    if (scale != 1.0) {
        std::printf("scale: %.6g (model units x factor)\n", scale);
    }
}

bool parse_mesher_arg(const std::string& m, polymesh::pipeline::VolumeMesher& out) {
    const auto parsed = polymesh::pipeline::mesher_from_name(m);
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

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

bool parse_load_flag(std::span<char*> args, std::size_t& i,
                     polymesh::fea::SurfaceLoadSpec& spec) {
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

} // namespace polymesh::cli
