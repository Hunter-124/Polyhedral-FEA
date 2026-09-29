// SPDX-License-Identifier: BSD-3-Clause

// `polymesh render`: headless PNG of the boundary surface the Studio viewport
// paints, with an optional numeric `--stats` report.

#include "cli_common.hpp"

#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"
#include "pipeline/scene.hpp"
#include "pipeline/surface_render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <span>
#include <string>
#include <utility>

namespace polymesh::cli {
namespace {

/// `WxH` render size. Rejects anything that is not two positive integers so a
/// typo cannot silently render at the default size.
bool parse_size(const char* text, int& width, int& height) {
    char* end = nullptr;
    const long w = std::strtol(text, &end, 10);
    if (end == text || (*end != 'x' && *end != 'X')) {
        return false;
    }
    const char* rest = end + 1;
    const long hgt = std::strtol(rest, &end, 10);
    if (end == rest || *end != '\0' || w < 1 || hgt < 1 || w > 16384 || hgt > 16384) {
        return false;
    }
    width = static_cast<int>(w);
    height = static_cast<int>(hgt);
    return true;
}

} // namespace

int cmd_render(std::span<char*> args) {
    if (args.size() < 3) {
        return usage();
    }
    const std::string path = args[2];
    double h = 0.0;
    std::string out_path;
    std::string stats_path;
    auto mesher = polymesh::pipeline::VolumeMesher::kGradedTet;
    bool curved = true;  // exact curved CAD geometry (ADR-0035); --no-curved opts out
    bool feature = true; // same geometry grading the product mesh path uses
    bool spectral = true;
    int subdiv = 8; // the subdivision count the Studio viewport tessellates with
    polymesh::pipeline::RenderView view;
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
        } else if (std::strcmp(args[i], "--no-curved") == 0) {
            curved = false;
        } else if (std::strcmp(args[i], "--no-feature") == 0) {
            feature = false;
        } else if (std::strcmp(args[i], "--no-spectral") == 0) {
            spectral = false;
        } else if (std::strcmp(args[i], "--scale") == 0) {
            if (!parse_scale(args, i, scale)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--subdiv") == 0 && i + 1 < args.size()) {
            // Clamped to the tessellator's own range so --stats reports the
            // subdivision count that was actually used.
            subdiv = std::clamp(std::atoi(args[++i]), 1, 16);
        } else if (std::strcmp(args[i], "--size") == 0 && i + 1 < args.size()) {
            if (!parse_size(args[++i], view.width, view.height)) {
                return usage();
            }
        } else if (std::strcmp(args[i], "--azimuth") == 0 && i + 1 < args.size()) {
            view.azimuth_deg = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--elevation") == 0 && i + 1 < args.size()) {
            view.elevation_deg = std::atof(args[++i]);
        } else if (std::strcmp(args[i], "--wireframe") == 0) {
            view.wireframe = true;
        } else if (std::strcmp(args[i], "--stats") == 0 && i + 1 < args.size()) {
            stats_path = args[++i];
        } else {
            return usage();
        }
    }
    if (out_path.empty()) {
        std::fputs("render: -o out.png is required\n", stderr);
        return usage();
    }

    const auto model = polymesh::pipeline::Model::load(path, 30.0, scale);
    report_scale(scale);
    const auto resolved = polymesh::pipeline::resolve_mesh_size(model, h, 30.0, 0, 0);
    h = resolved.h;

    // Exactly the product mesh path `polymesh mesh` runs, so the render can only
    // show geometry the shipped mesher actually produced.
    const auto plan =
        polymesh::pipeline::build_refinement_plan(model, h, {}, feature, spectral, 0);
    auto vol = polymesh::pipeline::volume_mesh(
        model, h, mesher, 2, feature, plan.refine_seeds, plan.seed_band, 0.0,
        resolved.element_ceiling, resolved.dof_ceiling, resolved.auto_chosen ? 3 : 0, {},
        plan.size_field);
    if (curved) {
        auto shaped = polymesh::pipeline::curve_volume_geometry(model, vol.mesh, h);
        vol.mesh = std::move(shaped.mesh);
    }
    vol.mesh.check_validity();

    // Never re-derive a surface here: the point of the tool is to rasterize the
    // very tessellation the Studio viewport uploads.
    const auto surface = polymesh::fea::tessellate_boundary_surface(vol.mesh, subdiv);
    const auto render = polymesh::pipeline::render_surface(vol.mesh, surface, view);
    if (!polymesh::pipeline::write_png(out_path, render.image)) {
        std::fprintf(stderr, "render: cannot write %s\n", out_path.c_str());
        return 1;
    }

    std::map<std::string, std::size_t> census;
    for (const auto& element : vol.mesh.elements) {
        ++census[polymesh::fea::element_type_name(element.type)];
    }
    std::string census_text;
    std::string census_json;
    for (const auto& [name, count] : census) {
        if (!census_text.empty()) {
            census_text += " ";
            census_json += ",";
        }
        census_text += std::format("{}={}", name, count);
        census_json += std::format("\"{}\":{}", name, count);
    }

    std::printf("render: %zu nodes, %zu elems (%s), %zu triangles (subdiv=%d), "
                "h=%.6g m, curved=%s → %s (%dx%d, %zu px covered)\n",
                vol.mesh.nodes.size(), vol.mesh.elements.size(), census_text.c_str(),
                surface.triangles.size(), subdiv, h, curved ? "true" : "false",
                out_path.c_str(), render.image.width, render.image.height,
                render.coverage.pixels_covered);

    if (stats_path.empty()) {
        return 0;
    }
    // The facet-normal audit is the number that separates curved from chordal
    // geometry, so it is measured against the exact BRep whenever one exists and
    // labelled with the reference it actually used. Non-CAD input has no exact
    // normal at all: the field is then omitted rather than reported as zero.
    std::string normal_json;
    if (model.cad && !model.cad->empty()) {
        auto deviation = polymesh::pipeline::exact_facet_normal_deviation(*model.cad, surface);
        const char* reference = "exact_brep";
        if (deviation.samples == 0) {
            deviation =
                polymesh::pipeline::tessellated_facet_normal_deviation(model.surface, surface);
            reference = "tessellated_surface";
        }
        if (deviation.samples > 0) {
            normal_json = std::format(
                "  \"normal_reference\": \"{}\",\n"
                "  \"normal_deviation_deg\": {{ \"samples\": {}, \"mean\": {:.6g}, "
                "\"p99\": {:.6g}, \"max\": {:.6g} }},\n",
                reference, deviation.samples, deviation.mean, deviation.p99, deviation.max);
        }
    }
    const std::string json =
        std::format("{{\n"
                    "  \"part\": \"{}\",\n"
                    "  \"mesher\": \"{}\",\n"
                    "  \"h\": {:.6g},\n"
                    "  \"curved\": {},\n"
                    "  \"subdiv\": {},\n"
                    "  \"nodes\": {},\n"
                    "  \"elements\": {},\n"
                    "  \"element_types\": {{{}}},\n"
                    "  \"triangles\": {},\n"
                    "  \"width\": {},\n"
                    "  \"height\": {},\n"
                    "  \"pixels_covered\": {},\n"
                    "  \"silhouette_area_px\": {},\n"
                    "{}"
                    "  \"png\": \"{}\"\n"
                    "}}\n",
                    model.name, polymesh::pipeline::mesher_name(mesher), h,
                    curved ? "true" : "false", subdiv, vol.mesh.nodes.size(),
                    vol.mesh.elements.size(), census_json, surface.triangles.size(),
                    render.image.width, render.image.height, render.coverage.pixels_covered,
                    render.coverage.silhouette_area_px, normal_json, out_path);
    std::FILE* file = std::fopen(stats_path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "render: cannot write %s\n", stats_path.c_str());
        return 1;
    }
    std::fputs(json.c_str(), file);
    std::fclose(file);
    std::printf("wrote %s\n", stats_path.c_str());
    return 0;
}

} // namespace polymesh::cli
