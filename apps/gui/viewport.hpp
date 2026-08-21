// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// 3D viewport: renders into an offscreen framebuffer shown as an ImGui
// image. Light studio-gradient background (AI-CAD viewport look), shaded
// model with per-region overlay colors, orbit/pan/zoom camera, CPU ray
// picking. Optional wireframe edges and undeformed outline on results.
// DisplayMode::kCinema is a separate draw path for the advisor cinema: the
// part's BRep skeleton plus per-element mesh geometry that reveals in the
// mesher's own emission order.

#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace polymesh::gui {

// Core types live in pipeline (headless). GUI only presents them.
using pipeline::Model;
using pipeline::RegionLoad;
using pipeline::SimSetup;
using pipeline::SolveJob;
using pipeline::SolveResult;
using pipeline::VolumeMeshOutput;

/// Canonical element-type colour shared by mesh preview, cinema geometry and
/// the cinema legend. Keeping one function prevents a "green hex" label from
/// disagreeing with the cell beside it.
[[nodiscard]] std::array<float, 3> element_type_color(fea::ElementType type);

class Camera {
  public:
    /// Points the camera at the AABB center and backs off to 1.9x its diagonal,
    /// keeping the current orbit angles. A zero-size box falls back to distance 1.
    void fit(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max);
    /// Tight framing for a shot whose orbit is already chosen: backs off only
    /// as far as the box's own PROJECTION at the current yaw/pitch needs,
    /// instead of clearing its bounding sphere. A flat plate framed by `fit`
    /// sits in the middle of a mostly empty pane, because its diagonal is
    /// several times its projected height. `fill` is the fraction of the pane
    /// the projection may occupy; the horizontal field is sized as if the pane
    /// were square, so the result can never clip in a pane at least as wide as
    /// it is tall (which the cinema pane always is) and only ever under-fills
    /// it by the aspect ratio.
    void fit_oriented(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                      float fill);
    /// Absolute orbit angles, radians. Pitch is clamped to the same range
    /// `orbit` clamps to. For a composed shot that has to pick its own
    /// elevation rather than inherit whatever the user last dragged.
    void set_orbit(float yaw, float pitch);
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

/// What the viewport is currently displaying. `kResultsGradient` shows the
/// extra per-node field handed to `set_result`, on the same deformed geometry
/// and through the same colormap as the other results modes.
enum class DisplayMode {
    kSetup = 0,
    kMeshPreview = 1,
    kResultsVonMises = 2,
    kResultsDisplacement = 3,
    kResultsError = 4,
    kCinema = 5,
    kResultsGradient = 6,
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
    ///
    /// `nodal_extra` is an OPTIONAL fourth scalar field, one value per node of
    /// `result.volume_mesh`, shown by `DisplayMode::kResultsGradient`. It is
    /// interpolated onto the boundary surface samples by the same weights as
    /// the von Mises field, so it is displayed on exactly the same footing.
    /// A null pointer, or a size that does not match the mesh's node count,
    /// clears it: `kResultsGradient` then bakes zeros rather than quietly
    /// showing some other field under the gradient's legend.
    void set_result(const SolveResult& result,
                    const std::vector<double>* nodal_extra = nullptr);

    /// BRep/feature-edge polylines of the part, drawn as the pre-mesh skeleton.
    void set_skeleton(const std::vector<std::vector<Eigen::Vector3d>>& polylines);
    /// Per-element geometry for the cinema reveal: every element's own faces,
    /// tagged with its index in `mesh.elements` so the reveal order is the
    /// mesher's own emission order. Interior faces are therefore stored once per
    /// owning element, which is the point — the two copies belong to different
    /// elements and appear at different times. Measured cost for tet4 cells:
    /// 4 triangles (12 vertices x 56 B) plus 6 edges (12 vertices x 44 B) =
    /// 1200 B per element exactly, so a 200k-tet mesh holds 229 MiB of GL
    /// buffers. Upload the mesh you intend to film, not the finest adapt pass.
    void set_cinema_mesh(const fea::NodalMesh& mesh);
    std::size_t cinema_element_count() const;
    /// Elements whose faces could not be built (degenerate connectivity, or a
    /// poly-VEM cell with no face loops). Counted, never silently dropped, so
    /// the panel can say on screen that the reveal is not the whole mesh.
    std::size_t cinema_skipped_element_count() const;
    /// Cinema draw parameters, applied only in `DisplayMode::kCinema`.
    struct CinemaView {
        float skeleton_alpha = 1.0f; // 0..1
        float reveal = 0.0f;         // elements with index < reveal * count are drawn
        float shrink = 0.0f;         // 0 = touching, 1 = collapsed to centroids
        float mesh_alpha = 1.0f;
        bool edges = true;
        /// Per-element edge opacity, multiplied into `mesh_alpha` for the edge
        /// pass only, and the GL line width that pass draws at.
        ///
        /// Both exist because element COUNT decides whether cell edges are
        /// information or noise. On the 568-element case this reveal was first
        /// tuned for, 1.5 px at full opacity drew a readable cell diagram. The
        /// film's case is sphere_box_s0 at 11,692 cells in the same pane, and
        /// the same settings there were measured (two binaries differing only in
        /// these numbers, same take, same frame indices) to put 22-50% of the
        /// part's own painted pixels into near-black cell outline, against
        /// 3.4-8.9% at 1.0 px and 0.30 opacity. At half the part being outline
        /// the fill's shading is gone and so is the reveal front, because a
        /// front made of dark lines does not read against a dark background.
        /// The defaults here stay the old values, so a caller that does not set
        /// them gets exactly the previous behaviour.
        float edge_alpha = 1.0f;
        float edge_width = 1.5f;
    };
    void set_cinema_view(const CinemaView& view);

    /// Spatial reveal of the scalar field in the results modes.
    ///
    /// This is a REVEAL, not a field modification. The colours behind the front
    /// are the pass's own values at the pass's own scale, byte for byte what
    /// the un-swept bake would have produced; ahead of the front the sample is
    /// painted an unstressed grey that no colormap entry can produce. Nothing
    /// about the field is animated -- only how much of it has been uncovered --
    /// so a frame grabbed mid-sweep can still be read straight off the legend.
    struct FieldSweep {
        bool active = false;
        Eigen::Vector3f axis{1, 0, 0}; // need not be unit
        float front = 1.0f;            // fraction of the result's own extent along axis
        float feather = 0.06f;         // width of the leading band, same fraction units
    };
    /// Inactive, a zero-length `axis`, or a result with zero extent along the
    /// axis all leave every colour exactly as the un-swept bake produced it.
    void set_field_sweep(const FieldSweep& sweep);

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
    /// `kCinema` fits the union of the skeleton and the cinema mesh, so the
    /// opening skeleton shot and the finished mesh share one framing, and uses
    /// the tight `Camera::fit_oriented` so the part fills the cinema pane.
    bool frame_content(DisplayMode mode);

    /// While set, an armed `set_result()` does NOT move the camera on its next
    /// bake. A cinema take is one continuous shot: re-framing when the result
    /// act swaps to the deformed field would read as a splice. The arming
    /// survives the lock, so the studio still frames a fresh result the first
    /// time it bakes one with the lock off.
    void set_camera_locked(bool locked) { camera_locked_ = locked; }

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
    // Cinema: pre-mesh skeleton polylines, drawn with the line program above.
    // The CPU copy stays resident because CinemaView::skeleton_alpha animates
    // and the line program has no alpha uniform to animate it with.
    std::uint32_t skeleton_vao_ = 0, skeleton_vbo_ = 0;
    int skeleton_vertex_count_ = 0;
    std::vector<float> skeleton_data_;
    float skeleton_baked_alpha_ = -1.0f;
    // Cinema: per-element faces and per-element edges, each vertex carrying its
    // element's centroid and normalised index so the reveal and the shrink are
    // pure uniform changes (see kCinemaVs).
    std::uint32_t cinema_program_ = 0, cinema_line_program_ = 0;
    std::uint32_t cinema_vao_ = 0, cinema_vbo_ = 0;
    int cinema_vertex_count_ = 0;
    std::uint32_t cinema_edge_vao_ = 0, cinema_edge_vbo_ = 0;
    int cinema_edge_vertex_count_ = 0;
    std::size_t cinema_element_count_ = 0;
    std::size_t cinema_skipped_element_count_ = 0;
    CinemaView cinema_view_;
    // CPU-side copies for overlay recolor and picking.
    std::vector<float> model_vertex_data_;
    // Results-mode CPU data, re-baked when mode/scale/range changes.
    std::vector<Eigen::Vector3d> result_rest_;
    std::vector<double> result_scalar_vm_;
    std::vector<double> result_scalar_u_;
    std::vector<double> result_scalar_eta_;
    /// The optional field from `set_result`'s `nodal_extra`, interpolated to the
    /// same surface samples. Empty means "the caller supplied none, or supplied
    /// one that did not fit the mesh"; kResultsGradient bakes zeros then.
    std::vector<double> result_scalar_extra_;
    std::vector<std::array<std::uint32_t, 4>> result_quads_;
    Eigen::VectorXd result_disp_;
    bool result_dirty_ = false;
    DisplayMode baked_mode_ = DisplayMode::kSetup;
    float baked_scale_ = -1.0f;
    float baked_max_ = -1.0f;
    /// Sweep requested by the caller, and the sweep the current VBO was baked
    /// with. A moving front changes only these, so render() has to compare them
    /// field by field to know the vertex colours are stale.
    FieldSweep field_sweep_;
    FieldSweep baked_sweep_;
    // World AABBs of the uploaded geometry, refreshed on every upload/bake and
    // consumed by frame_content().
    Bounds model_bounds_;
    Bounds mesh_bounds_;
    Bounds result_bounds_;
    Bounds skeleton_bounds_;
    Bounds cinema_bounds_;
    /// set_result() arms this; the first bake_result() of that result frames it.
    bool frame_on_bake_ = false;
    /// set_camera_locked(): holds the shot still for the duration of a take.
    bool camera_locked_ = false;
    void bake_result(DisplayMode mode, float deform_scale, float result_max);
    void ensure_framebuffer(int width, int height);
    void draw_cinema(const Eigen::Matrix4f& view, const Eigen::Matrix4f& proj,
                     const Eigen::Vector3f& eye);
    void upload_boundary_edges(const std::vector<Eigen::Vector3d>& nodes,
                               const std::vector<std::array<std::uint32_t, 4>>& quads, float r,
                               float g, float b, float a, std::uint32_t vbo,
                               int& vertex_count);
};

} // namespace polymesh::gui
