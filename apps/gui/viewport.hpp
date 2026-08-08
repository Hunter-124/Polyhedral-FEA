// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// 3D viewport: renders into an offscreen framebuffer shown as an ImGui
// image. Light studio-gradient background (AI-CAD viewport look), shaded
// model with per-region overlay colors, orbit/pan/zoom camera, CPU ray
// picking. Optional wireframe edges and undeformed outline on results.

#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <optional>

namespace polymesh::gui {

// Core types live in pipeline (headless). GUI only presents them.
using pipeline::Model;
using pipeline::RegionLoad;
using pipeline::SimSetup;
using pipeline::SolveJob;
using pipeline::SolveResult;
using pipeline::VolumeMeshOutput;

class Camera {
  public:
    /// Points the camera at the AABB center and backs off to 1.9x its diagonal,
    /// keeping the current orbit angles. A zero-size box falls back to distance 1.
    void fit(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max);
    void orbit(float dx, float dy);
    void pan(float dx, float dy, float viewport_height);
    void dolly(float scroll);

    Eigen::Matrix4f view() const;
    Eigen::Matrix4f projection(float aspect) const;
    Eigen::Vector3f eye() const;
    /// World-space ray through a pixel (coords in [0,1] across the image).
    void pixel_ray(float u, float v, float aspect, Eigen::Vector3f& origin,
                   Eigen::Vector3f& direction) const;

  private:
    Eigen::Vector3f target_ = Eigen::Vector3f::Zero();
    float distance_ = 3.0f;
    float yaw_ = 0.7f;   // radians
    float pitch_ = 0.5f; // radians
    float fov_y_ = 40.0f * 3.14159265f / 180.0f;
};

/// What the viewport is currently displaying.
enum class DisplayMode {
    kSetup = 0,
    kMeshPreview = 1,
    kResultsVonMises = 2,
    kResultsDisplacement = 3,
    kResultsError = 4,
};

class Viewport {
  public:
    ~Viewport();

    /// (Re)creates GL resources. Call once after GL context creation.
    void init();

    /// Uploads model geometry (setup mode).
    void set_model(const Model& model);
    /// Uploads volume mesh boundary for mesh-preview mode (element-type colors).
    void set_mesh(const VolumeMeshOutput& mesh);
    /// Uploads solve results (deformed boundary mesh + nodal scalars).
    void set_result(const SolveResult& result);

    /// Rebuilds per-triangle overlay colors from the setup + selection.
    void update_overlays(const Model& model, const SimSetup& setup, int selected_region,
                         int hovered_region);

    /// Drops palette-baked vertex colors after a theme swap. Result/mesh colors
    /// re-bake on the next render(); setup-mode overlays need the caller to
    /// re-issue update_overlays() (it owns the Model/SimSetup).
    void invalidate_colors();

    /// Renders the scene into the offscreen buffer at the given size.
    /// Wireframe draws boundary edges; undeformed draws rest outline on results.
    void render(int width, int height, DisplayMode mode, float deform_scale, float result_max,
                bool show_wireframe = false, bool show_undeformed = false);

    /// Texture handle to show via ImGui::Image.
    std::uint32_t texture() const { return color_texture_; }

    bool has_mesh_preview() const { return mesh_vertex_count_ > 0; }
    bool has_result() const { return !result_rest_.empty(); }

    /// Frames whatever `mode` shows: camera target = the AABB center of the
    /// uploaded geometry, distance = 1.9x its diagonal, orbit angles untouched.
    /// Falls back to any other uploaded buffer when `mode` has none, and keeps
    /// the current camera when nothing is uploaded at all. True = camera moved.
    bool frame_content(DisplayMode mode);

    Camera camera;

    /// Picks the model triangle under the pixel; returns its region id.
    std::optional<int> pick_region(const Model& model, float u, float v, float aspect) const;

  private:
    /// World AABB of one uploaded buffer; `valid` is false while it is empty.
    struct Bounds {
        Eigen::Vector3d min = Eigen::Vector3d::Zero();
        Eigen::Vector3d max = Eigen::Vector3d::Zero();
        bool valid = false;

        void reset() { valid = false; }
        void add(const Eigen::Vector3d& p) {
            if (valid) {
                min = min.cwiseMin(p);
                max = max.cwiseMax(p);
            } else {
                min = p;
                max = p;
                valid = true;
            }
        }
    };

    std::uint32_t fbo_ = 0, color_texture_ = 0, depth_rbo_ = 0;
    int fb_width_ = 0, fb_height_ = 0;
    std::uint32_t model_program_ = 0, background_program_ = 0, line_program_ = 0;
    // Setup-mode model buffers.
    std::uint32_t model_vao_ = 0, model_vbo_ = 0;
    int model_vertex_count_ = 0;
    // Mesh-preview buffers (undeformed boundary, element-type colors).
    std::uint32_t mesh_vao_ = 0, mesh_vbo_ = 0;
    int mesh_vertex_count_ = 0;
    // Rest-position boundary edges (mesh wireframe + undeformed outline).
    std::uint32_t mesh_edge_vao_ = 0, mesh_edge_vbo_ = 0;
    int mesh_edge_vertex_count_ = 0;
    // Results-mode buffers (deformed voxel boundary, scalar-colored).
    std::uint32_t result_vao_ = 0, result_vbo_ = 0;
    int result_vertex_count_ = 0;
    // Deformed boundary edges (wireframe on results).
    std::uint32_t result_edge_vao_ = 0, result_edge_vbo_ = 0;
    int result_edge_vertex_count_ = 0;
    std::uint32_t background_vao_ = 0;
    // CPU-side copies for overlay recolor and picking.
    std::vector<float> model_vertex_data_;
    // Results-mode CPU data, re-baked when mode/scale/range changes.
    std::vector<Eigen::Vector3d> result_rest_;
    std::vector<double> result_scalar_vm_;
    std::vector<double> result_scalar_u_;
    std::vector<double> result_scalar_eta_;
    std::vector<std::array<std::uint32_t, 4>> result_quads_;
    Eigen::VectorXd result_disp_;
    bool result_dirty_ = false;
    DisplayMode baked_mode_ = DisplayMode::kSetup;
    float baked_scale_ = -1.0f;
    float baked_max_ = -1.0f;
    // World AABBs of the uploaded geometry, refreshed on every upload/bake and
    // consumed by frame_content().
    Bounds model_bounds_;
    Bounds mesh_bounds_;
    Bounds result_bounds_;
    /// set_result() arms this; the first bake_result() of that result frames it.
    bool frame_on_bake_ = false;
    void bake_result(DisplayMode mode, float deform_scale, float result_max);
    void ensure_framebuffer(int width, int height);
    void upload_boundary_edges(const std::vector<Eigen::Vector3d>& nodes,
                               const std::vector<std::array<std::uint32_t, 4>>& quads, float r,
                               float g, float b, float a, std::uint32_t vbo,
                               int& vertex_count);
};

} // namespace polymesh::gui
