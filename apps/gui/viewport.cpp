// SPDX-License-Identifier: BSD-3-Clause
#include "viewport.hpp"

#include "fea/nodal_mesh.hpp"
#include "theme.hpp"

// OpenGL 3.3 core for offscreen FBO + shaders.
// Windows: glad (system opengl32 is 1.1 only). Elsewhere: GLEXT prototypes.
#if defined(_WIN32)
#include <glad/glad.h>
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <Eigen/Geometry>

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace polymesh::gui {
namespace {

constexpr const char* kModelVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
uniform mat4 u_view;
uniform mat4 u_proj;
out vec3 v_normal;
out vec4 v_color;
out vec3 v_pos;
void main() {
    v_normal = in_normal;
    v_color = in_color;
    v_pos = in_pos;
    gl_Position = u_proj * u_view * vec4(in_pos, 1.0);
})";

constexpr const char* kModelFs = R"(#version 330 core
in vec3 v_normal;
in vec4 v_color;
in vec3 v_pos;
uniform vec3 u_eye;
out vec4 frag;
void main() {
    // Headlight diffuse + slight rim, double-sided.
    vec3 n = normalize(v_normal);
    vec3 view_dir = normalize(u_eye - v_pos);
    float ndv = abs(dot(n, view_dir));
    float shade = 0.35 + 0.6 * ndv;
    float rim = pow(1.0 - ndv, 3.0) * 0.15;
    frag = vec4(v_color.rgb * shade + vec3(rim), 1.0);
})";

constexpr const char* kLineVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
uniform mat4 u_view;
uniform mat4 u_proj;
out vec4 v_color;
void main() {
    v_color = in_color;
    gl_Position = u_proj * u_view * vec4(in_pos, 1.0);
})";

constexpr const char* kLineFs = R"(#version 330 core
in vec4 v_color;
out vec4 frag;
void main() {
    frag = v_color;
})";

constexpr const char* kBackgroundVs = R"(#version 330 core
out vec2 v_uv;
void main() {
    // Fullscreen triangle.
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.999, 1.0);
})";

constexpr const char* kBackgroundFs = R"(#version 330 core
in vec2 v_uv;
uniform vec3 u_top;
uniform vec3 u_mid;
uniform vec3 u_bottom;
out vec4 frag;
void main() {
    float t = v_uv.y;
    vec3 color = t > 0.5 ? mix(u_mid, u_top, (t - 0.5) * 2.0)
                         : mix(u_bottom, u_mid, t * 2.0);
    frag = vec4(color, 1.0);
})";

GLuint compile(GLenum type, const char* src) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
    }
    return shader;
}

GLuint link(const char* vs, const char* fs) {
    const GLuint program = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs);
    const GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    return program;
}

/// Maps a scalar in [0,1] to a blue->cyan->green->yellow->red FEA colormap.
std::array<float, 3> fea_colormap(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float r = std::clamp(std::min(4.0f * t - 2.0f, 4.0f - 4.0f * t) + 1.0f, 0.0f, 1.0f);
    const float g = std::clamp(std::min(4.0f * t, 3.4f - 3.0f * t), 0.0f, 1.0f);
    const float b = std::clamp(2.0f - 4.0f * t, 0.0f, 1.0f);
    return {t > 0.75f ? 1.0f : r * 0.9f, g * 0.85f, b};
}

void bind_line_attr(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    constexpr GLsizei stride = 7 * sizeof(float); // pos3 color4
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
}

} // namespace

// ---- Camera ---------------------------------------------------------------

void Camera::fit(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max) {
    target_ = 0.5f * (bbox_min + bbox_max).cast<float>();
    const float radius = 0.5f * static_cast<float>((bbox_max - bbox_min).norm());
    distance_ = std::max(1e-6f, radius / std::tan(0.5f * fov_y_) * 1.15f);
}

void Camera::orbit(float dx, float dy) {
    yaw_ -= dx * 0.008f;
    pitch_ = std::clamp(pitch_ + dy * 0.008f, -1.55f, 1.55f);
}

void Camera::pan(float dx, float dy, float viewport_height) {
    const float scale = 2.0f * distance_ * std::tan(0.5f * fov_y_) / viewport_height;
    const Eigen::Matrix4f v = view();
    const Eigen::Vector3f right = v.block<1, 3>(0, 0).transpose();
    const Eigen::Vector3f up = v.block<1, 3>(1, 0).transpose();
    target_ += (-dx * right + dy * up) * scale;
}

void Camera::dolly(float scroll) { distance_ *= std::pow(0.88f, scroll); }

Eigen::Vector3f Camera::eye() const {
    const Eigen::Vector3f dir(std::cos(pitch_) * std::cos(yaw_), std::sin(pitch_),
                              std::cos(pitch_) * std::sin(yaw_));
    // Y-up orbit in view math, but the models are Z-up: swap so Z is vertical.
    return target_ + distance_ * Eigen::Vector3f(dir.x(), dir.z(), dir.y());
}

Eigen::Matrix4f Camera::view() const {
    const Eigen::Vector3f e = eye();
    const Eigen::Vector3f up(0, 0, 1);
    const Eigen::Vector3f f = (target_ - e).normalized();
    const Eigen::Vector3f s = f.cross(up).normalized();
    const Eigen::Vector3f u = s.cross(f);
    Eigen::Matrix4f m = Eigen::Matrix4f::Identity();
    m.block<1, 3>(0, 0) = s.transpose();
    m.block<1, 3>(1, 0) = u.transpose();
    m.block<1, 3>(2, 0) = -f.transpose();
    m(0, 3) = -s.dot(e);
    m(1, 3) = -u.dot(e);
    m(2, 3) = f.dot(e);
    return m;
}

Eigen::Matrix4f Camera::projection(float aspect) const {
    const float near = distance_ * 0.01f;
    const float far = distance_ * 40.0f;
    const float f = 1.0f / std::tan(0.5f * fov_y_);
    Eigen::Matrix4f m = Eigen::Matrix4f::Zero();
    m(0, 0) = f / aspect;
    m(1, 1) = f;
    m(2, 2) = (far + near) / (near - far);
    m(2, 3) = 2.0f * far * near / (near - far);
    m(3, 2) = -1.0f;
    return m;
}

void Camera::pixel_ray(float u, float v, float aspect, Eigen::Vector3f& origin,
                       Eigen::Vector3f& direction) const {
    origin = eye();
    const float ndc_x = 2.0f * u - 1.0f;
    const float ndc_y = 1.0f - 2.0f * v;
    const float tan_half = std::tan(0.5f * fov_y_);
    const Eigen::Matrix4f vm = view();
    const Eigen::Vector3f right = vm.block<1, 3>(0, 0).transpose();
    const Eigen::Vector3f up = vm.block<1, 3>(1, 0).transpose();
    const Eigen::Vector3f forward = -vm.block<1, 3>(2, 0).transpose();
    direction =
        (forward + right * (ndc_x * tan_half * aspect) + up * (ndc_y * tan_half)).normalized();
}

// ---- Viewport ---------------------------------------------------------------

Viewport::~Viewport() = default;

void Viewport::init() {
    model_program_ = link(kModelVs, kModelFs);
    background_program_ = link(kBackgroundVs, kBackgroundFs);
    line_program_ = link(kLineVs, kLineFs);
    glGenVertexArrays(1, &background_vao_);
    glGenVertexArrays(1, &model_vao_);
    glGenBuffers(1, &model_vbo_);
    glGenVertexArrays(1, &mesh_vao_);
    glGenBuffers(1, &mesh_vbo_);
    glGenVertexArrays(1, &result_vao_);
    glGenBuffers(1, &result_vbo_);
    glGenVertexArrays(1, &mesh_edge_vao_);
    glGenBuffers(1, &mesh_edge_vbo_);
    glGenVertexArrays(1, &result_edge_vao_);
    glGenBuffers(1, &result_edge_vbo_);

    const auto bind_attr = [](GLuint vao, GLuint vbo) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        constexpr GLsizei stride = 10 * sizeof(float); // pos3 normal3 color4
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(3 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(6 * sizeof(float)));
    };
    bind_attr(model_vao_, model_vbo_);
    bind_attr(mesh_vao_, mesh_vbo_);
    bind_attr(result_vao_, result_vbo_);
    bind_line_attr(mesh_edge_vao_, mesh_edge_vbo_);
    bind_line_attr(result_edge_vao_, result_edge_vbo_);
    glBindVertexArray(0);
}

void Viewport::ensure_framebuffer(int width, int height) {
    if (width == fb_width_ && height == fb_height_ && fbo_ != 0) {
        return;
    }
    fb_width_ = width;
    fb_height_ = height;
    if (fbo_ == 0) {
        glGenFramebuffers(1, &fbo_);
        glGenTextures(1, &color_texture_);
        glGenRenderbuffers(1, &depth_rbo_);
    }
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_,
                           0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              depth_rbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Viewport::upload_boundary_edges(const std::vector<Eigen::Vector3d>& nodes,
                                     const std::vector<std::array<std::uint32_t, 4>>& quads,
                                     float r, float g, float b, float a, std::uint32_t vbo,
                                     int& vertex_count) {
    // pos3 + color4 per endpoint; 4 edges × 2 verts per boundary quad.
    std::vector<float> data;
    data.reserve(quads.size() * 8 * 7);
    const auto emit = [&](std::uint32_t ni) {
        const auto& p = nodes[ni];
        data.push_back(static_cast<float>(p[0]));
        data.push_back(static_cast<float>(p[1]));
        data.push_back(static_cast<float>(p[2]));
        data.push_back(r);
        data.push_back(g);
        data.push_back(b);
        data.push_back(a);
    };
    for (const auto& q : quads) {
        // Degenerate tri-as-quad: skip zero-length edges (q[2]==q[3] for triangles).
        const std::array<std::pair<int, int>, 4> edges = {{{0, 1}, {1, 2}, {2, 3}, {3, 0}}};
        for (const auto& [ia, ib] : edges) {
            if (q[static_cast<std::size_t>(ia)] == q[static_cast<std::size_t>(ib)]) {
                continue;
            }
            emit(q[static_cast<std::size_t>(ia)]);
            emit(q[static_cast<std::size_t>(ib)]);
        }
    }
    vertex_count = static_cast<int>(data.size() / 7);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);
}

void Viewport::set_model(const Model& model) {
    // pos3 normal3 color4 per vertex, one vertex per triangle corner
    // (duplicated for flat shading and per-triangle overlay colors).
    model_vertex_data_.clear();
    model_vertex_data_.reserve(model.surface.triangles.size() * 3 * 10);
    const auto& p = palette;
    for (const auto& tri : model.surface.triangles) {
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d& b = model.surface.vertices[tri[1]];
        const Eigen::Vector3d& c = model.surface.vertices[tri[2]];
        const Eigen::Vector3d n = (b - a).cross(c - a).normalized();
        for (const auto* v : {&a, &b, &c}) {
            for (int i = 0; i < 3; ++i) {
                model_vertex_data_.push_back(static_cast<float>((*v)[i]));
            }
            for (int i = 0; i < 3; ++i) {
                model_vertex_data_.push_back(static_cast<float>(n[i]));
            }
            model_vertex_data_.insert(
                model_vertex_data_.end(),
                {p.part_default.x, p.part_default.y, p.part_default.z, 1.0f});
        }
    }
    model_vertex_count_ = static_cast<int>(model.surface.triangles.size() * 3);
    glBindBuffer(GL_ARRAY_BUFFER, model_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(model_vertex_data_.size() * sizeof(float)),
                 model_vertex_data_.data(), GL_DYNAMIC_DRAW);
}

void Viewport::update_overlays(const Model& model, const SimSetup& setup, int selected_region,
                               int hovered_region) {
    const auto& p = palette;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        const int region = model.triangle_region[t];
        ImVec4 color = p.part_default;
        if (setup.fixtures.contains(region)) {
            color = p.sim_fixture;
        } else if (setup.loads.contains(region)) {
            color = p.sim_load;
        }
        if (region == selected_region) {
            color = ImVec4(p.selection.x, p.selection.y, p.selection.z, 1.0f);
        } else if (region == hovered_region) {
            color.x = 0.6f * color.x + 0.4f * p.hover.x;
            color.y = 0.6f * color.y + 0.4f * p.hover.y;
            color.z = 0.6f * color.z + 0.4f * p.hover.z;
        }
        for (int corner = 0; corner < 3; ++corner) {
            const std::size_t base = (t * 3 + static_cast<std::size_t>(corner)) * 10 + 6;
            model_vertex_data_[base + 0] = color.x;
            model_vertex_data_[base + 1] = color.y;
            model_vertex_data_[base + 2] = color.z;
            model_vertex_data_[base + 3] = 1.0f;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, model_vbo_);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(model_vertex_data_.size() * sizeof(float)),
                    model_vertex_data_.data());
}

void Viewport::set_mesh(const VolumeMeshOutput& mesh_out) {
    // Exterior faces (from element connectivity when available): quads or
    // degenerate tri-as-quads, colored by element type with a light checkerboard.
    // O(nodes + elems + faces) — never scan all elements per face (that froze
    // the GUI for ~seconds on ~200k-element auto meshes).
    namespace fea = polymesh::fea;
    auto type_color = [](fea::ElementType t) -> std::array<float, 3> {
        switch (t) {
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
    };
    // Prefer type of any incident element for a boundary node (majority not needed
    // for uniform fills; mixed hybrids still get a stable color per face).
    std::vector<fea::ElementType> node_type(mesh_out.mesh.nodes.size(),
                                            fea::ElementType::kTet4);
    std::vector<char> node_set(mesh_out.mesh.nodes.size(), 0);
    for (const auto& el : mesh_out.mesh.elements) {
        for (auto n : el.nodes) {
            if (n < node_type.size()) {
                node_type[n] = el.type;
                node_set[n] = 1;
            }
        }
    }
    auto elem_type_for_quad = [&](const std::array<std::uint32_t, 4>& q) {
        for (auto qn : q) {
            if (qn < node_set.size() && node_set[qn]) {
                return node_type[qn];
            }
        }
        return fea::ElementType::kTet4;
    };

    std::vector<float> data;
    data.reserve(mesh_out.boundary_quads.size() * 6 * 10);
    const auto& nodes = mesh_out.mesh.nodes;
    for (std::size_t qi = 0; qi < mesh_out.boundary_quads.size(); ++qi) {
        const auto& quad = mesh_out.boundary_quads[qi];
        if (quad[0] >= nodes.size() || quad[1] >= nodes.size() || quad[2] >= nodes.size() ||
            quad[3] >= nodes.size()) {
            continue;
        }
        const Eigen::Vector3d a = nodes[quad[0]];
        const Eigen::Vector3d b = nodes[quad[1]];
        const Eigen::Vector3d c = nodes[quad[2]];
        Eigen::Vector3d n = (b - a).cross(c - a);
        const double nn = n.norm();
        if (nn > 1e-30) {
            n /= nn;
        } else {
            n = Eigen::Vector3d::UnitZ();
        }
        auto rgb = type_color(elem_type_for_quad(quad));
        // Deterministic face checker: alternate brightness so edges read clearly
        // against a uniform sea of Cartesian cells.
        const std::uint32_t face_hash =
            quad[0] * 73856093u ^ quad[1] * 19349663u ^ quad[2] * 83492791u ^
            static_cast<std::uint32_t>(qi) * 2654435761u;
        const float shade = (face_hash & 1u) ? 1.0f : 0.78f;
        rgb[0] *= shade;
        rgb[1] *= shade;
        rgb[2] *= shade;
        // Degenerate tri-as-quad (v2==v3): one triangle. Else two tris of the quad.
        const bool is_tri = (quad[2] == quad[3]);
        const int corners[] = {0, 1, 2, 0, 2, 3};
        const int n_idx = is_tri ? 3 : 6;
        for (int k = 0; k < n_idx; ++k) {
            const auto& p = nodes[quad[static_cast<std::size_t>(corners[k])]];
            for (int i = 0; i < 3; ++i) {
                data.push_back(static_cast<float>(p[i]));
            }
            for (int i = 0; i < 3; ++i) {
                data.push_back(static_cast<float>(n[i]));
            }
            data.insert(data.end(), {rgb[0], rgb[1], rgb[2], 1.0f});
        }
    }
    mesh_vertex_count_ = static_cast<int>(data.size() / 10);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);

    // High-contrast dark wireframe so individual elements stay identifiable.
    upload_boundary_edges(nodes, mesh_out.boundary_quads, 0.02f, 0.02f, 0.04f, 1.0f,
                          mesh_edge_vbo_, mesh_edge_vertex_count_);
}

void Viewport::set_result(const SolveResult& result) {
    // Store rest positions + scalars; bake when mode/scale/range changes.
    result_rest_.clear();
    result_scalar_vm_.clear();
    result_scalar_u_.clear();
    result_scalar_eta_.clear();
    result_quads_ = result.boundary_quads;
    result_disp_ = result.displacement;
    const auto& nodes = result.volume_mesh.nodes;
    result_rest_.reserve(nodes.size());
    for (const auto& n : nodes) {
        result_rest_.push_back(n);
    }
    result_scalar_vm_ = result.von_mises;
    result_scalar_u_ = result.u_magnitude;
    result_scalar_eta_ = result.nodal_eta;
    if (result_scalar_eta_.size() != nodes.size()) {
        result_scalar_eta_.assign(nodes.size(), 0.0);
    }
    // Also upload undeformed mesh boundary for mesh-preview after solve.
    VolumeMeshOutput preview;
    preview.mesh = result.volume_mesh;
    preview.boundary_quads = result.boundary_quads;
    preview.mesher_note = result.mesh_note;
    set_mesh(preview);
    result_dirty_ = true;
}

void Viewport::bake_result(DisplayMode mode, float deform_scale, float result_max) {
    std::vector<float> data;
    data.reserve(result_quads_.size() * 6 * 10);
    const std::vector<double>* scalars = &result_scalar_u_;
    if (mode == DisplayMode::kResultsVonMises) {
        scalars = &result_scalar_vm_;
    } else if (mode == DisplayMode::kResultsError) {
        scalars = &result_scalar_eta_;
    }
    const float denom = result_max > 0.0f ? result_max : 1.0f;
    const Eigen::Index n_disp = result_disp_.size();
    const auto disp_at = [&](std::uint32_t node) -> Eigen::Vector3d {
        const Eigen::Index base = 3 * static_cast<Eigen::Index>(node);
        if (base + 2 >= n_disp) {
            return Eigen::Vector3d::Zero();
        }
        return result_disp_.segment<3>(base);
    };
    const auto emit = [&](std::uint32_t node, const Eigen::Vector3d& normal) {
        // Apply exaggerated displacement so the deformed shape is visible.
        const Eigen::Vector3d pos =
            result_rest_[node] + static_cast<double>(deform_scale) * disp_at(node);
        for (int i = 0; i < 3; ++i) {
            data.push_back(static_cast<float>(pos[i]));
        }
        for (int i = 0; i < 3; ++i) {
            data.push_back(static_cast<float>(normal[i]));
        }
        const double s = node < scalars->size() ? (*scalars)[node] : 0.0;
        const auto rgb = fea_colormap(static_cast<float>(s) / denom);
        data.insert(data.end(), {rgb[0], rgb[1], rgb[2], 1.0f});
    };
    for (const auto& quad : result_quads_) {
        // Normals from deformed positions so lighting follows the warped surface.
        const Eigen::Vector3d a =
            result_rest_[quad[0]] + static_cast<double>(deform_scale) * disp_at(quad[0]);
        const Eigen::Vector3d b =
            result_rest_[quad[1]] + static_cast<double>(deform_scale) * disp_at(quad[1]);
        const Eigen::Vector3d c =
            result_rest_[quad[2]] + static_cast<double>(deform_scale) * disp_at(quad[2]);
        Eigen::Vector3d n = (b - a).cross(c - a);
        const double nn = n.norm();
        if (nn > 1e-30) {
            n /= nn;
        } else {
            n = Eigen::Vector3d::UnitZ();
        }
        const bool is_tri = (quad[2] == quad[3]);
        const int corners[] = {0, 1, 2, 0, 2, 3};
        const int n_idx = is_tri ? 3 : 6;
        for (int k = 0; k < n_idx; ++k) {
            emit(quad[static_cast<std::size_t>(corners[k])], n);
        }
    }
    result_vertex_count_ = static_cast<int>(data.size() / 10);
    glBindBuffer(GL_ARRAY_BUFFER, result_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);

    // Deformed wireframe edges (same scale as shaded surface).
    std::vector<Eigen::Vector3d> deformed(result_rest_.size());
    for (std::size_t i = 0; i < result_rest_.size(); ++i) {
        deformed[i] =
            result_rest_[i] + static_cast<double>(deform_scale) * disp_at(static_cast<std::uint32_t>(i));
    }
    upload_boundary_edges(deformed, result_quads_, 0.02f, 0.02f, 0.04f, 0.95f,
                          result_edge_vbo_, result_edge_vertex_count_);

    baked_mode_ = mode;
    baked_scale_ = deform_scale;
    baked_max_ = result_max;
    result_dirty_ = false;
}

void Viewport::render(int width, int height, DisplayMode mode, float deform_scale,
                      float result_max, bool show_wireframe, bool show_undeformed) {
    if (width <= 0 || height <= 0) {
        return;
    }
    ensure_framebuffer(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Background gradient.
    glDisable(GL_DEPTH_TEST);
    glUseProgram(background_program_);
    const auto& p = palette;
    glUniform3f(glGetUniformLocation(background_program_, "u_top"), p.viewport_top.x,
                p.viewport_top.y, p.viewport_top.z);
    glUniform3f(glGetUniformLocation(background_program_, "u_mid"), p.viewport_mid.x,
                p.viewport_mid.y, p.viewport_mid.z);
    glUniform3f(glGetUniformLocation(background_program_, "u_bottom"), p.viewport_bottom.x,
                p.viewport_bottom.y, p.viewport_bottom.z);
    glBindVertexArray(background_vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_DEPTH_TEST);
    glUseProgram(model_program_);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Eigen::Matrix4f view = camera.view();
    const Eigen::Matrix4f proj = camera.projection(aspect);
    const Eigen::Vector3f eye = camera.eye();
    glUniformMatrix4fv(glGetUniformLocation(model_program_, "u_view"), 1, GL_FALSE,
                       view.data());
    glUniformMatrix4fv(glGetUniformLocation(model_program_, "u_proj"), 1, GL_FALSE,
                       proj.data());
    glUniform3f(glGetUniformLocation(model_program_, "u_eye"), eye.x(), eye.y(), eye.z());

    const bool results_mode = mode == DisplayMode::kResultsVonMises ||
                              mode == DisplayMode::kResultsDisplacement ||
                              mode == DisplayMode::kResultsError;

    // Push filled surfaces slightly back so edge lines win the depth test.
    const bool draw_edges = show_wireframe || (show_undeformed && results_mode);
    if (draw_edges) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.0f, 1.0f);
    }

    if (mode == DisplayMode::kSetup) {
        if (model_vertex_count_ > 0) {
            glBindVertexArray(model_vao_);
            glDrawArrays(GL_TRIANGLES, 0, model_vertex_count_);
        }
    } else if (mode == DisplayMode::kMeshPreview) {
        if (mesh_vertex_count_ > 0) {
            glBindVertexArray(mesh_vao_);
            glDrawArrays(GL_TRIANGLES, 0, mesh_vertex_count_);
        }
    } else {
        if (result_dirty_ || baked_mode_ != mode || baked_scale_ != deform_scale ||
            baked_max_ != result_max) {
            bake_result(mode, deform_scale, result_max);
        }
        if (result_vertex_count_ > 0) {
            glBindVertexArray(result_vao_);
            glDrawArrays(GL_TRIANGLES, 0, result_vertex_count_);
        }
    }

    if (draw_edges) {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthFunc(GL_LEQUAL);
        glUseProgram(line_program_);
        glUniformMatrix4fv(glGetUniformLocation(line_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(line_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        // Core profile may clamp >1; still request 1.5 for drivers that honor it.
        glLineWidth(1.5f);

        if (show_undeformed && results_mode && mesh_edge_vertex_count_ > 0) {
            // Rest outline behind deformed mesh.
            glDepthMask(GL_FALSE);
            glBindVertexArray(mesh_edge_vao_);
            glDrawArrays(GL_LINES, 0, mesh_edge_vertex_count_);
            glDepthMask(GL_TRUE);
        }
        if (show_wireframe) {
            if (results_mode && result_edge_vertex_count_ > 0) {
                glBindVertexArray(result_edge_vao_);
                glDrawArrays(GL_LINES, 0, result_edge_vertex_count_);
            } else if (mode == DisplayMode::kMeshPreview && mesh_edge_vertex_count_ > 0) {
                glBindVertexArray(mesh_edge_vao_);
                glDrawArrays(GL_LINES, 0, mesh_edge_vertex_count_);
            }
        }
        glDepthFunc(GL_LESS);
        glLineWidth(1.0f);
        glDisable(GL_BLEND);
    }

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::optional<int> Viewport::pick_region(const Model& model, float u, float v,
                                         float aspect) const {
    Eigen::Vector3f origin_f, dir_f;
    camera.pixel_ray(u, v, aspect, origin_f, dir_f);
    const Eigen::Vector3d origin = origin_f.cast<double>();
    const Eigen::Vector3d dir = dir_f.cast<double>();

    double best_t = std::numeric_limits<double>::max();
    int best_region = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        const auto& tri = model.surface.triangles[t];
        // Möller–Trumbore.
        const Eigen::Vector3d& a = model.surface.vertices[tri[0]];
        const Eigen::Vector3d e1 = model.surface.vertices[tri[1]] - a;
        const Eigen::Vector3d e2 = model.surface.vertices[tri[2]] - a;
        const Eigen::Vector3d pvec = dir.cross(e2);
        const double det = e1.dot(pvec);
        if (std::abs(det) < 1e-14) {
            continue;
        }
        const double inv_det = 1.0 / det;
        const Eigen::Vector3d tvec = origin - a;
        const double bu = tvec.dot(pvec) * inv_det;
        if (bu < 0.0 || bu > 1.0) {
            continue;
        }
        const Eigen::Vector3d qvec = tvec.cross(e1);
        const double bv = dir.dot(qvec) * inv_det;
        if (bv < 0.0 || bu + bv > 1.0) {
            continue;
        }
        const double hit_t = e2.dot(qvec) * inv_det;
        if (hit_t > 1e-9 && hit_t < best_t) {
            best_t = hit_t;
            best_region = model.triangle_region[t];
        }
    }
    if (best_region < 0) {
        return std::nullopt;
    }
    return best_region;
}

} // namespace polymesh::gui
