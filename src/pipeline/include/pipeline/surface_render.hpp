// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Headless CPU render of the authoritative curved boundary surface, plus the
// facet-normal audit that says how close the rendered facets sit to the exact
// CAD surface.
//
// This lives in `pipeline` rather than `fea` on purpose. The renderer's whole
// job is to reproduce what the Studio viewport paints, so it consumes
// `fea::tessellate_boundary_surface` (never its own surface derivation) AND the
// `geom::CadModel` BRep for the exact-normal reference. `pipeline` is already
// the module that owns the headless product paths `apps/gui` merely presents,
// and it is the only core library that legitimately depends on both fea and
// geom — putting a rasterizer inside `polymesh::fea` would push presentation
// code into the solver library instead.

#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "geom/tri_surface.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace polymesh::pipeline {

/// Orbit camera + framing for the headless render. Angles are degrees and mean
/// exactly what a Studio orbit means: yaw about the model-up (+Z) axis,
/// elevation above the XY plane. The projection is orthographic so a render is
/// reproducible from these numbers alone — no field-of-view/standoff coupling.
struct RenderView {
    int width = 1200;
    int height = 900;
    double azimuth_deg = 35.0;
    double elevation_deg = 25.0;
    /// Overlay the tessellation triangle edges in the Studio edge colour.
    bool wireframe = false;
};

/// Deterministic 8-bit RGB image, row-major, top row first, 3 bytes per pixel.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

struct RenderCoverage {
    /// Pixels whose colour differs from the untouched background gradient.
    /// Includes wireframe edge pixels, so this is the "something was actually
    /// drawn" number.
    std::size_t pixels_covered = 0;
    /// Pixels that received a finite surface depth: the projected silhouette
    /// area of the geometry, independent of the wireframe overlay.
    std::size_t silhouette_area_px = 0;
};

struct SurfaceRender {
    Image image;
    RenderCoverage coverage;
};

/// Orthographic z-buffered render of `surface`, flat-shaded with the same
/// headlight + rim term and element-type palette the Studio viewport uses, over
/// the Studio dark viewport gradient. The camera is fitted to the tessellated
/// point cloud, so framing depends only on the geometry and `view`.
///
/// `mesh` supplies the per-node element type the palette keys on; it must be
/// the mesh `surface` was tessellated from.
SurfaceRender render_surface(const fea::NodalMesh& mesh,
                             const fea::SurfaceTessellation& surface,
                             const RenderView& view);

/// Angle between rendered facet normals and a reference surface normal, in
/// degrees. Orientation-agnostic (|cos| is used), so a facet whose winding is
/// inward still reports its true tilt rather than ~180 degrees.
struct NormalDeviation {
    std::size_t samples = 0;
    double mean = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

/// Facet-normal deviation against the **exact BRep** normal at each sampled
/// triangle's centroid (`geom::project_point_on_surface`). Every BRep
/// projection is an extrema solve, so triangles are sampled on a deterministic
/// stride that keeps at most `max_samples` of them. Returns `samples == 0` when
/// the model carries no BRep, this build has no OpenCASCADE, or no projection
/// succeeded — the caller must then report a different reference rather than a
/// zero deviation.
NormalDeviation exact_facet_normal_deviation(const geom::CadModel& cad,
                                            const fea::SurfaceTessellation& surface,
                                            std::size_t max_samples = 2000);

/// Same measure against the tessellated `TriSurface` normal at the closest
/// point: the honest fallback reference when no exact BRep normal exists.
NormalDeviation tessellated_facet_normal_deviation(const geom::TriSurface& reference,
                                                   const fea::SurfaceTessellation& surface,
                                                   std::size_t max_samples = 2000);

/// Write `image` as an 8-bit RGB PNG. Self-contained encoder (Sub-filtered
/// rows, fixed-Huffman DEFLATE) so the CLI gains no image-library dependency
/// and byte-identical inputs always produce a byte-identical file.
/// Returns false only when the file cannot be opened or written.
bool write_png(const std::string& path, const Image& image);

} // namespace polymesh::pipeline
