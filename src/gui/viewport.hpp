// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// 3D viewport: renders into an offscreen framebuffer shown as an ImGui
// image. Light studio-gradient background (AI-CAD viewport look), shaded
// model with per-region overlay colors, orbit/pan/zoom camera, CPU ray
// picking.

#include "scene.hpp"

#include <Eigen/Core>

#include <cstdint>
#include <optional>

namespace polymesh::gui {

class Camera {
  public:
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
enum class DisplayMode { kSetup, kResultsVonMises, kResultsDisplacement };

class Viewport {
  public:
    ~Viewport();

    /// (Re)creates GL resources. Call once after GL context creation.
    void init();

    /// Uploads model geometry (setup mode).
    void set_model(const Model& model);
    /// Uploads solve results (deformed boundary mesh + nodal scalars).
    void set_result(const SolveResult& result);

    /// Rebuilds per-triangle overlay colors from the setup + selection.
    void update_overlays(const Model& model, const SimSetup& setup, int selected_region,
                         int hovered_region);

    /// Renders the scene into the offscreen buffer at the given size.
    void render(int width, int height, DisplayMode mode, float deform_scale, float result_max);

    /// Texture handle to show via ImGui::Image.
    std::uint32_t texture() const { return color_texture_; }

    Camera camera;

    /// Picks the model triangle under the pixel; returns its region id.
    std::optional<int> pick_region(const Model& model, float u, float v, float aspect) const;

  private:
    std::uint32_t fbo_ = 0, color_texture_ = 0, depth_rbo_ = 0;
    int fb_width_ = 0, fb_height_ = 0;
    std::uint32_t model_program_ = 0, background_program_ = 0;
    // Setup-mode model buffers.
    std::uint32_t model_vao_ = 0, model_vbo_ = 0;
    int model_vertex_count_ = 0;
    // Results-mode buffers (deformed voxel boundary, scalar-colored).
    std::uint32_t result_vao_ = 0, result_vbo_ = 0;
    int result_vertex_count_ = 0;
    std::uint32_t background_vao_ = 0;
    // CPU-side copies for overlay recolor and picking.
    std::vector<float> model_vertex_data_;
    // Results-mode CPU data, re-baked when mode/scale/range changes.
    std::vector<Eigen::Vector3d> result_rest_;
    std::vector<double> result_scalar_vm_;
    std::vector<double> result_scalar_u_;
    std::vector<std::array<std::uint32_t, 4>> result_quads_;
    Eigen::VectorXd result_disp_;
    bool result_dirty_ = false;
    DisplayMode baked_mode_ = DisplayMode::kSetup;
    float baked_scale_ = -1.0f;
    float baked_max_ = -1.0f;
    void bake_result(DisplayMode mode, float deform_scale, float result_max);
    void ensure_framebuffer(int width, int height);
};

} // namespace polymesh::gui
