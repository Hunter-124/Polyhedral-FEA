// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/surface_render.hpp"

#include "mesh/surface_project.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace polymesh::pipeline {
namespace {

// --- rasterizer -----------------------------------------------------------

/// Orthographic camera basis. `eye_dir` points from the model toward the eye,
/// which for an orthographic projection is also the constant view direction the
/// headlight uses.
struct Basis {
    Eigen::Vector3d right = Eigen::Vector3d::UnitX();
    Eigen::Vector3d up = Eigen::Vector3d::UnitZ();
    Eigen::Vector3d eye_dir = Eigen::Vector3d::UnitY();
};

/// Same yaw/pitch → eye convention as the Studio camera (models are Z-up, the
/// orbit math is Y-up and swaps), so --azimuth/--elevation name the same view
/// an interactive orbit reaches.
Basis make_basis(double azimuth_deg, double elevation_deg) {
    constexpr double kDegToRad = 0.017453292519943295;
    const double yaw = azimuth_deg * kDegToRad;
    const double pitch = std::clamp(elevation_deg * kDegToRad, -1.55, 1.55);
    Basis basis;
    basis.eye_dir = Eigen::Vector3d(std::cos(pitch) * std::cos(yaw),
                                    std::cos(pitch) * std::sin(yaw), std::sin(pitch))
                        .normalized();
    const Eigen::Vector3d forward = -basis.eye_dir;
    const Eigen::Vector3d world_up(0.0, 0.0, 1.0);
    Eigen::Vector3d right = forward.cross(world_up);
    if (right.norm() < 1e-9) {
        // Straight down/up: any horizontal right vector is as good as another.
        right = forward.cross(Eigen::Vector3d::UnitY());
    }
    basis.right = right.normalized();
    basis.up = basis.right.cross(forward).normalized();
    return basis;
}

std::array<float, 3> element_type_color(fea::ElementType type) {
    // Same palette as the Studio mesh-preview viewport: a render that claims to
    // prove what Studio paints must not invent its own colours.
    switch (type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        return {0.42f, 0.58f, 0.92f};
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        return {0.35f, 0.78f, 0.50f};
    case fea::ElementType::kPyramid5:
        return {0.95f, 0.58f, 0.28f};
    case fea::ElementType::kPrism6:
        return {0.72f, 0.45f, 0.90f};
    case fea::ElementType::kPolyVem:
        return {0.25f, 0.82f, 0.85f};
    }
    return {0.6f, 0.6f, 0.6f};
}

std::uint8_t to_byte(double v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

/// Studio's dark viewport gradient: bottom #0F131A → mid #141922 → top #1B2028.
void fill_background(Image& image) {
    constexpr std::array<double, 3> kBottom{0.059, 0.075, 0.102};
    constexpr std::array<double, 3> kMid{0.078, 0.098, 0.133};
    constexpr std::array<double, 3> kTop{0.106, 0.125, 0.157};
    for (int y = 0; y < image.height; ++y) {
        // t = 0 at the bottom row, matching the GUI background shader.
        const double t = image.height > 1 ? 1.0 - static_cast<double>(y) /
                                                      static_cast<double>(image.height - 1)
                                          : 0.5;
        std::array<double, 3> rgb{};
        for (int c = 0; c < 3; ++c) {
            const double s = t > 0.5 ? (t - 0.5) * 2.0 : t * 2.0;
            rgb[static_cast<std::size_t>(c)] =
                t > 0.5 ? kMid[static_cast<std::size_t>(c)] * (1.0 - s) +
                              kTop[static_cast<std::size_t>(c)] * s
                        : kBottom[static_cast<std::size_t>(c)] * (1.0 - s) +
                              kMid[static_cast<std::size_t>(c)] * s;
        }
        for (int x = 0; x < image.width; ++x) {
            const std::size_t o =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
                 static_cast<std::size_t>(x)) *
                3;
            image.rgb[o + 0] = to_byte(rgb[0]);
            image.rgb[o + 1] = to_byte(rgb[1]);
            image.rgb[o + 2] = to_byte(rgb[2]);
        }
    }
}

Eigen::Vector3d facet_normal(const fea::SurfaceTessellation& surface,
                             const std::array<std::uint32_t, 3>& tri) {
    const Eigen::Vector3d& a = surface.samples[tri[0]].position;
    const Eigen::Vector3d& b = surface.samples[tri[1]].position;
    const Eigen::Vector3d& c = surface.samples[tri[2]].position;
    const Eigen::Vector3d n = (b - a).cross(c - a);
    const double len = n.norm();
    return len > 1e-30 ? Eigen::Vector3d(n / len) : Eigen::Vector3d::Zero();
}

NormalDeviation summarize(std::vector<double>& degrees) {
    NormalDeviation out;
    if (degrees.empty()) {
        return out;
    }
    std::sort(degrees.begin(), degrees.end());
    out.samples = degrees.size();
    double sum = 0.0;
    for (const double d : degrees) {
        sum += d;
    }
    out.mean = sum / static_cast<double>(degrees.size());
    const std::size_t p99_index =
        static_cast<std::size_t>(std::llround(0.99 * static_cast<double>(degrees.size() - 1)));
    out.p99 = degrees[p99_index];
    out.max = degrees.back();
    return out;
}

/// Deterministic stride that keeps at most `max_samples` triangles.
std::size_t sample_stride(std::size_t count, std::size_t max_samples) {
    if (max_samples == 0 || count <= max_samples) {
        return 1;
    }
    return (count + max_samples - 1) / max_samples;
}

double angle_between_deg(const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
    constexpr double kRadToDeg = 57.29577951308232;
    // |cos|: a facet whose winding points inward is still only tilted by its
    // true angle, not 180 degrees minus it.
    const double c = std::clamp(std::abs(a.dot(b)), 0.0, 1.0);
    return std::acos(c) * kRadToDeg;
}

} // namespace

SurfaceRender render_surface(const fea::NodalMesh& mesh,
                             const fea::SurfaceTessellation& surface, const RenderView& view) {
    SurfaceRender out;
    out.image.width = std::max(view.width, 1);
    out.image.height = std::max(view.height, 1);
    const std::size_t pixels =
        static_cast<std::size_t>(out.image.width) * static_cast<std::size_t>(out.image.height);
    out.image.rgb.assign(pixels * 3, 0);
    fill_background(out.image);
    if (surface.triangles.empty() || surface.samples.empty()) {
        return out;
    }

    // Per-node element type, exactly as the Studio mesh preview keys its palette.
    std::vector<fea::ElementType> node_type(mesh.nodes.size(), fea::ElementType::kTet4);
    std::vector<char> node_set(mesh.nodes.size(), 0);
    for (const auto& element : mesh.elements) {
        for (const std::uint32_t node : element.nodes) {
            if (node < node_type.size()) {
                node_type[node] = element.type;
                node_set[node] = 1;
            }
        }
    }

    const Basis basis = make_basis(view.azimuth_deg, view.elevation_deg);

    // Camera-space coordinates of every tessellation sample, then an
    // orthographic fit around them.
    std::vector<Eigen::Vector3d> camera(surface.samples.size());
    Eigen::Vector2d lo(std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::infinity());
    Eigen::Vector2d hi = -lo;
    for (std::size_t i = 0; i < surface.samples.size(); ++i) {
        const Eigen::Vector3d& p = surface.samples[i].position;
        camera[i] = Eigen::Vector3d(basis.right.dot(p), basis.up.dot(p), basis.eye_dir.dot(p));
        lo[0] = std::min(lo[0], camera[i][0]);
        lo[1] = std::min(lo[1], camera[i][1]);
        hi[0] = std::max(hi[0], camera[i][0]);
        hi[1] = std::max(hi[1], camera[i][1]);
    }
    constexpr double kMargin = 0.06; // fraction of the frame left empty per side
    const double usable_w = (1.0 - 2.0 * kMargin) * static_cast<double>(out.image.width);
    const double usable_h = (1.0 - 2.0 * kMargin) * static_cast<double>(out.image.height);
    const double span_x = std::max(hi[0] - lo[0], 1e-12);
    const double span_y = std::max(hi[1] - lo[1], 1e-12);
    const double scale = std::min(usable_w / span_x, usable_h / span_y);
    const Eigen::Vector2d center = 0.5 * (lo + hi);

    std::vector<Eigen::Vector3d> screen(camera.size());
    double depth_lo = std::numeric_limits<double>::infinity();
    double depth_hi = -depth_lo;
    for (std::size_t i = 0; i < camera.size(); ++i) {
        screen[i] = Eigen::Vector3d(
            0.5 * static_cast<double>(out.image.width) + (camera[i][0] - center[0]) * scale,
            0.5 * static_cast<double>(out.image.height) - (camera[i][1] - center[1]) * scale,
            camera[i][2]);
        depth_lo = std::min(depth_lo, camera[i][2]);
        depth_hi = std::max(depth_hi, camera[i][2]);
    }

    // Larger depth = nearer the eye, so the z-test is a plain greater-than.
    std::vector<double> zbuffer(pixels, -std::numeric_limits<double>::infinity());
    std::vector<char> touched(pixels, 0);
    const auto put = [&](std::size_t index, const std::array<double, 3>& rgb) {
        out.image.rgb[index * 3 + 0] = to_byte(rgb[0]);
        out.image.rgb[index * 3 + 1] = to_byte(rgb[1]);
        out.image.rgb[index * 3 + 2] = to_byte(rgb[2]);
        touched[index] = 1;
    };

    for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d& s0 = screen[tri[0]];
        const Eigen::Vector3d& s1 = screen[tri[1]];
        const Eigen::Vector3d& s2 = screen[tri[2]];
        const double area =
            (s1[0] - s0[0]) * (s2[1] - s0[1]) - (s2[0] - s0[0]) * (s1[1] - s0[1]);
        if (std::abs(area) < 1e-12) {
            continue;
        }

        const Eigen::Vector3d normal = facet_normal(surface, tri);
        // Studio's fragment shader verbatim: double-sided headlight diffuse
        // (0.35 ambient + 0.6 Lambert) plus a small Fresnel-ish rim. Studio also
        // dithers per-facet brightness so coarse cells stay legible; that is
        // deliberately dropped here, because at subdiv 8 it degenerates into
        // per-sub-triangle noise over exactly the curvature this render exists to
        // show — `--wireframe` is the cell-separation tool in a headless render.
        const double ndv = std::abs(normal.dot(basis.eye_dir));
        const double shade = 0.35 + 0.6 * ndv;
        const double rim = std::pow(1.0 - ndv, 3.0) * 0.15;
        fea::ElementType type = fea::ElementType::kTet4;
        const std::uint32_t source = surface.samples[tri[0]].source_nodes[0];
        if (source < node_set.size() && node_set[source] != 0) {
            type = node_type[source];
        }
        const auto base = element_type_color(type);
        const std::array<double, 3> color{static_cast<double>(base[0]) * shade + rim,
                                          static_cast<double>(base[1]) * shade + rim,
                                          static_cast<double>(base[2]) * shade + rim};

        int x0 = static_cast<int>(std::floor(std::min({s0[0], s1[0], s2[0]})));
        int x1 = static_cast<int>(std::ceil(std::max({s0[0], s1[0], s2[0]})));
        int y0 = static_cast<int>(std::floor(std::min({s0[1], s1[1], s2[1]})));
        int y1 = static_cast<int>(std::ceil(std::max({s0[1], s1[1], s2[1]})));
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, out.image.width - 1);
        y1 = std::min(y1, out.image.height - 1);
        const double inv_area = 1.0 / area;
        for (int y = y0; y <= y1; ++y) {
            const double py = static_cast<double>(y) + 0.5;
            for (int x = x0; x <= x1; ++x) {
                const double px = static_cast<double>(x) + 0.5;
                const double w1 =
                    ((px - s0[0]) * (s2[1] - s0[1]) - (s2[0] - s0[0]) * (py - s0[1])) *
                    inv_area;
                const double w2 =
                    ((s1[0] - s0[0]) * (py - s0[1]) - (px - s0[0]) * (s1[1] - s0[1])) *
                    inv_area;
                const double w0 = 1.0 - w1 - w2;
                if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) {
                    continue;
                }
                const double z = w0 * s0[2] + w1 * s1[2] + w2 * s2[2];
                const std::size_t index =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(out.image.width) +
                    static_cast<std::size_t>(x);
                if (z <= zbuffer[index]) {
                    continue;
                }
                zbuffer[index] = z;
                put(index, color);
            }
        }
    }

    for (const double z : zbuffer) {
        if (std::isfinite(z)) {
            ++out.coverage.silhouette_area_px;
        }
    }

    if (view.wireframe) {
        // Studio draws edges with a depth bias (glPolygonOffset + GL_LEQUAL);
        // the CPU equivalent is a depth tolerance scaled to the scene depth.
        const double bias = 1e-3 * std::max(depth_hi - depth_lo, 1e-12);
        constexpr std::array<double, 3> kEdge{0.02, 0.02, 0.04};
        const auto line = [&](const Eigen::Vector3d& a, const Eigen::Vector3d& b) {
            int ax = static_cast<int>(std::lround(a[0]));
            int ay = static_cast<int>(std::lround(a[1]));
            const int bx = static_cast<int>(std::lround(b[0]));
            const int by = static_cast<int>(std::lround(b[1]));
            const int dx = std::abs(bx - ax);
            const int dy = -std::abs(by - ay);
            const int sx = ax < bx ? 1 : -1;
            const int sy = ay < by ? 1 : -1;
            const int steps = std::max(dx, -dy);
            int error = dx + dy;
            for (int step = 0;; ++step) {
                if (ax >= 0 && ay >= 0 && ax < out.image.width && ay < out.image.height) {
                    const double t =
                        steps > 0 ? static_cast<double>(step) / static_cast<double>(steps)
                                  : 0.0;
                    const double z = a[2] + (b[2] - a[2]) * t;
                    const std::size_t index = static_cast<std::size_t>(ay) *
                                                  static_cast<std::size_t>(out.image.width) +
                                              static_cast<std::size_t>(ax);
                    if (z + bias >= zbuffer[index]) {
                        put(index, kEdge);
                    }
                }
                if (ax == bx && ay == by) {
                    break;
                }
                const int e2 = 2 * error;
                if (e2 >= dy) {
                    error += dy;
                    ax += sx;
                }
                if (e2 <= dx) {
                    error += dx;
                    ay += sy;
                }
            }
        };
        for (const auto& tri : surface.triangles) {
            line(screen[tri[0]], screen[tri[1]]);
            line(screen[tri[1]], screen[tri[2]]);
            line(screen[tri[2]], screen[tri[0]]);
        }
    }

    for (const char t : touched) {
        if (t != 0) {
            ++out.coverage.pixels_covered;
        }
    }
    return out;
}

NormalDeviation exact_facet_normal_deviation(const geom::CadModel& cad,
                                             const fea::SurfaceTessellation& surface,
                                             std::size_t max_samples) {
    if (cad.empty() || surface.triangles.empty()) {
        return {};
    }
    const std::size_t stride = sample_stride(surface.triangles.size(), max_samples);
    std::vector<double> degrees;
    degrees.reserve(surface.triangles.size() / stride + 1);
    for (std::size_t ti = 0; ti < surface.triangles.size(); ti += stride) {
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d normal = facet_normal(surface, tri);
        if (normal.squaredNorm() < 0.5) {
            continue; // degenerate facet: no orientation to compare
        }
        const Eigen::Vector3d centroid =
            (surface.samples[tri[0]].position + surface.samples[tri[1]].position +
             surface.samples[tri[2]].position) /
            3.0;
        const auto projected = geom::project_point_on_surface(cad, centroid);
        if (!projected || projected->normal.squaredNorm() < 0.5) {
            continue;
        }
        degrees.push_back(angle_between_deg(normal, projected->normal));
    }
    return summarize(degrees);
}

NormalDeviation tessellated_facet_normal_deviation(const geom::TriSurface& reference,
                                                   const fea::SurfaceTessellation& surface,
                                                   std::size_t max_samples) {
    if (reference.triangles.empty() || surface.triangles.empty()) {
        return {};
    }
    const std::size_t stride = sample_stride(surface.triangles.size(), max_samples);
    std::vector<double> degrees;
    degrees.reserve(surface.triangles.size() / stride + 1);
    for (std::size_t ti = 0; ti < surface.triangles.size(); ti += stride) {
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d normal = facet_normal(surface, tri);
        if (normal.squaredNorm() < 0.5) {
            continue;
        }
        const Eigen::Vector3d centroid =
            (surface.samples[tri[0]].position + surface.samples[tri[1]].position +
             surface.samples[tri[2]].position) /
            3.0;
        const auto closest = mesh::closest_on_surface(reference, centroid);
        const auto& ref_tri = reference.triangles[closest.triangle];
        const Eigen::Vector3d ref_normal =
            (reference.vertices[ref_tri[1]] - reference.vertices[ref_tri[0]])
                .cross(reference.vertices[ref_tri[2]] - reference.vertices[ref_tri[0]]);
        if (ref_normal.norm() < 1e-30) {
            continue;
        }
        degrees.push_back(angle_between_deg(normal, ref_normal.normalized()));
    }
    return summarize(degrees);
}

} // namespace polymesh::pipeline
