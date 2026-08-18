// SPDX-License-Identifier: BSD-3-Clause
// Headless CPU render of the authoritative curved boundary surface.
//
// The renderer exists so curved-geometry work can be verified without an
// OpenGL Studio session, so the properties under test are exactly the ones a
// screenshot cannot fake:
// 1. It draws something. Every pixel it reports as covered really does differ
//    from the background-only render of the same view, and the sphere occupies
//    a plausible fraction of the frame instead of one stray triangle.
// 2. It is reproducible. Identical inputs give a byte-identical image buffer
//    and a byte-identical PNG file, so a diff between two renders is evidence
//    about the geometry and never about the rasterizer.
// 3. It measures curvature honestly. Facet normals of the tessellated tet10
//    surface track the exact BRep normal, and the straight-edged tet4 mesh of
//    the same fill is measurably worse.

#include "fea/traction.hpp"
#include "geom/step.hpp"
#include "pipeline/scene.hpp"
#include "pipeline/surface_render.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr char kSphere[] = "tests/fixtures/parts/sphere.step";

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
    std::vector<std::uint8_t> bytes;
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return bytes;
    }
    bytes.resize(std::filesystem::file_size(path));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    bytes.resize(read);
    return bytes;
}

} // namespace

TEST_CASE("headless render: draws, repeats byte-for-byte, and tracks the exact BRep") {
    if (!polymesh::geom::occ_enabled()) {
        SKIP("OpenCASCADE disabled");
    }
    if (!std::filesystem::exists(kSphere)) {
        SKIP("sphere.step missing");
    }
    namespace pipeline = polymesh::pipeline;
    namespace fea = polymesh::fea;

    // Coarse on purpose: the properties are structural, not resolution-limited.
    constexpr double kH = 0.035; // sphere.step is R = 0.05 m
    constexpr int kSubdiv = 4;
    const auto model = pipeline::Model::load(kSphere);
    REQUIRE(model.cad.has_value());
    const auto vol = pipeline::volume_mesh(model, kH, pipeline::VolumeMesher::kGradedTet);
    REQUIRE_FALSE(vol.mesh.elements.empty());

    // The straight-edged fill and the promoted/projected curved geometry, both
    // tessellated by the very routine the Studio viewport uploads.
    const auto chordal = fea::tessellate_boundary_surface(vol.mesh, kSubdiv);
    const auto curved = pipeline::curve_volume_geometry(model, vol.mesh, kH);
    const auto surface = fea::tessellate_boundary_surface(curved.mesh, kSubdiv);
    REQUIRE(surface.triangles.size() > chordal.triangles.size());

    pipeline::RenderView view;
    view.width = 320;
    view.height = 240;
    const auto render = pipeline::render_surface(curved.mesh, surface, view);
    REQUIRE(render.image.width == view.width);
    REQUIRE(render.image.height == view.height);

    SECTION("the render is not a no-op") {
        // A sphere fitted with a 6% margin covers ~pi/4 of the shorter frame
        // dimension squared; anything near 0 or near the whole frame means the
        // camera fit or the z-buffer collapsed.
        const std::size_t frame = static_cast<std::size_t>(view.width) *
                                  static_cast<std::size_t>(view.height);
        REQUIRE(render.coverage.silhouette_area_px > frame / 4);
        REQUIRE(render.coverage.silhouette_area_px < (frame * 7) / 10);

        // Every pixel claimed as covered really differs from the same view drawn
        // with nothing in it, and no other pixel does.
        const auto empty = pipeline::render_surface(curved.mesh, fea::SurfaceTessellation{},
                                                   view);
        REQUIRE(empty.coverage.pixels_covered == 0);
        std::size_t differing = 0;
        for (std::size_t p = 0; p < frame; ++p) {
            const bool same = render.image.rgb[p * 3 + 0] == empty.image.rgb[p * 3 + 0] &&
                              render.image.rgb[p * 3 + 1] == empty.image.rgb[p * 3 + 1] &&
                              render.image.rgb[p * 3 + 2] == empty.image.rgb[p * 3 + 2];
            differing += same ? 0u : 1u;
        }
        REQUIRE(differing == render.coverage.pixels_covered);
    }

    SECTION("identical inputs give a byte-identical image and PNG") {
        const auto again = pipeline::render_surface(curved.mesh, surface, view);
        // Compared through std::equal, not operator==: Catch2 stringifies both
        // operands of a container comparison even when it succeeds, and a
        // quarter-megabyte pixel buffer overruns the reporter's string builder.
        REQUIRE(again.image.rgb.size() == render.image.rgb.size());
        REQUIRE(std::equal(again.image.rgb.begin(), again.image.rgb.end(),
                           render.image.rgb.begin()));
        REQUIRE(again.coverage.pixels_covered == render.coverage.pixels_covered);

        const auto dir = std::filesystem::temp_directory_path();
        const auto first = dir / "polymesh_render_determinism_a.png";
        const auto second = dir / "polymesh_render_determinism_b.png";
        REQUIRE(pipeline::write_png(first.string(), render.image));
        REQUIRE(pipeline::write_png(second.string(), again.image));
        const auto a = read_all(first);
        const auto b = read_all(second);
        REQUIRE_FALSE(a.empty());
        REQUIRE(a.size() == b.size());
        REQUIRE(std::equal(a.begin(), a.end(), b.begin()));

        // Valid PNG framing: signature, IHDR dimensions, IEND terminator. The
        // pixel payload is exercised by the coverage section above.
        constexpr std::uint8_t signature[] = {0x89, 0x50, 0x4E, 0x47,
                                             0x0D, 0x0A, 0x1A, 0x0A};
        REQUIRE(std::equal(std::begin(signature), std::end(signature), a.begin()));
        REQUIRE(std::string(a.begin() + 12, a.begin() + 16) == "IHDR");
        const auto be32 = [&a](std::size_t at) {
            return (static_cast<std::uint32_t>(a[at]) << 24) |
                   (static_cast<std::uint32_t>(a[at + 1]) << 16) |
                   (static_cast<std::uint32_t>(a[at + 2]) << 8) |
                   static_cast<std::uint32_t>(a[at + 3]);
        };
        REQUIRE(be32(16) == static_cast<std::uint32_t>(view.width));
        REQUIRE(be32(20) == static_cast<std::uint32_t>(view.height));
        REQUIRE(std::string(a.end() - 8, a.end() - 4) == "IEND");

        std::filesystem::remove(first);
        std::filesystem::remove(second);
    }

    SECTION("curved facet normals track the BRep and beat the chordal fill") {
        const auto curved_deviation =
            pipeline::exact_facet_normal_deviation(*model.cad, surface);
        const auto chordal_deviation =
            pipeline::exact_facet_normal_deviation(*model.cad, chordal);
        REQUIRE(curved_deviation.samples > 0);
        REQUIRE(chordal_deviation.samples > 0);

        // Measured on this fixture at h=0.035, subdiv=4, graded-tet fill:
        // curved p99 1.695 deg (mean 0.630 over 1984 sampled facets)
        // against chordal p99 6.588 deg (mean 2.532 over 372 facets). The 2.5 deg
        // ceiling and the 2x margin are those numbers with headroom for mesher
        // jitter, not aspirational targets.
        REQUIRE(curved_deviation.p99 < 2.5);
        REQUIRE(curved_deviation.p99 * 2.0 < chordal_deviation.p99);
        REQUIRE(curved_deviation.mean < chordal_deviation.mean);
    }
}
