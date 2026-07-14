// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// In-memory axis-aligned box geometry for tests, replacing on-disk STL inputs
// (STL is no longer an accepted product input; see pipeline::Model::load).
// The 12-triangle box has two triangles per face, so triangle_region = t/2
// yields the same 6 CAD-style face regions that Model::load grows from a box.

#include "geom/tri_surface.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <string>

namespace polymesh::testsupport {

/// Welded surface of an axis-aligned box [0,lx]×[0,ly]×[0,lz] (12 triangles,
/// outward winding). Face order: z-min, z-max, y-min, x-max, y-max, x-min.
inline geom::TriSurface box_surface(double lx, double ly, double lz) {
    geom::TriSurface s;
    s.vertices = {
        {0, 0, 0},   {lx, 0, 0},   {lx, ly, 0},  {0, ly, 0},
        {0, 0, lz},  {lx, 0, lz},  {lx, ly, lz}, {0, ly, lz},
    };
    s.triangles = {
        {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6}, {0, 4, 5}, {0, 5, 1},
        {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0},
    };
    return s;
}

/// In-memory box Model equivalent to Model::load of a box STL: exact bbox and
/// six face regions. `cad` stays empty (no BRep) — for CAD-topology paths use a
/// STEP fixture instead.
inline pipeline::Model box_model(double lx, double ly, double lz,
                                 const std::string& name = "box") {
    pipeline::Model m;
    m.surface = box_surface(lx, ly, lz);
    m.surface.validate();
    m.bbox_min = Eigen::Vector3d(0, 0, 0);
    m.bbox_max = Eigen::Vector3d(lx, ly, lz);
    m.triangle_region.resize(m.surface.triangles.size());
    for (std::size_t t = 0; t < m.surface.triangles.size(); ++t) {
        m.triangle_region[t] = static_cast<int>(t / 2);
    }
    m.region_count = 6;
    m.name = name;
    return m;
}

/// Build a Model from an in-memory surface (single region). For tests that need
/// a specific geometry from an STL fixture as internal scaffolding — STL is not
/// a product input, but the parser (geom::load_stl) remains for test geometry.
inline pipeline::Model model_from_surface(geom::TriSurface surface,
                                          const std::string& name = "surface") {
    pipeline::Model m;
    m.surface = std::move(surface);
    m.surface.validate();
    m.bbox_min = m.surface.vertices.front();
    m.bbox_max = m.surface.vertices.front();
    for (const auto& v : m.surface.vertices) {
        m.bbox_min = m.bbox_min.cwiseMin(v);
        m.bbox_max = m.bbox_max.cwiseMax(v);
    }
    m.triangle_region.assign(m.surface.triangles.size(), 0);
    m.region_count = 1;
    m.name = name;
    return m;
}

} // namespace polymesh::testsupport