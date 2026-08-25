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
#include <unordered_map>

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
uniform float u_alpha;
out vec4 frag;
void main() {
    // Bright, still-shaped headlight: fields stay legible at README scale while
    // normals and a restrained rim continue to carry the three-dimensional form.
    vec3 n = normalize(v_normal);
    vec3 view_dir = normalize(u_eye - v_pos);
    float ndv = abs(dot(n, view_dir));
    float shade = 0.50 + 0.48 * ndv;
    float rim = pow(1.0 - ndv, 3.0) * 0.20;
    frag = vec4(min(v_color.rgb * shade + vec3(rim), vec3(1.0)), u_alpha * v_color.a);
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
uniform float u_alpha;
out vec4 frag;
void main() {
    frag = vec4(v_color.rgb, u_alpha * v_color.a);
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
    vec2 p = v_uv - vec2(0.54, 0.52);
    float halo = exp(-5.5 * dot(p, p));
    float vignette = smoothstep(0.82, 0.28, length(p));
    color += vec3(0.012, 0.035, 0.050) * halo;
    color *= 0.90 + 0.10 * vignette;
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
layout(location = 5) in float in_role;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_shrink;
uniform float u_reveal;
uniform float u_arrival;
uniform float u_transition;
uniform int u_transition_active;
out vec3 v_normal;
out vec4 v_color;
out vec3 v_pos;
flat out float v_index;
void main() {
    float shrink = u_shrink;
    float alpha = 1.0;
    float index_value = in_index;
    if (u_transition_active != 0) {
        if (in_role < -0.5) {
            float q = smoothstep(0.0, 0.45, u_transition);
            shrink = 0.92 * q;
            alpha = 1.0 - q;
            index_value = -1.0;
        } else if (in_role > 0.5) {
            float q = clamp((u_transition - 0.40) / 0.60, 0.0, 1.0);
            shrink = 0.72 * (1.0 - smoothstep(0.0, 0.35, q));
            alpha = smoothstep(0.0, 0.20, q);
        } else {
            shrink = 0.06 * sin(3.14159265 * u_transition);
            index_value = -1.0;
        }
    } else if (u_arrival > 0.001) {
        // A cell that has just been emitted arrives collapsed toward its own
        // centroid and opens to full size as the front moves past it, so the
        // reveal reads as cells being placed one after another rather than as a
        // colour wash sweeping over finished geometry.
        float heat = 1.0 - clamp((u_reveal - index_value) / u_arrival, 0.0, 1.0);
        shrink = max(shrink, 0.62 * heat * heat);
    }
    vec3 pos = mix(in_pos, in_centroid, shrink);
    v_normal = in_normal;
    v_color = vec4(in_color.rgb, in_color.a * alpha);
    v_pos = pos;
    v_index = index_value;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
})";

// The arrival band is a pure uniform: `u_arrival` is a width in the same
// normalised element-index units as `u_reveal`, so a cell's age behind the
// front is `u_reveal - v_index`. Cells inside the band are lit hot and cool
// back into their own element-type colour as the front leaves them behind. A
// zero width disables it, which is what a settled mesh asks for.
constexpr const char* kCinemaFs = R"(#version 330 core
in vec3 v_normal;
in vec4 v_color;
in vec3 v_pos;
flat in float v_index;
uniform vec3 u_eye;
uniform float u_reveal;
uniform float u_arrival;
uniform float u_alpha;
out vec4 frag;
void main() {
    if (v_index >= u_reveal || v_color.a <= 0.001) {
        discard;
    }
    vec3 n = normalize(v_normal);
    vec3 view_dir = normalize(u_eye - v_pos);
    float ndv = abs(dot(n, view_dir));
    float shade = 0.50 + 0.48 * ndv;
    float rim = pow(1.0 - ndv, 3.0) * 0.20;
    vec3 lit = min(v_color.rgb * shade + vec3(rim), vec3(1.0));
    if (u_arrival > 0.001) {
        float heat = 1.0 - clamp((u_reveal - v_index) / u_arrival, 0.0, 1.0);
        float w = heat * heat;
        vec3 hot = mix(vec3(1.00, 0.72, 0.30), vec3(1.00, 0.97, 0.88), w);
        lit = mix(lit, hot, 0.96 * w);
    }
    frag = vec4(lit, u_alpha * v_color.a);
})";

constexpr const char* kCinemaLineVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_centroid;
layout(location = 3) in float in_index;
layout(location = 4) in float in_role;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_shrink;
uniform float u_reveal;
uniform float u_arrival;
uniform float u_transition;
uniform int u_transition_active;
out vec4 v_color;
flat out float v_index;
void main() {
    float shrink = u_shrink;
    float alpha = 1.0;
    float index_value = in_index;
    if (u_transition_active != 0) {
        if (in_role < -0.5) {
            float q = smoothstep(0.0, 0.45, u_transition);
            shrink = 0.92 * q;
            alpha = 1.0 - q;
            index_value = -1.0;
        } else if (in_role > 0.5) {
            float q = clamp((u_transition - 0.40) / 0.60, 0.0, 1.0);
            shrink = 0.72 * (1.0 - smoothstep(0.0, 0.35, q));
            alpha = smoothstep(0.0, 0.20, q);
        } else {
            shrink = 0.06 * sin(3.14159265 * u_transition);
            index_value = -1.0;
        }
    } else if (u_arrival > 0.001) {
        // Same arrival pop as the fill, so a cell's outline never separates
        // from the cell it belongs to.
        float heat = 1.0 - clamp((u_reveal - index_value) / u_arrival, 0.0, 1.0);
        shrink = max(shrink, 0.62 * heat * heat);
    }
    vec3 pos = mix(in_pos, in_centroid, shrink);
    v_color = vec4(in_color.rgb, in_color.a * alpha);
    v_index = index_value;
    gl_Position = u_proj * u_view * vec4(pos, 1.0);
})";

constexpr const char* kCinemaLineFs = R"(#version 330 core
in vec4 v_color;
flat in float v_index;
uniform float u_reveal;
uniform float u_arrival;
uniform float u_alpha;
out vec4 frag;
void main() {
    if (v_index >= u_reveal || v_color.a <= 0.001) {
        discard;
    }
    vec3 rgb = v_color.rgb;
    float a = u_alpha * v_color.a;
    if (u_arrival > 0.001) {
        // The dense-mesh edge is a near-black annotation. Inside the arrival
        // band it becomes a bright outline instead, so the newest cells read as
        // individually drawn rather than as a lit patch.
        float heat = 1.0 - clamp((u_reveal - v_index) / u_arrival, 0.0, 1.0);
        float w = heat * heat;
        rgb = mix(rgb, vec3(1.00, 0.88, 0.62), w);
        a = mix(a, min(1.0, a + 0.55), w);
    }
    frag = vec4(rgb, a);
})";

// Opening-act evidence glyphs. Surface samples carry physical target spacing;
// the representative edge carries the κ(s) cursor; the remaining edge-network
// samples show every independently filtered CAD curve on the part. Curvature is
// normalised independently of cell size, so colour never conflates the two.
constexpr const char* kSizingVs = R"(#version 330 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in float in_value_before;
layout(location = 2) in float in_value_after;
layout(location = 3) in float in_order;
layout(location = 4) in float in_kind;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_edge_reveal;
uniform float u_edge_cursor;
uniform float u_edge_cursor_alpha;
uniform float u_field_reveal;
uniform float u_filter_mix;
uniform float u_alpha;
out vec4 v_color;
flat out float v_kind;
void main() {
    bool edge = in_kind > 0.5;
    bool selected_edge = in_kind > 0.5 && in_kind < 1.5;
    bool edge_network = in_kind > 1.5;
    float reveal = edge ? u_edge_reveal : u_field_reveal;
    float value = mix(in_value_before, in_value_after, u_filter_mix);
    float visible = in_order <= reveal ? 1.0 : 0.0;
    vec3 low = vec3(0.20, 0.82, 0.91);
    vec3 high = vec3(0.98, 0.39, 0.18);
    vec3 color = mix(low, high, value);
    float cursor = selected_edge
        ? (1.0 - smoothstep(0.018, 0.085, abs(in_order - u_edge_cursor))) *
              u_edge_cursor_alpha
        : 0.0;
    color = mix(color, vec3(1.0), 0.48 * cursor);
    float opacity = selected_edge ? (0.52 + 0.48 * cursor)
                                  : (edge_network ? 0.72 : 1.0);
    v_color = vec4(color, u_alpha * visible * opacity);
    v_kind = in_kind;
    gl_PointSize = selected_edge ? mix(9.0, 23.0, cursor)
                                 : (edge_network ? mix(5.0, 8.0, value)
                                                 : mix(4.0, 12.0, value));
    gl_Position = u_proj * u_view * vec4(in_pos, 1.0);
    // Keep a sample drawn on the surface from losing a z-fight with the cells
    // it is explaining.
    gl_Position.z -= 0.0015 * gl_Position.w;
})";

constexpr const char* kSizingFs = R"(#version 330 core
in vec4 v_color;
flat in float v_kind;
out vec4 frag;
void main() {
    if (v_color.a <= 0.001) {
        discard;
    }
    float radius = length(gl_PointCoord - vec2(0.5));
    if (radius > 0.5) {
        discard;
    }
    float ring = smoothstep(0.31, 0.39, radius);
    float interior_alpha = v_kind > 1.5 ? 0.34 : (v_kind > 0.5 ? 0.58 : 0.22);
    float alpha = mix(interior_alpha, 0.95, ring) * v_color.a;
    vec3 color = mix(v_color.rgb * 0.72, v_color.rgb, ring);
    frag = vec4(color, alpha);
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

// Interleaved float counts per vertex for the two cinema buffers. The final
// role scalar is -1 removed, 0 persistent/ordinary, +1 newly added.
constexpr int kCinemaFillFloats = 15; // pos3 normal3 rgba4 centroid3 index1 role1
constexpr int kCinemaEdgeFloats = 12; // pos3 rgba4 centroid3 index1 role1

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
    attr(5, 1, 14); // transition role
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
    attr(4, 1, 11); // transition role
}

void bind_sizing_attr(GLuint vao, GLuint vbo) {
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    constexpr GLsizei stride = 7 * sizeof(float); // pos3 h_before h_after order kind
    const auto attr = [](GLuint index, GLint size, int offset_floats) {
        glEnableVertexAttribArray(index);
        glVertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(
                                  static_cast<std::uintptr_t>(offset_floats) * sizeof(float)));
    };
    attr(0, 3, 0);
    attr(1, 1, 3);
    attr(2, 1, 4);
    attr(3, 1, 5);
    attr(4, 1, 6);
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
enum class CinemaCellFamily : std::uint8_t { kTet, kHex, kPrism, kPyramid, kPoly };

struct CinemaCellKey {
    CinemaCellFamily family = CinemaCellFamily::kPoly;
    std::uint8_t count = 0;
    std::array<std::array<std::int64_t, 3>, 8> corners{};

    bool operator==(const CinemaCellKey&) const = default;
};

struct CinemaCellKeyHash {
    std::size_t operator()(const CinemaCellKey& key) const {
        std::size_t h = static_cast<std::size_t>(key.family) * 1315423911u + key.count;
        for (std::size_t i = 0; i < key.count; ++i) {
            for (const std::int64_t v : key.corners[i]) {
                const auto x = static_cast<std::uint64_t>(v);
                h ^= static_cast<std::size_t>(x + 0x9e3779b97f4a7c15ull + (h << 6u) +
                                              (h >> 2u));
            }
        }
        return h;
    }
};

std::optional<CinemaCellKey> cinema_cell_key(const fea::NodalMesh& mesh,
                                             const fea::NodalElement& element,
                                             const Eigen::Vector3d& origin,
                                             double inverse_tolerance) {
    const std::size_t corners = corner_node_count(element);
    if (corners < 4 || corners > 8 || element.nodes.size() < corners) {
        return std::nullopt;
    }
    CinemaCellKey key;
    key.count = static_cast<std::uint8_t>(corners);
    switch (element.type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        key.family = CinemaCellFamily::kTet;
        break;
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        key.family = CinemaCellFamily::kHex;
        break;
    case fea::ElementType::kPrism6:
        key.family = CinemaCellFamily::kPrism;
        break;
    case fea::ElementType::kPyramid5:
        key.family = CinemaCellFamily::kPyramid;
        break;
    case fea::ElementType::kPolyVem:
        key.family = CinemaCellFamily::kPoly;
        break;
    }
    for (auto& corner : key.corners) {
        corner = {std::numeric_limits<std::int64_t>::max(),
                  std::numeric_limits<std::int64_t>::max(),
                  std::numeric_limits<std::int64_t>::max()};
    }
    for (std::size_t i = 0; i < corners; ++i) {
        if (element.nodes[i] >= mesh.nodes.size()) {
            return std::nullopt;
        }
        const Eigen::Vector3d p = (mesh.nodes[element.nodes[i]] - origin) * inverse_tolerance;
        key.corners[i] = {std::llround(p.x()), std::llround(p.y()), std::llround(p.z())};
    }
    std::sort(key.corners.begin(), key.corners.end());
    return key;
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

/// Neutral carry colour for the first solved field, deliberately outside the
/// `fea_colormap` range. Later handoffs carry the preceding measured field
/// instead, at that field's own scale.
constexpr std::array<float, 3> kUnsweptGrey = {0.24f, 0.25f, 0.28f};
/// How far the front itself pulls the blended colour toward white. A straight
/// crossfade has no visible edge; this band is the only thing that makes the
/// handoff read as a moving front rather than an abrupt field swap.
constexpr float kFrontHighlight = 0.65f;

/// Colour of one surface sample under the spatial handoff, where `x` is the
/// sample's position along the sweep axis as a fraction of the result's own
/// extent. `rgb` is the arriving field and `carry` is either the prior measured
/// field or the neutral mesh-grey state.
///
/// Behind `front - feather`, the arriving colour is returned bit for bit. Ahead
/// of `front`, the carried colour is returned bit for bit. Only the narrow
/// feather blends their display colours and lifts its leading edge toward white.
/// Neither source scalar nor either field's normalisation is interpolated.
std::array<float, 3> sweep_sample_color(std::array<float, 3> rgb,
                                        const std::array<float, 3>& carry, float x,
                                        float front, float feather) {
    if (x >= front) {
        return carry;
    }
    const float band = std::max(feather, 0.0f);
    const float lead = front - band;
    if (x <= lead) {
        return rgb;
    }
    const float w = band > 0.0f ? (x - lead) / band : 1.0f;
    for (std::size_t i = 0; i < rgb.size(); ++i) {
        const float faded = rgb[i] + w * (carry[i] - rgb[i]);
        rgb[i] = faded + kFrontHighlight * w * (1.0f - faded);
    }
    return rgb;
}

} // namespace

std::array<float, 3> element_type_color(fea::ElementType type) { return type_color(type); }

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
                          float fill, float aspect) {
    target_ = (0.5 * (bbox_min + bbox_max)).cast<float>();
    const Eigen::Vector3f half = (0.5 * (bbox_max - bbox_min)).cast<float>().cwiseAbs();
    if (!(half.maxCoeff() > 1.0e-9f)) {
        distance_ = 1.0f;
        return;
    }
    // Orbit basis, built exactly as eye()/view() build it. It depends only on
    // yaw/pitch, so it is available before the distance this function solves for.
    const Eigen::Vector3f dir(std::cos(pitch_) * std::cos(yaw_), std::sin(pitch_),
                              std::cos(pitch_) * std::sin(yaw_));
    const Eigen::Vector3f f = -Eigen::Vector3f(dir.x(), dir.z(), dir.y()); // eye -> target
    const Eigen::Vector3f s = f.cross(Eigen::Vector3f(0, 0, 1)).normalized();
    const Eigen::Vector3f u = s.cross(f);
    const float safe_aspect = std::max(aspect, 1.0e-6f);
    const float tan_y =
        std::clamp(fill, 0.05f, 1.0f) * std::tan(0.5f * fov_y_);
    const float tan_x = tan_y * safe_aspect;
    float required = 0.0f;
    // Solve the perspective inequalities on the eight AABB corners themselves:
    //   |x| <= (distance + f·d) tan_x
    //   |y| <= (distance + f·d) tan_y
    // The old support-function bound maximised numerator and depth on different
    // corners, discarding roughly half the frame on the previous hero.
    for (int ix = -1; ix <= 1; ix += 2) {
        for (int iy = -1; iy <= 1; iy += 2) {
            for (int iz = -1; iz <= 1; iz += 2) {
                const Eigen::Vector3f d(static_cast<float>(ix) * half.x(),
                                        static_cast<float>(iy) * half.y(),
                                        static_cast<float>(iz) * half.z());
                const float depth_offset = f.dot(d);
                required = std::max(
                    {required, std::fabs(s.dot(d)) / tan_x - depth_offset,
                     std::fabs(u.dot(d)) / tan_y - depth_offset,
                     -depth_offset + 1.0e-3f * std::max(half.norm(), 1.0f)});
            }
        }
    }
    distance_ = required > 1.0e-9f ? required : 1.0f;
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
    sizing_program_ = link(kSizingVs, kSizingFs);
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
    glGenVertexArrays(1, &sizing_vao_);
    glGenBuffers(1, &sizing_vbo_);

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
    bind_sizing_attr(sizing_vao_, sizing_vbo_);
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
    cinema_motion_bounds_.reset();
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

void Viewport::set_cinema_feature_samples(
    const std::vector<Eigen::Vector3d>& field_points,
    const std::vector<double>& field_h_before,
    const std::vector<double>& field_h_after,
    const std::vector<Eigen::Vector3d>& curve_points,
    const std::vector<double>& curvature_raw,
    const std::vector<double>& curvature_filtered,
    const std::vector<Eigen::Vector3d>& network_points,
    const std::vector<double>& network_curvature_raw,
    const std::vector<double>& network_curvature_filtered) {
    sizing_vertex_count_ = 0;
    const bool field_ok = field_points.size() == field_h_before.size() &&
                          field_points.size() == field_h_after.size();
    const bool curve_ok = curve_points.size() == curvature_raw.size() &&
                          curve_points.size() == curvature_filtered.size();
    const bool network_ok =
        network_points.size() == network_curvature_raw.size() &&
        network_points.size() == network_curvature_filtered.size();
    if (!field_ok || !curve_ok || !network_ok) {
        glBindBuffer(GL_ARRAY_BUFFER, sizing_vbo_);
        glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW);
        return;
    }

    const auto value_range = [](const std::vector<double>& before,
                                const std::vector<double>& after) {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        const auto measure = [&](const std::vector<double>& values) {
            for (const double value : values) {
                if (std::isfinite(value)) {
                    lo = std::min(lo, value);
                    hi = std::max(hi, value);
                }
            }
        };
        measure(before);
        measure(after);
        return std::pair{lo, hi};
    };
    const auto [h_lo, h_hi] = value_range(field_h_before, field_h_after);
    double k_lo = std::numeric_limits<double>::infinity();
    double k_hi = -std::numeric_limits<double>::infinity();
    for (const auto& [before, after] :
         {std::pair{&curvature_raw, &curvature_filtered},
          std::pair{&network_curvature_raw, &network_curvature_filtered}}) {
        const auto [lo_value, hi_value] = value_range(*before, *after);
        k_lo = std::min(k_lo, lo_value);
        k_hi = std::max(k_hi, hi_value);
    }
    const auto normalise = [](double value, double lo, double hi) {
        if (!std::isfinite(value) || !std::isfinite(lo) || !std::isfinite(hi)) {
            return 0.5f;
        }
        const double span = hi - lo;
        if (!(span > std::numeric_limits<double>::epsilon())) {
            return 0.5f;
        }
        return static_cast<float>(std::clamp((value - lo) / span, 0.0, 1.0));
    };

    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
    if (!field_points.empty()) {
        lo = field_points.front();
        hi = field_points.front();
        for (const auto& p : field_points) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
    }
    const Eigen::Vector3d extent = hi - lo;
    int sweep_axis = 0;
    for (int axis = 1; axis < 3; ++axis) {
        if (extent[axis] > extent[sweep_axis]) {
            sweep_axis = axis;
        }
    }
    const double sweep_span = std::max(extent[sweep_axis], 1.0e-12);

    std::vector<float> data;
    data.reserve((field_points.size() + curve_points.size() + network_points.size()) * 7);
    const auto append = [&](const Eigen::Vector3d& p, float before, float after, float order,
                            float kind) {
        data.insert(data.end(), {static_cast<float>(p.x()), static_cast<float>(p.y()),
                                 static_cast<float>(p.z()), before, after, order, kind});
    };
    for (std::size_t i = 0; i < field_points.size(); ++i) {
        const float order = static_cast<float>(
            std::clamp((field_points[i][sweep_axis] - lo[sweep_axis]) / sweep_span, 0.0, 1.0));
        append(field_points[i], normalise(field_h_before[i], h_lo, h_hi),
               normalise(field_h_after[i], h_lo, h_hi), order, 0.0f);
    }
    const double curve_denom =
        curve_points.size() > 1 ? static_cast<double>(curve_points.size() - 1) : 1.0;
    for (std::size_t i = 0; i < curve_points.size(); ++i) {
        append(curve_points[i], normalise(curvature_raw[i], k_lo, k_hi),
               normalise(curvature_filtered[i], k_lo, k_hi),
               static_cast<float>(static_cast<double>(i) / curve_denom), 1.0f);
    }
    const double network_denom =
        network_points.size() > 1 ? static_cast<double>(network_points.size() - 1) : 1.0;
    for (std::size_t i = 0; i < network_points.size(); ++i) {
        append(network_points[i], normalise(network_curvature_raw[i], k_lo, k_hi),
               normalise(network_curvature_filtered[i], k_lo, k_hi),
               static_cast<float>(static_cast<double>(i) / network_denom), 2.0f);
    }

    sizing_vertex_count_ =
        static_cast<int>(field_points.size() + curve_points.size() + network_points.size());
    glBindBuffer(GL_ARRAY_BUFFER, sizing_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                 data.data(), GL_DYNAMIC_DRAW);
}

void Viewport::set_cinema_mesh(const fea::NodalMesh& mesh) {
    std::vector<CinemaCellRef> cells;
    cells.reserve(mesh.elements.size());
    const double denom = mesh.elements.empty() ? 1.0 : static_cast<double>(mesh.elements.size());
    for (std::size_t i = 0; i < mesh.elements.size(); ++i) {
        cells.push_back({&mesh, i, 0.0f, static_cast<float>(static_cast<double>(i) / denom)});
    }
    cinema_transition_active_ = false;
    cinema_unchanged_element_count_ = 0;
    cinema_removed_element_count_ = 0;
    cinema_added_element_count_ = 0;
    upload_cinema_cells(cells, mesh.elements.size());
}

void Viewport::set_cinema_mesh_transition(const fea::NodalMesh& previous,
                                          const fea::NodalMesh& next) {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
    bool have_bounds = false;
    for (const fea::NodalMesh* mesh : {&previous, &next}) {
        for (const Eigen::Vector3d& p : mesh->nodes) {
            if (have_bounds) {
                lo = lo.cwiseMin(p);
                hi = hi.cwiseMax(p);
            } else {
                lo = p;
                hi = p;
                have_bounds = true;
            }
        }
    }
    const double diagonal = have_bounds ? (hi - lo).norm() : 0.0;
    const double tolerance = std::max(diagonal * 1.0e-10, 1.0e-12);
    const double inverse_tolerance = 1.0 / tolerance;

    using KeyMap = std::unordered_multimap<CinemaCellKey, std::size_t, CinemaCellKeyHash>;
    KeyMap old_by_key;
    old_by_key.reserve(previous.elements.size());
    for (std::size_t i = 0; i < previous.elements.size(); ++i) {
        if (const auto key =
                cinema_cell_key(previous, previous.elements[i], lo, inverse_tolerance)) {
            old_by_key.emplace(*key, i);
        }
    }

    std::vector<bool> old_matched(previous.elements.size(), false);
    std::vector<std::size_t> unchanged;
    std::vector<std::size_t> added;
    unchanged.reserve(std::min(previous.elements.size(), next.elements.size()));
    added.reserve(next.elements.size() / 4 + 8);
    for (std::size_t i = 0; i < next.elements.size(); ++i) {
        const auto key = cinema_cell_key(next, next.elements[i], lo, inverse_tolerance);
        if (!key) {
            added.push_back(i);
            continue;
        }
        const auto range = old_by_key.equal_range(*key);
        auto match = range.first;
        while (match != range.second && old_matched[match->second]) {
            ++match;
        }
        if (match == range.second) {
            added.push_back(i);
            continue;
        }
        old_matched[match->second] = true;
        unchanged.push_back(i);
    }

    std::vector<std::size_t> removed;
    removed.reserve(previous.elements.size() - std::min(previous.elements.size(),
                                                        unchanged.size()));
    for (std::size_t i = 0; i < old_matched.size(); ++i) {
        if (!old_matched[i]) {
            removed.push_back(i);
        }
    }

    std::vector<CinemaCellRef> cells;
    cells.reserve(unchanged.size() + removed.size() + added.size());
    for (const std::size_t i : unchanged) {
        cells.push_back({&next, i, 0.0f, -1.0f});
    }
    for (const std::size_t i : removed) {
        cells.push_back({&previous, i, -1.0f, -1.0f});
    }
    const double added_denom = added.empty() ? 1.0 : static_cast<double>(added.size());
    for (std::size_t ordinal = 0; ordinal < added.size(); ++ordinal) {
        cells.push_back({&next, added[ordinal], 1.0f,
                         static_cast<float>(static_cast<double>(ordinal) / added_denom)});
    }

    cinema_transition_active_ = true;
    cinema_unchanged_element_count_ = unchanged.size();
    cinema_removed_element_count_ = removed.size();
    cinema_added_element_count_ = added.size();
    upload_cinema_cells(cells, next.elements.size());
}

void Viewport::upload_cinema_cells(const std::vector<CinemaCellRef>& cells,
                                   std::size_t logical_element_count) {
    // EVERY selected element's own faces, not the shared boundary surface. In a
    // transition the selection is persistent-next + removed-previous +
    // added-next, and the role scalar lets the shader animate those sets
    // independently without rebuilding topology per frame.
    cinema_bounds_.reset();
    cinema_element_count_ = logical_element_count;
    cinema_skipped_element_count_ = 0;

    std::vector<float> data;  // pos3 normal3 rgba4 centroid3 index1 role1
    std::vector<float> edata; // pos3 rgba4 centroid3 index1 role1
    data.reserve(cells.size() * 12 * kCinemaFillFloats);
    edata.reserve(cells.size() * 12 * kCinemaEdgeFloats);

    FaceLoops loops;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
    for (const CinemaCellRef& cell : cells) {
        if (cell.mesh == nullptr || cell.element >= cell.mesh->elements.size()) {
            continue;
        }
        const auto& mesh = *cell.mesh;
        const auto& el = mesh.elements[cell.element];
        if (!element_face_loops(el, mesh.nodes.size(), loops)) {
            // Removed cells are historical transition geometry; skipped counts
            // describe only the authoritative next mesh.
            if (cell.role >= 0.0f) {
                ++cinema_skipped_element_count_;
            }
            continue;
        }
        const std::size_t corners = corner_node_count(el);
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (std::size_t i = 0; i < corners; ++i) {
            centroid += mesh.nodes[el.nodes[i]];
        }
        centroid /= static_cast<double>(corners);
        auto rgb = type_color(el.type);
        if (cell.role < -0.5f) {
            rgb = {0.95f, 0.42f, 0.22f};
        } else if (cell.role > 0.5f) {
            rgb = {0.20f, 0.82f, 0.88f};
        }
        constexpr float shade = 0.92f;

        for (std::size_t li = 0; li < loops.count(); ++li) {
            const std::uint32_t begin = loops.starts[li];
            const std::size_t n_face = loops.starts[li + 1] - begin;
            if (n_face < 3) {
                continue;
            }
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
                             static_cast<float>(centroid.z()), cell.reveal_index, cell.role});
            };
            for (std::size_t k = 1; k + 1 < n_face; ++k) {
                emit_fill(loops.nodes[begin]);
                emit_fill(loops.nodes[begin + k]);
                emit_fill(loops.nodes[begin + k + 1]);
            }
        }

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
                          static_cast<float>(centroid.z()), cell.reveal_index, cell.role});
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
std::size_t Viewport::cinema_unchanged_element_count() const {
    return cinema_unchanged_element_count_;
}

std::size_t Viewport::cinema_removed_element_count() const {
    return cinema_removed_element_count_;
}

std::size_t Viewport::cinema_added_element_count() const {
    return cinema_added_element_count_;
}


void Viewport::set_cinema_view(const CinemaView& view) { cinema_view_ = view; }

void Viewport::set_result(const SolveResult& result, const std::vector<double>* nodal_extra) {
    // Store rest positions + scalars; bake when mode/scale/range changes.
    result_rest_.clear();
    result_scalar_vm_.clear();
    result_scalar_u_.clear();
    result_scalar_eta_.clear();
    result_scalar_extra_.clear();
    result_quads_.clear();
    const auto surface = polymesh::fea::tessellate_boundary_surface(result.volume_mesh, 8);
    result_rest_.reserve(surface.samples.size());
    result_scalar_vm_.reserve(surface.samples.size());
    result_scalar_u_.reserve(surface.samples.size());
    result_scalar_eta_.reserve(surface.samples.size());
    // A wrong-sized extra field is a caller bug, and the honest response is to
    // show nothing rather than a field that is not the one the legend names:
    // an off-by-one node count would otherwise paint a plausible-looking
    // picture of the wrong quantity. Zeros are unmistakable.
    const bool has_extra =
        nodal_extra != nullptr && nodal_extra->size() == result.volume_mesh.nodes.size();
    if (has_extra) {
        result_scalar_extra_.reserve(surface.samples.size());
    }
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
        if (has_extra) {
            result_scalar_extra_.push_back(interpolate_scalar(sample, *nodal_extra));
        }
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

void Viewport::set_cinema_motion_bounds(const SolveResult& result, float deform_scale) {
    cinema_motion_bounds_.reset();
    const auto& nodes = result.volume_mesh.nodes;
    const bool has_displacement =
        result.displacement.size() == 3 * static_cast<Eigen::Index>(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        cinema_motion_bounds_.add(nodes[i]);
        if (has_displacement) {
            const Eigen::Index base = 3 * static_cast<Eigen::Index>(i);
            const Eigen::Vector3d moved =
                nodes[i] + static_cast<double>(deform_scale) *
                               result.displacement.segment<3>(base);
            cinema_motion_bounds_.add(moved);
        }
    }
}

void Viewport::set_field_sweep(const FieldSweep& sweep) { field_sweep_ = sweep; }

bool Viewport::frame_content(DisplayMode mode, float aspect) {
    // The cinema spans several uploads whose union is framed before frame zero:
    // exact skeleton, every recorded mesh, and the final displayed motion
    // envelope. Once recording starts the camera remains locked.
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
    case DisplayMode::kResultsGradient:
        bounds = result_bounds_.valid ? &result_bounds_ : &mesh_bounds_;
        break;
    case DisplayMode::kCinema:
        for (const Bounds* part :
             {&skeleton_bounds_, &cinema_bounds_, &cinema_motion_bounds_}) {
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
        constexpr float kCinemaFill = 0.84f; // reserves room for BC glyphs and force arrow
        // A raised three-quarter view exposes the A-arm's fork, two rear eyes
        // and out-of-plane loaded boss in one readable silhouette.
        constexpr float kCinemaYaw = 0.70f;
        constexpr float kCinemaPitch = 0.72f;
        camera.set_orbit(kCinemaYaw, kCinemaPitch);
        camera.fit_oriented(bounds->min, bounds->max, kCinemaFill, aspect);
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
    } else if (mode == DisplayMode::kResultsGradient) {
        // Empty when the caller passed no extra field, and `emit` reads out of
        // range as zero, so the mode bakes a flat floor instead of borrowing
        // whichever field happens to be loaded.
        //
        // A zero INSIDE a field the caller did supply is a different thing and
        // is drawn as what it is: fea::nodal_scalar_gradient_magnitude returns
        // an exact 0.0 for a rank-deficient node patch as well as for a
        // genuinely flat field, and only its n_unresolved counter separates
        // them. So an isolated dark spot here can be legitimate -- measured
        // none on any structured hex or Kuhn-tet lattice, all 132,651 nodes of
        // a 50^3 lattice resolved -- and this viewport cannot tell the two
        // apart. Anything that needs to must read n_unresolved at the source.
        scalars = &result_scalar_extra_;
    }

    // Optional measured carry field for a field-to-field handoff. kCinema is
    // the sentinel for the neutral mesh-grey state that precedes the first solve.
    const std::vector<double>* carry_scalars = nullptr;
    switch (field_sweep_.carry_mode) {
    case DisplayMode::kResultsVonMises:
        carry_scalars = &result_scalar_vm_;
        break;
    case DisplayMode::kResultsDisplacement:
        carry_scalars = &result_scalar_u_;
        break;
    case DisplayMode::kResultsError:
        carry_scalars = &result_scalar_eta_;
        break;
    case DisplayMode::kResultsGradient:
        carry_scalars = &result_scalar_extra_;
        break;
    default:
        break;
    }
    const float carry_denom = field_sweep_.carry_max > 0.0f ? field_sweep_.carry_max : 1.0f;

    // Spatial handoff, per surface sample (see sweep_sample_color). Both fields
    // retain their own values and scales; only the moving feather blends their
    // display colours.
    const float sweep_axis_norm = field_sweep_.axis.norm();
    Eigen::Vector3d sweep_dir = Eigen::Vector3d::Zero();
    double sweep_u_min = 0.0;
    double sweep_span = 0.0;
    bool sweep_on = field_sweep_.active && sweep_axis_norm > 0.0f && !result_rest_.empty();
    if (sweep_on) {
        // One pass over the rest positions per bake: `x` below is a fraction of
        // the result's OWN extent along the axis, so a caller animating `front`
        // from 0 to 1 always crosses the whole part exactly once, whatever the
        // part's size or where it sits in world space.
        sweep_dir = (field_sweep_.axis / sweep_axis_norm).cast<double>();
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (const Eigen::Vector3d& p : result_rest_) {
            const double u = sweep_dir.dot(p);
            lo = std::min(lo, u);
            hi = std::max(hi, u);
        }
        sweep_u_min = lo;
        sweep_span = hi - lo;
        sweep_on = sweep_span > 0.0;
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
        auto rgb = fea_colormap(static_cast<float>(s) / denom);
        if (sweep_on) {
            std::array<float, 3> carry = kUnsweptGrey;
            if (carry_scalars != nullptr) {
                const double prior = node < carry_scalars->size() ? (*carry_scalars)[node] : 0.0;
                carry = fea_colormap(static_cast<float>(prior) / carry_denom);
            }
            // Rest position, not the deformed one: the reveal is a fixed plane
            // through the part, and letting the exaggerated warp move it would
            // make the front wobble as the load ramps.
            const float x = static_cast<float>(
                (sweep_dir.dot(result_rest_[node]) - sweep_u_min) / sweep_span);
            rgb = sweep_sample_color(rgb, carry, x, field_sweep_.front,
                                     field_sweep_.feather);
        }
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
    baked_sweep_ = field_sweep_;
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

    const bool results_mode =
        mode == DisplayMode::kResultsVonMises || mode == DisplayMode::kResultsDisplacement ||
        mode == DisplayMode::kResultsError || mode == DisplayMode::kResultsGradient;
    const float result_alpha =
        results_mode ? std::clamp(cinema_view_.result_alpha, 0.0f, 1.0f) : 1.0f;
    const float rest_alpha =
        results_mode ? std::clamp(cinema_view_.rest_surface_alpha, 0.0f, 1.0f) : 0.0f;
    if (rest_alpha > 0.0f && model_vertex_count_ > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(glGetUniformLocation(model_program_, "u_alpha"), rest_alpha);
        glDepthMask(GL_FALSE);
        glBindVertexArray(model_vao_);
        glDrawArrays(GL_TRIANGLES, 0, model_vertex_count_);
        glDepthMask(GL_TRUE);
    }
    glUniform1f(glGetUniformLocation(model_program_, "u_alpha"), result_alpha);
    if (result_alpha < 1.0f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

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
        // The sweep is part of the bake key because the front lives in the
        // vertex colours: an animated front changes nothing else about the
        // result, so without this a moving wavefront would never re-upload.
        const FieldSweep& baked = baked_sweep_;
        const bool sweep_changed =
            baked.active != field_sweep_.active || baked.axis != field_sweep_.axis ||
            baked.front != field_sweep_.front || baked.feather != field_sweep_.feather ||
            baked.carry_mode != field_sweep_.carry_mode ||
            baked.carry_max != field_sweep_.carry_max;
        if (result_dirty_ || baked_mode_ != mode || baked_scale_ != deform_scale ||
            baked_max_ != result_max || sweep_changed) {
            bake_result(mode, deform_scale, result_max);
        }
        if (result_alpha > 0.001f && result_vertex_count_ > 0) {
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
        glUniform1f(glGetUniformLocation(line_program_, "u_alpha"), result_alpha);
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

    if (results_mode && cinema_view_.overlay_on_results) {
        // The carried mesh is composited over the fading field. Its geometry is
        // the exact recorded stage/transition already uploaded by the cinema.
        draw_cinema(view, proj, eye);
    }
    glDisable(GL_BLEND);

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
    const float arrival = std::clamp(cinema_view_.arrival_band, 0.0f, 1.0f);
    const bool incremental = cinema_view_.incremental_transition && cinema_transition_active_;
    const float transition = std::clamp(cinema_view_.transition_progress, 0.0f, 1.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float model_alpha = std::clamp(cinema_view_.model_alpha, 0.0f, 1.0f);
    if (model_vertex_count_ > 0 && model_alpha > 0.0f) {
        glUseProgram(model_program_);
        glUniformMatrix4fv(glGetUniformLocation(model_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(model_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        glUniform3f(glGetUniformLocation(model_program_, "u_eye"), eye.x(), eye.y(), eye.z());
        glUniform1f(glGetUniformLocation(model_program_, "u_alpha"), model_alpha);
        glBindVertexArray(model_vao_);
        glDrawArrays(GL_TRIANGLES, 0, model_vertex_count_);
    }

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
        glUniform1f(glGetUniformLocation(line_program_, "u_alpha"), 1.0f);
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

    const float sizing_alpha = std::clamp(cinema_view_.spectral_overlay_alpha, 0.0f, 1.0f);
    if (sizing_vertex_count_ > 0 && sizing_alpha > 0.0f) {
        glUseProgram(sizing_program_);
        glUniformMatrix4fv(glGetUniformLocation(sizing_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(sizing_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        glUniform1f(glGetUniformLocation(sizing_program_, "u_edge_reveal"),
                    std::clamp(cinema_view_.spectral_edge_reveal, 0.0f, 1.0f));
        glUniform1f(glGetUniformLocation(sizing_program_, "u_edge_cursor"),
                    std::clamp(cinema_view_.spectral_curve_cursor, 0.0f, 1.0f));
        glUniform1f(glGetUniformLocation(sizing_program_, "u_edge_cursor_alpha"),
                    std::clamp(cinema_view_.spectral_curve_cursor_alpha, 0.0f, 1.0f));
        glUniform1f(glGetUniformLocation(sizing_program_, "u_field_reveal"),
                    std::clamp(cinema_view_.spectral_field_reveal, 0.0f, 1.0f));
        glUniform1f(glGetUniformLocation(sizing_program_, "u_filter_mix"),
                    std::clamp(cinema_view_.spectral_filter_mix, 0.0f, 1.0f));
        glUniform1f(glGetUniformLocation(sizing_program_, "u_alpha"), sizing_alpha);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDepthMask(GL_FALSE);
        glDepthFunc(GL_LEQUAL);
        glBindVertexArray(sizing_vao_);
        glDrawArrays(GL_POINTS, 0, sizing_vertex_count_);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_PROGRAM_POINT_SIZE);
    }

    // An incremental transition still draws persistent/removed cells when the
    // added-cell reveal is zero.
    if (cinema_vertex_count_ > 0 && (reveal > 0.0f || incremental) && mesh_alpha > 0.0f) {
        const bool draw_edges = cinema_view_.edges && cinema_edge_vertex_count_ > 0 &&
                                cinema_view_.edge_alpha > 0.0f;
        if (draw_edges) {
            // Push ordinary cinema fill back so its own edges win. A cinema mesh
            // carried over an already depth-written result surface must instead
            // come slightly forward or the previous state would be requested but
            // fully occluded by the new one.
            glEnable(GL_POLYGON_OFFSET_FILL);
            const float offset = cinema_view_.overlay_on_results ? -1.0f : 1.0f;
            glPolygonOffset(offset, offset);
        }
        glDepthFunc(GL_LEQUAL);
        glUseProgram(cinema_program_);
        glUniformMatrix4fv(glGetUniformLocation(cinema_program_, "u_view"), 1, GL_FALSE,
                           view.data());
        glUniformMatrix4fv(glGetUniformLocation(cinema_program_, "u_proj"), 1, GL_FALSE,
                           proj.data());
        glUniform3f(glGetUniformLocation(cinema_program_, "u_eye"), eye.x(), eye.y(), eye.z());
        glUniform1f(glGetUniformLocation(cinema_program_, "u_reveal"), reveal);
        glUniform1f(glGetUniformLocation(cinema_program_, "u_arrival"), arrival);
        glUniform1f(glGetUniformLocation(cinema_program_, "u_shrink"), shrink);
        glUniform1f(glGetUniformLocation(cinema_program_, "u_transition"), transition);
        glUniform1i(glGetUniformLocation(cinema_program_, "u_transition_active"),
                    incremental ? 1 : 0);
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
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_arrival"), arrival);
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_shrink"), shrink);
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_transition"),
                        transition);
            glUniform1i(glGetUniformLocation(cinema_line_program_, "u_transition_active"),
                        incremental ? 1 : 0);
            glUniform1f(glGetUniformLocation(cinema_line_program_, "u_alpha"),
                        mesh_alpha * std::clamp(cinema_view_.edge_alpha, 0.0f, 1.0f));
            glLineWidth(std::clamp(cinema_view_.edge_width, 0.5f, 8.0f));
            glBindVertexArray(cinema_edge_vao_);
            glDrawArrays(GL_LINES, 0, cinema_edge_vertex_count_);
            glLineWidth(1.0f);
            glDepthFunc(GL_LESS);
        }
    }
    glDepthFunc(GL_LESS);

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
