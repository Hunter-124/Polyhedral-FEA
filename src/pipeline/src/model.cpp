// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "geom/cad_model.hpp"
#include "geom/tri_surface.hpp"
#include "mesh/mirror.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace polymesh::pipeline {

namespace {

Eigen::Vector3d triangle_normal(const geom::TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    return ab.cross(ac).normalized();
}

} // namespace

Model Model::load(const std::string& path, double sharp_angle_deg, double scale) {
    Model model;
    const auto lower = [&] {
        std::string s = path;
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    model.source_path = path;
    const auto slash = path.find_last_of("/\\");
    model.name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        throw std::runtime_error(
            std::format("import scale must be finite and positive (got {:.6g})", scale));
    }

    // CAD-only inputs (ADR-0020): STEP/BREP retain the live CadModel; the
    // tessellation is derived for regions, viewport, and legacy hybrid fill.
    // STL is not an accepted input.
    if (lower.ends_with(".step") || lower.ends_with(".stp")) {
        model.cad = geom::CadModel::load_step(path);
    } else if (lower.ends_with(".brep") || lower.ends_with(".brp")) {
        model.cad = geom::CadModel::load_brep(path);
    } else {
        throw std::runtime_error(
            std::format("unsupported input '{}': only CAD files are accepted "
                        "(.step, .stp, .brep, .brp). STL inputs are no longer supported.",
                        path));
    }
    // Unit conversion happens on the exact geometry, before a single derived
    // quantity exists: the tessellation, bbox, regions and mirror frame below
    // are all computed from the scaled BRep, so no consumer can observe a mix
    // of authored and scaled lengths.
    if (scale != 1.0) {
        model.cad = model.cad->scaled(scale);
    }
    model.surface = model.cad->tessellate();
    model.bbox_min = model.cad->bbox_min();
    model.bbox_max = model.cad->bbox_max();
    model.surface.validate();
    // Reflection symmetry of the exact geometry, once per load. Detected from the
    // BRep when there is one; the tessellation path is for OCC-disabled builds,
    // where the tessellation IS the geometry.
    model.mirror = model.cad && !model.cad->empty()
                       ? mesh::detect_mirror_frame(*model.cad, model.bbox_min, model.bbox_max)
                       : mesh::detect_mirror_frame(model.surface);

    // CAD-style face regions: grow across edges whose dihedral angle is
    // below the sharp threshold.
    const std::size_t n_tris = model.surface.triangles.size();
    std::vector<Eigen::Vector3d> normals(n_tris);
    for (std::size_t t = 0; t < n_tris; ++t) {
        normals[t] = triangle_normal(model.surface, t);
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edge_tris;
    for (std::size_t t = 0; t < n_tris; ++t) {
        const auto& tri = model.surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                         tri[static_cast<std::size_t>((e + 1) % 3)]);
            edge_tris[key].push_back(static_cast<std::uint32_t>(t));
        }
    }
    const double cos_sharp = std::cos(sharp_angle_deg * std::numbers::pi / 180.0);
    model.triangle_region.assign(n_tris, -1);
    for (std::size_t seed = 0; seed < n_tris; ++seed) {
        if (model.triangle_region[seed] >= 0) {
            continue;
        }
        const int region = model.region_count++;
        std::queue<std::uint32_t> frontier;
        frontier.push(static_cast<std::uint32_t>(seed));
        model.triangle_region[seed] = region;
        while (!frontier.empty()) {
            const auto t = frontier.front();
            frontier.pop();
            const auto& tri = model.surface.triangles[t];
            for (int e = 0; e < 3; ++e) {
                const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                             tri[static_cast<std::size_t>((e + 1) % 3)]);
                for (const auto other : edge_tris.at(key)) {
                    if (model.triangle_region[other] >= 0) {
                        continue;
                    }
                    if (normals[t].dot(normals[other]) > cos_sharp) {
                        model.triangle_region[other] = region;
                        frontier.push(other);
                    }
                }
            }
        }
    }
    return model;
}

} // namespace polymesh::pipeline
