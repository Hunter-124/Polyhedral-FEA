// SPDX-License-Identifier: BSD-3-Clause
#include "viewport.hpp"

#include "colormap.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/traction.hpp"
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

#include <algorithm>
#include <array>
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

// Cinema pass. Separate programs from the model/line ones above because every
// vertex carries two extra attributes -- its element's centroid and its
// element's normalised index -- which is what lets the reveal and the shrink be
// uniform writes instead of per-frame geometry rebuilds.
constexpr const char* kCinemaVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec3 in_centroid;
layout(location = 4) in float in_index;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_shrink;
out vec3 v_normal;
out vec4 v_color;
out vec3 v_pos;
flat out float v_index;
void main() {
    // Toward its OWN element's centroid, so the cells separate from each other
    // instead of the whole mesh scaling about one point.
    vec3 pos = mix(in_pos, in_centroid, u_shrink);
    v_normal = in_normal;
    v_color = in_color;
    v_pos = pos;
    // flat: the index is per-element, and interpolating it would let a
    // triangle straddle the reveal threshold and get cut through the middle.
    v_index = in_index;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
})";

constexpr const char* kCinemaFs = R"(#version 330 core
in vec3 v_normal;
in vec4 v_color;
in vec3 v_pos;
flat in float v_index;
uniform vec3 u_eye;
uniform float u_reveal;
uniform float u_alpha;
out vec4 frag;
void main() {
    // v_index is element_index / element_count, so this draws exactly the
    // elements with index < u_reveal * count -- whole cells, never a part of one.
    if (v_index >= u_reveal) {
        discard;
    }
    // Same headlight/rim shading as kModelFs so a revealed cell looks like the
    // same material the mesh-preview mode shows.
    vec3 n = normalize(v_normal);
    vec3 view_dir = normalize(u_eye - v_pos);
    float ndv = abs(dot(n, view_dir));
    float shade = 0.35 + 0.6 * ndv;
    float rim = pow(1.0 - ndv, 3.0) * 0.15;
    frag = vec4(v_color.rgb * shade + vec3(rim), u_alpha * v_color.a);
})";

constexpr const char* kCinemaLineVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_centroid;
layout(location = 3) in float in_index;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_shrink;
out vec4 v_color;
flat out float v_index;
void main() {
    vec3 pos = mix(in_pos, in_centroid, u_shrink);
    v_color = in_color;
    v_index = in_index;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
})";

constexpr const char* kCinemaLineFs = R"(#version 330 core
in vec4 v_color;
flat in float v_index;
uniform float u_reveal;
uniform float u_alpha;
out vec4 frag;
void main() {
    if (v_index >= u_reveal) {
        discard;
    }
    frag = vec4(v_color.rgb, u_alpha * v_color.a);
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

// Interleaved float counts per vertex for the two cinema buffers. Named here so
// the attribute binding and the packing loops cannot drift apart.
constexpr int kCinemaFillFloats = 14; // pos3 normal3 rgba4 centroid3 index1
constexpr int kCinemaEdgeFloats = 11; // pos3 rgba4 centroid3 index1

void bind_cinema_attr(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    constexpr GLsizei stride = kCinemaFillFloats * sizeof(float);
    const auto attr = [](GLuint index, GLint size, int offset_floats) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(
                                  static_cast<std::uintptr_t>(offset_floats) * sizeof(float)));
    };
    attr(0, 3, 0);  // position
    attr(1, 3, 3);  // normal
    attr(2, 4, 6);  // color
    attr(3, 3, 10); // element centroid
    attr(4, 1, 13); // element index / element count
}

void bind_cinema_line_attr(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    constexpr GLsizei stride = kCinemaEdgeFloats * sizeof(float);
    const auto attr = [](GLuint index, GLint size, int offset_floats) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(
                                  static_cast<std::uintptr_t>(offset_floats) * sizeof(float)));
    };
    attr(0, 3, 0);
    attr(1, 4, 3);
    attr(2, 3, 7);
    attr(3, 1, 10);
}

/// Element-type colors for mesh preview and for the cinema reveal. One function
/// so a hex is the same green in both; the cinema would otherwise re-invent the
/// legend the mesh-preview mode already taught the user.
std::array<float, 3> type_color(fea::ElementType t) {
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
}

/// One element's face loops in global node ids, appended flat: loop k spans
/// `nodes[starts[k] .. starts[k + 1])`. Flat and reused across elements because
/// set_cinema_mesh walks every element of a mesh that can be hundreds of
/// thousands of cells, where a vector-of-vectors would allocate per face.
struct FaceLoops {
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> starts{0};

    void clear() {
        nodes.clear();
        starts.assign(1, 0);
    }
    std::size_t count() const { return starts.size() - 1; }
    void push(std::initializer_list<std::uint32_t> loop) {
        nodes.insert(nodes.end(), loop);
        starts.push_back(static_cast<std::uint32_t>(nodes.size()));
    }
};

/// Corner nodes of an element: the ones that define its shape. Higher-order
/// elements keep their mid-edge nodes out of this, so a tet10's centroid is the
/// tet's centroid and not the centroid of its mid-edge cloud.
std::size_t corner_node_count(const fea::NodalElement& el) {
    switch (el.type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        return 4;
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        return 8;
    case fea::ElementType::kPrism6:
        return 6;
    case fea::ElementType::kPyramid5:
        return 5;
    case fea::ElementType::kPolyVem:
        return el.nodes.size();
    }
    return el.nodes.size();
}

/// Every face of ONE element, outward-oriented, in the same node-order
/// convention as fea::extract_boundary_polys' corner topology (tet10/hex20 come
/// back as their corner faces; poly-VEM cells use their own `faces` loops).
/// False means the element carries too little connectivity to have faces, or
/// references a node outside the mesh: the caller must count it, not ignore it.
bool element_face_loops(const fea::NodalElement& el, std::size_t node_count, FaceLoops& out) {
    out.clear();
    const auto& n = el.nodes;
    const std::size_t corners = corner_node_count(el);
    if (n.size() < corners || corners < 4) {
        return false;
    }
    for (std::size_t i = 0; i < corners; ++i) {
        if (n[i] >= node_count) {
            return false;
        }
    }
    switch (el.type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        out.push({n[0], n[2], n[1]});
        out.push({n[0], n[1], n[3]});
        out.push({n[0], n[3], n[2]});
        out.push({n[1], n[2], n[3]});
        break;
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        out.push({n[0], n[3], n[2], n[1]});
        out.push({n[4], n[5], n[6], n[7]});
        out.push({n[0], n[1], n[5], n[4]});
        out.push({n[1], n[2], n[6], n[5]});
        out.push({n[2], n[3], n[7], n[6]});
        out.push({n[3], n[0], n[4], n[7]});
        break;
    case fea::ElementType::kPrism6:
        out.push({n[0], n[2], n[1]});
        out.push({n[3], n[4], n[5]});
        out.push({n[0], n[1], n[4], n[3]});
        out.push({n[1], n[2], n[5], n[4]});
        out.push({n[2], n[0], n[3], n[5]});
        break;
    case fea::ElementType::kPyramid5:
        out.push({n[0], n[1], n[2], n[3]});
        out.push({n[0], n[1], n[4]});
        out.push({n[1], n[2], n[4]});
        out.push({n[2], n[3], n[4]});
        out.push({n[3], n[0], n[4]});
        break;
    case fea::ElementType::kPolyVem:
        for (const auto& face : el.faces) {
            if (face.size() < 3) {
                continue;
            }
            const std::size_t begin = out.nodes.size();
            bool valid = true;
            for (const std::uint32_t local : face) {
                if (local >= n.size() || n[local] >= node_count) {
                    valid = false;
                    break;
                }
                out.nodes.push_back(n[local]);
            }
            if (valid) {
                out.starts.push_back(static_cast<std::uint32_t>(out.nodes.size()));
            } else {
                out.nodes.resize(begin);
            }
        }
        break;
    }
    return out.count() > 0;
}

} // namespace

// ---- Camera ---------------------------------------------------------------

void Camera::fit(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max) {
    target_ = (0.5 * (bbox_min + bbox_max)).cast<float>();
    const float diagonal = static_cast<float>((bbox_max - bbox_min).norm());
    // 1.9x the diagonal clears the bounding sphere at the 40 deg vertical FOV
    // with margin to spare, including the aspect < 1 (tall, narrow) viewport
    // where the horizontal field is the tighter of the two. Degenerate box
    // (single point / empty content) → a neutral 1 m standoff.
    distance_ = diagonal > 1e-9f ? 1.9f * diagonal : 1.0f;
}

void Camera::fit_oriented(const Eigen::Vector3d& bbox_min, const Eigen::Vector3d& bbox_max,
                          float fill) {
    target_ = (0.5 * (bbox_min + bbox_max)).cast<float>();
    const Eigen::Vector3f half = (0.5 * (bbox_max - bbox_min)).cast<float>().cwiseAbs();
    // Orbit basis, built exactly as eye()/view() build it. It depends only on
    // yaw/pitch, so it is available before the distance this function solves for.
    const Eigen::Vector3f dir(std::cos(pitch_) * std::cos(yaw_), std::sin(pitch_),
                              std::cos(pitch_) * std::sin(yaw_));
    const Eigen::Vector3f f = -Eigen::Vector3f(dir.x(), dir.z(), dir.y()); // eye -> target
    const Eigen::Vector3f s = f.cross(Eigen::Vector3f(0, 0, 1)).normalized();
    const Eigen::Vector3f u = s.cross(f);
    // Support function of the AABB along a camera axis: the box's half-extent
    // projected onto that axis, which for an axis-aligned box is the |dot| sum.
    const auto extent = [&half](const Eigen::Vector3f& axis) {
        return std::fabs(axis.x()) * half.x() + std::fabs(axis.y()) * half.y() +
               std::fabs(axis.z()) * half.z();
    };
    const float lateral = std::max(extent(s), extent(u));
    if (!(lateral > 1e-9f)) {
        distance_ = 1.0f; // degenerate box (single point) -> neutral standoff
        return;
    }
    // Half-angle budget. `extent(f)` is added because the near face of the box
    // is that much closer than its centre and therefore projects that much
    // larger; without it a deep box clips at its leading corner.
    const float k = std::clamp(fill, 0.05f, 1.0f) * std::tan(0.5f * fov_y_);
    distance_ = lateral / k + extent(f);
}

void Camera::set_orbit(float yaw, float pitch) {
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -1.55f, 1.55f);
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
    cinema_program_ = link(kCinemaVs, kCinemaFs);
    cinema_line_program_ = link(kCinemaLineVs, kCinemaLineFs);
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
    glGenVertexArrays(1, &skeleton_vao_);
    glGenBuffers(1, &skeleton_vbo_);
    glGenVertexArrays(1, &cinema_vao_);
    glGenBuffers(1, &cinema_vbo_);
    glGenVertexArrays(1, &cinema_edge_vao_);
    glGenBuffers(1, &cinema_edge_vbo_);

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
    bind_line_attr(skeleton_vao_, skeleton_vbo_);
    bind_cinema_attr(cinema_vao_, cinema_vbo_);
    bind_cinema_line_attr(cinema_edge_vao_, cinema_edge_vbo_);
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
    // pos3 normal3 color4 per vertex, one vertex per triangle corner. Normals are
    // crease-aware smoothed: incident faces within kCreaseCos of a corner's own
    // face are averaged (area-weighted), so curved walls (tubes/fillets) shade
    // smoothly while sharp CAD edges (caps, mitres) stay crisp.
    model_vertex_data_.clear();
    model_vertex_data_.reserve(model.surface.triangles.size() * 3 * 10);
    model_bounds_.reset();
    const auto& p = palette;
    const auto& verts = model.surface.vertices;
    const auto& tris = model.surface.triangles;
    constexpr double kCreaseCos = 0.707; // 45°: smooth within, hard beyond

    // Per-triangle area-weighted normal (unnormalized) + its unit direction.
    std::vector<Eigen::Vector3d> face_area_n(tris.size());
    std::vector<Eigen::Vector3d> face_unit_n(tris.size());
    for (std::size_t t = 0; t < tris.size(); ++t) {
        const auto& tri = tris[t];
        const Eigen::Vector3d an =
            (verts[tri[1]] - verts[tri[0]]).cross(verts[tri[2]] - verts[tri[0]]);
        face_area_n[t] = an;
        const double nn = an.norm();
        face_unit_n[t] = nn > 1e-30 ? (an / nn).eval() : Eigen::Vector3d(0, 0, 1);
    }
    // Faces incident to each welded vertex.
    std::vector<std::vector<std::uint32_t>> incident(verts.size());
    for (std::size_t t = 0; t < tris.size(); ++t) {
        for (int k = 0; k < 3; ++k) {
            incident[tris[t][k]].push_back(static_cast<std::uint32_t>(t));
        }
    }
    const auto corner_normal = [&](std::size_t t, std::uint32_t v) {
        Eigen::Vector3d acc = face_area_n[t];
        for (const std::uint32_t f : incident[v]) {
            if (f != t && face_unit_n[f].dot(face_unit_n[t]) >= kCreaseCos) {
                acc += face_area_n[f];
            }
        }
        const double nn = acc.norm();
        return nn > 1e-30 ? (acc / nn).eval() : face_unit_n[t];
    };

    for (std::size_t t = 0; t < tris.size(); ++t) {
        const auto& tri = tris[t];
        for (int k = 0; k < 3; ++k) {
            const Eigen::Vector3d& v = verts[tri[k]];
            const Eigen::Vector3d n = corner_normal(t, tri[k]);
            model_bounds_.add(v);
            for (int i = 0; i < 3; ++i) {
                model_vertex_data_.push_back(static_cast<float>(v[i]));
            }
            for (int i = 0; i < 3; ++i) {
                model_vertex_data_.push_back(static_cast<float>(n[i]));
            }
            model_vertex_data_.insert(
                model_vertex_data_.end(),
                {p.part_default.x, p.part_default.y, p.part_default.z, 1.0f});
        }
    }
    model_vertex_count_ = static_cast<int>(tris.size() * 3);
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

void Viewport::invalidate_colors() {
    // Results-mode vertex colors are baked into GL buffers; force a rebake on
    // the next render by clearing the bake signature (a plain dirty flag would
    // be dropped by the mode/scale/max comparison when nothing else changed).
    result_dirty_ = true;
    baked_scale_ = -1.0f;
    baked_max_ = -1.0f;
}

void Viewport::set_mesh(const VolumeMeshOutput& mesh_out) {
    // Render exterior faces as TRUE polygons (poly-aware): each cell facet is
    // one flat-shaded polygon (centroid fan for the fill, per-facet normal) and
    // the wireframe traces only real polygon edges — never fan-triangulation
    // diagonals. This is what makes polyhedral cells read as clean facets
    // instead of a triangle soup. FEM faces come back as their quad/tri loops.
    namespace fea = polymesh::fea;
    std::vector<fea::ElementType> node_type(mesh_out.mesh.nodes.size(),
                                            fea::ElementType::kTet4);
    std::vector<char> node_set(mesh_out.mesh.nodes.size(), 0);
    for (const auto& el : mesh_out.mesh.elements) {
        for (auto nidx : el.nodes) {
            if (nidx < node_type.size()) {
                node_type[nidx] = el.type;
                node_set[nidx] = 1;
            }
        }
    }

    const auto surface = fea::tessellate_boundary_surface(mesh_out.mesh, 8);
    std::vector<float> data; // fill: pos3 + normal3 + rgba4
    data.reserve(surface.triangles.size() * 3 * 10);
    std::vector<float> edata; // edges: pos3 + rgba4
    edata.reserve(surface.triangles.size() * 6 * 7);
    const float er = 0.02f, eg = 0.02f, eb = 0.04f, ea = 1.0f;
    mesh_bounds_.reset();

    for (std::size_t ti = 0; ti < surface.triangles.size(); ++ti) {
        const auto& tri = surface.triangles[ti];
        const Eigen::Vector3d& a = surface.samples[tri[0]].position;
        const Eigen::Vector3d& b = surface.samples[tri[1]].position;
        const Eigen::Vector3d& c = surface.samples[tri[2]].position;
        mesh_bounds_.add(a);
        mesh_bounds_.add(b);
        mesh_bounds_.add(c);
        Eigen::Vector3d nrm = (b - a).cross(c - a);
        const double nn = nrm.norm();
        nrm = nn > 1e-30 ? Eigen::Vector3d(nrm / nn) : Eigen::Vector3d::UnitZ();

        fea::ElementType type = fea::ElementType::kTet4;
        const auto source = surface.samples[tri[0]].source_nodes[0];
        if (source < node_set.size() && node_set[source]) {
            type = node_type[source];
        }
        auto rgb = type_color(type);
        const std::uint32_t hash =
            static_cast<std::uint32_t>(ti) * 2654435761u ^ source * 73856093u;
        const float shade = (hash & 1u) ? 1.0f : 0.82f;
        rgb[0] *= shade;
        rgb[1] *= shade;
        rgb[2] *= shade;
        const auto emit_fill = [&](const Eigen::Vector3d& p) {
            data.insert(data.end(), {static_cast<float>(p.x()), static_cast<float>(p.y()),
                                     static_cast<float>(p.z()), static_cast<float>(nrm.x()),
                                     static_cast<float>(nrm.y()), static_cast<float>(nrm.z()),
                                     rgb[0], rgb[1], rgb[2], 1.0f});
        };
        const auto emit_edge = [&](const Eigen::Vector3d& p) {
            edata.insert(edata.end(), {static_cast<float>(p.x()), static_cast<float>(p.y()),
                                       static_cast<float>(p.z()), er, eg, eb, ea});
        };
        emit_fill(a);
        emit_fill(b);
        emit_fill(c);
        for (const auto& edge : {std::pair{&a, &b}, std::pair{&b, &c}, std::pair{&c, &a}}) {
            emit_edge(*edge.first);
            emit_edge(*edge.second);
        }
    }
    mesh_vertex_count_ = static_cast<int>(data.size() / 10);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);

    mesh_edge_vertex_count_ = static_cast<int>(edata.size() / 7);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_edge_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(edata.size() * sizeof(float)),
                 edata.data(), GL_DYNAMIC_DRAW);
}

void Viewport::set_skeleton(const std::vector<std::vector<Eigen::Vector3d>>& polylines) {
    // The cinema opens on the part before any mesh exists, so this buffer is
    // drawn whether or not set_cinema_mesh() has ever been called. Polylines
    // come in as they are: no resampling, no smoothing — a BRep edge that is a
    // straight chord in the model is a straight chord here.
    skeleton_data_.clear();
    skeleton_bounds_.reset();
    std::size_t segments = 0;
    for (const auto& line : polylines) {
        if (line.size() >= 2) {
            segments += line.size() - 1;
        }
    }
    skeleton_data_.reserve(segments * 2 * 7); // pos3 rgba4 per endpoint
    // The brighter end of the accent family, not the accent itself: the
    // opening act is nothing but these lines on a near-black gradient, and the
    // mid accent reads as a faint scribble there. Still a palette token, so a
    // theme swap moves the skeleton with the rest of the chrome.
    const auto& c = palette.accent_soft_top;
    const float alpha = std::clamp(cinema_view_.skeleton_alpha, 0.0f, 1.0f);
    const auto emit = [&](const Eigen::Vector3d& p) {
        skeleton_bounds_.add(p);
        skeleton_data_.insert(skeleton_data_.end(),
                              {static_cast<float>(p.x()), static_cast<float>(p.y()),
                               static_cast<float>(p.z()), c.x, c.y, c.z, alpha});
    };
    for (const auto& line : polylines) {
        for (std::size_t i = 1; i < line.size(); ++i) {
            emit(line[i - 1]);
            emit(line[i]);
        }
    }
    skeleton_vertex_count_ = static_cast<int>(skeleton_data_.size() / 7);
    skeleton_baked_alpha_ = alpha;
    glBindBuffer(GL_ARRAY_BUFFER, skeleton_vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(skeleton_data_.size() * sizeof(float)),
                 skeleton_data_.data(), GL_DYNAMIC_DRAW);
}

void Viewport::set_cinema_mesh(const fea::NodalMesh& mesh) {
    // EVERY element's own faces, not the shared boundary surface: a solid that
    // grows one cell at a time has to show its real cut faces, and the shrink
    // only reads as discrete cells if the interior facets exist to be seen.
    // The header carries the measured memory this costs.
    cinema_bounds_.reset();
    cinema_element_count_ = mesh.elements.size();
    cinema_skipped_element_count_ = 0;

    std::vector<float> data;  // fill:  pos3 normal3 rgba4 centroid3 index1
    std::vector<float> edata; // edges: pos3 rgba4 centroid3 index1
    // A tet — the common case — gives 4 triangles (12 fill vertices) and 6
    // distinct edges (12 line vertices). Reserve for that; hexes and poly cells
    // grow the buffers from there.
    data.reserve(mesh.elements.size() * 12 * kCinemaFillFloats);
    edata.reserve(mesh.elements.size() * 12 * kCinemaEdgeFloats);

    // Normalising by the element COUNT (not count - 1) is what makes the shader
    // test `index / count >= reveal` equivalent to `index >= reveal * count`,
    // so reveal = 0 draws nothing and reveal = 1 draws every element.
    const double denom =
        cinema_element_count_ > 0 ? static_cast<double>(cinema_element_count_) : 1.0;
    FaceLoops loops;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (std::size_t ei = 0; ei < mesh.elements.size(); ++ei) {
        const auto& el = mesh.elements[ei];
        if (!element_face_loops(el, mesh.nodes.size(), loops)) {
            // Degenerate connectivity or a poly-VEM cell with no usable face
            // loops. Counted so the panel can say the reveal is not the whole
            // mesh; the index normalisation still uses the full element count,
            // so the remaining cells keep their true position in the order.
            ++cinema_skipped_element_count_;
            continue;
        }
        const std::size_t corners = corner_node_count(el);
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (std::size_t i = 0; i < corners; ++i) {
            centroid += mesh.nodes[el.nodes[i]];
        }
        // Node average, not the volume centroid: the shrink only needs a point
        // inside the cell to collapse toward. Exact for a simplex, and inside
        // the hull for anything convex, which every element type here is.
        centroid /= static_cast<double>(corners);
        const float index_t = static_cast<float>(static_cast<double>(ei) / denom);
        const auto rgb = type_color(el.type);
        // Alternate the shade on the emission index, which is also the reveal
        // order: the cell that just appeared reads as distinct from the one
        // before it instead of merging into one flat blob of element colour.
        const float shade = (ei & 1u) ? 1.0f : 0.82f;

        for (std::size_t li = 0; li < loops.count(); ++li) {
            const std::uint32_t begin = loops.starts[li];
            const std::size_t n_face = loops.starts[li + 1] - begin;
            if (n_face < 3) {
                continue;
            }
            // Newell's normal: the area-weighted average over the whole loop, so
            // it is right for quads and for the non-planar poly-VEM facets that
            // a three-point cross product would get wrong.
            Eigen::Vector3d nrm = Eigen::Vector3d::Zero();
            for (std::size_t k = 0; k < n_face; ++k) {
                const Eigen::Vector3d& p = mesh.nodes[loops.nodes[begin + k]];
                const Eigen::Vector3d& q = mesh.nodes[loops.nodes[begin + (k + 1) % n_face]];
                nrm += p.cross(q);
            }
            const double nn = nrm.norm();
            nrm = nn > 1e-30 ? Eigen::Vector3d(nrm / nn) : Eigen::Vector3d::UnitZ();
            const auto emit_fill = [&](std::uint32_t node) {
                const Eigen::Vector3d& p = mesh.nodes[node];
                cinema_bounds_.add(p);
                data.insert(data.end(),
                            {static_cast<float>(p.x()), static_cast<float>(p.y()),
                             static_cast<float>(p.z()), static_cast<float>(nrm.x()),
                             static_cast<float>(nrm.y()), static_cast<float>(nrm.z()),
                             rgb[0] * shade, rgb[1] * shade, rgb[2] * shade, 1.0f,
                             static_cast<float>(centroid.x()),
                             static_cast<float>(centroid.y()),
                             static_cast<float>(centroid.z()), index_t});
            };
            // Fan from the first corner. Every element type here has convex
            // faces, so a fan covers them without self-overlap.
            for (std::size_t k = 1; k + 1 < n_face; ++k) {
                emit_fill(loops.nodes[begin]);
                emit_fill(loops.nodes[begin + k]);
                emit_fill(loops.nodes[begin + k + 1]);
            }
        }

        // Distinct edges of THIS element. Deduplicated within the element only,
        // by linear search: a cell has at most a few dozen edge instances (12
        // for a hex, 6 for a tet), so the quadratic scan is cheaper than the
        // hash set it would replace, and neighbouring elements deliberately
        // keep their own copy of a shared edge.
        edges.clear();
        for (std::size_t li = 0; li < loops.count(); ++li) {
            const std::uint32_t begin = loops.starts[li];
            const std::size_t n_face = loops.starts[li + 1] - begin;
            for (std::size_t k = 0; k < n_face; ++k) {
                const std::uint32_t a = loops.nodes[begin + k];
                const std::uint32_t b = loops.nodes[begin + (k + 1) % n_face];
                if (a == b) {
                    continue;
                }
                const std::pair<std::uint32_t, std::uint32_t> key =
                    a < b ? std::pair{a, b} : std::pair{b, a};
                if (std::find(edges.begin(), edges.end(), key) == edges.end()) {
                    edges.push_back(key);
                }
            }
        }
        const auto emit_edge = [&](std::uint32_t node) {
            const Eigen::Vector3d& p = mesh.nodes[node];
            edata.insert(edata.end(),
                         {static_cast<float>(p.x()), static_cast<float>(p.y()),
                          static_cast<float>(p.z()), 0.02f, 0.02f, 0.04f, 1.0f,
                          static_cast<float>(centroid.x()), static_cast<float>(centroid.y()),
                          static_cast<float>(centroid.z()), index_t});
        };
        for (const auto& [a, b] : edges) {
            emit_edge(a);
            emit_edge(b);
        }
    }

    cinema_vertex_count_ = static_cast<int>(data.size() / kCinemaFillFloats);
    glBindBuffer(GL_ARRAY_BUFFER, cinema_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);
    cinema_edge_vertex_count_ = static_cast<int>(edata.size() / kCinemaEdgeFloats);
    glBindBuffer(GL_ARRAY_BUFFER, cinema_edge_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(edata.size() * sizeof(float)),
                 edata.data(), GL_DYNAMIC_DRAW);
}

/// Total elements in the uploaded cinema mesh, including any this viewport
/// could not triangulate: it is the number `CinemaView::reveal` scales, so it
/// has to be the mesher's own element count and nothing else.
std::size_t Viewport::cinema_element_count() const { return cinema_element_count_; }

std::size_t Viewport::cinema_skipped_element_count() const {
    return cinema_skipped_element_count_;
}

void Viewport::set_cinema_view(const CinemaView& view) { cinema_view_ = view; }

void Viewport::set_result(const SolveResult& result) {
    // Store rest positions + scalars; bake when mode/scale/range changes.
    result_rest_.clear();
    result_scalar_vm_.clear();
    result_scalar_u_.clear();
    result_scalar_eta_.clear();
    result_quads_.clear();
    const auto surface = polymesh::fea::tessellate_boundary_surface(result.volume_mesh, 8);
    result_rest_.reserve(surface.samples.size());
    result_scalar_vm_.reserve(surface.samples.size());
    result_scalar_u_.reserve(surface.samples.size());
    result_scalar_eta_.reserve(surface.samples.size());
    result_disp_ =
        Eigen::VectorXd::Zero(3 * static_cast<Eigen::Index>(surface.samples.size()));
    const auto interpolate_scalar = [](const polymesh::fea::SurfaceSample& sample,
                                       const std::vector<double>& values) {
        double value = 0.0;
        for (std::size_t i = 0; i < sample.count; ++i) {
            const auto node = sample.source_nodes[i];
            if (node < values.size()) {
                value += sample.weights[i] * values[node];
            }
        }
        return value;
    };
    for (std::size_t si = 0; si < surface.samples.size(); ++si) {
        const auto& sample = surface.samples[si];
        result_rest_.push_back(sample.position);
        result_scalar_vm_.push_back(interpolate_scalar(sample, result.von_mises));
        result_scalar_u_.push_back(interpolate_scalar(sample, result.u_magnitude));
        result_scalar_eta_.push_back(interpolate_scalar(sample, result.nodal_eta));
        for (std::size_t i = 0; i < sample.count; ++i) {
            const Eigen::Index base = 3 * static_cast<Eigen::Index>(sample.source_nodes[i]);
            if (base + 2 < result.displacement.size()) {
                result_disp_.segment<3>(3 * static_cast<Eigen::Index>(si)) +=
                    sample.weights[i] * result.displacement.segment<3>(base);
            }
        }
    }
    result_quads_.reserve(surface.triangles.size());
    for (const auto& tri : surface.triangles) {
        result_quads_.push_back({tri[0], tri[1], tri[2], tri[2]});
    }
    // Also upload undeformed mesh boundary for mesh-preview after solve.
    VolumeMeshOutput preview;
    preview.mesh = result.volume_mesh;
    preview.boundary_quads = result.boundary_quads;
    preview.mesher_note = result.mesh_note;
    set_mesh(preview);
    result_dirty_ = true;
    result_bounds_.reset();
    // The deformed shape only exists after the first bake (it needs the caller's
    // exaggeration scale), so framing has to wait for it.
    frame_on_bake_ = true;
}

bool Viewport::frame_content(DisplayMode mode) {
    // The cinema spans two uploads whose union has to be framed ONCE: the
    // opening shot has only the skeleton and the closing one only the mesh, and
    // a camera that moved between them would read as a cut, not a reveal.
    Bounds cinema_union;
    const Bounds* bounds = nullptr;
    switch (mode) {
    case DisplayMode::kSetup:
        bounds = &model_bounds_;
        break;
    case DisplayMode::kMeshPreview:
        bounds = &mesh_bounds_;
        break;
    case DisplayMode::kResultsVonMises:
    case DisplayMode::kResultsDisplacement:
    case DisplayMode::kResultsError:
        // Results are only bounded after the first bake; the undeformed mesh
        // uploaded alongside them is the right stand-in until then.
        bounds = result_bounds_.valid ? &result_bounds_ : &mesh_bounds_;
        break;
    case DisplayMode::kCinema:
        for (const Bounds* part : {&skeleton_bounds_, &cinema_bounds_}) {
            if (part->valid) {
                cinema_union.add(part->min);
                cinema_union.add(part->max);
            }
        }
        bounds = &cinema_union;
        break;
    }
    // A frame request must never leave the user staring at an empty gradient:
    // if the requested mode has nothing, frame whatever else is uploaded.
    if (bounds == nullptr || !bounds->valid) {
        for (const Bounds* alt : {&model_bounds_, &mesh_bounds_, &result_bounds_,
                                  &skeleton_bounds_, &cinema_bounds_}) {
            if (alt->valid) {
                bounds = alt;
                break;
            }
        }
    }
    if (bounds == nullptr || !bounds->valid) {
        return false; // nothing uploaded — keep the current camera
    }
    if (mode == DisplayMode::kCinema) {
        // The cinema pane is a fixed landscape rectangle and the orbit is not
        // touched during a take, so the shot can be fitted to what the part
        // actually projects to. `fit`'s bounding-sphere standoff leaves a wide
        // plate occupying under half the pane, which is the wrong picture of a
        // part that is meant to be the subject of the frame.
        constexpr float kCinemaFill = 0.90f;
        // The take also picks its own elevation. The studio's default 0.5 rad
        // is a shallow three-quarter view, and a flat plate seen that flat
        // projects to a third of the pane height however tightly it is fitted;
        // 0.72 rad opens the top face up without turning the shot into a plan
        // view. Same azimuth as the studio, so the cinema still looks like the
        // same application.
        constexpr float kCinemaYaw = 0.70f;
        constexpr float kCinemaPitch = 0.72f;
        camera.set_orbit(kCinemaYaw, kCinemaPitch);
        camera.fit_oriented(bounds->min, bounds->max, kCinemaFill);
        return true;
    }
    camera.fit(bounds->min, bounds->max);
    return true;
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
    result_bounds_.reset();
    const auto emit = [&](std::uint32_t node, const Eigen::Vector3d& normal) {
        // Apply exaggerated displacement so the deformed shape is visible.
        const Eigen::Vector3d pos =
            result_rest_[node] + static_cast<double>(deform_scale) * disp_at(node);
        result_bounds_.add(pos);
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
        deformed[i] = result_rest_[i] + static_cast<double>(deform_scale) *
                                            disp_at(static_cast<std::uint32_t>(i));
    }
    upload_boundary_edges(deformed, result_quads_, 0.02f, 0.02f, 0.04f, 0.95f,
                          result_edge_vbo_, result_edge_vertex_count_);

    baked_mode_ = mode;
    baked_scale_ = deform_scale;
    baked_max_ = result_max;
    result_dirty_ = false;

    // First bake of a fresh result: frame the deformed shape once, then leave
    // the camera to the user (later bakes only re-scale the exaggeration).
    // Inside a cinema take the arming is left standing instead: the take is one
    // continuous shot, and the result act must not cut to a new framing.
    if (frame_on_bake_ && !camera_locked_) {
        frame_on_bake_ = false;
        if (result_bounds_.valid) {
            camera.fit(result_bounds_.min, result_bounds_.max);
        }
    }
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
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const Eigen::Matrix4f view = camera.view();
    const Eigen::Matrix4f proj = camera.projection(aspect);
    const Eigen::Vector3f eye = camera.eye();

    // The cinema is its own pass end to end — own programs, own buffers, own
    // blend and depth state — so it returns here instead of threading a fifth
    // case through the setup/mesh/results ladder below. deform_scale,
    // result_max, show_wireframe and show_undeformed do not apply to it; the
    // parameters it does read all come from set_cinema_view().
    if (mode == DisplayMode::kCinema) {
        draw_cinema(view, proj, eye);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glUseProgram(model_program_);
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

void Viewport::draw_cinema(const Eigen::Matrix4f& view, const Eigen::Matrix4f& proj,
                           const Eigen::Vector3f& eye) {
    // Clamped here rather than in set_cinema_view so the panel can drive these
    // from an easing curve that briefly overshoots without tearing the frame.
    const float skeleton_alpha = std::clamp(cinema_view_.skeleton_alpha, 0.0f, 1.0f);
    const float reveal = std::clamp(cinema_view_.reveal, 0.0f, 1.0f);
    const float shrink = std::clamp(cinema_view_.shrink, 0.0f, 1.0f);
    const float mesh_alpha = std::clamp(cinema_view_.mesh_alpha, 0.0f, 1.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (skeleton_vertex_count_ > 0 && skeleton_alpha > 0.0f) {
        if (skeleton_alpha != skeleton_baked_alpha_) {
            // The line program is shared with the wireframe passes and stays
            // untouched, so it has no alpha uniform: fading the skeleton means
            // rewriting the alpha channel of its vertex colours. That is one
            // buffer update of 7 floats per vertex — 280 KB for a 5k-segment
            // skeleton — and only on frames where the alpha actually moved.
            for (std::size_t i = 6; i < skeleton_data_.size(); i += 7) {
                skeleton_data_[i] = skeleton_alpha;
            }
            glBindBuffer(GL_ARRAY_BUFFER, skeleton_vbo_);
            glBufferSubData(GL_ARRAY_BUFFER, 0,
                            static_cast<GLsizeiptr>(skeleton_data_.size() * sizeof(float)),
                            skeleton_data_.data());
            skeleton_baked_alpha_ = skeleton_alpha;
        }
        glUseProgram(line_program_);
        glUniformMatrix4fv(glGetUniformLocation(line_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(line_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        // The opening act carries the frame on these lines alone, so they are
        // drawn heavier than the studio wireframe. Core profile may clamp the
        // width; drivers that honour it look better, and this path is cinema-
        // only so the studio's own 1.5 px wireframe is untouched.
        glLineWidth(2.6f);
        // Depth writes off: the skeleton is the part's whole edge graph, front
        // and back, and it must not occlude the cells that grow through it. The
        // solid drawn next still covers whatever it should cover.
        glDepthMask(GL_FALSE);
        glBindVertexArray(skeleton_vao_);
        glDrawArrays(GL_LINES, 0, skeleton_vertex_count_);
        glDepthMask(GL_TRUE);
        glLineWidth(1.0f);
    }

    // reveal == 0 would discard every fragment anyway; skipping the draw keeps
    // the pre-mesh act from paying for a full pass over the element buffer.
    if (cinema_vertex_count_ > 0 && reveal > 0.0f && mesh_alpha > 0.0f) {
        const bool draw_edges = cinema_view_.edges && cinema_edge_vertex_count_ > 0 &&
                                cinema_view_.edge_alpha > 0.0f;
        if (draw_edges) {
            // Push the fill back so the cell edges win the depth test against
            // their own faces, same as the wireframe pass above.
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        glUseProgram(cinema_program_);
        glUniformMatrix4fv(glGetUniformLocation(cinema_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(cinema_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        glUniform3f(glGetUniformLocation(cinema_program_, "u_eye"), eye.x(), eye.y(), eye.z());
        glUniform1f(glGetUniformLocation(cinema_program_, "u_reveal"), reveal);
        glUniform1f(glGetUniformLocation(cinema_program_, "u_shrink"), shrink);
        glUniform1f(glGetUniformLocation(cinema_program_, "u_alpha"), mesh_alpha);
        // Depth writes stay ON while mesh_alpha < 1: the fade is an act
        // transition, not a material. A solid that stops occluding itself
        // mid-fade reads as a rendering bug rather than a dissolve.
        glBindVertexArray(cinema_vao_);
        glDrawArrays(GL_TRIANGLES, 0, cinema_vertex_count_);

        if (draw_edges) {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDepthFunc(GL_LEQUAL);
            glUseProgram(cinema_line_program_);
            glUniformMatrix4fv(glGetUniformLocation(cinema_line_program_, "u_view"), 1,
                               GL_FALSE, view.data());
            glUniformMatrix4fv(glGetUniformLocation(cinema_line_program_, "u_proj"), 1,
                               GL_FALSE, proj.data());
            // The edge buffer carries the same centroids and indices as the
            // fill, so the same reveal/shrink keep the two exactly in step.
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_reveal"), reveal);
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_shrink"), shrink);
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_alpha"),
                        mesh_alpha * std::clamp(cinema_view_.edge_alpha, 0.0f, 1.0f));
            glLineWidth(std::clamp(cinema_view_.edge_width, 0.5f, 8.0f));
            glBindVertexArray(cinema_edge_vao_);
            glDrawArrays(GL_LINES, 0, cinema_edge_vertex_count_);
            glLineWidth(1.0f);
            glDepthFunc(GL_LESS);
        }
    }

    glDisable(GL_BLEND);
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
